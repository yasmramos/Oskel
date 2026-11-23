#include "oskelp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include "core/util.h"
#include "version.h"

namespace fs = std::filesystem;

namespace oskelp {

// ---------------------------------------------------------------------------
// Small helpers

namespace {

void reportError(const std::string& message) {
  std::cerr << "oskelp: error: " << message << '\n';
}

void reportWarning(const std::string& message) {
  std::cerr << "oskelp: warning: " << message << '\n';
}

bool writeTextFile(const fs::path& path, const std::string& text,
                   std::string& error) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "cannot write '" + path.string() + "'";
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) {
    error = "cannot write '" + path.string() + "'";
    return false;
  }
  return true;
}

bool isIdentifier(const std::string& name) {
  if (name.empty()) return false;
  unsigned char first = static_cast<unsigned char>(name[0]);
  if (!(std::isalpha(first) != 0 || first == '_')) return false;
  for (char c : name) {
    unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) != 0 || u == '_')) return false;
  }
  return true;
}

bool allDigits(const std::string& text) {
  if (text.empty()) return false;
  for (char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
  }
  return true;
}

// A package version is `major.minor.patch` with an optional `-prerelease` and
// `+build`. Requirements add `^`, `~`, comparators and wildcards; the parser
// below is shared by the manifest, the lockfile and the registry selector.
std::string trimCopy(const std::string& text);  // defined below

namespace {

bool parseNumericComponent(const std::string& text, int& out) {
  if (text.empty() || !allDigits(text)) return false;
  if (text.size() > 1 && text[0] == '0') return false;
  long long value = 0;
  for (char c : text) {
    value = value * 10 + (c - '0');
    if (value > INT_MAX) return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool isWildcardComponent(const std::string& text) {
  return text == "*" || text == "x" || text == "X";
}

bool parsePrerelease(const std::string& text) {
  if (text.empty()) return false;
  size_t start = 0;
  while (true) {
    size_t dot = text.find('.', start);
    std::string identifier =
        text.substr(start, dot == std::string::npos ? std::string::npos
                                                    : dot - start);
    if (identifier.empty()) return false;
    bool numeric = true;
    for (char c : identifier) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
        return false;
      }
      if (!std::isdigit(static_cast<unsigned char>(c))) numeric = false;
    }
    if (numeric && identifier.size() > 1 && identifier[0] == '0') {
      return false;
    }
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return true;
}

struct PartialVersion {
  bool hasMajor = false;
  bool hasMinor = false;
  bool hasPatch = false;
  bool majorWild = false;
  bool minorWild = false;
  bool patchWild = false;
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string prerelease;

  int specified() const {
    if (majorWild || !hasMajor) return 0;
    if (minorWild || !hasMinor) return 1;
    if (patchWild || !hasPatch) return 2;
    return 3;
  }
};

bool parsePartialVersion(const std::string& text, PartialVersion& out) {
  out = PartialVersion{};
  std::string value = trimCopy(text);
  if (value.empty()) return false;
  size_t plus = value.find('+');
  if (plus != std::string::npos) {
    if (plus + 1 >= value.size()) return false;
    value = value.substr(0, plus);
  }
  size_t dash = value.find('-');
  std::string core = value;
  if (dash != std::string::npos) {
    core = value.substr(0, dash);
    out.prerelease = value.substr(dash + 1);
    if (!parsePrerelease(out.prerelease)) return false;
  }
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t dot = core.find('.', start);
    parts.push_back(core.substr(start, dot == std::string::npos
                                           ? std::string::npos
                                           : dot - start));
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  if (parts.empty() || parts.size() > 3) return false;
  bool wildcardSeen = false;
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    if (isWildcardComponent(part)) {
      wildcardSeen = true;
      if (i == 0) out.majorWild = true;
      else if (i == 1) out.minorWild = true;
      else out.patchWild = true;
      continue;
    }
    if (wildcardSeen) return false;
    int number = 0;
    if (!parseNumericComponent(part, number)) return false;
    if (i == 0) {
      out.hasMajor = true;
      out.major = number;
    } else if (i == 1) {
      out.hasMinor = true;
      out.minor = number;
    } else {
      out.hasPatch = true;
      out.patch = number;
    }
  }
  if (!out.hasMajor && !out.majorWild) return false;
  if (!out.prerelease.empty() && out.specified() != 3) return false;
  return true;
}

struct Bound {
  bool present = false;
  bool inclusive = true;
  SemVer version;
};

struct Range {
  Bound lower;
  Bound upper;
};

bool makeRange(const std::string& token, Range& out) {
  out = Range{};
  std::string text = trimCopy(token);
  if (text.empty()) return false;
  std::string op;
  for (const std::string& candidate :
       {std::string(">="), std::string("<="), std::string(">"),
        std::string("<"), std::string("="), std::string("^"),
        std::string("~")}) {
    if (text.compare(0, candidate.size(), candidate) == 0) {
      op = candidate;
      text = trimCopy(text.substr(candidate.size()));
      break;
    }
  }
  if (text.empty()) return false;
  if (op.empty() && (text[0] == '>' || text[0] == '<' || text[0] == '=' ||
                     text[0] == '^' || text[0] == '~')) {
    return false;
  }
  PartialVersion partial;
  if (!parsePartialVersion(text, partial)) return false;
  int specified = partial.specified();
  if (specified == 0) return op.empty() || op == "=";

  SemVer base;
  base.major = partial.major;
  base.minor = partial.minor;
  base.patch = partial.patch;
  base.prerelease = partial.prerelease;
  auto nextMajor = [&]() {
    SemVer version = base;
    version.major += 1;
    version.minor = 0;
    version.patch = 0;
    version.prerelease.clear();
    return version;
  };
  auto nextMinor = [&]() {
    SemVer version = base;
    version.minor += 1;
    version.patch = 0;
    version.prerelease.clear();
    return version;
  };
  auto nextPatch = [&]() {
    SemVer version = base;
    version.patch += 1;
    version.prerelease.clear();
    return version;
  };

  if (op.empty() || op == "=") {
    out.lower = Bound{true, true, base};
    if (specified == 3) {
      out.upper = Bound{true, true, base};
    } else if (specified == 2) {
      out.upper = Bound{true, false, nextMinor()};
    } else {
      out.upper = Bound{true, false, nextMajor()};
    }
    return true;
  }
  if (op == ">=") {
    out.lower = Bound{true, true, base};
    return true;
  }
  if (op == ">") {
    if (specified == 3) out.lower = Bound{true, false, base};
    else if (specified == 2) out.lower = Bound{true, true, nextMinor()};
    else out.lower = Bound{true, true, nextMajor()};
    return true;
  }
  if (op == "<") {
    out.upper = Bound{true, false, base};
    return true;
  }
  if (op == "<=") {
    if (specified == 3) out.upper = Bound{true, true, base};
    else if (specified == 2) out.upper = Bound{true, false, nextMinor()};
    else out.upper = Bound{true, false, nextMajor()};
    return true;
  }
  if (op == "^") {
    if (partial.majorWild || partial.minorWild || partial.patchWild) return false;
    out.lower = Bound{true, true, base};
    if (partial.major > 0) {
      out.upper = Bound{true, false, nextMajor()};
    } else if (specified == 1) {
      out.upper = Bound{true, false, nextMajor()};
    } else if (partial.minor > 0) {
      out.upper = Bound{true, false, nextMinor()};
    } else if (specified == 3) {
      out.upper = Bound{true, false, nextPatch()};
    } else {
      out.upper = Bound{true, false, nextMinor()};
    }
    return true;
  }
  if (op == "~") {
    if (partial.majorWild || partial.minorWild || partial.patchWild) return false;
    out.lower = Bound{true, true, base};
    if (specified == 1) out.upper = Bound{true, false, nextMajor()};
    else out.upper = Bound{true, false, nextMinor()};
    return true;
  }
  return false;
}

bool matchesRange(const Range& range, const SemVer& version) {
  if (range.lower.present) {
    int cmp = compareSemVer(version, range.lower.version);
    if (cmp < 0 || (cmp == 0 && !range.lower.inclusive)) return false;
  }
  if (range.upper.present) {
    int cmp = compareSemVer(version, range.upper.version);
    if (cmp > 0 || (cmp == 0 && !range.upper.inclusive)) return false;
  }
  return true;
}

std::vector<std::string> requirementTokens(const std::string& requirement) {
  std::vector<std::string> tokens;
  std::string normalized = requirement;
  for (char& c : normalized) {
    if (c == ',') c = ' ';
  }
  std::istringstream stream(normalized);
  std::string token;
  while (stream >> token) {
    if (token == ">=" || token == "<=" || token == ">" || token == "<" ||
        token == "=" || token == "^" || token == "~") {
      std::string next;
      if (!(stream >> next)) return {};
      token += next;
    }
    tokens.push_back(token);
  }
  return tokens;
}

}  // namespace

std::string escapeToml(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

bool stripPathSource(const std::string& source, std::string& path) {
  const std::string prefix = "path+";
  if (!startsWith(source, prefix)) return false;
  path = source.substr(prefix.size());
  return true;
}

bool samePath(const fs::path& a, const fs::path& b) {
  std::error_code ecA;
  std::error_code ecB;
  fs::path canonicalA = fs::weakly_canonical(a, ecA);
  fs::path canonicalB = fs::weakly_canonical(b, ecB);
  if (ecA || ecB) return a.lexically_normal() == b.lexically_normal();
  return canonicalA == canonicalB;
}

std::string relativeDisplay(const fs::path& base, const fs::path& path) {
  std::error_code ec;
  fs::path relative = fs::relative(path, base, ec);
  if (ec) return path.string();
  return relative.generic_string();
}

// A directory name is turned into a valid Oskel package name: `my-app` becomes
// `my_app`, and anything that is not an identifier character is dropped.
std::string derivePackageName(const std::string& directory) {
  std::string base = fs::path(directory).filename().string();
  std::string name;
  for (char c : base) {
    unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0 || c == '_') {
      name += c;
    } else if (c == '-' || c == '.' || c == ' ') {
      name += '_';
    }
  }
  if (name.empty()) name = "app";
  return name;
}

}  // namespace

bool parseSemVer(const std::string& text, SemVer& out) {
  PartialVersion partial;
  if (!parsePartialVersion(text, partial) || partial.specified() != 3) {
    return false;
  }
  out.major = partial.major;
  out.minor = partial.minor;
  out.patch = partial.patch;
  out.prerelease = partial.prerelease;
  return true;
}

int compareSemVer(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.prerelease == b.prerelease) return 0;
  if (a.prerelease.empty()) return 1;
  if (b.prerelease.empty()) return -1;
  size_t aStart = 0;
  size_t bStart = 0;
  while (true) {
    size_t aDot = a.prerelease.find('.', aStart);
    size_t bDot = b.prerelease.find('.', bStart);
    std::string aId =
        a.prerelease.substr(aStart, aDot == std::string::npos
                                       ? std::string::npos
                                       : aDot - aStart);
    std::string bId =
        b.prerelease.substr(bStart, bDot == std::string::npos
                                       ? std::string::npos
                                       : bDot - bStart);
    bool aNumeric = allDigits(aId);
    bool bNumeric = allDigits(bId);
    if (aNumeric && bNumeric) {
      if (aId.size() != bId.size()) return aId.size() < bId.size() ? -1 : 1;
      if (aId != bId) return aId < bId ? -1 : 1;
    } else if (aNumeric != bNumeric) {
      return aNumeric ? -1 : 1;
    } else if (aId != bId) {
      return aId < bId ? -1 : 1;
    }
    bool aDone = aDot == std::string::npos;
    bool bDone = bDot == std::string::npos;
    if (aDone && bDone) return 0;
    if (aDone) return -1;
    if (bDone) return 1;
    aStart = aDot + 1;
    bStart = bDot + 1;
  }
}

bool isValidRequirement(const std::string& requirement) {
  if (requirement.find("||") != std::string::npos) return false;
  std::vector<std::string> tokens = requirementTokens(requirement);
  if (tokens.empty()) return false;
  for (const std::string& token : tokens) {
    Range range;
    if (!makeRange(token, range)) return false;
  }
  return true;
}

bool satisfiesRequirement(const std::string& requirement, const SemVer& version) {
  if (requirement.find("||") != std::string::npos) return false;
  if (!version.prerelease.empty()) {
    SemVer exact;
    std::string text = trimCopy(requirement);
    if (!text.empty() && text[0] == '=') text = trimCopy(text.substr(1));
    return parseSemVer(text, exact) && compareSemVer(exact, version) == 0;
  }
  std::vector<std::string> tokens = requirementTokens(requirement);
  if (tokens.empty()) return false;
  for (const std::string& token : tokens) {
    Range range;
    if (!makeRange(token, range)) return false;
    if (!matchesRange(range, version)) return false;
  }
  return true;
}

bool isSemver(const std::string& version) {
  SemVer parsed;
  return parseSemVer(version, parsed);
}

// ---------------------------------------------------------------------------
// TOML subset

namespace {

class TomlParser {
 public:
  TomlParser(const std::string& source, std::string& error)
      : source_(source), error_(error) {}

  bool parse(TomlDocument& document) {
    std::map<std::string, TomlValue>* current = &document.values;
    while (true) {
      skipWhitespaceAndComments();
      if (atEnd()) break;
      if (peek() == '[') {
        if (!parseSection(document.values, current)) return false;
      } else {
        if (!parseKeyValue(*current)) return false;
      }
      skipHorizontalWhitespace();
      if (!atEnd() && peek() == '#') {
        while (!atEnd() && peek() != '\n') advance();
      }
      if (!atEnd() && peek() != '\n' && peek() != '\r') {
        return fail("expected a newline after the value");
      }
    }
    return true;
  }

 private:
  bool parseSection(std::map<std::string, TomlValue>& root,
                    std::map<std::string, TomlValue>*& current) {
    advance();  // '['
    skipHorizontalWhitespace();
    std::vector<std::string> parts;
    while (true) {
      std::string key;
      if (!parseKey(key)) return false;
      parts.push_back(key);
      skipHorizontalWhitespace();
      if (!atEnd() && peek() == '.') {
        advance();
        skipHorizontalWhitespace();
        continue;
      }
      break;
    }
    if (atEnd() || peek() != ']') return fail("expected ']' after the section name");
    advance();
    std::map<std::string, TomlValue>* table = &root;
    for (const std::string& part : parts) {
      auto it = table->find(part);
      if (it == table->end()) {
        TomlValue value;
        value.type = TomlValue::Type::Table;
        it = table->emplace(part, std::move(value)).first;
      } else if (!it->second.isTable()) {
        return fail("'" + part + "' is already defined as a value");
      }
      table = &it->second.table;
    }
    current = table;
    return true;
  }

  bool parseKeyValue(std::map<std::string, TomlValue>& table) {
    std::string key;
    if (!parseKey(key)) return false;
    skipHorizontalWhitespace();
    if (atEnd() || peek() != '=') return fail("expected '=' after the key");
    advance();
    skipHorizontalWhitespace();
    TomlValue value;
    if (!parseValue(value)) return false;
    if (table.count(key) != 0) return fail("duplicate key '" + key + "'");
    table.emplace(key, std::move(value));
    return true;
  }

  bool parseKey(std::string& key) {
    if (atEnd()) return fail("expected a key");
    if (peek() == '"') {
      TomlValue value;
      if (!parseString(value)) return false;
      key = value.text;
      return true;
    }
    size_t start = pos_;
    while (!atEnd()) {
      char c = peek();
      if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-') {
        advance();
      } else {
        break;
      }
    }
    if (pos_ == start) return fail("expected a key");
    key = source_.substr(start, pos_ - start);
    return true;
  }

  bool parseValue(TomlValue& value) {
    if (atEnd()) return fail("expected a value");
    char c = peek();
    if (c == '"') return parseString(value);
    if (c == '[') return parseArray(value);
    if (c == '{') return parseInlineTable(value);
    if (c == 't' || c == 'f') {
      if (source_.compare(pos_, 4, "true") == 0) {
        for (int i = 0; i < 4; ++i) advance();
        value.type = TomlValue::Type::Boolean;
        value.boolean = true;
        return true;
      }
      if (source_.compare(pos_, 5, "false") == 0) {
        for (int i = 0; i < 5; ++i) advance();
        value.type = TomlValue::Type::Boolean;
        value.boolean = false;
        return true;
      }
      return fail("expected 'true' or 'false'");
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
      return parseInteger(value);
    }
    return fail("unsupported value");
  }

  bool parseString(TomlValue& value) {
    advance();  // opening quote
    std::string out;
    while (!atEnd()) {
      char c = advance();
      if (c == '"') {
        value.type = TomlValue::Type::String;
        value.text = out;
        return true;
      }
      if (c == '\n') return fail("unterminated string");
      if (c == '\\') {
        if (atEnd()) return fail("unterminated escape");
        char escaped = advance();
        switch (escaped) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          default: return fail(std::string("unknown escape '\\") + escaped + "'");
        }
        continue;
      }
      out += c;
    }
    return fail("unterminated string");
  }

  bool parseInteger(TomlValue& value) {
    size_t start = pos_;
    if (peek() == '-') advance();
    if (atEnd() || std::isdigit(static_cast<unsigned char>(peek())) == 0) {
      return fail("expected a digit");
    }
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) advance();
    std::string text = source_.substr(start, pos_ - start);
    try {
      value.integer = std::stoll(text);
    } catch (const std::exception&) {
      return fail("integer out of range");
    }
    value.type = TomlValue::Type::Integer;
    return true;
  }

  bool parseArray(TomlValue& value) {
    advance();  // '['
    value.type = TomlValue::Type::Array;
    skipWhitespaceAndComments();
    if (!atEnd() && peek() == ']') {
      advance();
      return true;
    }
    while (true) {
      skipWhitespaceAndComments();
      TomlValue element;
      if (!parseValue(element)) return false;
      value.array.push_back(std::move(element));
      skipWhitespaceAndComments();
      if (!atEnd() && peek() == ',') {
        advance();
        skipWhitespaceAndComments();
        if (!atEnd() && peek() == ']') {
          advance();
          return true;
        }
        continue;
      }
      if (!atEnd() && peek() == ']') {
        advance();
        return true;
      }
      return fail("expected ',' or ']' in array");
    }
  }

  bool parseInlineTable(TomlValue& value) {
    advance();  // '{'
    value.type = TomlValue::Type::Table;
    skipWhitespaceAndComments();
    if (!atEnd() && peek() == '}') {
      advance();
      return true;
    }
    while (true) {
      skipWhitespaceAndComments();
      std::string key;
      if (!parseKey(key)) return false;
      skipHorizontalWhitespace();
      if (atEnd() || peek() != '=') return fail("expected '=' in inline table");
      advance();
      skipHorizontalWhitespace();
      TomlValue element;
      if (!parseValue(element)) return false;
      if (value.table.count(key) != 0) return fail("duplicate key '" + key + "'");
      value.table.emplace(key, std::move(element));
      skipWhitespaceAndComments();
      if (!atEnd() && peek() == ',') {
        advance();
        skipWhitespaceAndComments();
        if (!atEnd() && peek() == '}') {
          advance();
          return true;
        }
        continue;
      }
      if (!atEnd() && peek() == '}') {
        advance();
        return true;
      }
      return fail("expected ',' or '}' in inline table");
    }
  }

  void skipHorizontalWhitespace() {
    while (!atEnd()) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\r') {
        advance();
      } else {
        break;
      }
    }
  }

  void skipWhitespaceAndComments() {
    while (!atEnd()) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else if (c == '#') {
        while (!atEnd() && peek() != '\n') advance();
      } else {
        break;
      }
    }
  }

  bool atEnd() const { return pos_ >= source_.size(); }
  char peek() const { return pos_ < source_.size() ? source_[pos_] : '\0'; }

  char advance() {
    char c = source_[pos_++];
    if (c == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    return c;
  }

  bool fail(const std::string& message) {
    error_ = std::to_string(line_) + ":" + std::to_string(column_) + ": " + message;
    return false;
  }

  const std::string& source_;
  std::string& error_;
  size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;
};

}  // namespace

bool parseToml(const std::string& source, TomlDocument& out, std::string& error) {
  TomlParser parser(source, error);
  return parser.parse(out);
}

// ---------------------------------------------------------------------------
// Manifest

bool loadManifest(const std::string& path, Manifest& out, std::string& error) {
  std::string source;
  if (!readSourceFile(path, source)) {
    error = "cannot read '" + path + "'";
    return false;
  }
  TomlDocument document;
  std::string parseError;
  if (!parseToml(source, document, parseError)) {
    error = path + ":" + parseError;
    return false;
  }

  out = Manifest{};
  out.path = absolutePath(path);
  out.dir = fs::path(out.path).parent_path().string();

  auto packageIt = document.values.find("package");
  if (packageIt == document.values.end() || !packageIt->second.isTable()) {
    error = "'" + path + "' has no [package] section";
    return false;
  }
  const auto& table = packageIt->second.table;

  auto nameIt = table.find("name");
  if (nameIt == table.end() || !nameIt->second.isString() ||
      nameIt->second.text.empty()) {
    error = "[package] needs a non-empty string 'name'";
    return false;
  }
  out.package.name = nameIt->second.text;
  if (!isIdentifier(out.package.name)) {
    error = "package name '" + out.package.name +
            "' is not a valid Oskel identifier";
    return false;
  }

  auto versionIt = table.find("version");
  if (versionIt == table.end() || !versionIt->second.isString() ||
      versionIt->second.text.empty()) {
    error = "[package] needs a non-empty string 'version'";
    return false;
  }
  out.package.version = versionIt->second.text;
  if (!isSemver(out.package.version)) {
    error = "package version '" + out.package.version +
            "' is not 'major.minor.patch'";
    return false;
  }

  auto entryIt = table.find("entry");
  if (entryIt != table.end()) {
    if (!entryIt->second.isString() || entryIt->second.text.empty()) {
      error = "[package] 'entry' must be a non-empty string";
      return false;
    }
    out.package.entry = entryIt->second.text;
  } else {
    out.package.entry = "src/" + out.package.name + "/main.oskl";
  }

  auto authorsIt = table.find("authors");
  if (authorsIt != table.end()) {
    if (!authorsIt->second.isArray()) {
      error = "[package] 'authors' must be an array of strings";
      return false;
    }
    for (const TomlValue& author : authorsIt->second.array) {
      if (!author.isString()) {
        error = "[package] 'authors' must be an array of strings";
        return false;
      }
      out.package.authors.push_back(author.text);
    }
  }

  for (const auto& [key, value] : table) {
    (void)value;
    if (key != "name" && key != "version" && key != "entry" &&
        key != "authors") {
      out.warnings.push_back("[package] unknown key '" + key + "'");
    }
  }

  auto dependenciesIt = document.values.find("dependencies");
  if (dependenciesIt != document.values.end()) {
    if (!dependenciesIt->second.isTable()) {
      error = "[dependencies] must be a table";
      return false;
    }
    for (const auto& [name, value] : dependenciesIt->second.table) {
      if (!isIdentifier(name)) {
        error = "dependency name '" + name + "' is not a valid Oskel identifier";
        return false;
      }
      Dependency dependency;
      dependency.name = name;
      if (value.isString()) {
        dependency.requirement = value.text;
        if (!isValidRequirement(dependency.requirement)) {
          error = "dependency '" + name +
                  "' has an invalid version requirement '" +
                  dependency.requirement +
                  "'; use a version, ^, ~, a comparator or a wildcard";
          return false;
        }
        out.package.dependencies.emplace(name, std::move(dependency));
        continue;
      }
      if (!value.isTable()) {
        error = "dependency '" + name +
                "' must be an inline table or a version requirement string";
        return false;
      }
      auto pathIt = value.table.find("path");
      if (pathIt == value.table.end() || !pathIt->second.isString() ||
          pathIt->second.text.empty()) {
        error = "dependency '" + name + "' needs a non-empty string 'path'";
        return false;
      }
      dependency.path = pathIt->second.text;
      auto depVersionIt = value.table.find("version");
      if (depVersionIt != value.table.end()) {
        if (!depVersionIt->second.isString()) {
          error = "dependency '" + name + "' 'version' must be a string";
          return false;
        }
        dependency.version = depVersionIt->second.text;
        if (!isSemver(dependency.version)) {
          error = "dependency '" + name + "' version '" + dependency.version +
                  "' is not 'major.minor.patch'";
          return false;
        }
      }
      for (const auto& [key, entry] : value.table) {
        (void)entry;
        if (key != "path" && key != "version") {
          out.warnings.push_back("dependency '" + name + "' unknown key '" +
                                 key + "'");
        }
      }
      out.package.dependencies.emplace(name, std::move(dependency));
    }
  }

  for (const auto& [key, value] : document.values) {
    (void)value;
    if (key != "package" && key != "dependencies") {
      out.warnings.push_back("unknown section '" + key + "'");
    }
  }

  return true;
}

bool findManifest(const std::string& startDir, std::string& manifestPath) {
  std::error_code ec;
  fs::path dir = fs::absolute(startDir, ec);
  if (ec) dir = fs::path(startDir);
  while (true) {
    fs::path candidate = dir / "oskel.toml";
    if (fs::is_regular_file(candidate, ec)) {
      manifestPath = candidate.string();
      return true;
    }
    fs::path parent = dir.parent_path();
    if (parent.empty() || parent == dir) break;
    dir = parent;
  }
  return false;
}

std::string renderManifest(const Package& package) {
  std::ostringstream out;
  out << "[package]\n";
  out << "name = \"" << escapeToml(package.name) << "\"\n";
  out << "version = \"" << escapeToml(package.version) << "\"\n";
  out << "entry = \"" << escapeToml(package.entry) << "\"\n";
  if (!package.authors.empty()) {
    out << "authors = [";
    for (size_t i = 0; i < package.authors.size(); ++i) {
      if (i != 0) out << ", ";
      out << '"' << escapeToml(package.authors[i]) << '"';
    }
    out << "]\n";
  }
  out << "\n[dependencies]\n";
  for (const auto& [name, dependency] : package.dependencies) {
    if (dependency.isRegistry()) {
      out << name << " = \"" << escapeToml(dependency.requirement) << "\"\n";
      continue;
    }
    out << name << " = { path = \"" << escapeToml(dependency.path) << '"';
    if (!dependency.version.empty()) {
      out << ", version = \"" << escapeToml(dependency.version) << '"';
    }
    out << " }\n";
  }
  return out.str();
}

namespace {

std::string trimCopy(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::vector<std::string> splitLines(const std::string& source) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (true) {
    size_t newline = source.find('\n', start);
    if (newline == std::string::npos) {
      lines.push_back(source.substr(start));
      return lines;
    }
    lines.push_back(source.substr(start, newline - start));
    start = newline + 1;
  }
}

std::string joinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) out += '\n';
    out += lines[i];
  }
  return out;
}

// A table header is `[name]`, optionally followed by a comment.
bool sectionHeaderName(const std::string& line, std::string& name) {
  std::string trimmed = trimCopy(line);
  if (trimmed.empty() || trimmed[0] != '[') return false;
  size_t close = trimmed.find(']');
  if (close == std::string::npos) return false;
  std::string after = trimCopy(trimmed.substr(close + 1));
  if (!after.empty() && after[0] != '#') return false;
  name = trimCopy(trimmed.substr(1, close - 1));
  return true;
}

// `key = value` at the start of a line. The key may be bare or quoted; the
// value is returned as written, so the caller can inspect it.
bool parseDependencyLine(const std::string& line, std::string& key,
                         std::string& value) {
  key.clear();
  size_t i = 0;
  while (i < line.size() &&
         std::isspace(static_cast<unsigned char>(line[i])) != 0) {
    ++i;
  }
  if (i >= line.size() || line[i] == '#' || line[i] == '[') return false;
  if (line[i] == '"' || line[i] == '\'') {
    char quote = line[i++];
    bool closed = false;
    while (i < line.size()) {
      char c = line[i++];
      if (c == quote) {
        closed = true;
        break;
      }
      if (quote == '"' && c == '\\' && i < line.size()) {
        key += line[i++];
      } else {
        key += c;
      }
    }
    if (!closed) return false;
  } else {
    size_t start = i;
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i])) == 0 &&
           line[i] != '=') {
      ++i;
    }
    if (i == start) return false;
    key = line.substr(start, i - start);
  }
  while (i < line.size() &&
         std::isspace(static_cast<unsigned char>(line[i])) != 0) {
    ++i;
  }
  if (i >= line.size() || line[i] != '=') return false;
  value = line.substr(i + 1);
  return true;
}

bool findDependenciesSection(const std::vector<std::string>& lines,
                             size_t& header, size_t& end) {
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string name;
    if (!sectionHeaderName(lines[i], name) || name != "dependencies") {
      continue;
    }
    header = i;
    end = lines.size();
    for (size_t j = i + 1; j < lines.size(); ++j) {
      if (sectionHeaderName(lines[j], name)) {
        end = j;
        break;
      }
    }
    return true;
  }
  return false;
}

std::string dependencyLine(const Dependency& dependency, bool crlf) {
  std::string line;
  if (dependency.isRegistry()) {
    line = dependency.name + " = \"" + escapeToml(dependency.requirement) + '"';
  } else {
    line = dependency.name + " = { path = \"" +
           escapeToml(dependency.path) + '"';
    if (!dependency.version.empty()) {
      line += ", version = \"" + escapeToml(dependency.version) + '"';
    }
    line += " }";
  }
  if (crlf) line += '\r';
  return line;
}

}  // namespace

bool addDependencyToManifest(const std::string& source,
                             const Dependency& dependency, std::string& out,
                             std::string& error) {
  bool crlf = source.find("\r\n") != std::string::npos;
  std::vector<std::string> lines = splitLines(source);
  size_t header = 0;
  size_t end = 0;
  if (!findDependenciesSection(lines, header, end)) {
    if (!lines.empty() && !lines.back().empty()) {
      lines.push_back(crlf ? "\r" : "");
    }
    lines.push_back("[dependencies]" + std::string(crlf ? "\r" : ""));
    lines.push_back(dependencyLine(dependency, crlf));
    out = joinLines(lines);
    return true;
  }

  for (size_t i = header + 1; i < end; ++i) {
    std::string key;
    std::string value;
    if (parseDependencyLine(lines[i], key, value) && key == dependency.name) {
      error = "dependency '" + dependency.name + "' is already declared";
      return false;
    }
  }

  size_t insert = header + 1;
  for (size_t i = header + 1; i < end; ++i) {
    if (!trimCopy(lines[i]).empty()) insert = i + 1;
  }
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert),
               dependencyLine(dependency, crlf));
  out = joinLines(lines);
  return true;
}

bool removeDependencyFromManifest(const std::string& source,
                                  const std::string& name, std::string& out,
                                  std::string& error) {
  std::vector<std::string> lines = splitLines(source);
  size_t header = 0;
  size_t end = 0;
  if (!findDependenciesSection(lines, header, end)) {
    error = "no [dependencies] section";
    return false;
  }

  for (size_t i = header + 1; i < end; ++i) {
    std::string key;
    std::string value;
    if (!parseDependencyLine(lines[i], key, value) || key != name) continue;
    std::string trimmed = trimCopy(value);
    bool opens =
        !trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[');
    char close =
        trimmed.empty() ? '\0' : (trimmed[0] == '{' ? '}' : ']');
    if (opens && trimmed.find(close) == std::string::npos) {
      error = "dependency '" + name +
              "' spans multiple lines; remove it by hand";
      return false;
    }
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
    out = joinLines(lines);
    return true;
  }

  error = "dependency '" + name + "' is not declared";
  return false;
}

// ---------------------------------------------------------------------------
// Lockfile

bool parseLockfile(const std::string& source, Lockfile& out, std::string& error) {
  TomlDocument document;
  std::string parseError;
  if (!parseToml(source, document, parseError)) {
    error = parseError;
    return false;
  }
  out = Lockfile{};
  auto versionIt = document.values.find("version");
  if (versionIt == document.values.end() || !versionIt->second.isInteger()) {
    error = "missing integer 'version'";
    return false;
  }
  if (versionIt->second.integer != 1) {
    error = "unsupported lockfile version " +
            std::to_string(versionIt->second.integer);
    return false;
  }
  auto packagesIt = document.values.find("packages");
  if (packagesIt == document.values.end()) return true;
  if (!packagesIt->second.isTable()) {
    error = "'packages' must be a table";
    return false;
  }
  for (const auto& [name, value] : packagesIt->second.table) {
    if (!value.isTable()) {
      error = "package '" + name + "' must be a table";
      return false;
    }
    auto versionEntry = value.table.find("version");
    if (versionEntry == value.table.end() || !versionEntry->second.isString()) {
      error = "package '" + name + "' needs a string 'version'";
      return false;
    }
    auto sourceEntry = value.table.find("source");
    if (sourceEntry == value.table.end() || !sourceEntry->second.isString()) {
      error = "package '" + name + "' needs a string 'source'";
      return false;
    }
    LockedPackage locked;
    locked.name = name;
    locked.version = versionEntry->second.text;
    locked.source = sourceEntry->second.text;
    out.packages[name] = std::move(locked);
  }
  return true;
}

std::string renderLockfile(const Lockfile& lock) {
  std::ostringstream out;
  out << "# Generated by oskelp. Do not edit by hand.\n";
  out << "version = " << lock.version << "\n";
  for (const auto& [name, package] : lock.packages) {
    out << "\n[packages." << name << "]\n";
    out << "version = \"" << escapeToml(package.version) << "\"\n";
    out << "source = \"" << escapeToml(package.source) << "\"\n";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// Dependency graph

const ResolvedPackage* Resolution::find(const std::string& name) const {
  for (const ResolvedPackage& package : packages) {
    if (package.name == name) return &package;
  }
  return nullptr;
}

namespace {

// Loads a dependency's manifest and checks that its identity matches the key
// used in the dependent's [dependencies] table.
bool loadDependencyManifest(const Manifest& parent,
                            const Dependency& dependency, Manifest& out,
                            std::string& error) {
  fs::path directory = fs::path(parent.dir) / dependency.path;
  std::error_code ec;
  fs::path canonical = fs::weakly_canonical(directory, ec);
  if (ec) canonical = directory.lexically_normal();
  std::string loadError;
  if (!loadManifest((canonical / "oskel.toml").string(), out, loadError)) {
    error = "dependency '" + dependency.name + "': " + loadError;
    return false;
  }
  if (out.package.name != dependency.name) {
    error = "dependency '" + dependency.name + "' points to a package named '" +
            out.package.name + "'";
    return false;
  }
  if (!dependency.version.empty() &&
      dependency.version != out.package.version) {
    error = "dependency '" + dependency.name + "' requires version '" +
            dependency.version + "' but the package is '" +
            out.package.version + "'";
    return false;
  }
  return true;
}

std::string registryRoot() {
  const char* value = std::getenv("OSKEL_REGISTRY");
  return value == nullptr ? std::string() : std::string(value);
}

// Every valid version directory under `<registry>/<name>`, sorted by version.
// The directory name is the version string, so build metadata is not preserved
// (the manifest ignores it too).
bool registryVersions(const std::string& name, const std::string& root,
                      std::vector<std::pair<SemVer, std::string>>& versions,
                      std::string& error) {
  fs::path packageDir = fs::path(root) / name;
  std::error_code ec;
  if (!fs::is_directory(packageDir, ec)) {
    error = "the registry at '" + root + "' has no package '" + name + "'";
    return false;
  }
  for (fs::directory_iterator it(packageDir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_directory()) continue;
    std::string candidate = it->path().filename().string();
    SemVer parsed;
    if (!parseSemVer(candidate, parsed)) continue;
    versions.emplace_back(parsed, candidate);
  }
  if (ec) {
    error = "cannot read '" + packageDir.string() + "': " + ec.message();
    return false;
  }
  std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) {
    return compareSemVer(a.first, b.first) < 0;
  });
  return true;
}

// Picks the version a registry dependency resolves to: a `forced` version when
// the caller pinned one (and it must exist and satisfy the requirement), the
// lockfile's pin when it still satisfies the requirement and exists, otherwise
// the highest published version that satisfies it.
bool selectRegistryVersion(const Dependency& dependency,
                           const std::map<std::string, std::string>& preferred,
                           const std::map<std::string, std::string>& forced,
                           std::string& version, fs::path& directory,
                           std::string& error) {
  std::string root = registryRoot();
  if (root.empty()) {
    error = "dependency '" + dependency.name +
            "' is a registry dependency; set OSKEL_REGISTRY to the registry "
            "directory";
    return false;
  }
  fs::path packageDir = fs::path(root) / dependency.name;
  std::vector<std::pair<SemVer, std::string>> versions;
  if (!registryVersions(dependency.name, root, versions, error)) return false;

  auto explicitVersion = forced.find(dependency.name);
  if (explicitVersion != forced.end()) {
    SemVer parsed;
    if (!parseSemVer(explicitVersion->second, parsed)) {
      error = "version '" + explicitVersion->second + "' for '" +
              dependency.name + "' is not a semantic version";
      return false;
    }
    std::error_code entryError;
    if (!fs::is_directory(packageDir / explicitVersion->second, entryError)) {
      error = "the registry at '" + root + "' has no '" + dependency.name +
              "' version '" + explicitVersion->second + "'";
      return false;
    }
    if (isRegistryVersionYanked(dependency.name, explicitVersion->second,
                                root)) {
      error = "version '" + explicitVersion->second + "' of '" +
              dependency.name +
              "' is yanked; run `oskelp yank --undo` to restore it";
      return false;
    }
    if (!satisfiesRequirement(dependency.requirement, parsed)) {
      error = "package '" + dependency.name + "' version '" +
              explicitVersion->second + "' does not satisfy '" +
              dependency.requirement + "'";
      return false;
    }
    version = explicitVersion->second;
    fs::path chosenDir = packageDir / version;
    std::error_code canonicalError;
    directory = fs::weakly_canonical(chosenDir, canonicalError);
    if (canonicalError) directory = chosenDir.lexically_normal();
    return true;
  }

  std::vector<std::pair<SemVer, std::string>> candidates;
  std::string published;
  for (const auto& entry : versions) {
    bool yanked = isRegistryVersionYanked(dependency.name, entry.second, root);
    if (!published.empty()) published += ", ";
    published += entry.second;
    if (yanked) published += " (yanked)";
    if (!yanked && satisfiesRequirement(dependency.requirement, entry.first)) {
      candidates.push_back(entry);
    }
  }
  if (candidates.empty()) {
    error = "no version of '" + dependency.name + "' satisfies '" +
            dependency.requirement + "' in '" + root + "'";
    if (!published.empty()) error += " (published: " + published + ")";
    return false;
  }

  // A lockfile pin is honoured even when it is yanked: a publisher withdrawing
  // a release must not break a project that already locked it.
  std::string chosen;
  auto pinned = preferred.find(dependency.name);
  if (pinned != preferred.end()) {
    SemVer parsed;
    std::error_code entryError;
    if (parseSemVer(pinned->second, parsed) &&
        satisfiesRequirement(dependency.requirement, parsed) &&
        fs::is_directory(packageDir / pinned->second, entryError)) {
      chosen = pinned->second;
    }
  }
  if (chosen.empty()) chosen = candidates.back().second;

  fs::path chosenDir = packageDir / chosen;
  std::error_code canonicalError;
  directory = fs::weakly_canonical(chosenDir, canonicalError);
  if (canonicalError) directory = chosenDir.lexically_normal();
  version = chosen;
  return true;
}

bool loadPreferredVersions(const Manifest& manifest,
                           std::map<std::string, std::string>& preferred,
                           std::string& error) {
  fs::path lockPath = fs::path(manifest.dir) / "oskel.lock";
  std::string source;
  if (!readSourceFile(lockPath.string(), source)) return true;
  Lockfile lock;
  std::string lockError;
  if (!parseLockfile(source, lock, lockError)) {
    error = "oskel.lock:" + lockError;
    return false;
  }
  for (const auto& [name, package] : lock.packages) {
    preferred[name] = package.version;
  }
  return true;
}

}  // namespace

bool isRegistryVersionYanked(const std::string& name, const std::string& version,
                             const std::string& registry) {
  if (registry.empty()) return false;
  std::error_code ec;
  return fs::is_regular_file(
      fs::path(registry) / name / version / ".yanked", ec);
}

bool latestRegistryVersion(const std::string& name, std::string& version,
                           bool includePrerelease, std::string& error) {
  std::string root = registryRoot();
  if (root.empty()) {
    error = "OSKEL_REGISTRY is not set; set it to the registry directory";
    return false;
  }
  std::vector<std::pair<SemVer, std::string>> versions;
  if (!registryVersions(name, root, versions, error)) return false;
  bool sawUsable = false;
  for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
    if (isRegistryVersionYanked(name, it->second, root)) continue;
    sawUsable = true;
    if (!includePrerelease && !it->first.prerelease.empty()) continue;
    version = it->second;
    return true;
  }
  if (versions.empty()) {
    error = "the registry at '" + root + "' has no versions of '" + name + "'";
  } else if (!sawUsable) {
    error = "the registry at '" + root + "' has only yanked versions of '" +
            name + "'";
  } else {
    error = "the registry at '" + root + "' has only prerelease versions of '" +
            name + "'; pass an explicit version";
  }
  return false;
}

bool resolveDependencies(
    const Manifest& root, Resolution& out, std::string& error,
    const std::map<std::string, std::string>& preferred,
    const std::map<std::string, std::string>& forced) {
  out.packages.clear();
  std::map<std::string, int> state;  // 1 = visiting, 2 = done
  std::map<std::string, ResolvedPackage> resolved;
  std::vector<std::string> stack;

  // Seed the root so a dependency that leads back to it is a cycle even when
  // the root's in-memory manifest is newer than the one on disk (`oskelp add`
  // resolves before it writes).
  ResolvedPackage rootEntry;
  rootEntry.name = root.package.name;
  rootEntry.dir = root.dir;
  state[root.package.name] = 1;
  stack.push_back(root.package.name);
  resolved[root.package.name] = rootEntry;

  std::function<bool(const Manifest&)> visit =
      [&](const Manifest& manifest) -> bool {
    for (const auto& [name, dependency] : manifest.package.dependencies) {
      fs::path canonical;
      std::string source;
      std::string selectedVersion;
      if (dependency.isRegistry()) {
        if (!selectRegistryVersion(dependency, preferred, forced,
                                   selectedVersion, canonical, error)) {
          return false;
        }
        source = "registry+" + name + "@" + selectedVersion;
      } else {
        fs::path directory = fs::path(manifest.dir) / dependency.path;
        std::error_code ec;
        canonical = fs::weakly_canonical(directory, ec);
        if (ec) canonical = directory.lexically_normal();
        source = "path+" + normalizeSlashes(dependency.path);
      }

      auto known = resolved.find(name);
      if (known != resolved.end()) {
        if (dependency.isRegistry()) {
          SemVer knownVersion;
          if (parseSemVer(known->second.version, knownVersion) &&
              !satisfiesRequirement(dependency.requirement, knownVersion)) {
            error = "package '" + name + "' is resolved at " +
                    known->second.version + " but '" + manifest.package.name +
                    "' requires '" + dependency.requirement + "'";
            return false;
          }
        }
        if (!samePath(known->second.dir, canonical)) {
          error = "package '" + name +
                  "' is required from two different locations: '" +
                  known->second.dir + "' and '" + canonical.string() + "'";
          return false;
        }
      }

      int& status = state[name];
      if (status == 2) continue;
      if (status == 1) {
        std::string cycle;
        auto start = std::find(stack.begin(), stack.end(), name);
        for (auto it = start; it != stack.end(); ++it) {
          cycle += *it;
          cycle += " -> ";
        }
        cycle += name;
        error = "dependency cycle: " + cycle;
        return false;
      }
      status = 1;
      stack.push_back(name);

      Manifest dependencyManifest;
      std::string loadError;
      if (dependency.isRegistry()) {
        if (!loadManifest((canonical / "oskel.toml").string(),
                          dependencyManifest, loadError)) {
          error = "dependency '" + name + "': " + loadError;
          return false;
        }
        if (dependencyManifest.package.name != name) {
          error = "dependency '" + name +
                  "' points to a package named '" +
                  dependencyManifest.package.name + "'";
          return false;
        }
        if (dependencyManifest.package.version != selectedVersion) {
          error = "registry entry '" + name + "/" + selectedVersion +
                  "' declares version '" +
                  dependencyManifest.package.version + "'";
          return false;
        }
      } else if (!loadDependencyManifest(manifest, dependency,
                                         dependencyManifest, loadError)) {
        error = loadError;
        return false;
      }

      if (!visit(dependencyManifest)) return false;

      ResolvedPackage package;
      package.name = name;
      package.version = dependencyManifest.package.version;
      package.source = source;
      package.dir = canonical.string();
      resolved[name] = package;
      out.packages.push_back(package);
      status = 2;
      stack.pop_back();
    }
    return true;
  };

  if (!visit(root)) return false;

  ResolvedPackage rootPackage;
  rootPackage.name = root.package.name;
  rootPackage.version = root.package.version;
  rootPackage.source = "path+.";
  rootPackage.dir = root.dir;
  rootPackage.root = true;
  out.packages.push_back(std::move(rootPackage));
  return true;
}

Lockfile buildLockfile(const Resolution& resolution) {
  Lockfile lock;
  for (const ResolvedPackage& package : resolution.packages) {
    if (package.root) continue;
    LockedPackage locked;
    locked.name = package.name;
    locked.version = package.version;
    locked.source = package.source;
    lock.packages[package.name] = std::move(locked);
  }
  return lock;
}

bool verifyLockfile(const Manifest& root, const Resolution& resolution,
                    const Lockfile& lock, std::string& error) {
  Lockfile expected = buildLockfile(resolution);

  for (const auto& [name, package] : expected.packages) {
    auto entry = lock.packages.find(name);
    if (entry == lock.packages.end()) {
      error = "the lockfile is missing '" + name + "'";
      return false;
    }
    if (entry->second.version != package.version) {
      error = "the lockfile pins '" + name + "' at " + entry->second.version +
              " but the manifest resolves to " + package.version;
      return false;
    }
    const ResolvedPackage* resolved = resolution.find(name);
    if (resolved == nullptr) {
      error = "the lockfile contains '" + name +
              "', which the manifest no longer resolves";
      return false;
    }
    std::string sourcePath;
    if (stripPathSource(entry->second.source, sourcePath)) {
      if (!samePath(fs::path(root.dir) / sourcePath,
                    fs::path(resolved->dir))) {
        error = "the lockfile points '" + name + "' at '" + sourcePath +
                "', but the manifest resolves it to '" + resolved->dir + "'";
        return false;
      }
    } else if (startsWith(entry->second.source, "registry+")) {
      if (entry->second.source != package.source) {
        error = "the lockfile source for '" + name + "' is '" +
                entry->second.source + "', but the manifest resolves it to '" +
                package.source + "'";
        return false;
      }
    } else {
      error = "the lockfile source for '" + name +
              "' is neither a path nor a registry source";
      return false;
    }
  }

  for (const auto& [name, package] : lock.packages) {
    (void)package;
    if (expected.packages.count(name) == 0) {
      error = "the lockfile contains '" + name +
              "', which the manifest no longer depends on";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Paths

std::string absolutePath(const std::string& path) {
  std::error_code ec;
  fs::path absolute = fs::absolute(path, ec);
  if (ec) return path;
  return absolute.lexically_normal().string();
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return (fs::path(a) / b).string();
}

std::string normalizeSlashes(const std::string& path) {
  std::string out = path;
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}

std::vector<std::string> listSourceFiles(const std::string& dir) {
  std::vector<std::string> files;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return files;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".oskl") {
      files.push_back(it->path().string());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// ---------------------------------------------------------------------------
// Processes and the toolchain

namespace {

std::string executableDirectory(const std::string& argv0) {
  fs::path invoked(argv0);
  if (invoked.has_parent_path()) {
    std::error_code ec;
    fs::path absolute = fs::absolute(invoked, ec);
    if (ec) absolute = invoked;
    return absolute.parent_path().string();
  }
#ifdef _WIN32
  char buffer[MAX_PATH];
  DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return fs::path(std::string(buffer, length)).parent_path().string();
  }
#elif defined(__linux__)
  char buffer[4096];
  ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length > 0) {
    return fs::path(std::string(buffer, static_cast<size_t>(length)))
        .parent_path()
        .string();
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size + 1);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    std::error_code ec;
    fs::path path = fs::weakly_canonical(buffer.data(), ec);
    if (ec) path = fs::path(buffer.data());
    return path.parent_path().string();
  }
#endif
  return std::string();
}

std::string withExeSuffix(const std::string& name) {
#ifdef _WIN32
  if (name.size() < 4 || name.compare(name.size() - 4, 4, ".exe") != 0) {
    return name + ".exe";
  }
#endif
  return name;
}

bool isRegularFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

}  // namespace

bool findExecutable(const std::string& argv0, const std::string& name,
                    std::string& out) {
  fs::path direct(name);
  if (direct.has_parent_path()) {
    if (isRegularFile(direct)) {
      out = direct.string();
      return true;
    }
    return false;
  }

  std::string directory = executableDirectory(argv0);
  if (!directory.empty()) {
    fs::path candidate = fs::path(directory) / withExeSuffix(name);
    if (isRegularFile(candidate)) {
      out = candidate.string();
      return true;
    }
  }

  const char* pathEnv = std::getenv("PATH");
  if (pathEnv == nullptr) return false;
#ifdef _WIN32
  const char separator = ';';
#else
  const char separator = ':';
#endif
  std::string path(pathEnv);
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(separator, start);
    std::string entry = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    // Windows allows a quoted PATH entry (`"C:\Program Files\..."`).
    if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
      entry = entry.substr(1, entry.size() - 2);
    }
    if (!entry.empty()) {
      fs::path candidate = fs::path(entry) / withExeSuffix(name);
      if (isRegularFile(candidate)) {
        out = candidate.string();
        return true;
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

bool findTool(const std::string& argv0, const std::string& name,
              std::string& out) {
  const char* override = nullptr;
  if (name == "oskel") override = std::getenv("OSKEL");
  if (name == "oskelc") override = std::getenv("OSKELC");
  if (override != nullptr && *override != '\0') {
    out = override;
    return true;
  }
  return findExecutable(argv0, name, out);
}

#ifdef _WIN32

namespace {

std::string quoteWindowsArgument(const std::string& argument) {
  if (argument.empty()) return "\"\"";
  bool needsQuotes = argument.find_first_of(" \t\n\v\"") != std::string::npos;
  if (!needsQuotes) return argument;
  std::string result = "\"";
  for (size_t i = 0; i < argument.size(); ++i) {
    size_t backslashes = 0;
    while (i < argument.size() && argument[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == argument.size()) {
      result.append(backslashes * 2, '\\');
      break;
    }
    if (argument[i] == '"') {
      result.append(backslashes * 2 + 1, '\\');
      result += '"';
    } else {
      result.append(backslashes, '\\');
      result += argument[i];
    }
  }
  result += '"';
  return result;
}

}  // namespace

#endif

int runProcess(const std::vector<std::string>& argv,
               const std::vector<std::pair<std::string, std::string>>& env,
               const std::string& workingDir) {
  if (argv.empty()) return 127;

#ifndef _WIN32
  pid_t pid = fork();
  if (pid < 0) return 127;
  if (pid == 0) {
    if (!workingDir.empty() && chdir(workingDir.c_str()) != 0) _exit(127);
    for (const auto& [key, value] : env) {
      setenv(key.c_str(), value.c_str(), 1);
    }
    std::vector<char*> arguments;
    arguments.reserve(argv.size() + 1);
    for (const std::string& argument : argv) {
      arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);
    execvp(arguments[0], arguments.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return 127;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 127;
#else
  std::string commandLine;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) commandLine += ' ';
    commandLine += quoteWindowsArgument(argv[i]);
  }
  std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back('\0');

  std::vector<std::pair<std::string, std::string>> previous;
  for (const auto& [key, value] : env) {
    char buffer[32767];
    DWORD length = GetEnvironmentVariableA(key.c_str(), buffer, sizeof(buffer));
    previous.emplace_back(
        key, length > 0 && length < sizeof(buffer)
                 ? std::string(buffer, length)
                 : std::string());
    SetEnvironmentVariableA(key.c_str(), value.c_str());
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  BOOL started = CreateProcessA(
      nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr,
      workingDir.empty() ? nullptr : workingDir.c_str(), &startup, &process);

  for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
    SetEnvironmentVariableA(it->first.c_str(),
                            it->second.empty() ? nullptr : it->second.c_str());
  }

  if (!started) return 127;
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 127;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return static_cast<int>(code);
#endif
}

// ---------------------------------------------------------------------------
// Project preparation

namespace {

std::vector<std::pair<std::string, std::string>> projectEnvironment(
    const Manifest& manifest) {
  return {
      {"OSKEL_MODULE_ROOT", (fs::path(manifest.dir) / "src").string()},
      {"OSKEL_PACKAGES", (fs::path(manifest.dir) / ".oskel" / "deps").string()},
  };
}

void printWarnings(const Manifest& manifest) {
  for (const std::string& warning : manifest.warnings) {
    reportWarning(warning);
  }
}

bool materializeDependencies(const Manifest& root, const Resolution& resolution,
                             std::string& error) {
  fs::path dependenciesDir = fs::path(root.dir) / ".oskel" / "deps";
  std::error_code ec;
  fs::remove_all(dependenciesDir, ec);
  if (ec) {
    error = "cannot remove '" + dependenciesDir.string() + "': " + ec.message();
    return false;
  }
  fs::create_directories(dependenciesDir, ec);
  if (ec) {
    error = "cannot create '" + dependenciesDir.string() + "': " + ec.message();
    return false;
  }
  for (const ResolvedPackage& package : resolution.packages) {
    if (package.root) continue;
    fs::path source = fs::path(package.dir) / "src" / package.name;
    if (!fs::is_directory(source, ec)) {
      if (ec) {
        error = "cannot read '" + source.string() + "': " + ec.message();
      } else {
        error = "package '" + package.name + "' has no '" + source.string() +
                "'; a package's modules must live in src/<package>/";
      }
      return false;
    }
    fs::path destination = dependenciesDir / package.name;
    ec.clear();
    fs::copy(source, destination,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
      error = "cannot copy '" + source.string() + "' to '" +
              destination.string() + "': " + ec.message();
      return false;
    }
  }
  return true;
}

bool writeLockfile(const Manifest& manifest, const Resolution& resolution,
                   std::string& error) {
  Lockfile lock = buildLockfile(resolution);
  return writeTextFile(fs::path(manifest.dir) / "oskel.lock",
                       renderLockfile(lock), error);
}

// Loads the manifest, resolves the graph, checks or writes the lockfile and
// materialises the dependencies. Returns an exit code; `error` is set on
// failure.
int prepareProject(const std::string& manifestPath, const BuildOptions& options,
                   Manifest& manifest, Resolution& resolution,
                   std::string& error) {
  if (!loadManifest(manifestPath, manifest, error)) return kIoError;
  printWarnings(manifest);

  std::map<std::string, std::string> preferred;
  if (!loadPreferredVersions(manifest, preferred, error)) return kIoError;
  if (!resolveDependencies(manifest, resolution, error, preferred)) {
    return kFailure;
  }

  if (options.locked) {
    fs::path lockPath = fs::path(manifest.dir) / "oskel.lock";
    std::string source;
    if (!readSourceFile(lockPath.string(), source)) {
      error = "--locked requires oskel.lock; run `oskelp build` once without it";
      return kFailure;
    }
    Lockfile lock;
    std::string lockError;
    if (!parseLockfile(source, lock, lockError)) {
      error = "oskel.lock:" + lockError;
      return kIoError;
    }
    if (!verifyLockfile(manifest, resolution, lock, error)) return kFailure;
  } else if (!writeLockfile(manifest, resolution, error)) {
    return kIoError;
  }

  if (!materializeDependencies(manifest, resolution, error)) return kFailure;
  return kOk;
}

bool executableIsWindows(const std::string& target) {
  if (!target.empty()) {
    return target.find("windows") != std::string::npos;
  }
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

std::string executableName(const std::string& name, const std::string& target) {
  return executableIsWindows(target) ? name + ".exe" : name;
}

// `oskelc` is the compiler; when it is not installed, `oskel build` is the same
// pipeline behind the development engine.
bool compilerInvocation(const std::string& argv0, std::string& executable,
                        std::vector<std::string>& prefix, std::string& error) {
  if (findTool(argv0, "oskelc", executable)) {
    prefix.clear();
    return true;
  }
  if (findTool(argv0, "oskel", executable)) {
    prefix = {"build"};
    return true;
  }
  error = "cannot find 'oskelc' or 'oskel'; set $OSKELC or $OSKEL";
  return false;
}

std::string targetBinary(const Manifest& manifest, const BuildOptions& options) {
  return (fs::path(manifest.dir) / "target" /
          executableName(manifest.package.name, options.target))
      .string();
}

int buildProject(const Manifest& manifest, const BuildOptions& options,
                 const std::string& argv0) {
  std::string error;
  std::string compiler;
  std::vector<std::string> prefix;
  if (!compilerInvocation(argv0, compiler, prefix, error)) {
    reportError(error);
    return kFailure;
  }

  fs::path output = targetBinary(manifest, options);
  std::error_code ec;
  fs::create_directories(output.parent_path(), ec);
  if (ec) {
    reportError("cannot create '" + output.parent_path().string() + "'");
    return kIoError;
  }

  std::vector<std::string> argv = {compiler};
  argv.insert(argv.end(), prefix.begin(), prefix.end());
  argv.push_back(manifest.package.entry);
  argv.push_back("-o");
  argv.push_back(output.string());
  if (!options.target.empty()) {
    argv.push_back("--target");
    argv.push_back(options.target);
  }
  if (!options.mode.empty()) {
    argv.push_back("--mode");
    argv.push_back(options.mode);
  }

  int child = runProcess(argv, projectEnvironment(manifest), manifest.dir);
  if (child != 0) {
    reportError("the compiler exited with status " + std::to_string(child));
    return child;
  }
  std::cout << "built " << relativeDisplay(manifest.dir, output) << "\n";
  return kOk;
}

std::string mainTemplate(const std::string& packageName) {
  return "// Entry point for the `" + packageName +
         "` package.\n"
         "// Run it with `oskelp run`; build a native binary with `oskelp build`.\n\n"
         "import { message } from " + packageName + ".greeting;\n\n"
         "print(message(\"world\"));\n";
}

std::string greetingTemplate() {
  return "// A module of the package. It is imported as `<package>.greeting`.\n\n"
         "fn message(name) {\n"
         "  return \"Hello, ${name}!\";\n"
         "}\n";
}

std::string testTemplate(const std::string& packageName) {
  return "// Run with `oskelp test`. Exit 0 means the test passed.\n\n"
         "import { message } from " + packageName + ".greeting;\n\n"
         "let text = message(\"test\");\n"
         "if (text != \"Hello, test!\") {\n"
         "  print(\"expected 'Hello, test!', got '${text}'\");\n"
         "  exit(1);\n"
         "}\n"
         "print(\"smoke: ok\");\n";
}

bool writeIfMissing(const fs::path& path, const std::string& text,
                    std::string& error) {
  std::error_code ec;
  if (fs::exists(path, ec)) return true;
  return writeTextFile(path, text, error);
}

bool initializeRepository(const std::string& argv0, const fs::path& root) {
  std::string git;
  if (!findExecutable(argv0, "git", git)) {
    reportWarning("git not found; the project was created without a repository");
    return false;
  }
  std::error_code ec;
  fs::path absoluteRoot = fs::absolute(root, ec);
  if (ec) absoluteRoot = root;
  if (runProcess({git, "init", "-q"}, {}, absoluteRoot.string()) != 0) {
    reportWarning("git init failed; the project was created without a repository");
    return false;
  }
  return true;
}

bool validVcs(const std::string& vcs) {
  return vcs.empty() || vcs == "none" || vcs == "git";
}

}  // namespace

// ---------------------------------------------------------------------------
// Commands

int commandNew(const std::string& directory, const std::string& name,
               const std::string& vcs, const std::string& argv0) {
  if (!validVcs(vcs)) {
    reportError("unknown --vcs '" + vcs + "' (supported: none, git)");
    return kUsage;
  }
  fs::path root(directory);
  std::error_code ec;
  if (fs::exists(root, ec)) {
    if (!fs::is_directory(root, ec) || !fs::is_empty(root, ec)) {
      reportError("directory '" + directory +
                  "' already exists and is not empty");
      return kFailure;
    }
  }

  std::string packageName = name.empty() ? derivePackageName(directory) : name;
  if (!isIdentifier(packageName)) {
    reportError("'" + packageName + "' is not a valid package name; use --name to choose one");
    return kFailure;
  }

  Package package;
  package.name = packageName;
  package.version = "0.1.0";
  package.entry = "src/" + packageName + "/main.oskl";

  fs::create_directories(root / "src" / packageName, ec);
  if (!ec) fs::create_directories(root / "tests", ec);
  if (ec) {
    reportError("cannot create '" + directory + "'");
    return kIoError;
  }

  std::string error;
  if (!writeTextFile(root / "oskel.toml", renderManifest(package), error) ||
      !writeTextFile(root / package.entry, mainTemplate(packageName), error) ||
      !writeTextFile(root / "src" / packageName / "greeting.oskl",
                     greetingTemplate(), error) ||
      !writeTextFile(root / "tests" / "smoke.oskl", testTemplate(packageName),
                     error) ||
      !writeTextFile(root / ".gitignore", "/target\n/.oskel\n", error)) {
    reportError(error);
    return kIoError;
  }

  if (vcs == "git") initializeRepository(argv0, root);

  std::cout << "created " << directory << " (package " << packageName << ")\n";
  return kOk;
}

int commandInit(const std::string& directory, const std::string& name,
                const std::string& vcs, const std::string& argv0) {
  if (!validVcs(vcs)) {
    reportError("unknown --vcs '" + vcs + "' (supported: none, git)");
    return kUsage;
  }
  fs::path root = directory.empty() ? fs::path(".") : fs::path(directory);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    reportError("'" + root.string() + "' is not a directory");
    return kIoError;
  }
  if (fs::exists(root / "oskel.toml", ec)) {
    reportError("'" + (root / "oskel.toml").string() + "' already exists");
    return kFailure;
  }

  std::string baseName = fs::absolute(root, ec).filename().string();
  if (baseName.empty() || baseName == "." || baseName == "..") baseName = "app";
  std::string packageName = name.empty() ? derivePackageName(baseName) : name;
  if (!isIdentifier(packageName)) {
    reportError("'" + packageName + "' is not a valid package name; use --name to choose one");
    return kFailure;
  }

  Package package;
  package.name = packageName;
  package.version = "0.1.0";
  package.entry = "src/" + packageName + "/main.oskl";

  fs::create_directories(root / "src" / packageName, ec);
  if (!ec) fs::create_directories(root / "tests", ec);
  if (ec) {
    reportError("cannot create the project layout in '" + root.string() + "'");
    return kIoError;
  }

  std::string error;
  if (!writeTextFile(root / "oskel.toml", renderManifest(package), error) ||
      !writeIfMissing(root / package.entry, mainTemplate(packageName), error) ||
      !writeIfMissing(root / "src" / packageName / "greeting.oskl",
                      greetingTemplate(), error) ||
      !writeIfMissing(root / "tests" / "smoke.oskl", testTemplate(packageName),
                      error) ||
      !writeIfMissing(root / ".gitignore", "/target\n/.oskel\n", error)) {
    reportError(error);
    return kIoError;
  }

  if (vcs == "git") initializeRepository(argv0, root);

  std::cout << "initialized " << root.string() << " (package " << packageName
            << ")\n";
  return kOk;
}

int commandAdd(const std::string& manifestPath, const std::string& name,
               const std::string& path, const std::string& version) {
  Manifest manifest;
  std::string error;
  if (!loadManifest(manifestPath, manifest, error)) {
    reportError(error);
    return kIoError;
  }
  printWarnings(manifest);

  if (path.empty()) {
    if (name.empty()) {
      reportError(
          "a registry dependency needs a name: oskelp add <name> --version "
          "<requirement>");
      return kUsage;
    }
    if (!isIdentifier(name)) {
      reportError("'" + name + "' is not a valid dependency name");
      return kUsage;
    }
    if (registryRoot().empty()) {
      reportError(
          "a registry dependency needs OSKEL_REGISTRY to name the registry "
          "directory");
      return kFailure;
    }
    std::string requirement = version;
    if (requirement.empty()) {
      std::string latest;
      if (!latestRegistryVersion(name, latest, false, error)) {
        reportError(error);
        return kFailure;
      }
      requirement = "^" + latest;
    }
    if (!isValidRequirement(requirement)) {
      reportError("version requirement '" + requirement +
                  "' is not supported; use a version, ^, ~, a comparator or "
                  "a wildcard");
      return kUsage;
    }
    if (name == manifest.package.name) {
      reportError("a package cannot depend on itself");
      return kFailure;
    }

    Dependency declaration;
    declaration.name = name;
    declaration.requirement = requirement;

    bool updating = false;
    auto existing = manifest.package.dependencies.find(name);
    if (existing != manifest.package.dependencies.end()) {
      if (!existing->second.isRegistry()) {
        reportError("dependency '" + name +
                    "' is already declared as a path dependency; remove it "
                    "first");
        return kFailure;
      }
      if (existing->second.requirement == declaration.requirement) {
        std::cout << "dependency '" << name << "' is already present\n";
        return kOk;
      }
      updating = true;
    }

    std::string source;
    if (!readSourceFile(manifest.path, source)) {
      reportError("cannot read '" + manifest.path + "'");
      return kIoError;
    }
    std::string edited = source;
    if (updating) {
      std::string without;
      if (!removeDependencyFromManifest(edited, name, without, error)) {
        reportError(error);
        return kFailure;
      }
      edited = without;
    }
    std::string added;
    if (!addDependencyToManifest(edited, declaration, added, error)) {
      reportError(error);
      return kFailure;
    }
    edited = added;

    Manifest updated = manifest;
    updated.package.dependencies[name] = declaration;
    Resolution resolution;
    std::map<std::string, std::string> preferred;
    if (!loadPreferredVersions(updated, preferred, error)) {
      reportError(error);
      return kIoError;
    }
    if (!resolveDependencies(updated, resolution, error, preferred)) {
      reportError(error);
      return kFailure;
    }

    if (!writeTextFile(manifest.path, edited, error) ||
        !writeLockfile(updated, resolution, error) ||
        !materializeDependencies(updated, resolution, error)) {
      reportError(error);
      return kIoError;
    }

    std::cout << (updating ? "updated '" : "added '") << name << "' ("
              << declaration.requirement << ")\n";
    return kOk;
  }

  if (!name.empty() && !isIdentifier(name)) {
    reportError("'" + name + "' is not a valid dependency name");
    return kUsage;
  }
  if (!version.empty() && !isSemver(version)) {
    reportError("version '" + version + "' is not 'major.minor.patch'");
    return kUsage;
  }

  std::error_code ec;
  fs::path dependencyDir(path);
  if (dependencyDir.is_relative()) {
    fs::path cwd = fs::current_path(ec);
    if (!ec) dependencyDir = cwd / dependencyDir;
  }
  ec.clear();
  fs::path dependency = fs::weakly_canonical(dependencyDir, ec);
  if (ec) dependency = dependencyDir.lexically_normal();
  ec.clear();
  fs::path project = fs::weakly_canonical(fs::path(manifest.dir), ec);
  if (ec) project = fs::path(manifest.dir).lexically_normal();

  if (samePath(project, dependency)) {
    reportError("a package cannot depend on itself");
    return kFailure;
  }
  ec.clear();
  if (!fs::is_regular_file(dependency / "oskel.toml", ec)) {
    reportError("'" + path + "' has no oskel.toml");
    return kFailure;
  }

  Manifest dependencyManifest;
  if (!loadManifest((dependency / "oskel.toml").string(), dependencyManifest,
                    error)) {
    reportError("dependency: " + error);
    return kFailure;
  }
  std::string dependencyName =
      name.empty() ? dependencyManifest.package.name : name;
  if (dependencyManifest.package.name != dependencyName) {
    reportError("the package at '" + path + "' is named '" +
                dependencyManifest.package.name + "', not '" + dependencyName +
                "'");
    return kFailure;
  }
  if (!version.empty() && dependencyManifest.package.version != version) {
    reportError("dependency '" + dependencyName + "' is version '" +
                dependencyManifest.package.version + "', not '" + version +
                "'");
    return kFailure;
  }

  ec.clear();
  fs::path relative = fs::relative(dependency, project, ec);
  Dependency declaration;
  declaration.name = dependencyName;
  if (ec || relative.empty()) {
    reportWarning("dependency '" + dependencyName +
                  "' is outside the project tree; storing its absolute path");
    declaration.path = normalizeSlashes(dependency.generic_string());
  } else {
    declaration.path = normalizeSlashes(relative.generic_string());
  }
  declaration.version = version;

  bool updating = false;
  auto existing = manifest.package.dependencies.find(dependencyName);
  if (existing != manifest.package.dependencies.end()) {
    if (existing->second.isRegistry()) {
      reportError("dependency '" + dependencyName +
                  "' is already declared as a registry dependency; remove it "
                  "first");
      return kFailure;
    }
    fs::path existingDir = fs::path(manifest.dir) / existing->second.path;
    if (!samePath(existingDir, dependency)) {
      reportError("dependency '" + dependencyName + "' is already declared at '" +
                  existing->second.path + "'; remove it first");
      return kFailure;
    }
    if (existing->second.version == declaration.version) {
      std::cout << "dependency '" << dependencyName << "' is already present\n";
      return kOk;
    }
    updating = true;
  }

  std::string source;
  if (!readSourceFile(manifest.path, source)) {
    reportError("cannot read '" + manifest.path + "'");
    return kIoError;
  }
  std::string edited = source;
  if (updating) {
    std::string without;
    if (!removeDependencyFromManifest(edited, dependencyName, without, error)) {
      reportError(error);
      return kFailure;
    }
    edited = without;
  }
  std::string added;
  if (!addDependencyToManifest(edited, declaration, added, error)) {
    reportError(error);
    return kFailure;
  }
  edited = added;

  Manifest updated = manifest;
  updated.package.dependencies[dependencyName] = declaration;
  Resolution resolution;
  std::map<std::string, std::string> preferred;
  if (!loadPreferredVersions(updated, preferred, error)) {
    reportError(error);
    return kIoError;
  }
  if (!resolveDependencies(updated, resolution, error, preferred)) {
    reportError(error);
    return kFailure;
  }

  if (!writeTextFile(manifest.path, edited, error) ||
      !writeLockfile(updated, resolution, error) ||
      !materializeDependencies(updated, resolution, error)) {
    reportError(error);
    return kIoError;
  }

  std::cout << (updating ? "updated '" : "added '") << dependencyName
            << "' (path " << declaration.path << ")\n";
  return kOk;
}

int commandRemove(const std::string& manifestPath, const std::string& name) {
  if (!isIdentifier(name)) {
    reportError("'" + name + "' is not a valid dependency name");
    return kUsage;
  }
  Manifest manifest;
  std::string error;
  if (!loadManifest(manifestPath, manifest, error)) {
    reportError(error);
    return kIoError;
  }
  printWarnings(manifest);

  if (manifest.package.dependencies.find(name) ==
      manifest.package.dependencies.end()) {
    reportError("dependency '" + name + "' is not declared");
    return kFailure;
  }

  std::string source;
  if (!readSourceFile(manifest.path, source)) {
    reportError("cannot read '" + manifest.path + "'");
    return kIoError;
  }
  std::string edited;
  if (!removeDependencyFromManifest(source, name, edited, error)) {
    reportError(error);
    return kFailure;
  }

  Manifest updated = manifest;
  updated.package.dependencies.erase(name);
  Resolution resolution;
  std::map<std::string, std::string> preferred;
  if (!loadPreferredVersions(updated, preferred, error)) {
    reportError(error);
    return kIoError;
  }
  if (!resolveDependencies(updated, resolution, error, preferred)) {
    reportError(error);
    return kFailure;
  }

  if (!writeTextFile(manifest.path, edited, error) ||
      !writeLockfile(updated, resolution, error) ||
      !materializeDependencies(updated, resolution, error)) {
    reportError(error);
    return kIoError;
  }

  std::cout << "removed '" << name << "'\n";
  return kOk;
}

int commandPublish(const std::string& manifestPath,
                   const std::string& registry) {
  Manifest manifest;
  std::string error;
  if (!loadManifest(manifestPath, manifest, error)) {
    reportError(error);
    return kIoError;
  }
  printWarnings(manifest);

  std::string root = registry.empty() ? registryRoot() : registry;
  if (root.empty()) {
    reportError(
        "publish needs a registry: set OSKEL_REGISTRY or pass --registry "
        "<directory>");
    return kFailure;
  }
  for (const auto& [name, dependency] : manifest.package.dependencies) {
    if (name == manifest.package.name) {
      reportError("cannot publish a package that depends on itself");
      return kFailure;
    }
    if (!dependency.isRegistry()) {
      reportError("cannot publish '" + manifest.package.name +
                  "' with path dependency '" + name +
                  "'; publish it and depend on a version");
      return kFailure;
    }
  }

  std::error_code ec;
  fs::path entryPath(manifest.package.entry);
  auto firstComponent = entryPath.begin();
  if (entryPath.is_absolute() || firstComponent == entryPath.end() ||
      *firstComponent != fs::path("src")) {
    reportError("publish supports only an entry under 'src/'; got '" +
                manifest.package.entry + "'");
    return kFailure;
  }
  if (!fs::is_regular_file(fs::path(manifest.dir) / entryPath, ec)) {
    reportError("entry '" + manifest.package.entry + "' does not exist");
    return kFailure;
  }
  fs::path sourceRoot = fs::path(manifest.dir) / "src" / manifest.package.name;
  if (!fs::is_directory(sourceRoot, ec)) {
    reportError("the package has no 'src/" + manifest.package.name +
                "' directory");
    return kFailure;
  }

  fs::path target = fs::path(absolutePath(root)) / manifest.package.name /
                    manifest.package.version;
  ec.clear();
  bool targetExists = fs::exists(target, ec);
  if (ec) {
    reportError("cannot inspect '" + target.string() + "': " + ec.message());
    return kIoError;
  }
  if (targetExists) {
    reportError("'" + manifest.package.name + "' " + manifest.package.version +
                " is already published at '" + target.string() + "'");
    return kFailure;
  }
  auto abortPublish = [&](const std::string& message) {
    std::error_code cleanup;
    fs::remove_all(target, cleanup);
    reportError(message);
    return kIoError;
  };
  ec.clear();
  fs::create_directories(target, ec);
  if (ec) {
    return abortPublish("cannot create '" + target.string() + "': " +
                        ec.message());
  }
  ec.clear();
  fs::copy_file(manifest.path, target / "oskel.toml",
                fs::copy_options::overwrite_existing, ec);
  if (ec) {
    return abortPublish("cannot copy the manifest to '" + target.string() +
                        "': " + ec.message());
  }
  ec.clear();
  fs::copy(fs::path(manifest.dir) / "src", target / "src",
           fs::copy_options::recursive | fs::copy_options::overwrite_existing,
           ec);
  if (ec) {
    return abortPublish("cannot copy 'src' to '" + target.string() + "': " +
                        ec.message());
  }

  std::cout << "published " << manifest.package.name << " "
            << manifest.package.version << " to " << target.string() << "\n";
  return kOk;
}

int commandYank(const std::string& name, const std::string& version,
                const std::string& registry, bool undo) {
  if (!isIdentifier(name)) {
    reportError("'" + name + "' is not a valid package name");
    return kUsage;
  }
  if (!isSemver(version)) {
    reportError("version '" + version + "' is not a semantic version");
    return kUsage;
  }
  std::string root = registry.empty() ? registryRoot() : registry;
  if (root.empty()) {
    reportError(
        "yank needs a registry: set OSKEL_REGISTRY or pass --registry "
        "<directory>");
    return kFailure;
  }

  fs::path directory = fs::path(root) / name / version;
  std::error_code ec;
  if (!fs::is_directory(directory, ec)) {
    reportError("the registry at '" + root + "' has no '" + name +
                "' version '" + version + "'");
    return kFailure;
  }
  Manifest published;
  std::string error;
  if (!loadManifest((directory / "oskel.toml").string(), published, error)) {
    reportError("registry entry '" + name + "/" + version + "': " + error);
    return kFailure;
  }
  if (published.package.name != name || published.package.version != version) {
    reportError("registry entry '" + name + "/" + version +
                "' declares '" + published.package.name + " " +
                published.package.version + "'");
    return kFailure;
  }

  fs::path marker = directory / ".yanked";
  bool yanked = fs::is_regular_file(marker, ec);
  if (undo) {
    if (!yanked) {
      std::cout << name << " " << version << " is not yanked\n";
      return kOk;
    }
    ec.clear();
    fs::remove(marker, ec);
    if (ec) {
      reportError("cannot remove '" + marker.string() + "': " + ec.message());
      return kIoError;
    }
    std::cout << "unyanked " << name << " " << version << "\n";
    return kOk;
  }

  if (yanked) {
    std::cout << name << " " << version << " is already yanked\n";
    return kOk;
  }
  if (!writeTextFile(marker, "", error)) {
    reportError(error);
    return kIoError;
  }
  std::cout << "yanked " << name << " " << version << "\n";
  return kOk;
}

int commandUpdate(const std::string& manifestPath, const std::string& name,
                  const std::string& precise) {
  if (!name.empty() && !isIdentifier(name)) {
    reportError("'" + name + "' is not a valid dependency name");
    return kUsage;
  }
  if (name.empty() && !precise.empty()) {
    reportError("--precise needs a package name");
    return kUsage;
  }
  if (!precise.empty() && !isSemver(precise)) {
    reportError("version '" + precise + "' is not a semantic version");
    return kUsage;
  }
  Manifest manifest;
  std::string error;
  if (!loadManifest(manifestPath, manifest, error)) {
    reportError(error);
    return kIoError;
  }
  printWarnings(manifest);

  Lockfile previous;
  std::map<std::string, std::string> preferred;
  fs::path lockPath = fs::path(manifest.dir) / "oskel.lock";
  std::string source;
  if (readSourceFile(lockPath.string(), source)) {
    std::string lockError;
    if (!parseLockfile(source, previous, lockError)) {
      reportError("oskel.lock:" + lockError);
      return kIoError;
    }
    for (const auto& [packageName, package] : previous.packages) {
      preferred[packageName] = package.version;
    }
  }

  std::map<std::string, std::string> forced;
  if (!name.empty()) {
    if (manifest.package.dependencies.count(name) == 0 &&
        preferred.count(name) == 0) {
      reportError("package '" + name + "' is not in the dependency graph");
      return kFailure;
    }
    if (!precise.empty()) {
      auto direct = manifest.package.dependencies.find(name);
      bool registryDependency = false;
      if (direct != manifest.package.dependencies.end()) {
        registryDependency = direct->second.isRegistry();
      } else {
        auto locked = previous.packages.find(name);
        registryDependency = locked != previous.packages.end() &&
                             locked->second.source.rfind("registry+", 0) == 0;
      }
      if (!registryDependency) {
        reportError("package '" + name +
                    "' is a path dependency; --precise only applies to a "
                    "registry dependency");
        return kFailure;
      }
      forced[name] = precise;
    } else {
      preferred.erase(name);
    }
  } else {
    preferred.clear();
  }

  Resolution resolution;
  if (!resolveDependencies(manifest, resolution, error, preferred, forced)) {
    reportError(error);
    return kFailure;
  }
  if (!precise.empty()) {
    const ResolvedPackage* selected = resolution.find(name);
    if (selected == nullptr || selected->version != precise ||
        selected->source.rfind("registry+", 0) != 0) {
      reportError("package '" + name +
                  "' did not resolve to registry version '" + precise + "'");
      return kFailure;
    }
  }
  if (!writeLockfile(manifest, resolution, error) ||
      !materializeDependencies(manifest, resolution, error)) {
    reportError(error);
    return kIoError;
  }

  Lockfile current = buildLockfile(resolution);
  bool changed = false;
  for (const auto& [packageName, package] : current.packages) {
    auto old = previous.packages.find(packageName);
    if (old == previous.packages.end()) {
      std::cout << "added " << packageName << " " << package.version << "\n";
      changed = true;
    } else if (old->second.version != package.version ||
               old->second.source != package.source) {
      std::cout << "updated " << packageName << " " << old->second.version
                << " -> " << package.version << "\n";
      changed = true;
    }
  }
  for (const auto& [packageName, package] : previous.packages) {
    (void)package;
    if (current.packages.count(packageName) == 0) {
      std::cout << "removed " << packageName << "\n";
      changed = true;
    }
  }
  if (!changed) std::cout << "everything is up to date\n";
  return kOk;
}

int commandOutdated(const std::string& manifestPath) {
  Manifest manifest;
  std::string error;
  if (!loadManifest(manifestPath, manifest, error)) {
    reportError(error);
    return kIoError;
  }
  printWarnings(manifest);

  std::map<std::string, std::string> preferred;
  if (!loadPreferredVersions(manifest, preferred, error)) {
    reportError(error);
    return kIoError;
  }
  Resolution resolution;
  if (!resolveDependencies(manifest, resolution, error, preferred)) {
    reportError(error);
    return kFailure;
  }

  bool any = false;
  for (const ResolvedPackage& package : resolution.packages) {
    if (package.root || package.source.rfind("registry+", 0) != 0) continue;
    SemVer current;
    if (!parseSemVer(package.version, current)) continue;

    const bool currentYanked = isRegistryVersionYanked(
        package.name, package.version, registryRoot());
    std::string latestText;
    bool haveLatest = latestRegistryVersion(
        package.name, latestText, !current.prerelease.empty(), error);
    if (!haveLatest && !currentYanked) {
      reportError(error);
      return kFailure;
    }

    const auto direct = manifest.package.dependencies.find(package.name);
    const bool isDirect = direct != manifest.package.dependencies.end() &&
                          direct->second.isRegistry();
    if (currentYanked) {
      any = true;
      std::cout << package.name << " " << package.version << " (yanked";
      if (haveLatest) {
        SemVer latest;
        parseSemVer(latestText, latest);
        if (compareSemVer(latest, current) > 0) {
          std::cout << "; latest " << latestText;
          if (isDirect) std::cout << " needs a new requirement";
        }
      }
      std::cout << ")\n";
      continue;
    }

    SemVer latest;
    if (!parseSemVer(latestText, latest)) continue;
    std::string targetText = latestText;
    std::string note;
    if (isDirect) {
      std::string compatibleText;
      fs::path ignored;
      if (!selectRegistryVersion(direct->second, {}, {}, compatibleText,
                                 ignored, error)) {
        reportError(error);
        return kFailure;
      }
      targetText = compatibleText;
      SemVer compatible;
      parseSemVer(compatibleText, compatible);
      if (compareSemVer(compatible, latest) < 0) {
        note = "; latest " + latestText + " needs a new requirement";
      }
    }

    SemVer target;
    parseSemVer(targetText, target);
    if (compareSemVer(target, current) > 0) {
      any = true;
      std::cout << package.name << " " << package.version << " -> "
                << targetText;
      if (isDirect) std::cout << " (" << direct->second.requirement << ")";
      std::cout << note << "\n";
    } else if (compareSemVer(latest, current) > 0) {
      any = true;
      std::cout << package.name << " " << package.version << " (latest "
                << latestText;
      if (isDirect) {
        std::cout << " needs a new requirement, current "
                  << direct->second.requirement;
      }
      std::cout << ")\n";
    }
  }
  if (!any) std::cout << "All registry dependencies are up to date.\n";
  return kOk;
}

int commandBuild(const std::string& manifestPath, const BuildOptions& options,
                 const std::string& argv0) {
  Manifest manifest;
  Resolution resolution;
  std::string error;
  int status =
      prepareProject(manifestPath, options, manifest, resolution, error);
  if (status != kOk) {
    reportError(error);
    return status;
  }
  return buildProject(manifest, options, argv0);
}

int commandRun(const std::string& manifestPath, const BuildOptions& options,
               bool native, const std::vector<std::string>& args,
               const std::string& argv0) {
  Manifest manifest;
  Resolution resolution;
  std::string error;
  int status =
      prepareProject(manifestPath, options, manifest, resolution, error);
  if (status != kOk) {
    reportError(error);
    return status;
  }

  if (native) {
    status = buildProject(manifest, options, argv0);
    if (status != kOk) return status;
    std::vector<std::string> argv = {targetBinary(manifest, options)};
    argv.insert(argv.end(), args.begin(), args.end());
    return runProcess(argv, projectEnvironment(manifest), manifest.dir);
  }

  std::string oskel;
  if (!findTool(argv0, "oskel", oskel)) {
    reportError("cannot find 'oskel'; set $OSKEL");
    return kFailure;
  }
  std::vector<std::string> argv = {oskel, manifest.package.entry};
  argv.insert(argv.end(), args.begin(), args.end());
  return runProcess(argv, projectEnvironment(manifest), manifest.dir);
}

int commandCheck(const std::string& manifestPath, const BuildOptions& options,
                 const std::string& argv0) {
  Manifest manifest;
  Resolution resolution;
  std::string error;
  int status =
      prepareProject(manifestPath, options, manifest, resolution, error);
  if (status != kOk) {
    reportError(error);
    return status;
  }

  std::string oskel;
  if (!findTool(argv0, "oskel", oskel)) {
    reportError("cannot find 'oskel'; set $OSKEL");
    return kFailure;
  }
  int child = runProcess({oskel, "-c", manifest.package.entry},
                         projectEnvironment(manifest), manifest.dir);
  if (child == 0) {
    std::cout << "checked " << manifest.package.entry << "\n";
  }
  return child;
}

int commandTest(const std::string& manifestPath, const BuildOptions& options,
                bool native, const std::string& argv0) {
  Manifest manifest;
  Resolution resolution;
  std::string error;
  int status =
      prepareProject(manifestPath, options, manifest, resolution, error);
  if (status != kOk) {
    reportError(error);
    return status;
  }

  fs::path testsDir = fs::path(manifest.dir) / "tests";
  std::vector<std::string> files = listSourceFiles(testsDir.string());
  if (files.empty()) {
    std::cout << "no tests found under "
              << relativeDisplay(manifest.dir, testsDir) << "\n";
    return kOk;
  }

  std::string oskel;
  std::string compiler;
  std::vector<std::string> prefix;
  if (native) {
    if (!compilerInvocation(argv0, compiler, prefix, error)) {
      reportError(error);
      return kFailure;
    }
  } else if (!findTool(argv0, "oskel", oskel)) {
    reportError("cannot find 'oskel'; set $OSKEL");
    return kFailure;
  }

  fs::path binaryDir = fs::path(manifest.dir) / "target" / "tests";
  if (native) {
    std::error_code ec;
    fs::create_directories(binaryDir, ec);
    if (ec) {
      reportError("cannot create '" + binaryDir.string() + "'");
      return kIoError;
    }
  }

  int passed = 0;
  int failed = 0;
  for (const std::string& file : files) {
    std::string display = relativeDisplay(manifest.dir, file);
    std::cout << "test " << display << " ... " << std::flush;
    int child = 0;
    if (native) {
      std::string binaryName;
      for (char c : relativeDisplay(testsDir, file)) {
        if (c == '/' || c == '\\' || c == '.') {
          binaryName += '_';
        } else {
          binaryName += c;
        }
      }
      fs::path binary =
          binaryDir /
          (binaryName + (executableIsWindows(options.target) ? ".exe" : ""));
      std::vector<std::string> argv = {compiler};
      argv.insert(argv.end(), prefix.begin(), prefix.end());
      argv.push_back(file);
      argv.push_back("-o");
      argv.push_back(binary.string());
      if (!options.target.empty()) {
        argv.push_back("--target");
        argv.push_back(options.target);
      }
      if (!options.mode.empty()) {
        argv.push_back("--mode");
        argv.push_back(options.mode);
      }
      child = runProcess(argv, projectEnvironment(manifest), manifest.dir);
      if (child == 0) {
        child = runProcess({binary.string()}, projectEnvironment(manifest),
                           manifest.dir);
      }
    } else {
      child = runProcess({oskel, file}, projectEnvironment(manifest),
                         manifest.dir);
    }
    if (child == 0) {
      std::cout << "ok\n";
      ++passed;
    } else {
      std::cout << "FAILED (exit " << child << ")\n";
      ++failed;
    }
  }

  std::cout << "\ntest result: " << (failed == 0 ? "ok" : "FAILED") << ". "
            << passed << " passed; " << failed << " failed\n";
  return failed == 0 ? kOk : kTestsFailed;
}

int commandClean(const std::string& manifestPath, const std::string& argv0) {
  (void)argv0;
  Manifest manifest;
  std::string error;
  if (!loadManifest(manifestPath, manifest, error)) {
    reportError(error);
    return kIoError;
  }
  std::error_code ec;
  fs::remove_all(fs::path(manifest.dir) / "target", ec);
  if (ec) {
    reportError("cannot remove '" +
                (fs::path(manifest.dir) / "target").string() + "': " +
                ec.message());
    return kIoError;
  }
  fs::remove_all(fs::path(manifest.dir) / ".oskel", ec);
  if (ec) {
    reportError("cannot remove '" +
                (fs::path(manifest.dir) / ".oskel").string() + "': " +
                ec.message());
    return kIoError;
  }
  std::cout << "removed target/ and .oskel/\n";
  return kOk;
}

namespace {

void printTreeNode(const Manifest& manifest, const Resolution& resolution,
                   const std::string& prefix, bool last, bool root,
                   std::set<std::string>& visiting) {
  if (root) {
    std::cout << manifest.package.name << " v" << manifest.package.version
              << "\n";
  } else {
    std::cout << prefix << (last ? "`-- " : "|-- ") << manifest.package.name
              << " v" << manifest.package.version << "\n";
  }
  std::string childPrefix =
      root ? std::string() : prefix + (last ? "    " : "|   ");
  size_t index = 0;
  for (const auto& [name, dependency] : manifest.package.dependencies) {
    (void)dependency;
    ++index;
    bool isLast = index == manifest.package.dependencies.size();
    if (visiting.count(name) != 0) {
      std::cout << childPrefix << (isLast ? "`-- " : "|-- ") << name
                << " (cycle)\n";
      continue;
    }
    visiting.insert(name);
    const ResolvedPackage* resolved = resolution.find(name);
    Manifest child;
    std::string error;
    if (resolved == nullptr ||
        !loadManifest((fs::path(resolved->dir) / "oskel.toml").string(), child,
                      error)) {
      std::cout << childPrefix << (isLast ? "`-- " : "|-- ") << name
                << " (missing)\n";
    } else {
      printTreeNode(child, resolution, childPrefix, isLast, false, visiting);
    }
    visiting.erase(name);
  }
}

}  // namespace

int commandTree(const std::string& manifestPath, const std::string& argv0) {
  (void)argv0;
  Manifest manifest;
  std::string error;
  if (!loadManifest(manifestPath, manifest, error)) {
    reportError(error);
    return kIoError;
  }
  Resolution resolution;
  std::map<std::string, std::string> preferred;
  if (!loadPreferredVersions(manifest, preferred, error)) {
    reportError(error);
    return kIoError;
  }
  if (!resolveDependencies(manifest, resolution, error, preferred)) {
    reportError(error);
    return kFailure;
  }
  std::set<std::string> visiting;
  visiting.insert(manifest.package.name);
  printTreeNode(manifest, resolution, std::string(), true, true, visiting);
  return kOk;
}

}  // namespace oskelp
