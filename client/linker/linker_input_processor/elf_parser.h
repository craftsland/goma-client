// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_LINKER_LINKER_INPUT_PROCESSOR_ELF_PARSER_H_
#define DEVTOOLS_GOMA_CLIENT_LINKER_LINKER_INPUT_PROCESSOR_ELF_PARSER_H_

#include <memory>
#include <string>
#include <vector>

#include "basictypes.h"

namespace devtools_goma {

class ElfParser {
 public:
  static std::unique_ptr<ElfParser> NewElfParser(const std::string& filename);
  virtual ~ElfParser() {}
  virtual bool valid() const = 0;
  virtual void UseProgramHeader(bool use_program_header) = 0;
  virtual bool ReadDynamicNeeded(std::vector<std::string>* needed) = 0;
  virtual bool ReadDynamicNeededAndRpath(std::vector<std::string>* needed,
                                         std::vector<std::string>* rpath) = 0;

  static bool IsElf(const std::string& filename);

 protected:
  ElfParser() {}
 private:
  DISALLOW_COPY_AND_ASSIGN(ElfParser);
};

}  // namespace devtools_goma


#endif  // DEVTOOLS_GOMA_CLIENT_LINKER_LINKER_INPUT_PROCESSOR_ELF_PARSER_H_
