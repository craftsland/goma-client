#!/usr/bin/python
#
# Copyright 2011 The Goma Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Generates compiler_proxy info string for GOMA.

This will collect user name, build date, changelist number, and
etc. for use in HTTP RPC for user-agent and compiler_proxy's console.
User-agents will help going through the server logs when debugging
and the console will help users to make a better bug report.
"""

from __future__ import print_function



import datetime
import getpass
import optparse
import os
import os.path
import re
import socket
import sys


def GetUserName():
  """Obtain user ID string."""
  return getpass.getuser()


def GetHostName():
  """Obtain host name string."""
  return socket.gethostname()


def GetRevisionNumber():
  """Obtain a number to represent revision of source code.

  Returns:
    a revision number string whose format is:
      <commit hash>@<committer date unix timestamp>
  """
  rev_number_file = os.environ.get('COMPILER_PROXY_REVISION_NUMBER_FILE')
  if rev_number_file:
    try:
      with open(rev_number_file) as f:
        rev = f.read().strip()
        if re.match(r'[0-9a-f]+@\d+', rev):
          return rev
        print('revision seems not match the pattern: %s' % rev)
    except IOError as ex:
      print('cannot open revision number file: %s' % ex)
  # <commit hash>@<committer date unix timestamp>
  git_hash_output = os.popen('git log -1 --pretty=format:%H@%ct', 'r')
  git_hash = git_hash_output.read().strip()
  if git_hash != "":
    return git_hash
  print('Could not get CL information, falling back to unknown.')
  return 'unknown'


def GetDateAndTime():
  """Obtain date and time."""
  return datetime.datetime.utcnow().isoformat() + "Z"


def GetGomaDirectory():
  """Obtain goma directory."""
  return os.path.dirname(os.getcwd())


def UserAgentString():
  # TODO: add platform string.
  return 'compiler-proxy built by %s at %s on %s ' % (GetUserName(),
                                                      GetRevisionNumber(),
                                                      GetDateAndTime())


def GetGNArgs(gn_out_dir):
  """Obtain gn args used for this build."""
  gn_args_file = os.path.join(gn_out_dir, 'args.gn')
  try:
    contents = []
    with open(gn_args_file) as f:
      for line in f.readlines():
        line = line.strip()
        if line.startswith('#'):
          continue
        contents.append(line.replace('"', '\'').replace('\\', '\\\\'))
    return ','.join(contents)
  except IOError as ex:
    print('cannot open args.gn file: %s' % ex)
    return 'unknown'


def GenerateSourceCode(out_dir):
  info_file = os.path.join(out_dir, 'compiler_proxy_info.h')
  try:
    fp = open(info_file, 'w')
    fp.write(
        """
// File autogenerated by generate_compiler_proxy_info.py, do not modify.
#ifndef COMPILER_PROXY_INFO_H_
#define COMPILER_PROXY_INFO_H_
static const char kUserAgentString[] = "%(user_agent)s";
static const char kBuiltTimeString[] = "%(built_time)s";
static const char kBuiltDirectoryString[] = "%(built_directory)s";
static const char kBuiltUserNameString[] = "%(user_name)s";
static const char kBuiltHostNameString[] = "%(host_name)s";
static const char kBuiltRevisionString[] = "%(revision)s";
static const char kBuiltGNArgsString[] = "%(gn_args)s";
#endif // COMPILER_PROXY_INFO_H_
""" % {
            'user_agent': UserAgentString(),
            'built_time': GetDateAndTime(),
            'built_directory': repr(GetGomaDirectory())[1:-1],
            'user_name': GetUserName(),
            'host_name': GetHostName(),
            'revision': GetRevisionNumber(),
            'gn_args': GetGNArgs(os.path.dirname(os.path.dirname(out_dir))),
        })
    fp.close()
  except Exception as ex:
    os.remove(info_file)
    print('Failed to generate %s: %s' % (info_file, ex))
    sys.exit(1)


def main():
  parser = optparse.OptionParser()
  parser.add_option('-o', '--out-dir', default='.',
                    help='Output directory')
  options, _ = parser.parse_args()
  GenerateSourceCode(options.out_dir)


if __name__ == '__main__':
  main()
