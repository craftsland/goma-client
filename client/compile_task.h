// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_COMPILE_TASK_H_
#define DEVTOOLS_GOMA_CLIENT_COMPILE_TASK_H_

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <json/json.h>

#include "absl/base/call_once.h"
#include "basictypes.h"
#include "compile_service.h"
#include "compiler_info.h"
#include "compiler_specific.h"
#include "deps_cache.h"
#include "file_stat.h"
#include "file_stat_cache.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "google/protobuf/repeated_field.h"
MSVC_POP_WARNING()
#include "http_rpc.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "prototmp/goma_data.pb.h"
#include "prototmp/subprocess.pb.h"
MSVC_POP_WARNING()
#include "simple_timer.h"
#include "subprocess_task.h"
#include "threadpool_http_server.h"
#include "timestamp.h"

namespace devtools_goma {

class Closure;
class CompileStats;
class CompilerFlags;
class CompilerProxyHistogram;

// CompileTask handles single compile request from gomacc.
// It basically runs on the same thread it is created, but InputFileTask and
// OutputFileTask would run on other threads.
// Note that DumpToString() may be called on other threads.
class CompileTask {
 public:
  enum State {
    // running state, or state at response sent if abort_ is true.
    INIT,  // Initialize the task.  Run subprocess for fast fallback/verify.
    SETUP,  // Setup the request. Run include processor.
    FILE_REQ,  // Upload input files.
    CALL_EXEC,  // Call Exec request.
    LOCAL_OUTPUT,  // LocalOutputCache lookup succeeded.
    FILE_RESP,  // Download output files.
    // finished state:  response sent.
    FINISHED,  // Finished.
    LOCAL_RUN,  // Local run started and didn't issue goma call.
    LOCAL_FINISHED,  // Local run finished for fast fallback.
    NUM_STATE,
  };
  CompileTask(CompileService* service, int id);

  void Ref();
  void Deref();

  // Task ID, a serial number.
  int id() const { return id_; }
  const string& trace_id() const { return trace_id_; }

  // Inits CompileTask.
  // It takes ownership of rpc, req, resp and done.
  void Init(CompileService::RpcController* rpc,
            const ExecReq* req,
            ExecResp* resp,
            OneshotClosure* done);

  // It will run on other thread than current thread, but done closure will
  // be called on the same thread where this method was called.
  void Start();

  bool failed() const;
  bool canceled() const;
  bool abort() const { return abort_; }
  bool local_run() const { return local_run_; }
  bool local_killed() const { return local_killed_; }
  bool fail_fallback() const { return fail_fallback_; }
  bool cache_hit() const;
  bool local_cache_hit() const;

  State state() const { return state_; }

  const CompileStats& stats() const { return *stats_; }
  CompileStats* mutable_stats() { return stats_.get(); }

  void DumpToJson(bool need_detail, Json::Value* root) const;

  // DumpRequest is called on finished task.
  void DumpRequest() const;

  void SetFrozenTimestampMs(millitime_t frozen_timestamp_ms) {
    frozen_timestamp_ms_ = frozen_timestamp_ms;
  }
  millitime_t GetFrozenTimestampMs() const { return frozen_timestamp_ms_; }
  millitime_t GetLastReqTimestampMs() const { return last_req_timestamp_ms_; }

 private:
  friend class CompileTaskTest;
  enum ErrDest {
    // To log: write in log file, and show on status page.
    TO_LOG,
    // To user: may send back to gomacc, so user will see the message.
    // including TO_LOG.
    TO_USER,
  };
  class InputFileTask;
  class OutputFileTask;
  struct OutputFileInfo;
  class LocalOutputFileTask;
  friend class InputFileTask;
  friend class OutputFileTask;
  friend class LocalOutputFileTask;
  friend class CompilerProxyHistogram;
  struct RenameParam;
  struct ContentOutputParam;
  struct RunCppIncludeProcessorParam;
  struct RunLinkerInputProcessorParam;
  struct RunJarParserParam;
  struct ReadThinLTOImportsParam;

  ~CompileTask();

  bool BelongsToCurrentThread() const;

  bool success() const { return resp_->result().exit_status() == 0; }
  bool IsGomaccRunning();
  // Notified from http server request of gomacc when the goma ipc is closed.
  void GomaccClosed();

  bool IsSubprocRunning() const;

  // Copies env and requester env from req_, and clear requester env
  // from req_.
  void CopyEnvFromRequest();
  string GenerateCompilerProxyId() const;

  // validate local compiler path.
  static bool IsLocalCompilerPathValid(
      const string& trace_id,
      const ExecReq& req, const CompilerFlags* flags);

  // Remove duplicate filepath from |filenames|
  // for files normalized by JoinPathRepectAbsolute with |cwd|.
  // Relative path is taken in high priority.
  static void RemoveDuplicateFiles(const std::string& cwd,
                                   std::set<std::string>* filenames);

  // Initializes compiler flags from the request.
  void InitCompilerFlags();

  // Finds local compiler path from the request.
  // Updates req_->command_spec().local_compiler_path() and local_path_.
  bool FindLocalCompilerPath();

  // Checks if we should fallback the request.
  bool ShouldFallback() const;

  // Checks if we should verify output.
  bool ShouldVerifyOutput() const;

  // Gets task weight.
  SubProcessReq::Weight GetTaskWeight() const;

  // Checks if we should stop goma and use local run only.
  bool ShouldStopGoma() const;

  // Sets up goma request. (e.g include processor).
  // state_: INIT -> SETUP
  void ProcessSetup();

  // Processes file request. (runs InputFileTasks).
  // state_: SETUP -> FILE_REQ
  void TryProcessFileRequest();
  void ProcessFileRequest();
  void ProcessFileRequestDone();
  void ProcessPendingFileRequest();

  // state_: FILE_REQ -> CALL_EXEC (call Exec service).
  void ProcessCallExec();
  void ProcessCallExecDone();

  // state_: CALL_EXEC -> FILE_RESP (runs OutputFileTasks).
  void ProcessFileResponse();
  void ProcessFileResponseDone();

  // state_: -> FINISHED or abort_. (ready to send response)
  // finished_ becomes true.
  // joins in FinishSubProcess if subproc_ is active.
  void ProcessFinished(const string& msg);

  // Replies with goma result.
  // state_: FINISHED && !abort_ && subprocess has been finished.
  void ProcessReply();

  // state_: !abort_, FINISHED.
  // If use_remote is true, it renames remote outputs to real outputs.
  // If use_remote is false, it just remove remote outputs.
  void CommitOutput(bool use_remote);

  // DoOutput will run closure to output in filename.
  // It doesn't take ownership of closure and err.
  // It is expected that closure will set error in *err, and
  // after calling closure, err->empty() means success, otherwise failure.
  // On Windows, it will retry several times, so closure must be permanent
  // callback.
  void DoOutput(const string& opname, const string& filename,
                PermanentClosure* closure, string* err);
  void RenameCallback(RenameParam* param, string* err);
  void ContentOutputCallback(ContentOutputParam* param, string* err);

  // If file is coff file, rewrite timestamp to the current time.
  void RewriteCoffTimestamp(const string& filename);

  // state_: FINISHED/LOCAL_FINISHED or abort_.
  void ReplyResponse(const string& msg);

  void ProcessLocalFileOutput();
  void ProcessLocalFileOutputDone();

  // Saves stats, clears proto messages and calls CompileTaskDone to make
  // this CompileTask expired.
  void Done();

  // Methods used in state_: SETUP
  void FillCompilerInfo();
  void FillCompilerInfoDone(
      std::unique_ptr<CompileService::GetCompilerInfoParam> param);
#ifndef _WIN32
  bool MakeWeakRelativeInArgv();
#endif
  void UpdateExpandedArgs();
  void ModifyRequestArgs();
  void ModifyRequestEnvs();
  void UpdateCommandSpec();
  // Updates SubprogramSpec if send_subprogram_spec is enabled.
  void MayUpdateSubprogramSpec();
  // Fix SubprogramSpec if send_subprogram_spec is enabled.
  void MayFixSubprogramSpec(
      google::protobuf::RepeatedPtrField<SubprogramSpec>* subprogram_specs)
          const;
  void UpdateRequiredFiles();
  void GetIncludeFiles();
  void RunCppIncludeProcessor(
      std::unique_ptr<RunCppIncludeProcessorParam> param);
  void RunCppIncludeProcessorDone(
      std::unique_ptr<RunCppIncludeProcessorParam> param);
  void GetLinkRequiredFiles();
  void RunLinkerInputProcessor(
      std::unique_ptr<RunLinkerInputProcessorParam> param);
  void RunLinkerInputProcessorDone(
      std::unique_ptr<RunLinkerInputProcessorParam> param);
  void GetJavaRequiredFiles();
  void RunJarParser(std::unique_ptr<RunJarParserParam> param);
  void RunJarParserDone(std::unique_ptr<RunJarParserParam> param);
  void GetThinLTOImports();
  void ReadThinLTOImports(std::unique_ptr<ReadThinLTOImportsParam> param);
  void ReadThinLTOImportsDone(std::unique_ptr<ReadThinLTOImportsParam> param);
  void UpdateRequiredFilesDone(bool ok);
  void SetupRequestDone(bool ok);

  // Methods used state_: FILE_REQ
  void SetInputFileCallback();
  void StartInputFileTask();
  void InputFileTaskFinished(InputFileTask* input_file_task);
  void MaybeRunInputFileCallback(bool task_finished);

  // Methods used in state_: CALL_EXEC
  void CheckCommandSpec();
  void CheckNoMatchingCommandSpec(const string& retry_reason);
  void StoreEmbeddedUploadInformationIfNeeded();

  // Methods used in state_: FILE_RESP
  void SetOutputFileCallback();
  void CheckOutputFilename(const string& filename);
  void StartOutputFileTask();
  void OutputFileTaskFinished(std::unique_ptr<OutputFileTask> output_file_task);
  void MaybeRunOutputFileCallback(int index, bool task_finished);
  bool VerifyOutput(const string& local_output_path,
                    const string& goma_output_path);
  void ClearOutputFile();

  // Methods used in state_: fail_fallback_, LOCAL_FINISHED or abort_
  // (after local run finished)
  void SetLocalOutputFileCallback();
  void StartLocalOutputFileTask();
  void LocalOutputFileTaskFinished(
      std::unique_ptr<LocalOutputFileTask> local_output_file_task);
  void MaybeRunLocalOutputFileCallback(bool task_finished);

  // Methods used in state_: FINISHED/LOCAL_FINISHED or abort_.
  void UpdateStats();
  void SaveInfoFromInputOutput();

  // ----------------------------------------------------------------
  // Sets subprocess for local run.  The subprocess becomes ready to run.
  void SetupSubProcess();

  // Runs subprocess in high priority with reason.
  void RunSubProcess(const string& reason);

  // Kills subprocess.  FinishSubProcess will be called later.
  void KillSubProcess();

  // Finished subprocess.
  void FinishSubProcess();

  // ----------------------------------------------------------------
  // Add error message to response and sets error exit status.
  void AddErrorToResponse(
      ErrDest dest, const string& error_message, bool set_error);

  static void InitializeStaticOnce();

  CompileService* service_;
  const int id_;  // A serial number.
  string trace_id_;

  // RPC between gomacc and compiler proxy.
  // These are vaild until ReplyResponse().
  CompileService::RpcController* rpc_;
  ExecResp* rpc_resp_;
  WorkerThreadManager::ThreadId caller_thread_id_;
  OneshotClosure* done_;

  std::unique_ptr<CompileStats> stats_;

  int responsecode_;

  State state_;
  bool abort_;  // local proc finished first.
  bool finished_;  // remote call finished (no active remote calls).

  std::unique_ptr<ExecReq> req_;
  CommandSpec command_spec_;
  ScopedCompilerInfoState compiler_info_state_;
  string local_compiler_path_;
  RequesterInfo requester_info_;
  RequesterEnv requester_env_;

  std::unique_ptr<CompilerFlags> flags_;
  bool linking_;
  bool precompiling_;

  // gomacc_pid_:
  //   gomacc_pid_ == SubprocessState::kInvalidPid if gomacc not running.
  int gomacc_pid_;
  // true if a connection to gomacc is lost, and the task is canceled.
  bool canceled_;

  string orig_flag_dump_;
  string flag_dump_;
  std::set<string> required_files_;

  // Caches all FileStat in this compilation unit, since creating FileStat is
  // slow especially on Windows. So that FileStatCache doesn't need to have
  // lock, 2 FileStatCache instances are used for input/output in CompileTask.
  // TODO: Maybe we can merge this with |required_files_|.
  std::unique_ptr<FileStatCache> input_file_stat_cache_;
  std::unique_ptr<FileStatCache> output_file_stat_cache_;

  // |system_library_paths_| is used only when linking_ == true.
  std::vector<string> system_library_paths_;
  // list of interleave uploaded files_to confirm the mechanism works fine.
  std::unordered_set<string> interleave_uploaded_files_;

  std::unique_ptr<ExecResp> resp_;
  std::unique_ptr<ExecResp> exec_resp_;

  std::vector<string> exec_output_file_;
  std::vector<string> exec_error_message_;
  // exit_status_ is an exit status of remote goma compilation.
  // if this is 0, remote goma compilation might have finished successfully,
  // or might not be executed.
  // in other words, if this is not 0, remote goma compilation failed.
  int exit_status_;
  string stdout_;
  string stderr_;

  // HttpRPC stt for ExecRequest.
  std::unique_ptr<HttpRPC::Status> http_rpc_status_;

  WorkerThreadManager::CancelableClosure* delayed_setup_subproc_;
  // local subprocess
  string local_path_;
  // PATHEXT environment variable in ExecReq for Windows.
  string pathext_;
  // subproc_ == NULL; subprocess is not ready to run or already finished.
  // subproc_ != NULL; subprocess is ready to run or running.
  SubProcessTask* subproc_;
  SubProcessReq::Weight subproc_weight_;
  // subproc_exit_status_ is an exit status of local compilation.
  // if this is 0, local compilation might have finished successfully,
  // might not be executed, or might have been killed because of fast goma.
  // in other words, if this is not 0, local compilation failed.
  // Note that local compilation might have been failed because of goma
  // if goma failed to setup env, cwd to run local compiler.
  // TODO: can we detect this kind of error?
  int subproc_exit_status_;
  string subproc_stdout_;
  string subproc_stderr_;
  // request fallback when exec call failed. initialized with
  // requester_env_.fallback(), but might be changed for hermetic fallback.
  bool want_fallback_;
  bool should_fallback_;  // do fallback because of setup failures etc.
  bool verify_output_;
  bool fail_fallback_;
  bool local_run_;
  bool local_killed_;
  bool depscache_used_;
  bool gomacc_revision_mismatched_;

  // Timers
  SimpleTimer handler_timer_;
  SimpleTimer compiler_info_timer_;
  SimpleTimer include_timer_;
  SimpleTimer include_wait_timer_;
  SimpleTimer rpc_call_timer_;
  SimpleTimer file_response_timer_;
  SimpleTimer file_request_timer_;

  // trace info.
  string resp_cache_key_;

  // Input file process.
  OneshotClosure* input_file_callback_;
  int num_input_file_task_;
  bool input_file_success_;

  // Output file process.
  OneshotClosure* output_file_callback_;
  std::vector<OutputFileInfo> output_file_;
  int num_output_file_task_;
  bool output_file_success_;

  // Local output file process.
  OneshotClosure* local_output_file_callback_;
  int num_local_output_file_task_;

  // DepsCache
  DepsCache::Identifier deps_identifier_;

  // Even if lookup failed, we'd like to keep key after calculation so that
  // we can put cache later and at that time we don't need to recalculate
  // the key.
  std::string local_output_cache_key_;

  // Protects ref counts, subproc_ and http_rpc_status_.
  mutable Lock mu_;
  int refcnt_;

  PlatformThreadId thread_id_;

  // Timestamp that this task transited to Finished or Failed.
  millitime_t frozen_timestamp_ms_;

  // Timestamp that this task transmitted the request to Goma.
  millitime_t last_req_timestamp_ms_;

  static absl::once_flag init_once_;

  static Lock global_mu_;
  static std::deque<CompileTask*>* link_file_req_tasks_ GUARDED_BY(global_mu_);

  DISALLOW_COPY_AND_ASSIGN(CompileTask);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_COMPILE_TASK_H_
