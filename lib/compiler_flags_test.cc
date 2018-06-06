// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.



#include "compiler_flags.h"

#include <limits.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>


#include "absl/strings/str_cat.h"
#include "clang_tidy_flags.h"
#include "file.h"
#include "file_dir.h"
#include "file_helper.h"
#include "filesystem.h"
#include "gcc_flags.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "gtest/gtest.h"
#include "java_flags.h"
#include "path.h"
#include "path_resolver.h"
#include "vc_flags.h"
#ifdef _WIN32
# include "config_win.h"
// we'll ignore the warnings:
// warning C4996: 'strdup': The POSIX name for this item is deprecated.
# pragma warning(disable:4996)
#endif  // _WIN32
using google::GetExistingTempDirectories;
using std::string;
using absl::StrCat;

namespace devtools_goma {

class CompilerFlagsTest : public ::testing::Test {
};

TEST_F(CompilerFlagsTest, CommandClassification) {
  static const int kGCC = 1 << 0;
  static const int kClang = 1 << 1;
  static const int kVC = 1 << 2;
  static const int kClangCl = 1 << 3;
  static const int kJavac = 1 << 4;
  static const int kClangTidy = 1 << 5;

  struct TestCast {
    const char* command;
    int expected;
  } testcases[] = {
    // gcc
    { "gcc", kGCC },
    { "/usr/bin/gcc", kGCC },
    { "x86_64-linux-gnu-gcc", kGCC },
    { "g++", kGCC },
    { "/usr/bin/g++", kGCC },
    { "x86_64-linux-gnu-g++", kGCC },
    { "c++", kGCC },
    { "/usr/bin/c++", kGCC },
    { "cc", kGCC },
    { "/usr/bin/cc", kGCC },
    { "i586-mingw32msvc-cc", kGCC },
    { "g++-4.8", kGCC },
    { "arm-gnueabihf-gcc-4.9", kGCC },
    { "nacl-gcc", kGCC },
    { "i686-nacl-gcc", kGCC },
    { "nacl-gcc.exe", kGCC },
    // clang
    { "clang", kGCC | kClang },
    { "clang.exe", kGCC | kClang },
    { "/usr/local/bin/clang", kGCC | kClang },
    { "clang++", kGCC | kClang },
    { "/usr/local/bin/clang++", kGCC | kClang },
    { "pnacl-clang", kGCC | kClang },
    { "pnacl-clang++", kGCC | kClang },
    { "clang++-3.7", kGCC | kClang },
    { "/usr/local/google/home/jlebar/bin/clang++-3.7", kGCC | kClang },
    { "armv7a-cros-linux-gnueabi-clang++", kGCC | kClang },  // ChromeOS clang
    { "/usr/bin/local/clang-tidy/clang", kGCC | kClang },  // not clang-tidy.
    // clang (negative)
    { "clang-check", 0 },
    { "clang-tblgen", 0 },
    { "clang-format", 0 },
    { "clang-tidy-diff", 0 },  // not clang-tidy, too.
    // cl
    { "cl", kVC },
    { "CL", kVC },
    { "cl.exe", kVC },
    { "CL.EXE", kVC },
    { "cL.eXe", kVC },
    { "Cl.Exe", kVC },
    { "C:\\VS10\\VC\\bin\\cl.exe", kVC },
    { "D:\\Program Files\\Microsoft Visual Studio 10\\VC\\bin\\Cl.Exe", kVC },
    { "D:\\VS9\\cl.exe\\cl.exe", kVC },
    // cl (negative)
    { "D:\\VS9\\cl.exe\\cl.exe.manifest", 0 },
    { "D:\\VS9\\cl.exe\\", 0 },
    { "cl.exe.manifest", 0 },
    // clang-cl
    { "clang-cl", kClangCl },
    { "clang-cl.exe", kClangCl },
    { "CLANG-CL.EXE", kClangCl },
    { "/usr/local/bin/clang-cl", kClangCl },
    { "/usr/local/bin/clang-cl.exe", kClangCl },
    { "C:\\clang-cl", kClangCl },
    { "C:\\clang-cl.exe", kClangCl },
    { "D:\\example\\clang-cl.exe", kClangCl },
    { "D:\\EXAMPLE\\CLANG-CL.EXE", kClangCl },
    // javac
    { "javac", kJavac },
    { "/usr/bin/javac", kJavac },
    // javac (negative)
    { "/usr/bin/javaco/yes", 0 },
    // clang-tidy
    { "clang-tidy", kClangTidy },
    { "/usr/bin/local/clang-tidy", kClangTidy },
    // others
    { "nacl.exe", 0 },
    { "D:\\nacl_sdk\\pepper_18\\toolchain\\win_x86_newlib\\bin\\nacl.exe", 0 },
    { "/usr/lib/gcc/bin/ar", 0 },
  };

  for (const auto& tc : testcases) {
    EXPECT_EQ(GCCFlags::IsGCCCommand(tc.command),
              (tc.expected & kGCC) ? true : false)
        << "command = " << tc.command;
    EXPECT_EQ(GCCFlags::IsClangCommand(tc.command),
              (tc.expected & kClang) ? true : false)
        << "command = " << tc.command;
    EXPECT_EQ(VCFlags::IsVCCommand(tc.command),
              (tc.expected & kVC) ? true : false)
        << "command = " << tc.command;
    EXPECT_EQ(VCFlags::IsClangClCommand(tc.command),
              (tc.expected & kClangCl) ? true : false)
        << "command = " << tc.command;
    EXPECT_EQ(JavacFlags::IsJavacCommand(tc.command),
              (tc.expected & kJavac) ? true : false)
        << "command = " << tc.command;
    EXPECT_EQ(ClangTidyFlags::IsClangTidyCommand(tc.command),
              (tc.expected & kClangTidy) ? true : false)
        << "command = " << tc.command;
  }
}

TEST_F(CompilerFlagsTest, GetCompilerName) {
  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("gcc"));
  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("gcc.exe"));
  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("/usr/bin/gcc"));
  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("x86_64-linux-gnu-gcc"));

  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("g++"));
  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("g++.exe"));
  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("/usr/bin/g++"));
  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("x86_64-linux-gnu-g++"));

  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("nacl-gcc"));
  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("nacl-gcc.exe"));
  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("i686-nacl-gcc"));
  EXPECT_EQ("gcc", CompilerFlags::GetCompilerName("i686-nacl-gcc.exe"));
  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("nacl-g++"));
  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("nacl-g++.exe"));
  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("i686-nacl-g++"));
  EXPECT_EQ("g++", CompilerFlags::GetCompilerName("i686-nacl-g++.exe"));
  EXPECT_EQ("", CompilerFlags::GetCompilerName("nacl.exe"));
  EXPECT_EQ("", CompilerFlags::GetCompilerName(
      "D:\\nacl_sdk\\pepper_18\\toolchain\\win_x86_newlib\\bin\\nacl.exe"));

  EXPECT_EQ("clang", CompilerFlags::GetCompilerName("clang"));
  EXPECT_EQ("clang", CompilerFlags::GetCompilerName("clang.exe"));
  EXPECT_EQ("clang", CompilerFlags::GetCompilerName("/usr/local/bin/clang"));
  EXPECT_EQ("clang", CompilerFlags::GetCompilerName("pnacl-clang"));
  EXPECT_EQ("clang", CompilerFlags::GetCompilerName("pnacl-clang.exe"));
  EXPECT_EQ("clang++", CompilerFlags::GetCompilerName("clang++"));
  EXPECT_EQ("clang++", CompilerFlags::GetCompilerName("clang++.exe"));
  EXPECT_EQ("clang++", CompilerFlags::GetCompilerName(
      "/usr/local/bin/clang++"));
  EXPECT_EQ("clang++", CompilerFlags::GetCompilerName("pnacl-clang++"));
  EXPECT_EQ("clang++", CompilerFlags::GetCompilerName("pnacl-clang++.exe"));
  EXPECT_EQ("", CompilerFlags::GetCompilerName("clang-tblgen"));

  EXPECT_EQ("cl.exe", CompilerFlags::GetCompilerName("cl"));
  EXPECT_EQ("cl.exe", CompilerFlags::GetCompilerName("CL"));
  EXPECT_EQ("cl.exe", CompilerFlags::GetCompilerName("cl.exe"));
  EXPECT_EQ("cl.exe", CompilerFlags::GetCompilerName("CL.EXE"));
  EXPECT_EQ("cl.exe", CompilerFlags::GetCompilerName(
      "C:\\VS10\\VC\\bin\\cl.exe"));
  EXPECT_EQ("cl.exe", CompilerFlags::GetCompilerName(
      "D:\\Program Files\\Microsoft Visual Studio 10\\VC\\bin\\Cl.Exe"));
  EXPECT_EQ("cl.exe", CompilerFlags::GetCompilerName(
      "D:\\VS9\\cl.exe\\cl.exe"));
  EXPECT_EQ("", CompilerFlags::GetCompilerName("cl.exe.manifest"));
  EXPECT_EQ("", CompilerFlags::GetCompilerName(
      "D:\\VS9\\cl.exe\\cl.exe.manifest"));
  EXPECT_EQ("", CompilerFlags::GetCompilerName(
      "D:\\VS9\\cl.exe\\"));

  EXPECT_EQ("javac", CompilerFlags::GetCompilerName("javac"));
  EXPECT_EQ("javac", CompilerFlags::GetCompilerName("/usr/bin/javac"));
}

}  // namespace devtools_goma
