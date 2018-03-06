// Copyright 2017 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cpp_tokenizer.h"

#ifndef NO_SSE2
#include <emmintrin.h>
#endif  // NO_SSE2

#ifdef _WIN32
#include <intrin.h>
#endif

#include "compiler_specific.h"
#include "string_util.h"

namespace {

#ifdef _WIN32
static inline int CountZero(int v) {
  unsigned long r;
  _BitScanForward(&r, v);
  return r;
}
#else
static inline int CountZero(int v) {
  return __builtin_ctz(v);
}
#endif

// __popcnt (on MSVC) emits POPCNT. Some engineers are still using older
// machine that does not have POPCNT. So, we'd like to avoid __popcnt.
// clang-cl.exe must have __builtin_popcunt, so use it.
// For cl.exe, use this somewhat fast algorithm.
// See b/65465347
#if defined(_WIN32) && !defined(__clang__)
static inline int PopCount(int v) {
  v = (v & 0x55555555) + (v >> 1 & 0x55555555);
  v = (v & 0x33333333) + (v >> 2 & 0x33333333);
  v = (v & 0x0f0f0f0f) + (v >> 4 & 0x0f0f0f0f);
  v = (v & 0x00ff00ff) + (v >> 8 & 0x00ff00ff);
  return (v & 0x0000ffff) + (v >>16 & 0x0000ffff);
}
#else
static inline int PopCount(int v) {
  return __builtin_popcount(v);
}
#endif

#ifndef NO_SSE2
typedef ALIGNAS(16) char aligned_char16[16];
const aligned_char16 kNewlinePattern = {
  0xA, 0xA, 0xA, 0xA, 0xA, 0xA, 0xA, 0xA,
  0xA, 0xA, 0xA, 0xA, 0xA, 0xA, 0xA, 0xA,
};
const aligned_char16 kSlashPattern = {
  '/', '/', '/', '/', '/', '/', '/', '/',
  '/', '/', '/', '/', '/', '/', '/', '/',
};
const aligned_char16 kSharpPattern = {
  '#', '#', '#', '#', '#', '#', '#', '#',
  '#', '#', '#', '#', '#', '#', '#', '#',
};
#endif  // NO_SSE2

}  // anonymous namespace

namespace devtools_goma {

std::set<std::string>* CppTokenizer::integer_suffixes_ = nullptr;
CppToken::Type kOpTokenTable[128][128];

static void InitializeTokenSwitchTables() {
  for (int i = 0; i < 128; ++i)
    for (int j = 0; j < 128; ++j)
      kOpTokenTable[i][j] = CppToken::PUNCTUATOR;
# define UC(c)   static_cast<unsigned char>(c)
  kOpTokenTable[UC('=')][UC('=')] = CppToken::EQ;
  kOpTokenTable[UC('!')][UC('=')] = CppToken::NE;
  kOpTokenTable[UC('>')][UC('=')] = CppToken::GE;
  kOpTokenTable[UC('<')][UC('=')] = CppToken::LE;
  kOpTokenTable[UC('&')][UC('&')] = CppToken::LAND;
  kOpTokenTable[UC('|')][UC('|')] = CppToken::LOR;
  kOpTokenTable[UC('>')][UC('>')] = CppToken::RSHIFT;
  kOpTokenTable[UC('<')][UC('<')] = CppToken::LSHIFT;
  kOpTokenTable[UC('#')][UC('#')] = CppToken::DOUBLESHARP;
  kOpTokenTable[UC('\r')][UC('\n')] = CppToken::NEWLINE;
  kOpTokenTable[UC('*')][0] = CppToken::MUL;
  kOpTokenTable[UC('+')][0] = CppToken::ADD;
  kOpTokenTable[UC('-')][0] = CppToken::SUB;
  kOpTokenTable[UC('>')][0] = CppToken::GT;
  kOpTokenTable[UC('<')][0] = CppToken::LT;
  kOpTokenTable[UC('&')][0] = CppToken::AND;
  kOpTokenTable[UC('^')][0] = CppToken::XOR;
  kOpTokenTable[UC('|')][0] = CppToken::OR;
  kOpTokenTable[UC('#')][0] = CppToken::SHARP;
  kOpTokenTable[UC('\n')][0] = CppToken::NEWLINE;
# undef UC
}

// static
void CppTokenizer::InitializeStaticOnce() {
  static const char* kLongSuffixes[] = { "l", "ll" };
  static const char* kUnsignedSuffix = "u";
  integer_suffixes_ = new std::set<std::string>;
  integer_suffixes_->insert(kUnsignedSuffix);
  for (const auto& suffix : kLongSuffixes) {
    integer_suffixes_->insert(suffix);
    integer_suffixes_->insert(std::string(kUnsignedSuffix) + suffix);
    integer_suffixes_->insert(suffix + std::string(kUnsignedSuffix));
  }

  InitializeTokenSwitchTables();
}

// static
bool CppTokenizer::NextTokenFrom(CppInputStream* stream,
                                 bool skip_space,
                                 CppToken* token,
                                 std::string* error_reason) {
  for (;;) {
    const char* cur = stream->cur();
    int c = stream->GetChar();
    if (c == EOF) {
      *token = CppToken(CppToken::END);
      return true;
    }
    if (c >= 128) {
      *token = CppToken(CppToken::PUNCTUATOR, static_cast<char>(c));
      return true;
    }
    if (IsCppBlank(c)) {
      if (skip_space) {
        stream->SkipWhiteSpaces();
        continue;
      }
      *token = CppToken(CppToken::SPACE, static_cast<char>(c));
      return true;
    }
    int c1 = stream->PeekChar();
    switch (c) {
      case '/':
        if (c1 == '/') {
          SkipUntilLineBreakIgnoreComment(stream);
          *token = CppToken(CppToken::NEWLINE);
          return true;
        }
        if (c1 == '*') {
          stream->Advance(1, 0);
          if (!SkipComment(stream, error_reason)) {
            *token = CppToken(CppToken::END);
            return false;
          }
          *token = CppToken(CppToken::SPACE, ' ');
          return true;
        }
        *token = CppToken(CppToken::DIV, '/');
        return true;
      case '%':
        if (c1 == ':') {
          stream->Advance(1, 0);
          if (stream->PeekChar(0) == '%' &&
              stream->PeekChar(1) == ':') {
            stream->Advance(2, 0);
            *token = CppToken(CppToken::DOUBLESHARP);
            return true;
          }
          *token = CppToken(CppToken::SHARP, '#');
          return true;
        }
        *token = CppToken(CppToken::MOD, '%');
        return true;
      case '.':
        if (c1 >= '0' && c1 <= '9') {
          *token = ReadNumber(stream, c, cur);
          return true;
        }
        if (c1 == '.' && stream->PeekChar(1) == '.') {
          stream->Advance(2, 0);
          *token = CppToken(CppToken::TRIPLEDOT);
          return true;
        }
        *token = CppToken(CppToken::PUNCTUATOR, '.');
        return true;
      case '\\':
        c = stream->GetChar();
        if (c != '\r' && c != '\n') {
          *token = CppToken(CppToken::ESCAPED, static_cast<char>(c));
          return true;
        }
        if (c == '\r' && stream->PeekChar() == '\n')
          stream->Advance(1, 1);
        break;
      case '"': {
        *token = CppToken(CppToken::STRING);
        if (!ReadString(stream, token, error_reason)) {
          return false;
        }
        return true;
      }
      default:
        if (c == '_' || IsAsciiAlpha(c)) {
          *token = ReadIdentifier(stream, cur);
          return true;
        }
        if (c >= '0' && c <= '9') {
          *token = ReadNumber(stream, c, cur);
          return true;
        }
        if (c1 == EOF) {
          *token = CppToken(kOpTokenTable[c][0], static_cast<char>(c));
          return true;
        }
        if ((c1 & ~0x7f) == 0 && kOpTokenTable[c][c1] != CppToken::PUNCTUATOR) {
          stream->Advance(1, 0);
          *token = CppToken(kOpTokenTable[c][c1],
                            static_cast<char>(c), static_cast<char>(c1));
          return true;
        }
        *token = CppToken(kOpTokenTable[c][0], static_cast<char>(c));
        return true;
    }
  }
}

// static
bool CppTokenizer::ReadStringUntilDelimiter(CppInputStream* stream,
                                            std::string* result_str,
                                            char delimiter,
                                            std::string* error_reason) {
  const char* begin = stream->cur();
  for (;;) {
    int c = stream->PeekChar();
    if (c == EOF) {
      return true;
    }
    if (c == delimiter) {
      const char* cur = stream->cur() - 1;
      stream->Advance(1, 0);
      if (*cur != '\\') {
        result_str->append(begin, stream->cur() - begin - 1);
        return true;
      }
    } else if (c == '\n') {
      const char* cur = stream->cur() - 1;
      stream->Advance(1, 1);
      cur -= (*cur == '\r');
      if (*cur != '\\') {
        *error_reason = "missing terminating character";
        return false;
      }
      result_str->append(begin, stream->cur() - begin - 2);
      begin = stream->cur();
    } else {
      stream->Advance(1, 0);
    }
  }
}

// static
CppToken CppTokenizer::ReadIdentifier(CppInputStream* stream,
                                      const char* begin) {
  CppToken token(CppToken::IDENTIFIER);
  for (;;) {
    int c = stream->GetChar();
    if (IsAsciiAlphaDigit(c) || c == '_' ||
        (c == '\\' && HandleLineFoldingWithToken(stream, &token, &begin))) {
      continue;
    }
    token.Append(begin, stream->GetLengthToCurrentFrom(begin, c));
    stream->UngetChar(c);
    return token;
  }
}

// (6.4.2) Preprocessing numbers
// pp-number :
//    digit
//    .digit
//    pp-number digit
//    pp-number nondigit
//    pp-number [eEpP] sign  ([pP] is new in C99)
//    pp-number .
//
// static
CppToken CppTokenizer::ReadNumber(CppInputStream* stream, int c0,
                                  const char* begin) {
  CppToken token(CppToken::NUMBER);

  bool maybe_int_constant = (c0 != '.');
  int base = 10;
  int value = 0;
  std::string suffix;
  int c;

  // Handle base prefix.
  if (c0 == '0') {
    base = 8;
    int c1 = stream->PeekChar();
    if (c1 == 'x' || c1 == 'X') {
      stream->Advance(1, 0);
      base = 16;
    }
  } else {
    value = c0 - '0';
  }

  if (maybe_int_constant) {
    // Read the digits part.
    c = ToLowerASCII(stream->GetChar());
    while ((c >= '0' && c <= ('0' + std::min(9, base - 1))) ||
           (base == 16 && c >= 'a' && c <= 'f')) {
      value = value * base + ((c >= 'a') ? (c - 'a' + 10) : (c - '0'));
      c = ToLowerASCII(stream->GetChar());
    }
    stream->UngetChar(c);
  }

  // (digit | [a-zA-Z_] | . | [eEpP][+-])*
  for (;;) {
    c = stream->GetChar();
    if (c == '\\' && HandleLineFoldingWithToken(stream, &token, &begin)) {
      continue;
    }
    if ((c >= '0' && c <= '9') || c == '.' || c == '_') {
      maybe_int_constant = false;
      continue;
    }
    c = ToLowerASCII(c);
    if (c >= 'a' && c <= 'z') {
      if (maybe_int_constant) {
        suffix += static_cast<char>(c);
      }
      if (c == 'e' || c == 'p') {
        int c1 = stream->PeekChar();
        if (c1 == '+' || c1 == '-') {
          maybe_int_constant = false;
          stream->Advance(1, 0);
        }
      }
      continue;
    }
    break;
  }

  token.Append(begin, stream->GetLengthToCurrentFrom(begin, c));
  stream->UngetChar(c);
  if (maybe_int_constant &&
      (suffix.empty() ||
       integer_suffixes_->find(suffix) != integer_suffixes_->end())) {
    token.v.int_value = value;
  }
  return token;
}

// static
bool CppTokenizer::ReadString(CppInputStream* stream,
                              CppToken* result_token,
                              std::string* error_reason) {
  CppToken token(CppToken::STRING);
  if (!ReadStringUntilDelimiter(stream, &token.string_value,
                                '"', error_reason)) {
    return false;
  }

  *result_token = std::move(token);
  return true;
}

// static
bool CppTokenizer::HandleLineFoldingWithToken(CppInputStream* stream,
                                              CppToken* token,
                                              const char** begin) {
  int c = stream->PeekChar();
  if (c != '\r' && c != '\n')
    return false;
  stream->ConsumeChar();
  token->Append(*begin, stream->cur() - *begin - 2);
  if (c == '\r' && stream->PeekChar() == '\n')
    stream->Advance(1, 1);
  *begin = stream->cur();
  return true;
}

// static
bool CppTokenizer::SkipComment(CppInputStream* stream,
                               std::string* error_reason) {
  const char* begin = stream->cur();
#ifndef NO_SSE2
  __m128i slash_pattern = *(__m128i*)kSlashPattern;
  __m128i newline_pattern = *(__m128i*)kNewlinePattern;
  while (stream->cur() + 16 < stream->end()) {
    __m128i s = _mm_loadu_si128((__m128i const*)stream->cur());
    __m128i slash_test = _mm_cmpeq_epi8(s, slash_pattern);
    __m128i newline_test = _mm_cmpeq_epi8(s, newline_pattern);
    int result = _mm_movemask_epi8(slash_test);
    int newline_result = _mm_movemask_epi8(newline_test);
    while (result) {
      int index = CountZero(result);
      unsigned int shift = (1 << index);
      result &= ~shift;
      const char* cur = stream->cur() + index - 1;
      if (*cur == '*') {
        unsigned int mask = shift - 1;
        stream->Advance(index + 1, PopCount(newline_result & mask));
        return true;
      }
    }
    stream->Advance(16, PopCount(newline_result));
  }
#endif  // NO_SSE2
  for (;;) {
    int c = stream->PeekChar();
    if (c == EOF) {
      *error_reason = "missing terminating '*/' for comment";
      return false;
    }
    if (c == '/' && stream->cur() != begin &&
        *(stream->cur() - 1) == '*') {
      stream->Advance(1, 0);
      return true;
    }
    stream->ConsumeChar();
  }
}

// static
bool CppTokenizer::SkipUntilDirective(CppInputStream* stream,
                                      std::string* error_reason) {
  const char* begin = stream->cur();
#ifndef NO_SSE2
  // TODO: String index instruction (pcmpestri) would work better
  // on sse4.2 enabled platforms.
  __m128i slash_pattern = *(__m128i*)kSlashPattern;
  __m128i sharp_pattern = *(__m128i*)kSharpPattern;
  __m128i newline_pattern = *(__m128i*)kNewlinePattern;
  while (stream->cur() + 16 < stream->end()) {
    __m128i s = _mm_loadu_si128((__m128i const*)stream->cur());
    __m128i slash_test = _mm_cmpeq_epi8(s, slash_pattern);
    __m128i sharp_test = _mm_cmpeq_epi8(s, sharp_pattern);
    __m128i newline_test = _mm_cmpeq_epi8(s, newline_pattern);
    int slash_result = _mm_movemask_epi8(slash_test);
    int sharp_result = _mm_movemask_epi8(sharp_test);
    int newline_result = _mm_movemask_epi8(newline_test);
    int result = slash_result | sharp_result;
    while (result) {
      int index = CountZero(result);
      unsigned int shift = (1 << index);
      result &= ~shift;
      unsigned int mask = shift - 1;
      const char* cur = stream->cur() + index;
      if (*cur == '/') {
        int c1 = *(cur + 1);
        if (c1 == '/') {
          stream->Advance(index + 2, PopCount(newline_result & mask));
          SkipUntilLineBreakIgnoreComment(stream);
          goto done;
        } else if (c1 == '*') {
          stream->Advance(index + 2, PopCount(newline_result & mask));
          if (!SkipComment(stream, error_reason))
            return false;
          goto done;
        }
      } else if (*cur == '#') {
        if (IsAfterEndOfLine(cur, stream->begin())) {
          stream->Advance(index + 1, PopCount(newline_result & mask));
          return true;
        }
      }
    }
    stream->Advance(16, PopCount(newline_result));
  done:
    continue;
  }
#endif  // NO_SSE2
  for (;;) {
    int c = stream->PeekChar();
    if (c == EOF)
      return false;
    if (stream->cur() != begin) {
      int c0 = *(stream->cur() - 1);
      if (c0 == '/' && c == '/') {
        stream->Advance(1, 0);
        SkipUntilLineBreakIgnoreComment(stream);
        continue;
      } else if (c0 == '/' && c == '*') {
        stream->Advance(1, 0);
        if (!SkipComment(stream, error_reason))
          return false;
      }
    }
    if (c == '#') {
      if (IsAfterEndOfLine(stream->cur(),
                           stream->begin())) {
        stream->Advance(1, 0);
        return true;
      }
      stream->Advance(1, 0);
      continue;
    }
    stream->ConsumeChar();
  }

  return false;
}

// static
void CppTokenizer::SkipUntilLineBreakIgnoreComment(CppInputStream* stream) {
#ifndef NO_SSE2
  __m128i newline_pattern = *(__m128i*)kNewlinePattern;
  while (stream->cur() + 16 < stream->end()) {
    __m128i s = _mm_loadu_si128((__m128i const*)stream->cur());
    __m128i newline_test = _mm_cmpeq_epi8(s, newline_pattern);
    int newline_result = _mm_movemask_epi8(newline_test);
    int result = newline_result;
    while (result) {
      int index = CountZero(result);
      unsigned int shift = (1 << index);
      result &= ~shift;
      unsigned int mask = shift - 1;
      const char* cur = stream->cur() + index - 1;
      cur -= (*cur == '\r');
      if (*cur != '\\') {
        stream->Advance(index + 1, PopCount(newline_result & mask));
        return;
      }
    }
    stream->Advance(16, PopCount(newline_result));
  }
#endif  // NO_SSE2
  for (;;) {
    int c = stream->PeekChar();
    if (c == EOF)
      return;
    if (c == '\n') {
      const char* cur = stream->cur() - 1;
      stream->Advance(1, 1);
      cur -= (*cur == '\r');
      if (*cur != '\\')
        return;
    } else {
      stream->Advance(1, 0);
    }
  }
}

// static
bool CppTokenizer::IsAfterEndOfLine(const char* cur, const char* begin) {
  for (;;) {
    if (cur == begin)
      return true;
    int c = *--cur;
    if (!IsCppBlank(c))
      break;
  }

  while (begin <= cur) {
    int c = *cur;
    if (c == '\n') {
      if (--cur < begin)
        return true;
      cur -= (*cur == '\r');
      if (cur < begin || *cur != '\\')
        return true;

      --cur;
      continue;
    }

    if (c == '/') {
      if (--cur < begin || *cur != '*')
        return false;

      --cur;
      bool block_comment_start_found = false;
      // Move backward until "/*" is found.
      while (cur - 1 >= begin) {
        if (*(cur - 1) == '/' && *cur == '*') {
          cur -= 2;
          block_comment_start_found = true;
          break;
        }
        --cur;
      }

      if (block_comment_start_found)
        continue;

      // When '/*' is not found, it's not after end of line.
      return false;
    }

    if (IsCppBlank(c)) {
      --cur;
      continue;
    }

    return false;
  }

  return true;
}

}  // namespace devtools_goma