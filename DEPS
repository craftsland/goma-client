# Copyright 2019 Google LLC.

vars = {
     "chromium_git": "https://chromium.googlesource.com",
     "clang_revision": "c4699251c482517d8ef49fdddc87bd72630cde5a",
     "gn_version": "git_revision:dfcbc6fed0a8352696f92d67ccad54048ad182b3",
}

deps = {
     # protobuf > 3.15.6
     # TODO: use released proto including
     # https://github.com/protocolbuffers/protobuf/blob/ee4f2492ea4e7ff120f68a792af870ee30435aa5/src/google/protobuf/io/zero_copy_stream.h#L122
     "client/third_party/protobuf/protobuf":
     "https://github.com/google/protobuf.git@6aa539bf0195f188ff86efe6fb8bfa2b676cdd46",

     # google-glog v0.4.0
     "client/third_party/glog":
     "https://github.com/google/glog.git@96a2f23dca4cc7180821ca5f32e526314395d26a",

     # googletest 1.8.1
     "client/third_party/gtest":
     Var('chromium_git') + '/external/github.com/google/googletest.git' + '@' +
         '2fe3bd994b3189899d93f1d5a881e725e046fdc2',

     # zlib
     "client/third_party/zlib":
     "https://chromium.googlesource.com/chromium/src/third_party/zlib@1337da5314a9716c0653301cceeb835d17fd7ea4",

     # xz v5.2.0
     "client/third_party/xz":
     "https://goma.googlesource.com/xz.git@fbafe6dd0892b04fdef601580f2c5b0e3745655b",

     # jsoncpp
     "client/third_party/jsoncpp/source":
     Var("chromium_git") + '/external/github.com/open-source-parsers/jsoncpp.git@9059f5cad030ba11d37818847443a53918c327b1', # 1.9.4

     # chrome's tools/clang
     "client/tools/clang":
     Var("chromium_git") + "/chromium/src/tools/clang.git@" +
         Var("clang_revision"),

     # chrome's deps/third_party/boringssl
     "client/third_party/boringssl/src":
     "https://boringssl.googlesource.com/boringssl@ae7c17868992ac2c64c46ba418d46cf75374054f",

     # google-breakpad
     "client/third_party/breakpad/breakpad":
     Var("chromium_git") + "/breakpad/breakpad.git@" +
         "a0e078365d0515f4ffdfc3389d92b2c062f62132",

     # lss
     "client/third_party/lss":
     Var("chromium_git") + "/linux-syscall-support.git@" +
         "a89bf7903f3169e6bc7b8efc10a73a7571de21cf",

     # nasm
     "client/third_party/nasm":
     Var("chromium_git") + "/chromium/deps/nasm.git@" +
         "e9be5fd6d723a435ca2da162f9e0ffcb688747c1",

     # chromium's buildtools containing libc++, libc++abi, clang_format and gn.
     "client/buildtools":
     Var("chromium_git") + "/chromium/src/buildtools@" +
         "69cc9b8a3ae010e0721c4bea12de7a352d9a93f9",

     # libFuzzer
     "client/third_party/libFuzzer/src":
     Var("chromium_git") +
         "/chromium/llvm-project/compiler-rt/lib/fuzzer.git@" +
         "debe7d2d1982e540fbd6bd78604bf001753f9e74",

     # libprotobuf-mutator
     "client/third_party/libprotobuf-mutator/src":
     Var("chromium_git") +
         "/external/github.com/google/libprotobuf-mutator.git@" +
         "439e81f8f4847ec6e2bf11b3aa634a5d8485633d",

     # abseil
     "client/third_party/abseil/src":
     "https://github.com/abseil/abseil-cpp.git@a048203a881f11f4b7b8df5fb563aec85522f8db",

     # google benchmark v1.4.1
     "client/third_party/benchmark/src":
     "https://github.com/google/benchmark.git@e776aa0275e293707b6a0901e0e8d8a8a3679508",

     # Jinja2 template engine v2.10
     "client/third_party/jinja2":
     "https://github.com/pallets/jinja.git@78d2f672149e5b9b7d539c575d2c1bfc12db67a9",

     # Markupsafe module v1.0
     "client/third_party/markupsafe":
     "https://github.com/pallets/markupsafe.git@d2a40c41dd1930345628ea9412d97e159f828157",

     # depot_tools
     'client/third_party/depot_tools':
     Var('chromium_git') + '/chromium/tools/depot_tools.git',

     # gflags 2.2.1
     "client/third_party/gflags/src":
     "https://github.com/gflags/gflags.git@46f73f88b18aee341538c0dfc22b1710a6abedef",

     # subprocess32 3.5.3
     "client/third_party/subprocess32":
     "https://github.com/google/python-subprocess32@" +
     "0a814da4a033875880534fd488770e2d97febe2f",

     # libyaml dist-0.2.2
     "client/third_party/libyaml/src":
     "https://github.com/yaml/libyaml.git@d407f6b1cccbf83ee182144f39689babcb220bd6",

     # chromium's build.
     "client/third_party/chromium_build":
     "https://chromium.googlesource.com/chromium/src/build/@909847cd6faf918168fcd73880daf81b5a15ec0a",

     'client/tools/clang/dsymutil': {
       'packages': [
         {
           'package': 'chromium/llvm-build-tools/dsymutil',
           'version': 'M56jPzDv1620Rnm__jTMYS62Zi8rxHVq7yw0qeBFEgkC',
         }
       ],
       'condition': 'checkout_mac or checkout_ios',
       'dep_type': 'cipd',
     },

     # Go toolchain.
     'client/third_party/go': {
         'packages': [
             {
                 'package': 'infra/3pp/tools/go/${{platform}}',
                 'version': 'version:2@1.16.5',
             },
         ],
         'dep_type': 'cipd',
     },

     # libc++
     'client/buildtools/third_party/libc++/trunk':
     Var('chromium_git') + '/external/github.com/llvm/llvm-project/libcxx.git' +
         '@' + '8fa87946779682841e21e2da977eccfb6cb3bded',

     # libc++abi
     'client/buildtools/third_party/libc++abi/trunk':
     Var('chromium_git') + '/external/github.com/llvm/llvm-project/libcxxabi.git' +
         '@' + '6918862bfc2bff22b45058fac22b1596c49982fb',

     # clang-format helper scripts, used by `git cl format`.
     'client/buildtools/clang_format/script':
     Var('chromium_git') +
     '/external/github.com/llvm/llvm-project/clang/tools/clang-format.git' +
         '@' + '99803d74e35962f63a775f29477882afd4d57d94',

     # GN
    'client/buildtools/linux64': {
      'packages': [
        {
          'package': 'gn/gn/linux-amd64',
          'version': Var('gn_version'),
        }
      ],
      'dep_type': 'cipd',
      'condition': 'host_os == "linux"',
    },
    'client/buildtools/mac': {
      'packages': [
        {
          'package': 'gn/gn/mac-${{arch}}',
          'version': Var('gn_version'),
        }
      ],
      'dep_type': 'cipd',
      'condition': 'host_os == "mac"',
    },
    'client/buildtools/win': {
      'packages': [
        {
          'package': 'gn/gn/windows-amd64',
          'version': Var('gn_version'),
        }
      ],
      'dep_type': 'cipd',
      'condition': 'host_os == "win"',
    },
}

hooks = [
     # Download to make Linux Goma client linked with an old libc.
     {
       'name': 'sysroot_x64',
       'pattern': '.',
       'condition': 'checkout_linux',
       'action': [
         'python',
         ('client/third_party/chromium_build/linux/sysroot_scripts/'
          'install-sysroot.py'),
          '--arch=x64',
       ],
     },

     # Update the Windows toolchain if necessary. Must run before 'clang' below.
     {
       'name': 'win_toolchain',
       'pattern': '.',
       'action': ['python', 'client/build/vs_toolchain.py', 'update'],
     },
     {
       "name": "clang",
       "pattern": ".",
       "action": ["python", "client/tools/clang/scripts/update.py"],
     },

     # Pull binutils for linux, it is used for simpletry test.
     {
       "name": "binutils",
       "pattern": ".",
       "action": [
         "python",
         "client/test/third_party/binutils/download.py",
       ],
     },

     {
       # Update LASTCHANGE.
       'name': 'lastchange',
       'pattern': '.',
       'action': ['python', 'client/build/util/lastchange.py',
                  '-o', 'client/build/util/LASTCHANGE'],
     },

     # Pull clang-format binaries using checked-in hashes.
     {
         'name': 'clang_format_win',
         'pattern': '.',
         'condition': 'host_os == "win"',
         'action': [ 'download_from_google_storage',
                     '--no_resume',
                     '--no_auth',
                     '--bucket', 'chromium-clang-format',
                     '-s', 'client/buildtools/win/clang-format.exe.sha1',
         ],
     },
     {
         'name': 'clang_format_mac',
         'pattern': '.',
         'condition': 'host_os == "mac"',
         'action': [ 'download_from_google_storage',
                     '--no_resume',
                     '--no_auth',
                     '--bucket', 'chromium-clang-format',
                     '-s', 'client/buildtools/mac/clang-format.sha1',
         ],
     },
     {
         'name': 'clang_format_linux',
         'pattern': '.',
         'condition': 'host_os == "linux"',
         'action': [ 'download_from_google_storage',
                     '--no_resume',
                     '--no_auth',
                     '--bucket', 'chromium-clang-format',
                     '-s', 'client/buildtools/linux64/clang-format.sha1',
         ],
     },
     # Update the Mac toolchain if necessary.
     {
       'name': 'mac_toolchain',
       'pattern': '.',
       'condition': 'checkout_ios or checkout_mac',
       'action': ['python', 'client/build/mac_toolchain.py'],
     },

     # Ensure that the DEPS'd "depot_tools" has its self-update capability
     # disabled.
     {
       'name': 'disable_depot_tools_selfupdate',
       'pattern': '.',
       'action': [
         'python',
         'client/third_party/depot_tools/update_depot_tools_toggle.py',
         '--disable',
       ],
     },
]
