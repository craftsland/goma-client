// Copyright 2015 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "oauth2_token.h"

#include <memory>
#include <sstream>
#include <vector>

#include "autolock_timer.h"
#include "callback.h"
#include "compiler_specific.h"
#include "file_helper.h"
#include "glog/logging.h"
#include "http.h"
#include "json/json.h"
#include "json_util.h"
#include "jwt.h"
#include "scoped_fd.h"
#include "socket_factory.h"
#include "string_piece.h"
#include "string_piece_utils.h"

namespace devtools_goma {

namespace {

using std::string;

const char kGCERefreshToken[] = "gce-metadata-service-account";
const char kServiceAccountRefreshTokenPrefix[] =
    "google-cloud-service-account:";
const char kgRPCType[] = "authorized_user";

// If something error happens during the refresh, the refresh task retries
// refresh for this time period.
static const int kRefreshTimeoutSec = 10;
// If something error happens in the refresh of access token, the refresh task
// will not fetch access token again for this period.
static const int kErrorRefreshPendingSec = 60;

class AuthRefreshConfig {
 public:
  virtual ~AuthRefreshConfig() {}
  virtual bool enabled() const = 0;
  virtual bool valid() const = 0;
  virtual bool GetOAuth2Config(OAuth2Config* config) const = 0;
  virtual bool SetOAuth2Config(const OAuth2Config& config) = 0;
  virtual bool CanRefresh() const = 0;
  virtual bool InitRequest(HttpRequest* req) const = 0;
  // TODO: use StringPiece for resp_body instead?
  virtual bool ParseResponseBody(const string& resp_body,
                                 string* token_type,
                                 string* access_token,
                                 int* expires_in) const = 0;
};

class GoogleOAuth2AccessTokenRefreshTask : public OAuth2AccessTokenRefreshTask {
 public:
  // Doesn't take ownership of wm.
  // Takes ownership of client and req.
  GoogleOAuth2AccessTokenRefreshTask(
      WorkerThreadManager* wm,
      std::unique_ptr<AuthRefreshConfig> config,
      std::unique_ptr<HttpClient> client,
      std::unique_ptr<HttpRequest> req)
      : wm_(wm),
        config_(std::move(config)),
        client_(std::move(client)),
        req_(std::move(req)),
        cond_(&mu_) {
    LOG_IF(ERROR, !config_->enabled() || !config_->valid())
        << "config enabled=" << config_->enabled()
        << " valid=" << config_->valid();
  }

  ~GoogleOAuth2AccessTokenRefreshTask() override {
    CHECK(!cancel_refresh_now_);
    CHECK(!cancel_refresh_);
    CHECK(shutting_down_);
  }

  string GetAccount() override {
    string access_token;
    {
      AUTOLOCK(lock, &mu_);
      access_token = access_token_;
      if (access_token.empty()) {
        return "";
      }
      if (!account_email_.empty()) {
        return account_email_;
      }
    }

    HttpClient::Options options = client_->options();
    options.InitFromURL(kGoogleTokenInfoURI);
    HttpClient client(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm_);

    HttpRequest req;
    std::ostringstream param;
    param << "?access_token=" << access_token;
    client.InitHttpRequest(&req, "GET", param.str());
    req.AddHeader("Connection", "close");

    HttpResponse resp;
    HttpClient::Status status;
    LOG(INFO) << "get tokeninfo for access_token";
    client.Do(&req, &resp, &status);
    if (status.err) {
      LOG(WARNING) << "tokeninfo err=" << status.err
                   << " " << status.err_message;
      return "";
    }
    if (status.http_return_code != 200) {
      LOG(WARNING) << "tokeninfo status=" << status.http_return_code;
      return "";
    }

    string email;
    {
      string err;
      Json::Reader reader;
      Json::Value root;
      if (reader.parse(string(resp.Body()), root, false)) {
        if (!GetNonEmptyStringFromJson(root, "email", &email, &err)) {
          LOG(WARNING) << "parse tokeninfo: " << err;
        }
      } else {
        LOG(WARNING) << "invalid json";
      }
    }
    {
      AUTOLOCK(lock, &mu_);
      account_email_ = email;
    }
    return email;
  }

  bool GetOAuth2Config(OAuth2Config* config) const override {
    return config_->GetOAuth2Config(config);
  }

  bool SetOAuth2Config(const OAuth2Config& config) override {
    if (!config_->SetOAuth2Config(config)) {
      LOG(WARNING) << "failed to set oauth2 config.";
      return false;
    }
    token_expires_at_ = time(nullptr);
    token_type_.clear();
    access_token_.clear();
    account_email_.clear();
    return true;
  }

  string GetAuthorization() const override {
    time_t now = time(nullptr);
    AUTOLOCK(lock, &mu_);
    if (now < token_expires_at_ &&
        !token_type_.empty() && !access_token_.empty()) {
      return token_type_ + " " + access_token_;
    }
    return "";
  }

  bool ShouldRefresh() const override {
    time_t now = time(nullptr);
    AUTOLOCK(lock, &mu_);
    if (!config_->CanRefresh()) {
      return false;
    }
    if (last_network_error_ > 0 &&
        now < last_network_error_ + kErrorRefreshPendingSec) {
      LOG(WARNING)
          << "prohibit to refresh OAuth2 access token for certain duration."
          << " last_network_error=" << last_network_error_
          << " pending=" << kErrorRefreshPendingSec;
      return false;
    }
    return now >= token_expires_at_ ||
        token_type_.empty() || access_token_.empty();
  }

  void RunAfterRefresh(WorkerThreadManager::ThreadId thread_id,
                       OneshotClosure* closure) override {
    time_t now = time(nullptr);
    {
      AUTOLOCK(lock, &mu_);
      if (now < token_expires_at_ || shutting_down_) {
        DCHECK(shutting_down_ || !access_token_.empty());
        // access token is valid or oauth2 not available, go ahead.
        wm_->RunClosureInThread(FROM_HERE,
                                thread_id, closure,
                                WorkerThreadManager::PRIORITY_MED);
        return;
      }
      if (last_network_error_ > 0 &&
          now < last_network_error_ + kErrorRefreshPendingSec) {
        LOG(WARNING) << "will not refresh token."
                     << " last_network_error=" << last_network_error_
                     << " pending=" << kErrorRefreshPendingSec;
        wm_->RunClosureInThread(FROM_HERE,
                                thread_id, closure,
                                WorkerThreadManager::PRIORITY_MED);
        return;
      }
      // should refresh access token.
      pending_tasks_.push_back(std::make_pair(thread_id, closure));
      switch (state_) {
        case NOT_STARTED: // first run.
          state_ = RUN;
          refresh_deadline_ = now + kRefreshTimeoutSec;
          refresh_backoff_ms_ = client_->options().min_retry_backoff_ms;
          break;
        case RUN:
          return;
      }
      if (!has_set_thread_id_) {
        refresh_task_thread_id_ = wm_->GetCurrentThreadId();
        has_set_thread_id_ = true;
      }
      wm_->RunClosureInThread(
          FROM_HERE,
          refresh_task_thread_id_,
          NewCallback(
              this, &GoogleOAuth2AccessTokenRefreshTask::RunRefresh),
          WorkerThreadManager::PRIORITY_IMMEDIATE);
    }
  }

  void Shutdown() override {
    {
      AUTOLOCK(lock, &mu_);
      if (shutting_down_) {
        return;
      }
      shutting_down_ = true;
      if (cancel_refresh_now_ || cancel_refresh_) {
        if (THREAD_ID_IS_SELF(refresh_task_thread_id_)) {
          // in goma_fetch.cc, refresh_task_thread_id_ and current thread
          // is same, so call cancel in the same thread.
          // since Wait() is also called on the same thread, there would be
          // no chance to run Cancel on the thread and never get cond_
          // signalled.
          if (cancel_refresh_now_) {
            LOG(INFO) << "cancel now " << cancel_refresh_now_;
            cancel_refresh_now_->Cancel();
            cancel_refresh_now_ = nullptr;
            cond_.Signal();
          }
          if (cancel_refresh_) {
            LOG(INFO) << "cancel " << cancel_refresh_now_;
            cancel_refresh_now_->Cancel();
            cancel_refresh_now_ = nullptr;
            cond_.Signal();
          }
        } else {
          LOG(INFO) << "cancelling now..." << cancel_refresh_now_;
          LOG(INFO) << "cancelling..." << cancel_refresh_;
          wm_->RunClosureInThread(
              FROM_HERE,
              refresh_task_thread_id_,
              NewCallback(
                  this, &GoogleOAuth2AccessTokenRefreshTask::Cancel),
              WorkerThreadManager::PRIORITY_IMMEDIATE);
        }
      }
    }
    client_->Shutdown();
  }

  void Wait() override {
    {
      AUTOLOCK(lock, &mu_);
      CHECK(shutting_down_) << "You must call Shutdown() beforehand.";
      LOG(INFO) << "Wait cancel_refresh_now=" << cancel_refresh_now_;
      LOG(INFO) << "Wait cancel_refresh_=" << cancel_refresh_;
      while (cancel_refresh_now_ != nullptr || cancel_refresh_ != nullptr) {
        cond_.Wait();
      }
    }
    client_.reset();
  }

 private:
  enum State {
    NOT_STARTED,
    RUN,
  };

  void InitRequest() {
    if (!config_->enabled()) {
      LOG(INFO) << "not enabled.";
      return;
    }
    if (!config_->InitRequest(req_.get())) {
      LOG(WARNING) << "failed to init request.";
    }
  }

  void ParseOAuth2AccessTokenUnlocked(int* next_update_in) {
    static const int kOAuthExpireTimeMarginInSec = 60;
    if (status_->err != OK) {
      LOG(ERROR) << "HTTP communication failed to refresh OAuth2 access token."
                 << " err_message=" << status_->err_message;
      return;
    }
    int expires_in = 0;
    if (!config_->ParseResponseBody(string(resp_.Body()),
                                    &token_type_,
                                    &access_token_,
                                    &expires_in)) {
      LOG(ERROR) << "Failed to parse OAuth2 access token:"
                 << resp_.Body();
      token_type_.clear();
      access_token_.clear();
      account_email_.clear();
      return;
    }
    time_t now = time(nullptr);
    token_expires_at_ = now + expires_in - kOAuthExpireTimeMarginInSec;
    LOG(INFO) << "Got new OAuth2 access token."
              << " now=" << now
              << " expires_in=" << expires_in
              << " token_expires_at=" << token_expires_at_;
    VLOG(1) << "access_token=" << access_token_;
    // expires_in is usually large enough. e.g. 3600.
    // If it is small, auto update of access token will not work.
    *next_update_in = expires_in - kOAuthExpireTimeMarginInSec * 2;
    LOG_IF(WARNING, *next_update_in <= 0)
        << "expires_in is too small.  auto update will not work."
        << " next_update_in=" << *next_update_in
        << " expires_in=" << expires_in
        << " kOAuthExpireTimeMarginInSec=" << kOAuthExpireTimeMarginInSec;
  }

  void Done() {
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    bool http_ok = true;
    if (status_->err != OK &&
        (status_->http_return_code == 0 ||
         status_->http_return_code / 100 == 5)) {
      time_t now = time(nullptr);
      http_ok = false;
      {
        AUTOLOCK(lock, &mu_);
        if (now < refresh_deadline_) {
          LOG(WARNING) << "refresh failed http=" << status_->http_return_code
                       << " retry until deadline=" << refresh_deadline_
                       << " refresh_backoff_ms_=" << refresh_backoff_ms_;

          refresh_backoff_ms_ = HttpClient::BackoffMsec(
              client_->options(), refresh_backoff_ms_, true);
          LOG(INFO) << "backoff"
                    << " refresh_backoff_ms=" << refresh_backoff_ms_;
          CHECK(cancel_refresh_ == nullptr)
              << "Somebody else seems to run refresh task and failing?";
          cancel_refresh_ = wm_->RunDelayedClosureInThread(
              FROM_HERE,
              wm_->GetCurrentThreadId(),
              refresh_backoff_ms_,
              NewCallback(
                  this, &GoogleOAuth2AccessTokenRefreshTask::RunRefresh));
          return;
        }
        LOG(WARNING) << "refresh failed http=" << status_->http_return_code
                     << " deadline_exceeded now=" << now
                     << " deadline=" << refresh_deadline_;

        // If last_network_error_ is set, ShouldRefresh() starts returning
        // false to make task local fallback.  Let me make it postponed
        // until refresh attempts reaches refresh_deadline_.
        last_network_error_ = now;
      }
    }
    LOG_IF(ERROR, status_->err != OK)
        << "refresh failed."
        << " err=" << status_->err
        << " err_message=" << status_->err_message
        << " http=" << status_->http_return_code;
    VLOG(1) << "Get access token done.";
    std::vector<std::pair<WorkerThreadManager::ThreadId,
                          OneshotClosure*>> callbacks;
    int next_update_in = 0;
    {
      AUTOLOCK(lock, &mu_);
      DCHECK_EQ(state_, RUN);
      state_ = NOT_STARTED;
      refresh_deadline_ = 0;
      ParseOAuth2AccessTokenUnlocked(&next_update_in);
      if (http_ok && !access_token_.empty()) {
        last_network_error_ = 0;
        refresh_backoff_ms_ = 0;
      }
      callbacks.swap(pending_tasks_);
    }
    for (const auto& callback : callbacks) {
      wm_->RunClosureInThread(FROM_HERE,
                              callback.first, callback.second,
                              WorkerThreadManager::PRIORITY_MED);
    }
    if (next_update_in > 0) {
      {
        AUTOLOCK(lock, &mu_);
        if (shutting_down_) {
          return;
        }
        if (cancel_refresh_now_) {
          // The other RunRefreshNow task seems to be running.
          // We will not add new delayed task.
          LOG(INFO) << "The other OAuth2 RunRefreshNow task has already been "
                    << "registred.  We will not override with newone.";
          return;
        }

        DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
        cancel_refresh_now_ = wm_->RunDelayedClosureInThread(
            FROM_HERE, refresh_task_thread_id_,
            next_update_in * 1000,
            NewCallback(
                this, &GoogleOAuth2AccessTokenRefreshTask::RunRefreshNow));
      }
      LOG(INFO) << "Registered the OAuth2 refresh task to be executed later."
                << " next_update_in=" << next_update_in;
    }
  }

  void RunRefreshUnlocked() {
    DCHECK_EQ(RUN, state_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    InitRequest();
    // Make HttpClient get access token.
    LOG(INFO) << "Going to refresh OAuth2 access token.";
    resp_.Reset();
    status_.reset(new HttpClient::Status);
    status_->trace_id = "oauth2Refresh";
    client_->DoAsync(
        req_.get(), &resp_, status_.get(),
        NewCallback(
            this, &GoogleOAuth2AccessTokenRefreshTask::Done));
  }

  void RunRefresh() {
    LOG(INFO) << "Run refresh.";

    AUTOLOCK(lock, &mu_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));

    // Set nullptr to make OAuth2AccessTokenRefreshTask::Cancel() know
    // it must not execute cancel_refresh_->Cancel().
    cancel_refresh_ = nullptr;
    cond_.Signal();
    if (shutting_down_) {
      return;
    }
    RunRefreshUnlocked();
  }

  // RunRefreshNow() is used for RunDelayedClosureInThread in Done() above.
  void RunRefreshNow() {
    LOG(INFO) << "Run refresh now.";

    AUTOLOCK(lock, &mu_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    CHECK(cancel_refresh_now_)
        << "RunRefreshNow has been cancelled, but called?";
    // Set nullptr to make OAuth2AccessTokenRefreshTask::Cancel() know
    // it must not execute cancel_refresh_now_->Cancel().
    cancel_refresh_now_ = nullptr;
    cond_.Signal();
    if (shutting_down_) {
      return;
    }
    switch (state_) {
      case NOT_STARTED: // first run.
        state_ = RUN;
        refresh_deadline_ = time(nullptr) + kRefreshTimeoutSec;
        refresh_backoff_ms_ = client_->options().min_retry_backoff_ms;
        break;
      case RUN:
        return;
    }
    RunRefreshUnlocked();
  }

  void Cancel() {
    AUTOLOCK(lock, &mu_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    if (cancel_refresh_now_) {
      cancel_refresh_now_->Cancel();
      cancel_refresh_now_ = nullptr;
      cond_.Signal();
      LOG(INFO) << "cancelled";
    }
    if (cancel_refresh_) {
      cancel_refresh_->Cancel();
      cancel_refresh_ = nullptr;
      cond_.Signal();
      LOG(INFO) << "cancelled";
    }
  }

  WorkerThreadManager* wm_;
  std::unique_ptr<AuthRefreshConfig> config_;
  std::unique_ptr<HttpClient> client_;
  std::unique_ptr<HttpRequest> req_;
  HttpResponse resp_;
  std::unique_ptr<HttpClient::Status> status_;

  Lock mu_;  // protecting following members.
  // signaled when cancel_refresh_now_ or cancel_refresh_ become nullptr.
  ConditionVariable cond_;
  State state_ = NOT_STARTED;
  time_t refresh_deadline_ = 0;
  string token_type_;
  string access_token_;
  string account_email_;
  time_t token_expires_at_ = 0;
  time_t last_network_error_ = 0;
  int refresh_backoff_ms_ = 0;
  std::vector<std::pair<WorkerThreadManager::ThreadId,
                        OneshotClosure*>> pending_tasks_;

  // This class cannot have an ownership of CancelableClosure.
  // It is valid until Cancel() is called or the closure is executed, and
  // cancel_refresh_now_ is used as a flag to represent the CancelableClosure
  // is valid (i.e. we can execute cancel_refresh_now_->Cancel()).
  //
  // cancel_refresh_now_ should set to nullptr when it become invalid.
  // cancel_refresh_ should also set to nullptr when it become invalid.
  WorkerThreadManager::CancelableClosure* cancel_refresh_now_ = nullptr;
  WorkerThreadManager::CancelableClosure* cancel_refresh_ = nullptr;
  WorkerThreadManager::ThreadId refresh_task_thread_id_;
  bool has_set_thread_id_ = false;
  bool shutting_down_ = false;

  DISALLOW_COPY_AND_ASSIGN(GoogleOAuth2AccessTokenRefreshTask);
};

class OAuth2RefreshConfig : public AuthRefreshConfig {
 public:
  OAuth2RefreshConfig(const OAuth2RefreshConfig&) = delete;
  OAuth2RefreshConfig& operator=(const OAuth2RefreshConfig&) = delete;

  bool enabled() const override {
    return config_.enabled();
  }

  bool valid() const override {
    return config_.valid();
  }

  bool GetOAuth2Config(OAuth2Config* config) const override {
    if (!config_.enabled() && config_.refresh_token != kGCERefreshToken) {
      return false;
    }
    *config = config_;
    return true;
  }

  bool SetOAuth2Config(const OAuth2Config& config) override {
    if (config_.token_uri != config.token_uri) {
      LOG(ERROR) << "unacceptable token_uri change:" << config.token_uri;
      return false;
    }
    if (config_.refresh_token.empty() && !config.refresh_token.empty()) {
      LOG(INFO) << "set refresh token";
    } else if (config.refresh_token.empty()) {
      LOG(WARNING) << "clear refresh token";
    } else if (config_.refresh_token != config.refresh_token) {
      LOG(INFO) << "update refresh token";
    }
    config_ = config;
    return true;
  }

  bool CanRefresh() const override {
    // if refresh token is not given, couldn't get access token and
    // no need to refresh.
    // go with logout state (i.e. no Authorization header).
    return !config_.refresh_token.empty();
  }

  bool ParseResponseBody(const string& resp_body,
                         string* token_type,
                         string* access_token,
                         int* expires_in) const override {
    return ParseOAuth2AccessToken(
        resp_body, token_type, access_token, expires_in);
  }

 protected:
  explicit OAuth2RefreshConfig(const OAuth2Config& config)
      : config_(config) {}

  OAuth2Config config_;
};

class GCEServiceAccountRefreshConfig : public OAuth2RefreshConfig {
 public:
  GCEServiceAccountRefreshConfig(const GCEServiceAccountRefreshConfig&)
      = delete;
  GCEServiceAccountRefreshConfig&
      operator=(const GCEServiceAccountRefreshConfig&) = delete;

  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    LOG(INFO) << "gce service account:"
              << http_options.gce_service_account;
    // https://cloud.google.com/compute/docs/authentication#applications
    const char kMetadataURI[] =
        "http://metadata/computeMetadata/v1/instance/service-accounts/";
    std::ostringstream url;
    url << kMetadataURI << http_options.gce_service_account << "/token";
    options.InitFromURL(url.str());
    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm));

    // HTTP setup.
    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "GET", "");
    req->AddHeader("Connection", "close");
    req->AddHeader("Metadata-Flavor", "Google");

    OAuth2Config config = http_options.oauth2_config;
    config.auth_uri = kGoogleAuthURI;
    config.token_uri = kGoogleTokenURI;
    config.scope = "scope_is_configured_when_instance_created";
    config.client_id = "client_is_not_needed";
    config.client_secret = "client_secret_is_not_needed";
    config.refresh_token = kGCERefreshToken;

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new GCEServiceAccountRefreshConfig(config));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool InitRequest(HttpRequest* req) const override {
    // on GCE, just get service account token from metadata server.
    LOG(INFO) << "init request:GCE service account";
    return true;
  }

 private:
  explicit GCEServiceAccountRefreshConfig(const OAuth2Config& config)
      : OAuth2RefreshConfig(config) {}
};

class ServiceAccountRefreshConfig : public OAuth2RefreshConfig {
 public:
  ServiceAccountRefreshConfig(const ServiceAccountRefreshConfig&) = delete;
  ServiceAccountRefreshConfig&
      operator=(const ServiceAccountRefreshConfig&) = delete;

  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    LOG(INFO) << "service account:"
              << http_options.service_account_json_filename;
    // https://developers.google.com/identity/protocols/OAuth2ServiceAccount#authorizingrequests
    options.InitFromURL(kGoogleTokenAudienceURI);
    string path = options.url_path_prefix;
    options.url_path_prefix = "/";
    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm));

    // HTTP setup.
    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "POST", path);
    req->SetContentType("application/x-www-form-urlencoded");
    req->AddHeader("Connection", "close");
    OAuth2Config config = http_options.oauth2_config;
    config.auth_uri = kGoogleAuthURI;
    config.token_uri = kGoogleTokenURI;
    config.scope = kGomaAuthScope;
    config.client_id = "client_is_not_needed";
    config.client_secret = "client_secret_is_not_needed";
    config.refresh_token = kServiceAccountRefreshTokenPrefix +
        http_options.service_account_json_filename;
    LOG(INFO) << config.refresh_token;

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new ServiceAccountRefreshConfig(config));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool InitRequest(HttpRequest* req) const override {
    const string& service_account_json_filename =
        config_.refresh_token.substr(
            strlen(kServiceAccountRefreshTokenPrefix));
    LOG(INFO) << service_account_json_filename;
    // service account.
    string saj;
    if (!ReadFileToString(service_account_json_filename, &saj)) {
      LOG(ERROR) << "Failed to read "
                 << service_account_json_filename;
      return false;
    }
    ServiceAccountConfig sa;
    if (!ParseServiceAccountJson(saj, &sa)) {
      LOG(ERROR) << "Failed to parse service account json in "
                 << service_account_json_filename;
      return false;
    }
    std::unique_ptr<JsonWebToken::Key> key(JsonWebToken::LoadKey(
        sa.private_key));
    if (key == nullptr) {
      LOG(ERROR) << "Invalid private key in "
                 << service_account_json_filename;
      return false;
    }
    LOG(INFO) << "service account:"
              << sa.client_email
              << " client_id=" << sa.client_id
              << " project_id=" << sa.project_id
              << " private_key_id=" << sa.private_key_id;
    JsonWebToken::ClaimSet cs;
    cs.iss = sa.client_email;
    cs.scopes.emplace_back(kGomaAuthScope);
    cs.expires_in_sec = 3600;
    JsonWebToken jwt(cs);
    string assertion = jwt.Token(*key, time(nullptr));
    const string req_body = strings::StrCat(
        "grant_type=", JsonWebToken::kGrantTypeEncoded,
        "&assertion=", assertion);
    VLOG(1) << req_body;
    req->SetBody(req_body);
    return true;
  }

 private:
  explicit ServiceAccountRefreshConfig(const OAuth2Config& config)
      : OAuth2RefreshConfig(config) {}
};

class RefreshTokenRefreshConfig : public OAuth2RefreshConfig {
 public:
  RefreshTokenRefreshConfig(const RefreshTokenRefreshConfig&) = delete;
  RefreshTokenRefreshConfig&
      operator=(const RefreshTokenRefreshConfig&) = delete;

  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    LOG(INFO) << "oauth2 enabled";

    OAuth2Config config = http_options.oauth2_config;
    if (config.token_uri != kGoogleTokenURI) {
      LOG(ERROR) << "unsupported token_uri=" << config.token_uri;
      return nullptr;
    }
    options.InitFromURL(kGoogleTokenURI);
    string path = options.url_path_prefix;
    // client will be used for tokeninfo path too
    // clear path prefix in options and put path in request.
    options.url_path_prefix = "/";
    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm));

    // HTTP setup.
    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "POST", path);
    req->SetContentType("application/x-www-form-urlencoded");
    req->AddHeader("Connection", "close");
    config.type = kgRPCType;

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new RefreshTokenRefreshConfig(config));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool InitRequest(HttpRequest* req) const override {
    LOG(INFO) << "init request:refresh token";

    // TODO: reconstruct client if config_.token_uri has been changed?
    const string req_body = strings::StrCat(
        "client_id=", config_.client_id,
        "&client_secret=", config_.client_secret,
        "&refresh_token=", config_.refresh_token,
        "&grant_type=refresh_token");
    VLOG(1) << req_body;
    req->SetBody(req_body);
    return true;
  }

 private:
  explicit RefreshTokenRefreshConfig(const OAuth2Config& config)
      : OAuth2RefreshConfig(config) {}
};

class LuciAuthRefreshConfig : public AuthRefreshConfig {
 public:
  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    static const char kLuciLocalAuthServiceHost[] = "127.0.0.1";
    static const char kLuciLocalAuthServicePath[] =
        "/rpc/LuciLocalAuthService.GetOAuthToken";

    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    const LuciContextAuth& local_auth = http_options.luci_context_auth;
    options.use_ssl = false;
    options.dest_host_name = kLuciLocalAuthServiceHost;
    options.dest_port = local_auth.rpc_port;
    options.url_path_prefix = kLuciLocalAuthServicePath;

    LOG(INFO) << "LUCI_CONTEXT local_auth is used with account: "
              << local_auth.default_account_id;

    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        nullptr, options, wm));

    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "POST", "");
    req->SetContentType("application/json");
    req->AddHeader("Connection", "close");

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new LuciAuthRefreshConfig(local_auth));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool enabled() const override {
    return true;
  }

  bool valid() const override {
    return local_auth_.enabled();
  }

  bool GetOAuth2Config(OAuth2Config* config) const override {
    LOG(WARNING) << "GetOAuth2Config won't work for LUCI_CONTEXT.";
    return false;
  }

  bool SetOAuth2Config(const OAuth2Config& config) override {
    LOG(WARNING) << "SetOAuth2Config won't work for LUCI_CONTEXT.";
    return false;
  }

  bool CanRefresh() const override {
    return valid();
  }

  bool InitRequest(HttpRequest* req) const override {
    LuciOAuthTokenRequest treq;
    treq.scopes.push_back(kGomaAuthScope);
    treq.secret = local_auth_.secret;
    treq.account_id = local_auth_.default_account_id;

    VLOG(1) << treq.ToString();
    req->SetBody(treq.ToString());
    return true;
  }

  bool ParseResponseBody(const string& resp_body,
                         string* token_type,
                         string* access_token,
                         int* expires_in) const override {
    static const char kTokenType[] = "Bearer";
    LuciOAuthTokenResponse resp;
    if (!ParseLuciOAuthTokenResponse(resp_body, &resp)) {
      LOG(WARNING) << "Failed to parse luci auth token response."
                   << " body=" << resp_body;
      return false;
    }
    time_t now = time(nullptr);
    *token_type = kTokenType;
    *access_token = resp.access_token;
    *expires_in = resp.expiry - now;
    return true;
  }

 private:
  explicit LuciAuthRefreshConfig(const LuciContextAuth& local_auth)
      : local_auth_(local_auth) {}

  LuciContextAuth local_auth_;
};

}  // namespace

/* static */
std::unique_ptr<OAuth2AccessTokenRefreshTask>
OAuth2AccessTokenRefreshTask::New(
    WorkerThreadManager* wm,
    const HttpClient::Options& http_options) {
  if (!http_options.gce_service_account.empty()) {
    return GCEServiceAccountRefreshConfig::New(wm, http_options);
  }

  if (!http_options.service_account_json_filename.empty()) {
    return ServiceAccountRefreshConfig::New(wm, http_options);
  }

  if (http_options.oauth2_config.enabled()) {
    return RefreshTokenRefreshConfig::New(wm, http_options);
  }

  if (http_options.luci_context_auth.enabled()) {
    return LuciAuthRefreshConfig::New(wm, http_options);
  }

  return nullptr;
}

string ExchangeOAuth2RefreshToken(
    WorkerThreadManager* wm,
    const HttpClient::Options& http_options,
    const OAuth2Config& config,
    const string& code,
    const string& redirect_uri) {
  if (config.token_uri != kGoogleTokenURI) {
    LOG(ERROR) << "unsupported token_uri=" << config.token_uri;
    return "";
  }
  HttpClient::Options options = http_options;
  options.InitFromURL(kGoogleTokenURI);
  HttpClient client(
      HttpClient::NewSocketFactoryFromOptions(options),
      HttpClient::NewTLSEngineFactoryFromOptions(options),
      options, wm);

  HttpRequest req;
  client.InitHttpRequest(&req, "POST", "");
  req.SetContentType("application/x-www-form-urlencoded");
  req.AddHeader("Connection", "close");

  std::ostringstream req_body;
  req_body << "code=" << code
           << "&client_id=" << config.client_id
           << "&client_secret=" << config.client_secret
           << "&redirect_uri=" << redirect_uri
           << "&grant_type=authorization_code";
  VLOG(1) << req_body.str();
  req.SetBody(req_body.str());

  HttpResponse resp;
  HttpClient::Status status;
  LOG(INFO) << "exchange code to refresh_token";
  client.Do(&req, &resp, &status);
  if (status.err) {
    LOG(WARNING) << "exchange refresh token err=" << status.err
                 << " " << status.err_message;
    return "";
  }
  if (status.http_return_code != 200) {
    LOG(WARNING) << "exchange refresh status=" << status.http_return_code;
    return "";
  }
  string token;
  {
    string err;
    Json::Reader reader;
    Json::Value root;
    if (reader.parse(string(resp.Body()), root, false)) {
      if (!GetNonEmptyStringFromJson(root, "refresh_token", &token, &err)) {
        LOG(WARNING) << "parse exchange result: " << err;
      }
    } else {
      LOG(WARNING) << "invalid json";
    }
  }
  return token;
}

}  // namespace devtools_goma