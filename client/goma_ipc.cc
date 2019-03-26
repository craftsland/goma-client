// Copyright 2010 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "goma_ipc.h"

#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#endif

#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include "absl/strings/str_cat.h"
#include "compiler_proxy_info.h"
#include "compiler_specific.h"
#include "env_flags.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "google/protobuf/message.h"
MSVC_POP_WARNING()
#include "http_util.h"
#include "glog/logging.h"
#include "goma_ipc_peer.h"
#include "scoped_fd.h"
#include "simple_timer.h"
#include "util.h"

namespace devtools_goma {

namespace {

constexpr absl::Duration kReadSelectTimeout = absl::Seconds(20);

void SetError(int err,
              const std::string error_message,
              GomaIPC::Status* status) {
  VLOG(1) << error_message;
  if (status->err == OK)
    status->err = err;
  if (status->error_message.empty())
    status->error_message = error_message;
  else
    status->error_message += "\n" + error_message;
}

}  // namespace

GomaIPC::GomaIPC(std::unique_ptr<ChanFactory> chan_factory)
    : chan_factory_(std::move(chan_factory)) {
}

GomaIPC::~GomaIPC() {
}

int GomaIPC::Call(const std::string& path,
                  const google::protobuf::Message* req,
                  google::protobuf::Message* resp,
                  Status* status) {
  DCHECK(status);

  std::unique_ptr<IOChannel> chan(CallAsync(path, req, status));
  if (chan == nullptr) {
    LOG(ERROR) << "call failed: " << status->error_message;
    return status->err;
  }
  return Wait(std::move(chan), resp, status);
}

std::unique_ptr<IOChannel> GomaIPC::CallAsync(
    const std::string& path,
    const google::protobuf::Message* req,
    Status* status) {
  DCHECK(status);
  status->connect_success = false;
  std::unique_ptr<IOChannel> chan(chan_factory_->New());
  if (chan == nullptr) {
    std::ostringstream ss;
    ss << "Failed to connect to " << chan_factory_->DestName();
    SetError(FAIL, ss.str(), status);
    return nullptr;
  }
  if (!CheckGomaIPCPeer(chan.get(), nullptr)) {
    std::ostringstream ss;
    ss << "Peer is serving by other user?";
    SetError(FAIL, ss.str(), status);
    return nullptr;
  }
  status->connect_success = true;

  std::string send_string;
  SimpleTimer req_send_timer;
  req->SerializeToString(&send_string);
  status->req_size = send_string.size();
  VLOG(1) << "sending " << send_string.size() << " bytes to server.";
  int err = SendRequest(chan.get(), path, send_string, status);
  if (err < 0) {
    std::ostringstream ss;
    ss << "Failed to send err=" << err
       << " duration=" << req_send_timer.GetDuration();
    SetError(err, ss.str(), status);
    return nullptr;
  }
  status->req_send_time = req_send_timer.GetDuration();
  return chan;
}

int GomaIPC::Wait(std::unique_ptr<IOChannel> chan,
                  google::protobuf::Message* resp,
                  Status* status) {
  DCHECK(status);
  if (chan == nullptr) {
    if (status->err != OK) {
      return status->err;
    }
    return FAIL;
  }

  std::string header;
  std::string body;
  status->http_return_code = 0;
  SimpleTimer resp_recv_timer;

  int err = ReadResponse(chan.get(), &header, &body, &status->http_return_code,
                         status);
  if (err < 0) {
    std::ostringstream ss;
    ss << "Failed to read response err=" << err
       << " duration=" << resp_recv_timer.GetDuration();
    SetError(err, ss.str(), status);
    return err;
  }

  if (status->http_return_code != 200) {
    std::ostringstream ss;
    ss << "Invalid HTTP response code: " << status->http_return_code;
    SetError(FAIL, ss.str(), status);
    VLOG(2) << header;
    VLOG(2) << body;
    return FAIL;
  }
  if (body.size() == 0) {
    SetError(FAIL, "Empty message", status);
    return FAIL;
  }

  status->resp_recv_time = resp_recv_timer.GetDuration();
  status->resp_size = body.size();

  if (!resp->ParseFromString(body)) {
    SetError(FAIL, "Failed to parse response body", status);
    return FAIL;
  }
  return OK;
}

int GomaIPC::SendRequest(const IOChannel* chan,
                         const std::string& path,
                         const std::string& s,
                         Status* status) {
  std::ostringstream http_send_message;
  // Using "Host: 0.0.0.0" is hack not to create goma ipc request
  // on browser.  Host field could not be modified on Browser.
  // Note: browser will have "Host: localhost:18088" or so on windows.
  // Also note that it doens't need to have Origin header, although
  // XMLHttpRequest will add this one automatically, and couldn't be
  // modified.
  // e.g. request generated by sample code in b/33103449
  // POST /e HTTP/1.1
  // Host: localhost:18088
  // User-Agent: ....
  // Content-Length: 381
  // Accept: */*
  // Accept-Encoding: gzip, deflate, br
  // Accept-Language: en-US,en;q=0.8,ja;q=0.6
  // Cache-Control: no-cache
  // Connection: keep-alive
  // Origin: null
  // Pragma: no-cache
  //
  // see also "forbidden header name" in
  // https://fetch.spec.whatwg.org/#terminology-headers
  //
  // This hack is not enough to protect from attack using Network Communication
  // API in chrome app.
  // https://developer.chrome.com/apps/app_network
  http_send_message
      << "POST " << path << " HTTP/1.1\r\n"
      << "Host: 0.0.0.0\r\n"
      << "User-Agent: " << kUserAgentString << "\r\n"
      << "Content-Type: binary/x-protocol-buffer\r\n"
      << "Content-Length: " << s.size() << "\r\n";
  http_send_message << "\r\n" << s;
  int err = chan->WriteString(http_send_message.str(), status->initial_timeout);
  if (err < 0) {
    LOG(ERROR) << "GOMA: sending request failed: "
               << chan->GetLastErrorMessage();
    SetError(err, "Failed to send request", status);
    return err;
  }
  return 0;
}

int GomaIPC::ReadResponse(const IOChannel* chan,
                          std::string* header,
                          std::string* body,
                          int* http_return_code,
                          Status* status) {
  absl::Duration timeout = status->initial_timeout;
  std::string response;
  size_t response_len = 0;
  size_t offset = 0;
  size_t content_length = 0;
  SimpleTimer timer;

  for (;;) {
    bool found_header = offset > 0 && content_length > 0;
    if (found_header) {
      if (response.size() < offset + content_length) {
        response.resize(offset + content_length);
      }
    } else {
      response.resize(response.size() + kNetworkBufSize);
    }
    char* buf = const_cast<char*>(response.data()) + response_len;
    int buf_size = response.size() - response_len;
    DCHECK_GT(buf_size, 0);
    int len = chan->ReadWithTimeout(buf, buf_size, timeout);
    if (len == 0) {
      LOG(ERROR) << "GOMA: Unexpected end-of-file at " << response_len
                 << "+" << buf_size
                 << ": " << chan->GetLastErrorMessage();
      SetError(FAIL, "Unexpected end-of-file", status);
      break;
    }
    if (len > 0) {
      response_len += len;
      // Now we've got the first response. The next response
      // should come soon. Let's make the timeout shorter.
      timeout = status->read_timeout;
      absl::string_view resp(response.data(), response_len);
      if ((found_header || ParseHttpResponse(resp, http_return_code,
                                             &offset, &content_length,
                                             nullptr)) &&
          response_len >= offset + content_length) {
        break;
      }
      continue;
    }
    LOG(WARNING)
        << "GOMA: http response read error:" << len
        << " after " << response_len << " bytes."
        << " http=" << *http_return_code
        << " offset=" << offset
        << " content_length=" << content_length;
    if (len == ERR_TIMEOUT && response_len == 0 &&
        status->health_check_on_timeout) {
      // long compile/link task and still running?
      len = CheckHealthz(status);
      if (len == OK) {
        LOG(INFO) << "healthy. wait more in pid:" << Getpid();
      }
      timeout = status->check_timeout;
      continue;
    }
    return len;
  }

  // sanity checking the data
  if (response_len < offset + content_length) {
    // if response size is too small, there was some network error.
    std::ostringstream ss;
    ss << "broken response string from server, it was cut short."
       << " response_len=" << response_len
       << " offset=" << offset
       << " content_length=" << content_length;
    SetError(FAIL, ss.str(), status);
    LOG(ERROR) << "GOMA: " << ss.str();
    return FAIL;
  }

  if (offset == 0) {
    *header = response;
    *body = "";
  } else {
    *header = std::string(response, offset);
    *body = std::string(response.c_str() + offset, content_length);
  }

  return OK;
}

int GomaIPC::CheckHealthz(Status* status) {
  // Check /healthz.
  pid_t pid = Getpid();
  std::unique_ptr<IOChannel> healthz_chan(chan_factory_->New());
  if (healthz_chan == nullptr) {
    std::ostringstream ss;
    ss << "Failed to connect to " << chan_factory_->DestName()
       << " from pid:" << pid;
    LOG(ERROR) << "GOMA: " << ss.str();
    SetError(FAIL, ss.str(), status);
    return FAIL;
  }
  {
    std::ostringstream ss;
    ss << "/healthz?pid=" << pid;
    int err = SendRequest(healthz_chan.get(), ss.str(), "", status);
    if (err < 0) {
      LOG(ERROR) << "GOMA: Failed to send to /healthz err=" << err
                 << " " << status->error_message
                 << " from pid:" << pid;
      return err;
    }
  }
  std::string healthz_response;
  healthz_response.resize(kNetworkBufSize);
  char* buf = const_cast<char*>(healthz_response.data());
  SimpleTimer timer;
  int len =
      healthz_chan->ReadWithTimeout(buf, kNetworkBufSize, kReadSelectTimeout);
  if (len <= 0) {
    std::ostringstream ss;
    ss << "Error /healthz err=" << len
       << " duration=" << timer.GetDuration()
       << " in pid:" << pid
       << " error=" << healthz_chan->GetLastErrorMessage();
    LOG(ERROR) << "GOMA: " << ss.str();
    SetError(FAIL, ss.str(), status);
    return FAIL;
  }
  int healthz_status = 0;
  size_t healthz_offset = 0;
  size_t healthz_content_length = 0;
  bool is_chunked = false;
  if (!ParseHttpResponse(healthz_response, &healthz_status,
                         &healthz_offset, &healthz_content_length,
                         &is_chunked)) {
    LOG(ERROR) << "GOMA: Bad response /healthz in pid:" << pid;
    SetError(FAIL, "Bad response /healthz", status);
    return FAIL;
  }
  if (healthz_status != 200) {
    std::ostringstream ss;
    ss << "not healthy? " << healthz_status << " in pid:" << pid;
    LOG(ERROR) << "GOMA: " << ss.str();
    SetError(FAIL, ss.str(), status);
    return FAIL;
  }
  return OK;
}

std::string GomaIPC::DebugString() const {
  std::ostringstream ss;
  ss << "Socket path: " << chan_factory_->DestName() << std::endl;
  return ss.str();
}

std::string GomaIPC::Status::DebugString() const {
  return absl::StrCat(
      "GomaIPC::Status",
      " connect_success=", connect_success,
      " err=", err,
      " error_message=", error_message,
      " http_return_code=", http_return_code,
      " req_size=", req_size,
      " resp_size=", resp_size,
      " req_send_time=", absl::FormatDuration(req_send_time),
      " resp_recv_time=", absl::FormatDuration(resp_recv_time));
}

}  // namespace devtools_goma
