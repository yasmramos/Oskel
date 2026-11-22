#include "bindgen.h"

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frontend/ast.h"  // isKnownTypeName / isIntegerTypeName: the FFI vocabulary
#include "ffi_types.h"  // the FFI vocabulary and layout: the single source of truth
                         // this tool validates a signature against

namespace {

// --------------------------------------------------------------------- lexer

// A C header token. `Str`/`Char` carry their raw spelling (quotes included) and
// are only ever skipped, never interpreted: a header's string literals live in
// macros, which the tool ignores, but they still have to be tokenized so a `;`
// or a `,` inside one does not split a declaration.
enum class TokKind { Ident, Number, Punct, Ellipsis, Str, Char, Eof };

struct Token {
  TokKind kind = TokKind::Eof;
  std::string text;
  int line = 1;
};

bool isIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentPart(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Tokenizes a C header. Comments (`//`, `/* ... */`), preprocessor directives
// (a `#` line, with `\`-continuations) and whitespace are dropped. Preprocessor
// conditionals are *not* evaluated: every branch is tokenized, which is the
// right default for a header whose declarations sit inside `#ifdef`s.
std::vector<Token> tokenize(const std::string& source) {
  std::vector<Token> tokens;
  std::size_t i = 0;
  int line = 1;
  const std::size_t n = source.size();
  auto push = [&](TokKind kind, const std::string& text) {
    tokens.push_back(Token{kind, text, line});
  };
  while (i < n) {
    const char c = source[i];
    if (c == '\n') {
      ++line;
      ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && source[i + 1] == '/') {
      while (i < n && source[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && source[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(source[i] == '*' && source[i + 1] == '/')) {
        if (source[i] == '\n') ++line;
        ++i;
      }
      i = (i + 1 < n) ? i + 2 : n;
      continue;
    }
    if (c == '#') {
      // Skip the whole directive, honoring `\`-continued lines.
      while (i < n) {
        if (source[i] == '\\' && i + 1 < n && source[i + 1] == '\n') {
          ++line;
          i += 2;
          continue;
        }
        if (source[i] == '\n') break;
        ++i;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      const int startLine = line;
      std::string text(1, c);
      ++i;
      while (i < n && source[i] != quote) {
        if (source[i] == '\\' && i + 1 < n) {
          text.push_back(source[i]);
          text.push_back(source[i + 1]);
          if (source[i + 1] == '\n') ++line;
          i += 2;
          continue;
        }
        if (source[i] == '\n') ++line;
        text.push_back(source[i]);
        ++i;
      }
      if (i < n) {
        text.push_back(source[i]);
        ++i;
      }
      tokens.push_back(Token{quote == '"' ? TokKind::Str : TokKind::Char, text,
                             startLine});
      continue;
    }
    if (isIdentStart(c)) {
      std::string text;
      while (i < n && isIdentPart(source[i])) text.push_back(source[i++]);
      push(TokKind::Ident, text);
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(source[i + 1])))) {
      std::string text;
      while (i < n && (isIdentPart(source[i]) || source[i] == '.')) {
        text.push_back(source[i++]);
      }
      push(TokKind::Number, text);
      continue;
    }
    if (c == '.' && i + 2 < n && source[i + 1] == '.' && source[i + 2] == '.') {
      i += 3;
      push(TokKind::Ellipsis, "...");
      continue;
    }
    // Multi-character operators that would otherwise split a declaration are
    // kept as single punctuation tokens; the parser only ever compares text.
    std::string op(1, c);
    static const char* twoChar[] = {"->", "::", "==", "!=", "<=", ">=", "&&", "||"};
    if (i + 1 < n) {
      for (const char* candidate : twoChar) {
        if (source.compare(i, 2, candidate) == 0) {
          op = candidate;
          break;
        }
      }
    }
    i += op.size();
    push(TokKind::Punct, op);
  }
  tokens.push_back(Token{TokKind::Eof, "", line});
  return tokens;
}

// -------------------------------------------------------------- type model

// A C type as far as the FFI vocabulary can express it.
struct Base {
  enum class Kind { Void, Char, Int, Float, Double, Bool, Struct, Unknown };
  Kind kind = Kind::Unknown;
  std::string oskel;    // the Oskel FFI name for a scalar (Int/Float/Double/...)
  std::string spelling; // the C spelling, for a diagnostic
  bool ok = true;       // false when the vocabulary cannot express the type
  std::string reason;   // why, when `ok` is false
  // A plain C `char`, as opposed to `signed char`/`unsigned char`. Only a plain
  // `char *` is a NUL-terminated string (`cstring`); the two explicitly signed
  // forms are byte buffers and cross as `ptr`.
  bool plainChar = false;
  // A callback: a C function pointer (`R (*)(P...)`), whose Oskel spelling is
  // `fn(P...): R`. `callbackParams`/`callbackReturn` hold the Oskel FFI names.
  bool isCallback = false;
  std::vector<std::string> callbackParams;
  std::string callbackReturn;  // "void" for a callback that returns nothing

  // A struct by value: the name the Oskel `struct` declaration gets, and its
  // fields. `oskel` carries that name, so a parameter/return spells it like any
  // other FFI type name. `anonymousStruct` marks a `typedef struct { ... } X`
  // whose body has no tag: the typedef declarator names it a moment later.
  std::vector<ffi::FfiField> fields;
  bool anonymousStruct = false;
};

// The compiler's view of a parsed type, so a generated signature can be
// validated by `ffi::validateSignature` instead of by rules this tool would
// otherwise have to keep in step with the compiler's by hand. `fallback` is the
// spelling the emitter uses when the type resolved to nothing (`int32` for a
// parameter, `void` for a return), so the validator is asked about exactly the
// text that goes into the module -- validating a different spelling would make
// the check an approximation of the emission rather than a check on it.
ffi::TypeSpec specOf(const Base& base, const char* fallback) {
  ffi::TypeSpec spec;
  if (base.isCallback) {
    spec.isCallback = true;
    spec.callbackParams = base.callbackParams;
    spec.callbackReturn = base.callbackReturn;
    return spec;
  }
  if (base.kind == Base::Kind::Struct) {
    spec.isStruct = true;
    spec.fields = base.fields;
    return spec;
  }
  spec.scalarType = base.oskel.empty() ? fallback : base.oskel;
  return spec;
}

// The Oskel spelling of a callback type: `fn(P...): R`.
std::string callbackSpelling(const Base& cb) {
  std::string s = "fn(";
  for (std::size_t i = 0; i < cb.callbackParams.size(); ++i) {
    if (i != 0) s += ", ";
    s += cb.callbackParams[i];
  }
  s += "): ";
  s += cb.callbackReturn.empty() ? "void" : cb.callbackReturn;
  return s;
}

// The words that select a calling convention. They carry no type information
// the FFI needs (its own `extern "C"` fixes how arguments travel), but they sit
// between the `(` and the `*` of a declarator, so they must be skipped there
// rather than mistaken for the declarator's name.
bool isCallingConvention(const std::string& w) {
  return w == "__stdcall" || w == "__cdecl" || w == "__fastcall" || w == "__thiscall" ||
         w == "__vectorcall" || w == "_stdcall" || w == "_cdecl" || w == "WINAPI" ||
         w == "APIENTRY" || w == "CALLBACK" || w == "PASCAL" || w == "NTAPI" ||
         w == "STDMETHODCALLTYPE";
}

// The C words that never change the type on their own: storage classes and
// qualifiers the FFI vocabulary ignores. A calling convention is one of them:
// it selects how the arguments travel, which the FFI's own `extern "C"` already
// fixes, so the word is dropped rather than ending the specifier list (which
// would drop the whole declaration in silence).
bool isIgnoredSpecifier(const std::string& w) {
  return w == "const" || w == "volatile" || w == "restrict" || w == "__restrict" ||
         w == "__restrict__" || w == "extern" || w == "static" || w == "inline" ||
         w == "__inline" || w == "__inline__" || w == "register" || w == "auto" ||
         w == "_Noreturn" || w == "__forceinline" || w == "__const" || w == "__volatile__" ||
         w == "_Atomic" || isCallingConvention(w);
}

// A fixed-width or standard typedef name recognized without seeing `<stdint.h>`
// (the tool does not resolve includes). The name is used verbatim as the Oskel
// FFI type when it is already part of the vocabulary.
bool knownFfiName(const std::string& name) {
  return isKnownTypeName(name) && (isIntegerTypeName(name) || name == "size_t" ||
                                   name == "float" || name == "double" || name == "bool" ||
                                   name == "ptr" || name == "cstring");
}

// Well-known C typedefs that map onto the FFI vocabulary even when the header
// does not spell the `typedef` (it usually comes from `<stdint.h>`/`<stddef.h>`,
// which the tool does not read).
bool resolveWellKnown(const std::string& name, Base& out) {
  static const std::unordered_map<std::string, std::string> table = {
      {"int8_t", "int8"},     {"int16_t", "int16"},   {"int32_t", "int32"},
      {"int64_t", "int64"},   {"uint8_t", "uint8"},   {"uint16_t", "uint16"},
      {"uint32_t", "uint32"}, {"uint64_t", "uint64"}, {"intptr_t", "int64"},
      {"uintptr_t", "uint64"}, {"intmax_t", "int64"}, {"uintmax_t", "uint64"},
      {"size_t", "size_t"},   {"ssize_t", "int64"},   {"ptrdiff_t", "int64"},
      {"unsigned", "uint32"}, {"signed", "int32"},
  };
  auto it = table.find(name);
  if (it == table.end()) return false;
  out.ok = true;
  out.reason.clear();
  out.spelling = name;
  out.oskel = it->second;
  if (out.oskel == "float") {
    out.kind = Base::Kind::Float;
  } else if (out.oskel == "double") {
    out.kind = Base::Kind::Double;
  } else if (out.oskel == "bool") {
    out.kind = Base::Kind::Bool;
  } else {
    out.kind = Base::Kind::Int;
  }
  return true;
}

// ------------------------------------------------------- object-like macros
//
// `#define NAME literal` is the C way to name a constant, and a header is full
// of them. The tokenizer drops every preprocessor line (it cannot evaluate
// conditionals), so the constants are collected from the raw source separately:
// an object-like macro whose replacement is a single integer, float, string or
// character literal becomes `let NAME = literal;`. A function-like macro and a
// replacement that is an expression (`#define X (A | B)`) are not constants;
// they are ignored, not guessed at, because the tool has no preprocessor.

struct MacroConstant {
  std::string name;
  std::string value;
};

std::string trimCopy(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

// Removes `//` and `/* ... */` comments from one logical line. Strings and
// character literals are copied untouched, so a `//` inside `"..."` survives.
// An unterminated block comment makes the whole line unusable (the caller skips
// the macro) because its replacement cannot be read reliably.
std::string stripLineComments(const std::string& line) {
  std::string out;
  bool inString = false;
  bool inChar = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (inString || inChar) {
      out.push_back(c);
      if (c == '\\' && i + 1 < line.size()) {
        out.push_back(line[++i]);
      } else if ((inString && c == '"') || (inChar && c == '\'')) {
        inString = inChar = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
      out.push_back(c);
      continue;
    }
    if (c == '\'') {
      inChar = true;
      out.push_back(c);
      continue;
    }
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
      const std::size_t end = line.find("*/", i + 2);
      if (end == std::string::npos) return std::string();
      i = end + 1;
      continue;
    }
    out.push_back(c);
  }
  return out;
}

// A single C integer literal (decimal, `0x` hex or leading-zero octal, with an
// optional `u`/`l` suffix). The value is emitted as decimal and must fit the
// signed 64-bit Oskel integer: a larger value would make `oskel -c` reject the
// module, and a `#define` is a constant, not a bit-pattern assertion.
bool parseIntegerLiteral(const std::string& text, std::string& out) {
  if (text.empty()) return false;
  std::size_t i = 0;
  bool negative = false;
  if (text[i] == '+' || text[i] == '-') {
    negative = text[i] == '-';
    ++i;
  }
  if (i >= text.size()) return false;
  const std::size_t bodyStart = i;
  int base = 10;
  if (text[i] == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
    base = 16;
    i += 2;
    const std::size_t digits = i;
    while (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
    if (i == digits) return false;
  } else if (text[i] == '0') {
    base = 8;
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '7') ++i;
  } else if (text[i] >= '1' && text[i] <= '9') {
    base = 10;
    ++i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
  } else {
    return false;
  }
  const std::size_t suffixStart = i;
  while (i < text.size() &&
         (text[i] == 'u' || text[i] == 'U' || text[i] == 'l' || text[i] == 'L')) {
    ++i;
  }
  if (i != text.size()) return false;
  const std::string body = text.substr(bodyStart, suffixStart - bodyStart);
  errno = 0;
  char* end = nullptr;
  const unsigned long long magnitude = std::strtoull(body.c_str(), &end, base == 16 ? 0 : base);
  if (errno == ERANGE || end == body.c_str() || *end != '\0') return false;
  const unsigned long long limit = negative ? (1ULL << 63) : ((1ULL << 63) - 1);
  if (magnitude > limit) return false;
  if (magnitude == 0) {
    out = "0";
    return true;
  }
  out = negative ? "-" : "";
  out += std::to_string(magnitude);
  return true;
}

// A single C floating literal. The `f` suffix is dropped (Oskel has one float
// literal, a binary64) and a `long double` suffix is rejected. A spelling with
// an exponent but no dot is normalized to `1.0e3`, which every Oskel reader
// accepts.
bool parseFloatLiteral(const std::string& text, std::string& out) {
  if (text.empty()) return false;
  std::size_t i = 0;
  bool negative = false;
  if (text[i] == '+' || text[i] == '-') {
    negative = text[i] == '-';
    ++i;
  }
  const std::size_t bodyStart = i;
  bool digitsBefore = false;
  while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
    ++i;
    digitsBefore = true;
  }
  bool dot = false;
  bool digitsAfter = false;
  if (i < text.size() && text[i] == '.') {
    dot = true;
    ++i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
      ++i;
      digitsAfter = true;
    }
  }
  if (!digitsBefore && !digitsAfter) return false;
  bool exponent = false;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    exponent = true;
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    const std::size_t exponentDigits = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    if (i == exponentDigits) return false;
  }
  bool floatSuffix = false;
  if (i < text.size() && (text[i] == 'f' || text[i] == 'F')) {
    floatSuffix = true;
    ++i;
  } else if (i < text.size() && (text[i] == 'l' || text[i] == 'L')) {
    return false;  // `long double`: Oskel has no 80-bit type
  }
  if (i != text.size()) return false;
  if (!dot && !exponent && !floatSuffix) return false;  // an integer, not a float
  std::string body = text.substr(bodyStart, i - bodyStart);
  if (floatSuffix) body.pop_back();
  if (body.empty()) return false;
  if (!dot && !exponent) body += ".0";
  if (body.front() == '.') body.insert(body.begin(), '0');
  if (body.back() == '.') body += '0';
  const std::size_t exponentPos = body.find_first_of("eE");
  if (exponentPos != std::string::npos && body.find('.') == std::string::npos) {
    body.insert(exponentPos, ".0");
  }
  out = negative ? "-" : "";
  out += body;
  return true;
}

// A single double-quoted C string whose escapes Oskel also understands. A
// concatenation (`"a" "b"`), a wide string (`L"a"`) and an escape with no Oskel
// equivalent (`\a`, a three-digit `\x`) are not emitted.
bool isCStringLiteral(const std::string& text) {
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\') {
      if (i + 1 >= text.size()) return false;
      const char escape = text[i + 1];
      if (escape == 'n' || escape == 't' || escape == 'r' || escape == '\\' ||
          escape == '"' || escape == '\'' || escape == '0') {
        ++i;
        continue;
      }
      if (escape == 'x') {
        if (i + 3 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[i + 2])) ||
            !std::isxdigit(static_cast<unsigned char>(text[i + 3]))) {
          return false;
        }
        // Oskel reads exactly two hex digits; a third would change the value.
        if (i + 4 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 4]))) {
          return false;
        }
        i += 3;
        continue;
      }
      if (escape == 'u') {
        if (i + 5 >= text.size()) return false;
        for (std::size_t k = 2; k <= 5; ++k) {
          if (!std::isxdigit(static_cast<unsigned char>(text[i + k]))) return false;
        }
        if (i + 6 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 6]))) {
          return false;
        }
        i += 5;
        continue;
      }
      return false;
    }
    if (text[i] == '"' && i != 0 && i != text.size() - 1) return false;
  }
  return true;
}

// A single C character literal, emitted as its integer code (a C character
// constant is an `int`, which is exactly what a module constant holds).
bool parseCharLiteral(const std::string& text, std::string& out) {
  if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') return false;
  const std::string inner = text.substr(1, text.size() - 2);
  unsigned int value = 0;
  if (inner.size() == 1) {
    value = static_cast<unsigned char>(inner[0]);
  } else if (inner.size() == 2 && inner[0] == '\\') {
    switch (inner[1]) {
      case 'n': value = 10; break;
      case 't': value = 9; break;
      case 'r': value = 13; break;
      case '0': value = 0; break;
      case '\\': value = 92; break;
      case '\'': value = 39; break;
      case '"': value = 34; break;
      default: return false;
    }
  } else {
    return false;
  }
  out = std::to_string(value);
  return true;
}

std::vector<MacroConstant> collectMacroConstants(const std::string& source) {
  std::vector<MacroConstant> constants;
  std::size_t i = 0;
  while (i < source.size()) {
    std::string line;
    while (i < source.size()) {
      const std::size_t end = source.find('\n', i);
      std::string part =
          source.substr(i, end == std::string::npos ? std::string::npos : end - i);
      i = end == std::string::npos ? source.size() : end + 1;
      if (!part.empty() && part.back() == '\r') part.pop_back();
      if (!part.empty() && part.back() == '\\') {
        part.pop_back();
        line += part;
        continue;
      }
      line += part;
      break;
    }
    line = trimCopy(line);
    if (line.empty() || line[0] != '#') continue;
    std::size_t p = 1;
    while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
    if (line.compare(p, 6, "define") != 0) continue;
    p += 6;
    if (p < line.size() && !std::isspace(static_cast<unsigned char>(line[p]))) continue;
    while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
    const std::size_t nameStart = p;
    if (p >= line.size() || !isIdentStart(line[p])) continue;
    while (p < line.size() && isIdentPart(line[p])) ++p;
    const std::string name = line.substr(nameStart, p - nameStart);
    if (p < line.size() && line[p] == '(') continue;  // function-like macro
    std::string value = trimCopy(stripLineComments(line.substr(p)));
    if (value.empty()) continue;
    // Strip balanced parentheses that wrap the whole replacement: `(1)`.
    for (;;) {
      if (value.size() < 2 || value.front() != '(' || value.back() != ')') break;
      int depth = 0;
      bool wraps = true;
      for (std::size_t k = 0; k < value.size(); ++k) {
        if (value[k] == '(') {
          ++depth;
        } else if (value[k] == ')') {
          --depth;
          if (depth == 0 && k + 1 != value.size()) {
            wraps = false;
            break;
          }
        }
      }
      if (!wraps || depth != 0) break;
      value = trimCopy(value.substr(1, value.size() - 2));
    }
    if (value.empty()) continue;
    std::string literal;
    if (parseIntegerLiteral(value, literal) || parseFloatLiteral(value, literal) ||
        isCStringLiteral(value) || parseCharLiteral(value, literal)) {
      if (literal.empty()) literal = value;  // a string keeps its spelling
      constants.push_back(MacroConstant{name, literal});
    }
  }
  return constants;
}

// -------------------------------------------------------------- the parser

class Bindgen {
 public:
  Bindgen(std::vector<Token> tokens, std::vector<MacroConstant> macroConstants,
          std::string sourceName, bindgen::Options options)
      : tokens_(std::move(tokens)),
        macroConstants_(std::move(macroConstants)),
        sourceName_(std::move(sourceName)),
        options_(options) {
    if (sourceName_.empty()) sourceName_ = "input";
  }

  bindgen::Result run() {
    // A C++ header wraps its C declarations in `extern "C" { ... }`. The
    // linkage keyword and the ABI string are consumed, and the block's braces
    // are dropped, so the declarations inside are parsed as if they were at the
    // top level (which is what they are, for binding purposes).
    while (!check(TokKind::Eof)) {
      // A stray `}` closes an `extern "C" { ... }` block (or a construct the
      // tool did not consume); it carries no binding of its own.
      if (checkPunct("}")) {
        advance();
        continue;
      }
      if (checkIdent("extern") && peek(1).kind == TokKind::Str) {
        advance();  // `extern`
        advance();  // the ABI string
        matchPunct("{");  // the block's declarations follow at the top level
        continue;
      }
      if (checkIdent("typedef")) {
        parseTypedef();
      } else {
        // `struct`/`union`/`enum` are handled by the ordinary declaration path
        // too: `parseSpecifiers` consumes a tag (and its definition, if any),
        // so a plain `struct X { ... };` is skipped as a non-binding while
        // `struct X make(...)` is seen as a function returning a struct.
        parseFunctionOrVariable();
      }
    }
    emitHeader();
    return std::move(result_);
  }

 private:
  // ---------------------------------------------------------------- helpers

  const Token& peek(std::size_t ahead = 0) const {
    const std::size_t index = pos_ + ahead;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
  }
  bool check(TokKind kind) const { return peek().kind == kind; }
  bool checkPunct(const std::string& text) const {
    return peek().kind == TokKind::Punct && peek().text == text;
  }
  bool checkIdent(const std::string& text) const {
    return peek().kind == TokKind::Ident && peek().text == text;
  }
  const Token& advance() {
    if (pos_ + 1 < tokens_.size()) ++pos_;
    return tokens_[pos_ - 1];
  }
  bool matchPunct(const std::string& text) {
    if (checkPunct(text)) {
      advance();
      return true;
    }
    return false;
  }
  bool matchIdent(const std::string& text) {
    if (checkIdent(text)) {
      advance();
      return true;
    }
    return false;
  }
  // Skips a balanced `{ ... }` (the caller has not consumed the `{`).
  void skipBraces() {
    if (!matchPunct("{")) return;
    int depth = 1;
    while (!check(TokKind::Eof) && depth > 0) {
      if (checkPunct("{")) ++depth;
      if (checkPunct("}")) --depth;
      advance();
    }
  }
  // Skips a balanced `( ... )` (the caller has not consumed the `(`).
  void skipParens() {
    if (!matchPunct("(")) return;
    int depth = 1;
    while (!check(TokKind::Eof) && depth > 0) {
      if (checkPunct("(")) ++depth;
      if (checkPunct(")")) --depth;
      advance();
    }
  }
  // `__attribute__((...))` / `__declspec(...)`: consumed and ignored.
  void skipAnnotations() {
    for (;;) {
      if (checkIdent("__attribute__") || checkIdent("__attribute")) {
        advance();
        if (checkPunct("(")) skipParens();
        continue;
      }
      if (checkIdent("__declspec") || checkIdent("_Nullable") || checkIdent("_Nonnull") ||
          checkIdent("__extension__") || checkIdent("_CRTIMP")) {
        advance();
        if (checkPunct("(")) skipParens();
        continue;
      }
      break;
    }
  }

  // -------------------------------------------------------- type specifiers

  struct Spec {
    int longCount = 0;
    int shortCount = 0;
    bool isUnsigned = false;
    bool isSigned = false;
    bool isStructTag = false;
    std::string word;  // the base word (a builtin keyword, a typedef, or "")
    // `struct`/`union`: the body when the declaration defines one. An anonymous
    // body has no tag, so the typedef declarator is what names it.
    std::vector<ffi::FfiField> structFields;
    bool hasStructBody = false;
    std::string structProblem;
    // `enum`: an enum is an `int` at the ABI, so it maps to `int32` (BACKLOG
    // §7.10). Its members become module constants.
    bool isEnumTag = false;
  };

  // Whether `word` is a type word the tool understands: a C type keyword, a
  // typedef this header declared, a well-known `stdint.h`/`stddef.h` name, or
  // an Oskel FFI name.
  bool isKnownTypeWord(const std::string& word) const {
    if (word == "void" || word == "char" || word == "int" || word == "float" ||
        word == "double" || word == "bool" || word == "_Bool") {
      return true;
    }
    if (typedefs_.count(word) != 0) return true;
    Base scratch;
    if (resolveWellKnown(word, scratch)) return true;
    return knownFfiName(word);
  }

  // Whether the token after an identifier means that identifier was a macro
  // rather than a type. A header is full of object-like macros (`#define API
  // __attribute__((...))`) that the tool does not expand, so a leading unknown
  // word immediately followed by a real type word is dropped as one. This is a
  // deliberate, narrow tolerance: without a preprocessor the tool cannot know
  // `API` is a macro, and dropping it keeps the rest of the declaration
  // parseable. For a header whose macros change the *type* (rare), preprocess
  // it first (`cc -E header.h | bindgen -`).
  bool looksLikeMacroPrefix() const {
    const Token& next = peek(1);
    if (next.kind != TokKind::Ident) return false;
    return isKnownTypeWord(next.text) || next.text == "struct" || next.text == "union" ||
           next.text == "enum" || next.text == "signed" || next.text == "unsigned" ||
           next.text == "short" || next.text == "long";
  }

  // A *function-like* macro used as a declaration prefix: `MY_API(int) f2(int);`.
  // Without a preprocessor the argument is as likely to be the return type as
  // decoration, so the declaration cannot be translated honestly. It is
  // reported as a skip rather than guessed at or dropped in silence.
  bool looksLikeFunctionLikeMacro(const std::string& word) const {
    if (isKnownTypeWord(word) || peek(1).kind != TokKind::Punct || peek(1).text != "(") {
      return false;
    }
    // `__attribute__((...))` and friends are annotations, not macros: they are
    // consumed by `skipAnnotations`, so they never reach the specifier list.
    if (word == "__attribute__" || word == "__attribute" || word == "__declspec" ||
        word == "__extension__" || word == "_CRTIMP") {
      return false;
    }
    int depth = 0;
    for (std::size_t a = 1;; ++a) {
      const Token& t = peek(a);
      if (t.kind == TokKind::Eof) return false;
      if (t.kind == TokKind::Punct && t.text == "(") {
        ++depth;
        continue;
      }
      if (t.kind == TokKind::Punct && t.text == ")") {
        if (--depth == 0) return peek(a + 1).kind == TokKind::Ident;
        continue;
      }
    }
  }

  // Reads a declaration specifier list: qualifiers/storage are consumed and
  // dropped, and the base type word is recorded.
  Spec parseSpecifiers() {
    Spec spec;
    for (;;) {
      skipAnnotations();
      const Token& t = peek();
      if (t.kind != TokKind::Ident) break;
      if (isIgnoredSpecifier(t.text)) {
        advance();
        continue;
      }
      if (t.text == "signed") {
        spec.isSigned = true;
        advance();
        continue;
      }
      if (t.text == "unsigned") {
        spec.isUnsigned = true;
        advance();
        continue;
      }
      if (t.text == "short") {
        spec.shortCount++;
        advance();
        continue;
      }
      if (t.text == "long") {
        spec.longCount++;
        advance();
        continue;
      }
      if (t.text == "struct" || t.text == "union" || t.text == "enum") {
        const std::string tagKeyword = t.text;
        advance();
        std::string tag;
        if (check(TokKind::Ident)) tag = advance().text;
        if (tagKeyword == "enum") {
          // A C enum is an `int` at the ABI, so the declaration maps to `int32`
          // and its members become module constants (BACKLOG §7.10).
          spec.isEnumTag = true;
          spec.word = tag.empty() ? "enum" : "enum " + tag;
          if (checkPunct("{")) parseEnumBody();
          continue;
        }
        spec.isStructTag = true;
        spec.word = tagKeyword + (tag.empty() ? "" : " " + tag);
        if (checkPunct("{")) {
          // A definition: read the body so the layout is known. A union is a
          // memory layout with overlapping members, which Oskel's `struct` (a
          // map of whole scalars) cannot express, so it is recorded as a
          // problem rather than silently mis-laid-out.
          if (tagKeyword == "union") {
            skipBraces();
            spec.hasStructBody = true;
            spec.structProblem =
                "a C union is a memory layout with overlapping members, which the Oskel "
                "`struct` (a map of whole scalars) cannot express";
          } else {
            parseStructBody(spec.structFields, spec.structProblem);
            spec.hasStructBody = true;
          }
          // Recorded even with a problem: a later bare reference must report the
          // real reason instead of "definition not in this header".
          recordStruct(tag, spec.structFields, spec.structProblem);
        } else if (!tag.empty()) {
          // A bare `struct X` reference: reuse a body read earlier in the file,
          // so a declaration that follows its definition still resolves.
          auto known = structTags_.find(tag);
          if (known != structTags_.end()) {
            spec.hasStructBody = true;
            spec.structFields = known->second.fields;
            spec.structProblem = known->second.problem;
          }
        }
        continue;
      }
      if (spec.word.empty()) {
        const bool haveModifiers = spec.isUnsigned || spec.isSigned || spec.longCount > 0 ||
                                   spec.shortCount > 0;
        if (!isKnownTypeWord(t.text)) {
          // A leading unknown word followed by a real type is a macro, not a
          // type: drop it and keep reading the specifiers.
          if (!haveModifiers && looksLikeMacroPrefix()) {
            advance();
            continue;
          }
          // After a modifier, a word that is not a type name is usually the
          // declarator itself: `unsigned len` is `unsigned int len`, not a
          // type `len`. But `unsigned __int64 x` (a spelling the vocabulary
          // does not know) has the unknown word as the *type*, told apart by a
          // declarator or a `*` right after it. Record it so the type check
          // reports it, instead of dropping the whole declaration in silence.
          if (haveModifiers && (peek(1).kind == TokKind::Ident ||
                                (peek(1).kind == TokKind::Punct && peek(1).text == "*"))) {
            spec.word = t.text;
            advance();
            continue;
          }
          if (haveModifiers) break;
        }
        spec.word = t.text;
        advance();
        continue;
      }
      break;
    }
    return spec;
  }

  // What the header said about a `struct`/`union` tag. A body with a field the
  // vocabulary cannot express is recorded with its problem, so a later bare
  // reference reports the real reason instead of "definition not in this
  // header".
  struct StructInfo {
    std::vector<ffi::FfiField> fields;
    std::string problem;
    bool defined = false;
  };

  // One member of a C `enum`, emitted as a module constant.
  struct EnumConstant {
    std::string name;
    long long value = 0;
  };

  // The C tag of a `struct`/`union` specifier (`struct Point` -> "Point"), or
  // "" for an anonymous body.
  static std::string structNameOf(const Spec& spec) {
    static const std::string keyword = "struct ";
    if (spec.word.compare(0, keyword.size(), keyword) == 0) {
      return spec.word.substr(keyword.size());
    }
    return "";  // a bare `struct` (an anonymous body)
  }

  // Records a struct's layout once, so a later bare `struct X` reference reuses
  // it and the emitted module declares it exactly once. Only a struct that can
  // actually be declared is queued for emission.
  void recordStruct(const std::string& name, const std::vector<ffi::FfiField>& fields,
                    const std::string& problem) {
    if (name.empty()) return;
    if (structTags_.count(name) != 0) return;
    StructInfo info;
    info.fields = fields;
    info.problem = problem;
    info.defined = true;
    // The layout is the same one the runtime builds, so a body it rejects (a
    // nested struct, an empty struct) is never emitted as a declarable type.
    if (info.problem.empty()) {
      ffi::StructLayout layout;
      std::string reason;
      if (!ffi::buildStructLayout(fields, layout, reason)) info.problem = reason;
    }
    const bool declarable = info.problem.empty();
    structTags_[name] = std::move(info);
    if (declarable) structOrder_.push_back(name);
  }

  // Reads a `{ field; ... }` body into the FFI field list. The caller has not
  // consumed the `{`. A field the vocabulary cannot express (a nested struct, an
  // array, a bitfield) makes the whole struct unbindable: the layout would be
  // wrong, so the declaration must be reported instead of mistranslated.
  void parseStructBody(std::vector<ffi::FfiField>& out, std::string& problem) {
    advance();  // `{`
    while (!check(TokKind::Eof) && !checkPunct("}")) {
      if (checkPunct(";")) {
        advance();
        continue;
      }
      Base fieldType = parsePointers(baseFromSpec(parseSpecifiers()));
      std::string name;
      if (check(TokKind::Ident)) name = advance().text;
      // An array member (`char name[32]`) is a buffer, not a scalar: the layout
      // is not expressible.
      if (checkPunct("[")) {
        skipBrackets();
        if (problem.empty()) {
          problem = "an array field ('" + name +
                    "') is not expressible in the Oskel `struct`, whose fields are whole scalars";
        }
      }
      // A bitfield (`int a : 3`) is an intra-struct packing detail.
      if (checkPunct(":")) {
        while (!check(TokKind::Eof) && !checkPunct(";") && !checkPunct(",")) advance();
        if (problem.empty()) {
          problem = "a bitfield ('" + name +
                    "') is not expressible in the Oskel `struct`, whose fields are whole scalars "
                    "at natural alignment";
        }
      }
      // Multiple declarators (`int a, b;`) share the base type.
      for (;;) {
        if (!fieldType.ok && problem.empty()) {
          problem = "field '" + name + "': " + quoteReason(fieldType);
        } else if (fieldType.ok && problem.empty()) {
          ffi::FfiField field;
          field.name = name;
          field.type = fieldType.oskel;
          out.push_back(std::move(field));
        }
        if (matchPunct(",")) {
          if (check(TokKind::Ident)) name = advance().text;
          continue;
        }
        break;
      }
      if (!matchPunct(";")) {
        // A malformed body: stop at the closing brace rather than run away.
        while (!check(TokKind::Eof) && !checkPunct(";") && !checkPunct("}")) advance();
        matchPunct(";");
      }
    }
    matchPunct("}");
  }

  // Reads `{ NAME [= value], ... }` and records each member as a module
  // constant. The caller has not consumed the `{`.
  void parseEnumBody() {
    advance();  // `{`
    long long next = 0;
    while (!check(TokKind::Eof) && !checkPunct("}")) {
      if (!check(TokKind::Ident)) {
        advance();
        continue;
      }
      const std::string name = advance().text;
      long long value = next;
      if (matchPunct("=")) {
        // The value is an integer constant expression; only a plain decimal,
        // hex or octal literal is evaluated, which is what a real header uses.
        // Anything else is left to the next member's implicit value.
        bool negate = matchPunct("-");
        if (check(TokKind::Number)) {
          const std::string text = advance().text;
          char* end = nullptr;
          const long long parsed = std::strtoll(text.c_str(), &end, 0);
          if (end != nullptr && *end == '\0') value = negate ? -parsed : parsed;
        } else {
          while (!check(TokKind::Eof) && !checkPunct(",") && !checkPunct("}")) advance();
        }
      }
      next = value + 1;
      enumConstants_.push_back({name, value});
      if (!matchPunct(",")) break;
    }
    matchPunct("}");
  }

  // The C `long` family is the one integer whose width depends on the data
  // model: LP64 has a 64-bit `long`, LLP64 a 32-bit one. `long long` is 64-bit
  // in both.
  std::string longOskel(bool isUnsigned, int longCount, std::string& spelling) const {
    const bool isLongLong = longCount >= 2;
    const bool wide = isLongLong || options_.target == bindgen::Target::Lp64;
    spelling = std::string(isUnsigned ? "unsigned " : "") + (isLongLong ? "long long" : "long");
    if (wide) return isUnsigned ? "uint64" : "int64";
    return isUnsigned ? "uint32" : "int32";
  }

  // Turns a specifier list into a `Base`.
  Base baseFromSpec(const Spec& spec) {
    Base base;
    base.spelling = spec.word;
    if (spec.isEnumTag) {
      // An enum is an `int` at the ABI (BACKLOG §7.10), so `int32` is the exact
      // spelling; the members travel as module constants.
      base.kind = Base::Kind::Int;
      base.oskel = "int32";
      base.spelling = spec.word;
      return base;
    }
    if (spec.isStructTag) {
      base.kind = Base::Kind::Struct;
      base.spelling = spec.word;
      if (!spec.structProblem.empty()) {
        base.ok = false;
        base.reason = "'" + spec.word + "': " + spec.structProblem;
        return base;
      }
      if (!spec.hasStructBody) {
        base.ok = false;
        base.reason = "'" + spec.word +
                      "' is used by value but its definition is not in this header, so its "
                      "layout is unknown";
        return base;
      }
      // The Oskel `struct` name: the C tag. An anonymous body
      // (`typedef struct { ... } div_t`) has no tag, so the typedef declarator
      // names it a moment later (`parseTypedef`); until then it is marked
      // anonymous so a use without an alias can be reported instead of emitted.
      const std::string name = structNameOf(spec);
      if (name.empty()) {
        base.ok = true;
        base.anonymousStruct = true;
        base.fields = spec.structFields;
        return base;
      }
      base.ok = true;
      base.oskel = name;
      base.fields = spec.structFields;
      recordStruct(name, spec.structFields, spec.structProblem);
      return base;
    }
    const std::string& w = spec.word;
    if (w.empty()) {
      // C's implicit `int`, possibly spelled only with the modifiers
      // (`unsigned`, `long`, `unsigned long`).
      base.kind = Base::Kind::Int;
      if (spec.shortCount > 0) {
        base.oskel = spec.isUnsigned ? "uint16" : "int16";
        base.spelling = "short";
      } else if (spec.longCount > 0) {
        base.oskel = longOskel(spec.isUnsigned, spec.longCount, base.spelling);
      } else {
        base.oskel = spec.isUnsigned ? "uint32" : "int32";
        base.spelling = "int";
      }
      return base;
    }
    if (w == "void") {
      base.kind = Base::Kind::Void;
      base.oskel = "void";
      return base;
    }
    if (w == "char") {
      base.kind = Base::Kind::Char;
      base.plainChar = !spec.isSigned && !spec.isUnsigned;
      base.oskel = spec.isUnsigned ? "uint8" : "int8";
      base.spelling = spec.isUnsigned ? "unsigned char"
                                      : (spec.isSigned ? "signed char" : "char");
      return base;
    }
    if (w == "int") {
      base.kind = Base::Kind::Int;
      if (spec.shortCount > 0) {
        base.oskel = spec.isUnsigned ? "uint16" : "int16";
        base.spelling = std::string(spec.isUnsigned ? "unsigned " : "") + "short";
      } else if (spec.longCount > 0) {
        base.oskel = longOskel(spec.isUnsigned, spec.longCount, base.spelling);
      } else {
        base.oskel = spec.isUnsigned ? "uint32" : "int32";
        base.spelling = std::string(spec.isUnsigned ? "unsigned " : "") + "int";
      }
      return base;
    }
    if (w == "float") {
      base.kind = Base::Kind::Float;
      base.oskel = "float";
      return base;
    }
    if (w == "double") {
      if (spec.longCount > 0) {
        base.ok = false;
        base.reason = "'long double' is not in the FFI vocabulary";
        return base;
      }
      base.kind = Base::Kind::Double;
      base.oskel = "double";
      return base;
    }
    if (w == "bool" || w == "_Bool") {
      base.kind = Base::Kind::Bool;
      base.oskel = "bool";
      return base;
    }
    // A typedef name: first the ones this header declared, then the well-known
    // `stdint.h`/`stddef.h` names, then the Oskel vocabulary itself.
    auto alias = typedefs_.find(w);
    if (alias != typedefs_.end()) return alias->second;
    if (w == "wchar_t") {
      // `wchar_t` is a header typedef on every real platform, but the tool
      // recognizes it without reading `<stddef.h>`. Its width follows the C
      // data model: 32-bit on LP64, 16-bit on Windows.
      base.kind = Base::Kind::Int;
      base.oskel = options_.target == bindgen::Target::Llp64 ? "uint16" : "int32";
      base.spelling = "wchar_t";
      return base;
    }
    if (resolveWellKnown(w, base)) return base;
    if (knownFfiName(w)) {
      base.kind = (w == "float")   ? Base::Kind::Float
                  : (w == "double") ? Base::Kind::Double
                  : (w == "bool")   ? Base::Kind::Bool
                                    : Base::Kind::Int;
      base.oskel = w;
      return base;
    }
    base.ok = false;
    base.reason = "'" + w + "' is not a known type";
    return base;
  }

  // Reads `*` (and the qualifiers around them) and applies pointer-ness. The
  // caller has already consumed the base specifiers.
  Base parsePointers(Base base) {
    int stars = 0;
    for (;;) {
      skipAnnotations();
      if (checkIdent("const") || checkIdent("volatile") || checkIdent("restrict") ||
          checkIdent("__restrict")) {
        advance();
        continue;
      }
      if (matchPunct("*")) {
        ++stars;
        continue;
      }
      break;
    }
    if (stars == 0) return base;
    // A pointer *to* a callback (`cb_t *pcb`) is not a callback: the FFI
    // carries it as an opaque handle, so the callback spelling must not leak
    // into the emitted type.
    if (base.isCallback) {
      base.isCallback = false;
      base.callbackParams.clear();
      base.callbackReturn.clear();
    }
    if (base.kind == Base::Kind::Void) {
      base.kind = Base::Kind::Unknown;
      base.ok = true;
      base.oskel = "ptr";
      base.spelling = "void *";
      return base;
    }
    if (base.kind == Base::Kind::Char && base.plainChar && stars == 1) {
      base.kind = Base::Kind::Unknown;
      base.ok = true;
      base.oskel = "cstring";
      base.spelling = "char *";
      return base;
    }
    // A pointer to an explicitly signed/unsigned `char` is a byte buffer, not a
    // NUL-terminated string, so it falls through to the opaque `ptr` below.
    // Any other pointer (including a multi-level one) is an opaque handle.
    base.kind = Base::Kind::Unknown;
    base.ok = true;
    base.oskel = "ptr";
    base.spelling += " *";
    return base;
  }

  // Whether the `(` at the cursor opens a function-pointer declarator
  // (`( * name )`), told apart from another parenthesized declarator by a `*`
  // before the closing `)`.
  bool looksLikeFunctionPointer() const {
    if (!checkPunct("(")) return false;
    for (std::size_t a = 1;; ++a) {
      const Token& t = peek(a);
      if (t.kind == TokKind::Eof) return false;
      if (t.kind == TokKind::Punct && t.text == ")") return false;
      if (t.kind == TokKind::Punct && t.text == "*") return true;
    }
  }

  // Starts a callback from its already-parsed return type, applying the FFI
  // rules the engine enforces: `void` and the scalar returns are accepted, a
  // struct-by-value or a callback return is not.
  Base callbackFromReturn(const Base& returnBase) {
    Base cb;
    cb.isCallback = true;
    if (!returnBase.ok) {
      cb.ok = false;
      cb.reason = returnBase.reason;
    } else if (returnBase.isCallback) {
      cb.ok = false;
      cb.reason = "a callback cannot return a callback";
    } else if (returnBase.kind == Base::Kind::Void) {
      cb.callbackReturn = "void";
    } else if (returnBase.kind == Base::Kind::Struct) {
      cb.ok = false;
      cb.reason = "a callback returning a struct by value is not generated yet";
    } else {
      cb.callbackReturn = returnBase.oskel.empty() ? "int32" : returnBase.oskel;
    }
    return cb;
  }

  // Parses a callback's parameter list `( P, P, ... )`, filling `cb`. The
  // engine's callback signature carries only scalar parameters: a nested
  // callback, a struct by value or a C variadic is marked not-ok with a reason.
  void parseCallbackParams(Base& cb) {
    if (!checkPunct("(")) return;
    advance();  // the parameter list
    if (checkIdent("void") && peek(1).kind == TokKind::Punct && peek(1).text == ")") {
      advance();  // `(void)` means no parameters
    } else if (!checkPunct(")")) {
      for (;;) {
        if (check(TokKind::Ellipsis)) {
          if (cb.ok) {
            cb.ok = false;
            cb.reason = "a C variadic callback is not expressible in the FFI vocabulary";
          }
          advance();
          break;
        }
        Base p = parsePointers(baseFromSpec(parseSpecifiers()));
        if (checkPunct("(")) {
          if (cb.ok) {
            cb.ok = false;
            cb.reason =
                "a callback that takes a callback is not expressible in the FFI vocabulary";
          }
          skipParens();
        } else if (checkPunct("[")) {
          while (checkPunct("[")) skipBrackets();
          p.ok = true;
          p.kind = Base::Kind::Unknown;
          p.oskel = "ptr";
        }
        if (check(TokKind::Ident)) advance();  // an optional parameter name
        if (!p.ok && cb.ok) {
          cb.ok = false;
          cb.reason = p.reason.empty()
                          ? std::string("a callback parameter type is not expressible in the FFI vocabulary")
                          : p.reason;
        }
        if (cb.ok) cb.callbackParams.push_back(p.oskel.empty() ? "int32" : p.oskel);
        if (!matchPunct(",")) break;
      }
    }
    if (checkPunct(")")) advance();
    if (cb.ok) cb.spelling = callbackSpelling(cb);
  }

  // Parses a C function-pointer declarator `( * name? ) ( params )` into a
  // callback `Base` whose Oskel spelling is `fn(P...): R`. `returnBase` is the
  // already-parsed return type; the cursor is on the `(` that opens the
  // declarator, and `name` receives the declarator's name (empty for an
  // abstract declarator).
  //
  // Exactly one `*` with no array suffix is a function pointer, which the FFI
  // takes as a callback. More than one `*` (`int (**pp)(int)`) or an array of
  // function pointers (`int (*arr[4])(int)`) is a *pointer to* a function
  // pointer, which the vocabulary passes as an opaque `ptr`, so it is returned
  // as one.
  Base parseCallbackDeclarator(const Base& returnBase, std::string& name) {
    int stars = 0;
    advance();  // `(`
    for (;;) {
      if (matchPunct("*")) {
        ++stars;
        continue;
      }
      if (checkIdent("const") || checkIdent("volatile") || checkIdent("restrict") ||
          (check(TokKind::Ident) && isCallingConvention(peek().text))) {
        advance();
        continue;
      }
      break;
    }
    if (check(TokKind::Ident)) name = advance().text;
    bool isArray = false;
    while (checkPunct("[")) {
      skipBrackets();
      isArray = true;
    }
    if (checkPunct(")")) advance();
    if (stars != 1 || isArray) {
      Base ptr;
      ptr.kind = Base::Kind::Unknown;
      ptr.ok = true;
      ptr.oskel = "ptr";
      ptr.spelling = "void *";
      return ptr;
    }
    Base cb = callbackFromReturn(returnBase);
    parseCallbackParams(cb);
    return cb;
  }

  // ---------------------------------------------------------------- typedef

  void parseTypedef() {
    advance();  // `typedef`
    Spec spec = parseSpecifiers();
    Base base = baseFromSpec(spec);
    // One or more declarators: `typedef int a, *b, c[4];`.
    for (;;) {
      Base declarator = base;
      std::string name;
      if (checkPunct("(") && looksLikeFunctionPointer()) {
        // A function-pointer typedef: `typedef int (*cb)(int);`. The name sits
        // inside the parentheses (`(*cb)`); the alias is recorded as a callback
        // so a parameter typed with it becomes `fn(P...): R`.
        declarator = parseCallbackDeclarator(base, name);
      } else if (checkPunct("(")) {
        // Another parenthesized declarator: not a shape the tool binds.
        advance();  // `(`
        while (matchPunct("*")) {
        }
        if (check(TokKind::Ident)) name = advance().text;
        skipParens();
        if (checkPunct("(")) skipParens();  // the parameter list
        declarator.ok = false;
        declarator.kind = Base::Kind::Unknown;
        declarator.reason =
            (name.empty() ? std::string("a parenthesized typedef")
                          : "'" + name + "'") +
            " is not expressible in the FFI vocabulary";
      } else {
        declarator = parsePointers(base);
        if (check(TokKind::Ident)) name = advance().text;
        // An anonymous struct body (`typedef struct { ... } div_t;`) is named by
        // this declarator: without an alias the shape could not be spelled in
        // Oskel at all.
        if (declarator.anonymousStruct && !name.empty()) {
          declarator.anonymousStruct = false;
          declarator.oskel = name;
          declarator.spelling = "struct " + name;
          recordStruct(name, declarator.fields, "");
        }
        // An array suffix decays to a pointer.
        while (checkPunct("[")) {
          skipBrackets();
          declarator.ok = true;
          declarator.kind = Base::Kind::Unknown;
          declarator.oskel = "ptr";
        }
      }
      if (!name.empty()) typedefs_[name] = declarator;
      if (matchPunct(",")) continue;
      break;
    }
    skipToSemicolon();
  }

  void skipBrackets() {
    if (!matchPunct("[")) return;
    int depth = 1;
    while (!check(TokKind::Eof) && depth > 0) {
      if (checkPunct("[")) ++depth;
      if (checkPunct("]")) --depth;
      advance();
    }
  }

  // The declared name, best-effort, for a skip message: the last identifier on
  // the declaration's first line that is neither a keyword nor a type word
  // (`Foo (*fp)(void);` reports `fp`, not `void`). A declarator the tool cannot
  // parse still has to be reported under *some* name.
  std::string declaratorName() const {
    const int line = peek().kind == TokKind::Eof ? 0 : peek().line;
    std::string name;
    bool sawStar = false;
    for (std::size_t a = 0;; ++a) {
      const Token& t = peek(a);
      if (t.kind == TokKind::Eof) break;
      if (t.line != line) break;
      if (t.kind == TokKind::Punct && t.text == ";") break;
      if (t.kind == TokKind::Punct && t.text == "*") {
        sawStar = true;
        continue;
      }
      if (t.kind != TokKind::Ident) continue;
      if (isIgnoredSpecifier(t.text) || t.text == "void" || isKnownTypeWord(t.text)) continue;
      // Inside a parenthesized declarator the first identifier after the `*` is
      // the declarator's name.
      name = t.text;
      if (sawStar) break;
    }
    return name.empty() ? std::string("(unnamed)") : name;
  }

  void skipToSemicolon() {
    int parenDepth = 0;
    int braceDepth = 0;
    while (!check(TokKind::Eof)) {
      if (checkPunct("{")) ++braceDepth;
      if (checkPunct("}")) {
        if (braceDepth == 0) return;
        --braceDepth;
      }
      if (checkPunct("(")) ++parenDepth;
      if (checkPunct(")")) {
        if (parenDepth > 0) --parenDepth;
      }
      if (checkPunct(";") && parenDepth == 0 && braceDepth == 0) {
        advance();
        return;
      }
      advance();
    }
  }

  // ------------------------------------------------------ tag declarations

  // `struct X { ... };`, `struct X;`, `enum E { ... };`, `union U {...};` are
  // consumed by `parseSpecifiers` (which reads the tag and any definition), so
  // a bare tag declaration falls out of `parseFunctionOrVariable` as a
  // non-binding and is skipped there.

  // ------------------------------------------------ function declarations

  void parseFunctionOrVariable() {
    // A function-like macro prefix is undecidable without a preprocessor: the
    // argument may be the return type (`MY_API(int) f(void);`). Report it
    // instead of guessing or dropping the declaration in silence.
    if (check(TokKind::Ident) && looksLikeFunctionLikeMacro(peek().text)) {
      const std::string macro = peek().text;
      skipToSemicolon();
      skipped_.push_back("(declaration prefixed by '" + macro +
                         "(...)', which the tool cannot expand; preprocess the header)");
      return;
    }
    Spec spec = parseSpecifiers();
    Base base = baseFromSpec(spec);
    Base returnType = parsePointers(base);
    if (returnType.anonymousStruct) {
      returnType.ok = false;
      returnType.reason = "an anonymous 'struct' has no name to spell in a signature";
    }

    // A declaration that is not a plain `name(` is a variable, a function
    // pointer, or something else slice 1 does not bind: skip it. A *declaration*
    // whose type the tool could not resolve is still reported, never dropped in
    // silence -- that is the defect this tool exists not to have.
    if (!check(TokKind::Ident)) {
      if (!returnType.ok && !spec.isStructTag) {
        std::string name = declaratorName();
        skipToSemicolon();
        skipped_.push_back(name + ": return type " + quoteReason(returnType));
        return;
      }
      // A `(` here opens a parenthesized declarator. Two shapes matter: a
      // function-pointer *variable* (`int (*fp)(void);`) is not a binding and
      // is skipped in silence, while a *function returning* a function pointer
      // (`int (*signal(int, void (*)(int)))(int);`) is a declaration the FFI
      // vocabulary cannot express and must be reported, never dropped in
      // silence. They are told apart by what follows the name inside the
      // parentheses: a `(` begins a parameter list (a function), a `)` closes
      // the declarator (a variable).
      if (checkPunct("(")) {
        std::size_t ahead = 1;
        while (peek(ahead).kind == TokKind::Punct && peek(ahead).text == "*") ++ahead;
        if (peek(ahead).kind == TokKind::Ident &&
            peek(ahead + 1).kind == TokKind::Punct && peek(ahead + 1).text == "(") {
          const std::string innerName = peek(ahead).text;
          skipToSemicolon();
          skipped_.push_back(innerName +
                             ": returns a callback, which the FFI vocabulary does "
                             "not accept as a return type");
          return;
        }
      }
      skipToSemicolon();
      return;
    }
    const std::string name = peek().text;
    const Token nameToken = peek();
    advance();  // the name

    if (!checkPunct("(")) {
      // A variable declaration (possibly an array): not a binding.
      skipToSemicolon();
      return;
    }

    std::vector<Param> params;
    bool variadic = false;
    std::string paramProblem;
    parseParamList(params, variadic, paramProblem);

    // A trailing `__attribute__((...))` may sit between the parameter list and
    // the body or the `;`.
    skipAnnotations();
    if (checkPunct("{")) {
      // A function *definition* (`static inline ... { ... }`): it has a body,
      // so it is not an exported symbol the FFI can bind. Skip it.
      skipBraces();
      return;
    }
    skipToSemicolon();

    if (isOskelKeyword(name)) {
      skipped_.push_back(name + ": '" + name +
                         "' is a reserved Oskel keyword, so it cannot be a binding name");
      return;
    }
    if (!returnType.ok) {
      skipped_.push_back(name + ": return type " + quoteReason(returnType));
      return;
    }
    if (returnType.isCallback) {
      skipped_.push_back(name +
                         ": returns a callback, which the FFI vocabulary does not "
                         "accept as a return type");
      return;
    }
    if (!paramProblem.empty()) {
      skipped_.push_back(name + ": " + paramProblem);
      return;
    }
    // Whether the signature can cross the C boundary is the compiler's call, so
    // the tool asks `ffi::validateSignature` instead of re-implementing the
    // rules (the register budget counts eightbytes, not parameters, and a
    // struct's availability depends on the target ABI). Asking twice is what
    // separates the two outcomes: a shape the *vocabulary* cannot express is
    // skipped with its reason, while one this *build* cannot marshal is written
    // commented out, so the module compiles here and the real signature stays
    // there to uncomment on a build that supports it.
    std::vector<ffi::TypeSpec> paramSpecs;
    paramSpecs.reserve(params.size());
    for (const Param& param : params) paramSpecs.push_back(param.spec);
    const ffi::TypeSpec returnSpec = specOf(returnType, "void");
    const std::string vocabularyProblem =
        ffi::validateSignature(paramSpecs, returnSpec, variadic, ffi::kFullAbiCapabilities);
    if (!vocabularyProblem.empty()) {
      skipped_.push_back(name + ": " + vocabularyProblem);
      return;
    }

    Function fn;
    fn.name = name;
    fn.params = std::move(params);
    fn.variadic = variadic;
    fn.returnType = returnType.oskel.empty() ? "void" : returnType.oskel;
    fn.line = nameToken.line;
    fn.commentedNote = ffi::validateSignature(paramSpecs, returnSpec, variadic);

    auto existing = emittedNames_.find(name);
    if (existing != emittedNames_.end()) {
      skipped_.push_back(name + ": already declared above");
      return;
    }
    emittedNames_.insert(name);
    if (!fn.commentedNote.empty()) ++commentedDeclarations_;
    functions_.push_back(std::move(fn));
  }

  struct Param {
    std::string name;
    std::string type;
    // The same type as the compiler's FFI sees it, so a signature is checked
    // by `ffi::validateSignature` instead of by rules the tool re-implements.
    ffi::TypeSpec spec;
  };
  struct Function {
    std::string name;
    std::vector<Param> params;
    bool variadic = false;
    std::string returnType;
    int line = 0;
    // Empty for a declaration this build can marshal. Otherwise the reason it
    // is written commented out: the vocabulary can express it, this build
    // cannot (a variadic, a MEMORY struct, a struct by value on a non-System V
    // ABI), so the real signature is there to uncomment on a build that can.
    std::string commentedNote;
  };

  void parseParamList(std::vector<Param>& out, bool& variadic, std::string& problem) {
    advance();  // `(`
    if (checkPunct(")")) {
      advance();
      return;
    }
    // `(void)` means no parameters.
    if (checkIdent("void") && peek(1).kind == TokKind::Punct && peek(1).text == ")") {
      advance();
      advance();
      return;
    }
    int index = 0;
    for (;;) {
      if (check(TokKind::Ellipsis)) {
        advance();
        variadic = true;
        break;
      }
      Param param;
      Base base = parsePointers(baseFromSpec(parseSpecifiers()));
      bool callbackDeclarator = false;
      if (checkPunct("(") && looksLikeFunctionPointer()) {
        // A function-pointer parameter: a callback the engine accepts as
        // `fn(P...): R` (PHASE 6.6).
        std::string cbName;
        base = parseCallbackDeclarator(base, cbName);
        callbackDeclarator = true;
        if (!base.ok && problem.empty()) {
          problem = base.reason.empty() ? std::string("a callback parameter is not expressible in the FFI vocabulary")
                                        : base.reason;
        }
        if (!cbName.empty()) param.name = cbName;
      } else if (checkPunct("(")) {
        // A parenthesized declarator without a `*` is a function *type* whose
        // parentheses are redundant: `int (cb)(int)` is `int cb(int)`, which C
        // adjusts to a function pointer exactly like the unparenthesized form.
        std::size_t ahead = 1;
        while (peek(ahead).kind == TokKind::Ident &&
               (peek(ahead).text == "const" || peek(ahead).text == "volatile" ||
                peek(ahead).text == "restrict")) {
          ++ahead;
        }
        const bool namedFunctionType =
            peek(ahead).kind == TokKind::Ident &&
            peek(ahead + 1).kind == TokKind::Punct && peek(ahead + 1).text == ")" &&
            peek(ahead + 2).kind == TokKind::Punct && peek(ahead + 2).text == "(";
        if (namedFunctionType) {
          advance();  // `(`
          param.name = advance().text;
          advance();  // `)`
          Base cb = callbackFromReturn(base);
          parseCallbackParams(cb);
          base = cb;
          callbackDeclarator = true;
          if (!base.ok && problem.empty()) problem = base.reason;
        } else {
          // Another parenthesized declarator: not a shape the tool binds.
          if (problem.empty()) problem = "a parenthesized declarator is not generated yet";
          skipParens();
        }
      }
      if (!callbackDeclarator) {
        if (check(TokKind::Ident)) param.name = advance().text;
        // A *function-type* parameter, `void takes(int foo(int));`, is adjusted
        // by C to a function pointer: it is the same callback shape with the
        // name before the parameter list.
        if (checkPunct("(") && !base.isCallback) {
          Base cb = callbackFromReturn(base);
          parseCallbackParams(cb);
          base = cb;
          callbackDeclarator = true;
          if (!base.ok && problem.empty()) problem = base.reason;
        }
        // An array parameter decays to a pointer: `int values[]` is `int *`,
        // and an array of callbacks decays to a pointer to a callback, which is
        // an opaque handle, not a callback.
        if (checkPunct("[")) {
          while (checkPunct("[")) skipBrackets();
          base.ok = true;
          base.kind = Base::Kind::Unknown;
          base.oskel = "ptr";
          base.isCallback = false;
          base.callbackParams.clear();
          base.callbackReturn.clear();
        }
      }
      if (base.anonymousStruct) {
        // `struct { int x; }` inline has no name, so a signature has nothing to
        // spell; emitting `int32` (the empty-`oskel` fallback) would silently
        // pass the struct by value as a scalar.
        base.ok = false;
        base.reason = "an anonymous 'struct' has no name to spell in a signature";
      }
      if (!base.ok && problem.empty()) {
        problem = "parameter type " + quoteReason(base);
      }
      if (param.name.empty()) param.name = "a" + std::to_string(index);
      param.type = base.isCallback
                       ? callbackSpelling(base)
                       : (base.oskel.empty() ? "int32" : base.oskel);
      param.spec = specOf(base, "int32");
      out.push_back(std::move(param));
      ++index;
      if (matchPunct(",")) continue;
      break;
    }
    if (checkPunct(")")) advance();
  }

  // ------------------------------------------------------------------ output

  void emitHeader() {
    std::string& text = result_.text;
    text += "// Generated by bindgen from ";
    text += sourceName_;
    text += ". Do not edit by hand.\n";
    text += "//\n";
    text += "// C declarations translated to the Oskel FFI vocabulary (PHASE 6.6).\n";
    text += "// A declaration the vocabulary cannot express is listed as a\n";
    text += "// '// skipped:' comment instead of being mistranslated.\n";
    text += "//\n";
    text += "// The AOT backend resolves these symbols in hosted mode\n";
    text += "// (`oskel build --mode hosted`); the development engine resolves them\n";
    text += "// through `Platform::lookupSymbol`.\n";
    text += "\n";

    // Structs first: a signature may spell one by value, so the declaration has
    // to be in scope before the `extern "C"` block reads it.
    for (const std::string& name : structOrder_) {
      const std::vector<ffi::FfiField>& fields = structTags_[name].fields;
      text += "struct ";
      text += name;
      text += " {\n";
      for (const ffi::FfiField& field : fields) {
        text += "  ";
        text += field.name;
        text += ": ";
        text += field.type;
        text += ";\n";
      }
      text += "}\n\n";
    }

    // Object-like macros are constants too, and the preprocessor runs before
    // the declarations; emit them first. A name that is already a function,
    // struct or the other kind of constant keeps the declaration: the header
    // would be ambiguous, and a duplicate `let` would not compile.
    int macroConstants = 0;
    for (const MacroConstant& constant : macroConstants_) {
      if (isOskelKeyword(constant.name)) continue;
      if (!emittedNames_.insert(constant.name).second) continue;
      text += "let ";
      text += constant.name;
      text += " = ";
      text += constant.value;
      text += ";\n";
      ++macroConstants;
    }
    if (macroConstants > 0) text += "\n";

    // An enum is an `int` at the ABI; its members are module constants so the
    // program can name them.
    int enumConstants = 0;
    for (const auto& constant : enumConstants_) {
      if (isOskelKeyword(constant.name)) continue;
      if (!emittedNames_.insert(constant.name).second) continue;
      text += "let ";
      text += constant.name;
      text += " = ";
      text += std::to_string(constant.value);
      text += ";\n";
      ++enumConstants;
    }
    if (enumConstants > 0) text += "\n";

    text += "extern \"C\" {\n";
    for (const Function& fn : functions_) {
      // A declaration this build cannot marshal is written commented out: the
      // compiler rejects it at compile time (a variadic needs the x86-64
      // descriptor stub; a struct by value needs the System V AMD64 ABI), so
      // commenting it keeps the module compiling here while the real signature
      // stays there to uncomment on a build that supports it. The note is the
      // compiler's own reason, not a second guess.
      const bool isCommented = !fn.commentedNote.empty();
      text += isCommented ? "  // " : "  ";
      text += "fn ";
      text += fn.name;
      text += "(";
      for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i != 0) text += ", ";
        text += fn.params[i].name;
        text += ": ";
        text += fn.params[i].type;
      }
      if (fn.variadic) {
        if (!fn.params.empty()) text += ", ";
        text += "...";
      }
      text += "): ";
      text += fn.returnType;
      text += ";";
      if (isCommented) {
        text += "  // " + fn.commentedNote;
      }
      text += "\n";
    }
    for (const std::string& note : skipped_) {
      text += "  // skipped: ";
      text += note;
      text += "\n";
    }
    text += "}\n";

    result_.emitted = static_cast<int>(functions_.size()) - commentedDeclarations_;
    result_.skipped = static_cast<int>(skipped_.size());
    result_.commented = commentedDeclarations_;
    result_.structs = static_cast<int>(structOrder_.size());
    result_.constants = enumConstants;
    result_.macroConstants = macroConstants;
  }

  static std::string quoteReason(const Base& base) {
    return base.reason.empty() ? std::string("is not expressible in the FFI vocabulary")
                               : base.reason;
  }

  static bool isOskelKeyword(const std::string& name) {
    static const std::unordered_set<std::string> keywords = {
        "let", "var", "const", "fn", "if", "else", "while", "do", "for", "in",
        "break", "continue", "return", "import", "switch", "case", "default",
        "defer", "struct", "native", "extern", "try", "catch", "true", "false",
        "nil"};
    return keywords.count(name) != 0;
  }

  std::vector<Token> tokens_;
  std::vector<MacroConstant> macroConstants_;
  std::string sourceName_;
  bindgen::Options options_;
  std::size_t pos_ = 0;
  std::unordered_map<std::string, Base> typedefs_;
  std::vector<Function> functions_;
  std::vector<std::string> skipped_;
  std::unordered_set<std::string> emittedNames_;
  // Declarations written commented out because this build cannot marshal them.
  int commentedDeclarations_ = 0;
  // The structs the header defined, keyed by tag, plus the order they were
  // defined in (so the emitted module reads like the header). Only a struct
  // whose layout the runtime accepts is queued in `structOrder_`.
  std::unordered_map<std::string, StructInfo> structTags_;
  std::vector<std::string> structOrder_;
  // The `enum` members, in declaration order, as module constants.
  std::vector<EnumConstant> enumConstants_;
  bindgen::Result result_;
};

}  // namespace

namespace bindgen {

Result generate(const std::string& headerSource, const std::string& sourceName,
                const Options& options) {
  Bindgen parser(tokenize(headerSource), collectMacroConstants(headerSource), sourceName,
                 options);
  return parser.run();
}

}  // namespace bindgen
