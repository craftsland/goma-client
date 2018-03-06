// Copyright 2010 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "util.h"

#include <algorithm>
#include <deque>

#include "compiler_flags.h"
#include "env_flags.h"
#include "file_id.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "path.h"
#include "path_resolver.h"
#include "split.h"
#include "string_piece.h"
#include "string_util.h"

using std::string;

namespace {
// Path separators are platform dependent
#ifndef _WIN32
const char* kPathListSep = ":";
#else
const char* kPathListSep = ";";
#endif

#ifdef _WIN32

std::deque<string> ParsePathExts(const string& pathext_spec) {
  std::vector<string> pathexts;
  if (!pathext_spec.empty()) {
    SplitStringUsing(pathext_spec, kPathListSep, &pathexts);
  } else {
    // If |pathext_spec| is empty, we should use the default PATHEXT.
    // See:
    // http://technet.microsoft.com/en-us/library/cc723564.aspx#XSLTsection127121120120
    static const char* kDefaultPathext = ".COM;.EXE;.BAT;.CMD";
    SplitStringUsing(kDefaultPathext, kPathListSep, &pathexts);
  }

  for (auto& pathext : pathexts) {
    std::transform(pathext.begin(), pathext.end(), pathext.begin(),
                   ::tolower);
  }
  return std::deque<string>(pathexts.begin(), pathexts.end());
}

bool HasExecutableExtension(const std::deque<string>& pathexts,
                            const string& filename) {
  const size_t pos = filename.rfind(".");
  if (pos == string::npos)
    return false;

  string ext = filename.substr(pos);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  for (const auto& pathext : pathexts) {
    if (ext == pathext)
      return true;
  }
  return false;
}

string GetExecutableWithExtension(const std::deque<string>& pathexts,
                                  const string& prefix) {
  for (const auto& pathext : pathexts) {
    const string& candidate = prefix + pathext;
    DWORD attr = GetFileAttributesA(candidate.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return candidate;
    }
  }
  return "";
}

#endif

}  // anonymous namespace

namespace devtools_goma {

static ReadCommandOutputFunc gReadCommandOutput = nullptr;

void InstallReadCommandOutputFunc(ReadCommandOutputFunc func) {
  gReadCommandOutput = func;
}

string ReadCommandOutput(
    const string& prog, const std::vector<string>& argv,
    const std::vector<string>& env,
    const string& cwd, CommandOutputOption option, int32_t* status) {
  if (gReadCommandOutput == nullptr) {
    LOG(FATAL) << "gReadCommandOutput should be set before calling."
               << " prog=" << prog
               << " cwd=" << cwd
               << " argv=" << argv
               << " env=" << env;
  }
  return gReadCommandOutput(prog, argv, env, cwd, option, status);
}

#ifdef _WIN32

string ResolveExtension(const string& cmd, const string& pathext_env,
                        const string& cwd) {
  std::deque<string> pathexts = ParsePathExts(pathext_env);
  if (HasExecutableExtension(pathexts, cmd)) {
    pathexts.push_front("");
  }
  const string& path = file::JoinPathRespectAbsolute(cwd, cmd);
  return GetExecutableWithExtension(pathexts, path);
}

#endif

// True if |candidate_path| is gomacc, by running it under an invalid GOMA env
// flag.  It is usually used to confirm |candidate_path| is not gomacc.
// If candadate_path is (a copy of or a symlink to) gomacc, it will die with
// "unknown GOMA_ parameter".
// It assumes real compiler doesn't emit "GOMA" in its output.
// On Windows, path must include a directory where mspdb*.dll,
// otherwise, real cl.exe will pops up a dialog:
//  This application has failed to start because mspdb100.dll was not found.
// Error mode SEM_FAILCRITICALERRORS and SEM_NOGPFAULTERRORBOX
// prevent from popping up message box on error, which we did in
// compiler_proxy.cc:main()
bool IsGomacc(
    const string& candidate_path,
    const string& path,
    const string& pathext,
    const string& cwd) {
  // TODO: fix workaround.
  // Workaround not to pause with dialog when cl.exe is executed.
  if (CompilerFlags::IsVCCommand(candidate_path))
    return false;

  std::vector<string> argv;
  argv.push_back(candidate_path);
  std::vector<string> env;
  env.push_back("GOMA_WILL_FAIL_WITH_UKNOWN_FLAG=true");
  env.push_back("PATH=" + path);
  if (!pathext.empty())
    env.push_back("PATHEXT=" + pathext);
  int32_t status = 0;
  string out = ReadCommandOutput(candidate_path, argv, env, cwd,
                                 MERGE_STDOUT_STDERR, &status);
  return (status == 1) && (out.find("GOMA") != string::npos);
}

bool GetRealExecutablePath(
    const FileId* gomacc_fileid,
    const string& cmd, const string& cwd,
    const string& path_env,
    const string& pathext_env,
    string* local_executable_path,
    string* no_goma_path_env,
    bool* is_in_relative_path) {
  CHECK(local_executable_path);
#ifndef _WIN32
  DCHECK(pathext_env.empty());
#else
  std::deque<string> pathexts = ParsePathExts(pathext_env);
  if (HasExecutableExtension(pathexts, cmd)) {
    pathexts.push_front("");
  }
#endif

  if (no_goma_path_env)
    *no_goma_path_env = path_env;

  // Fast path.
  // If cmd contains '/', it is just cwd/cmd.
  if (cmd.find_first_of(PathResolver::kPathSep) != string::npos) {
    string candidate_path = file::JoinPathRespectAbsolute(cwd, cmd);
#ifndef _WIN32
    if (access(candidate_path.c_str(), X_OK) != 0)
      return false;
#else
    candidate_path = GetExecutableWithExtension(pathexts, candidate_path);
    if (candidate_path.empty())
      return false;
#endif
    const FileId candidate_fileid(candidate_path);
    if (is_in_relative_path)
      *is_in_relative_path = !file::IsAbsolutePath(cmd);

    if (!candidate_fileid.IsValid())
      return false;

    if (gomacc_fileid && candidate_fileid == *gomacc_fileid)
      return false;

    if (gomacc_fileid &&
        IsGomacc(candidate_path, path_env, pathext_env, cwd))
      return false;

    *local_executable_path = candidate_path;
    return true;
  }

  for (size_t pos = 0, next_pos; pos != string::npos; pos = next_pos) {
    next_pos = path_env.find(kPathListSep, pos);
    StringPiece dir;
    if (next_pos == StringPiece::npos) {
      dir.set(path_env.c_str() + pos, path_env.size() - pos);
    } else {
      dir.set(path_env.c_str() + pos, next_pos - pos);
      ++next_pos;
    }

    if (is_in_relative_path)
      *is_in_relative_path = !file::IsAbsolutePath(dir);

    // Empty paths should be treated as the current directory.
    if (dir.empty()) {
      dir = cwd;
    }
    VLOG(2) << "dir:" << dir;

    string candidate_path(PathResolver::ResolvePath(file::JoinPath(
        file::JoinPathRespectAbsolute(cwd, dir),
        cmd)));
    VLOG(2) << "candidate:" << candidate_path;

#ifndef _WIN32
    if (access(candidate_path.c_str(), X_OK) != 0)
      continue;
#else
    candidate_path = GetExecutableWithExtension(pathexts, candidate_path);
    if (candidate_path.empty())
      continue;
#endif

    FileId candidate_fileid(candidate_path);
    if (candidate_fileid.IsValid()) {
      if (gomacc_fileid && candidate_fileid == *gomacc_fileid &&
          next_pos != string::npos) {
        // file is the same as gomacc.
        // Update local path.
        // TODO: drop a path of gomacc only. preserve other paths
        // For example,
        // PATH=c:\P\MVS10.0\Common7\Tools;c:\goma;c:\P\MVS10.0\VC\bin
        // we should not drop c:\P\MVS10.0\Common7\Tools.
        if (no_goma_path_env)
          *no_goma_path_env = path_env.substr(next_pos);
      } else {
        // file is executable, and from file id, it is different
        // from gomacc.
        if (gomacc_fileid &&
            IsGomacc(candidate_path, path_env.substr(pos), pathext_env, cwd)) {
          LOG(ERROR) << "You have 2 goma directories in your path? "
                     << candidate_path << " seems gomacc";
          if (next_pos != string::npos && no_goma_path_env)
            *no_goma_path_env = path_env.substr(next_pos);
          continue;
        }
        *local_executable_path = candidate_path;
        return true;
      }
    }
  }
  return false;
}

// Platform independent getenv.
string GetEnv(const string& name) {
#ifndef _WIN32
  char* ret = getenv(name.c_str());
  if (ret == nullptr)
    return "";
  return ret;
#else
  DWORD size = GetEnvironmentVariableA(name.c_str(), nullptr, 0);
  if (size == 0) {
    CHECK(GetLastError() == ERROR_ENVVAR_NOT_FOUND);
    return "";
  }
  string envvar(size, '\0');
  DWORD ret = GetEnvironmentVariableA(name.c_str(), &envvar[0], size);
  CHECK_EQ(ret, size - 1)
      << "GetEnvironmentVariableA failed but should not:" << name
      << " ret=" << ret << " size=" << size;
  CHECK_EQ(envvar[ret], '\0');
  // cut off the null-terminating character.
  return envvar.substr(0, ret);
#endif
}

void SetEnv(const string& name, const string& value) {
#ifndef _WIN32
  if (setenv(name.c_str(), value.c_str(), 1) != 0) {
    PLOG(ERROR) << "setenv name=" << name << " value=" << value;
  }
#else
  BOOL ret = SetEnvironmentVariableA(name.c_str(), value.c_str());
  if (!ret) {
    LOG_SYSRESULT(GetLastError());
    LOG(ERROR) << "setenv name=" << name << " value=" << value;
  }
#endif
}

pid_t Getpid() {
#ifdef _WIN32
  return static_cast<pid_t>(::GetCurrentProcessId());
#else
  return getpid();
#endif
}

string ToShortNodename(const string& nodename) {
  std::vector<string> entries = strings::Split(nodename, ".");
  return ToLower(entries[0]);
}

}  // namespace devtools_goma