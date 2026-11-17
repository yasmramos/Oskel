#include "bindgen.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"  // isKnownTypeName / isIntegerTypeName: the FFI vocabulary

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
};

// The C words that never change the type on their own: storage classes and
// qualifiers the FFI vocabulary ignores.
bool isIgnoredSpecifier(const std::string& w) {
  return w == "const" || w == "volatile" || w == "restrict" || w == "__restrict" ||
         w == "__restrict__" || w == "extern" || w == "static" || w == "inline" ||
         w == "__inline" || w == "__inline__" || w == "register" || w == "auto" ||
         w == "_Noreturn" || w == "__forceinline" || w == "__const" || w == "__volatile__" ||
         w == "_Atomic";
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
      {"wchar_t", "int32"},   {"unsigned", "uint32"}, {"signed", "int32"},
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

// -------------------------------------------------------------- the parser

class Bindgen {
 public:
  Bindgen(std::vector<Token> tokens, std::string sourceName)
      : tokens_(std::move(tokens)), sourceName_(std::move(sourceName)) {
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
        advance();
        std::string tag;
        if (check(TokKind::Ident)) tag = advance().text;
        spec.isStructTag = true;
        spec.word = t.text + (tag.empty() ? "" : " " + tag);
        // A definition (`struct X { ... }`) is consumed here; a bare tag stays.
        if (checkPunct("{")) skipBraces();
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
          // After a modifier, a word that is not a type name is the declarator
          // itself: `unsigned len` is `unsigned int len`, not a type `len`.
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

  // Turns a specifier list into a `Base`.
  Base baseFromSpec(const Spec& spec) {
    Base base;
    base.spelling = spec.word;
    if (spec.isStructTag) {
      base.kind = Base::Kind::Struct;
      base.ok = false;
      base.reason = "struct/union/enum '" + spec.word + "' crosses by value";
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
        base.oskel = spec.isUnsigned ? "uint64" : "int64";
        base.spelling = "long";
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
      base.oskel = spec.isUnsigned ? "uint8" : "int8";
      base.spelling = std::string(spec.isUnsigned ? "unsigned " : "") + "char";
      return base;
    }
    if (w == "int") {
      base.kind = Base::Kind::Int;
      if (spec.shortCount > 0) {
        base.oskel = spec.isUnsigned ? "uint16" : "int16";
        base.spelling = std::string(spec.isUnsigned ? "unsigned " : "") + "short";
      } else if (spec.longCount > 0) {
        base.oskel = spec.isUnsigned ? "uint64" : "int64";
        base.spelling = std::string(spec.isUnsigned ? "unsigned " : "") + "long";
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
    base.reason = "unknown type '" + w + "'";
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
    if (base.kind == Base::Kind::Void) {
      base.kind = Base::Kind::Unknown;
      base.ok = true;
      base.oskel = "ptr";
      base.spelling = "void *";
      return base;
    }
    if (base.kind == Base::Kind::Char && stars == 1) {
      base.kind = Base::Kind::Unknown;
      base.ok = true;
      base.oskel = "cstring";
      base.spelling = "char *";
      return base;
    }
    // Any other pointer (including a multi-level one) is an opaque handle.
    base.kind = Base::Kind::Unknown;
    base.ok = true;
    base.oskel = "ptr";
    base.spelling += " *";
    return base;
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
      if (checkPunct("(")) {
        // A function-pointer typedef: `typedef int (*cb)(int);`. The name sits
        // inside the parentheses (`(*cb)`), so it is read from there; slice 1
        // does not generate callbacks, so the alias is recorded as unsupported
        // rather than silently mis-typed.
        advance();  // `(`
        while (matchPunct("*")) {
        }
        if (check(TokKind::Ident)) name = advance().text;
        skipParens();
        if (checkPunct("(")) skipParens();  // the parameter list
        declarator.ok = false;
        declarator.kind = Base::Kind::Unknown;
        declarator.reason = "a function-pointer typedef is not in the FFI vocabulary";
      } else {
        declarator = parsePointers(base);
        if (check(TokKind::Ident)) name = advance().text;
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
    Spec spec = parseSpecifiers();
    Base base = baseFromSpec(spec);
    Base returnType = parsePointers(base);

    // A declaration that is not a plain `name(` is a variable, a function
    // pointer, or something else slice 1 does not bind: skip it.
    if (!check(TokKind::Ident)) {
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
    if (!paramProblem.empty()) {
      skipped_.push_back(name + ": " + paramProblem);
      return;
    }

    Function fn;
    fn.name = name;
    fn.params = std::move(params);
    fn.variadic = variadic;
    fn.returnType = returnType.oskel.empty() ? "void" : returnType.oskel;
    fn.line = nameToken.line;

    auto existing = emittedNames_.find(name);
    if (existing != emittedNames_.end()) {
      skipped_.push_back(name + ": already declared above");
      return;
    }
    emittedNames_.insert(name);
    if (variadic) ++commentedVariadics_;
    functions_.push_back(std::move(fn));
  }

  struct Param {
    std::string name;
    std::string type;
  };
  struct Function {
    std::string name;
    std::vector<Param> params;
    bool variadic = false;
    std::string returnType;
    int line = 0;
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
      if (checkPunct("(")) {
        // A function pointer parameter: a callback, which slice 1 does not
        // generate.
        if (problem.empty()) {
          problem = "a callback parameter ('fn(...)') is not generated yet";
        }
        skipParens();
      }
      std::string name;
      if (check(TokKind::Ident)) name = advance().text;
      // An array parameter decays to a pointer: `int values[]` is `int *`.
      if (checkPunct("[")) {
        while (checkPunct("[")) skipBrackets();
        base.ok = true;
        base.kind = Base::Kind::Unknown;
        base.oskel = "ptr";
      }
      if (!base.ok && problem.empty()) {
        problem = "parameter type " + quoteReason(base);
      }
      if (name.empty()) name = "a" + std::to_string(index);
      param.name = name;
      param.type = base.oskel.empty() ? "int32" : base.oskel;
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

    text += "extern \"C\" {\n";
    for (const Function& fn : functions_) {
      // A variadic declaration is written commented out: it needs libffi and
      // the System V AMD64 ABI, and a build without both rejects the
      // declaration at compile time. Commented, the module still compiles, and
      // the signature is right there to uncomment on a supporting build.
      const bool isVariadic = fn.variadic;
      text += isVariadic ? "  // " : "  ";
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
      if (isVariadic) {
        text += "  // needs libffi and the System V AMD64 ABI";
      }
      text += "\n";
    }
    for (const std::string& note : skipped_) {
      text += "  // skipped: ";
      text += note;
      text += "\n";
    }
    text += "}\n";

    result_.emitted = static_cast<int>(functions_.size()) - commentedVariadics_;
    result_.skipped = static_cast<int>(skipped_.size());
    result_.commented = commentedVariadics_;
  }

  static std::string quoteReason(const Base& base) {
    return base.reason.empty() ? std::string("is not in the FFI vocabulary") : base.reason;
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
  std::string sourceName_;
  std::size_t pos_ = 0;
  std::unordered_map<std::string, Base> typedefs_;
  std::vector<Function> functions_;
  std::vector<std::string> skipped_;
  std::unordered_set<std::string> emittedNames_;
  int commentedVariadics_ = 0;
  bindgen::Result result_;
};

}  // namespace

namespace bindgen {

Result generate(const std::string& headerSource, const std::string& sourceName) {
  Bindgen parser(tokenize(headerSource), sourceName);
  return parser.run();
}

}  // namespace bindgen
