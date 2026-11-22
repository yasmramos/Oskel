#include "cxxbind.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cxxbind {
namespace {

std::string trim(const std::string& s) {
  std::size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  std::size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool isIdent(const std::string& s) {
  if (s.empty()) return false;
  if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return true;
}

// Removes a `#`/`//` comment that is not inside a quoted string.
std::string stripComment(const std::string& line) {
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') quoted = !quoted;
    if (quoted) continue;
    if (c == '#') return line.substr(0, i);
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') return line.substr(0, i);
  }
  return line;
}

// Reads the single quoted argument of `key "value"`; returns false when the
// line is not that directive.
bool quotedDirective(const std::string& line, const std::string& key, std::string* out) {
  if (line.compare(0, key.size(), key) != 0) return false;
  std::size_t i = key.size();
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  if (i >= line.size() || line[i] != '"') return false;
  ++i;
  std::string value;
  while (i < line.size() && line[i] != '"') {
    if (line[i] == '\\' && i + 1 < line.size()) {
      value.push_back(line[i + 1]);
      i += 2;
      continue;
    }
    value.push_back(line[i++]);
  }
  if (i >= line.size() || line[i] != '"') return false;
  *out = value;
  return true;
}

struct Type {
  enum class Kind { Unknown, Void, Bool, Int, Int64, UInt32, UInt64, Float, Double, String, Ptr, Class };
  Kind kind = Kind::Unknown;
  std::string name;     // the Oskel class name
  std::string cppName;  // the qualified C++ class name, for a class handle
  bool isConst = false; // `const Class` parameter
};

struct Param {
  std::string name;
  Type type;
};

struct Method {
  std::string name;
  std::vector<Param> params;
  Type ret;
  bool isCtor = false;
  bool isDtor = false;
  bool isConst = false;
};

struct Class {
  std::string name;     // the Oskel class name
  std::string cppName;  // the qualified C++ class name
  std::vector<Method> methods;
};

struct Function {
  std::string name;
  std::string cppName;
  std::vector<Param> params;
  Type ret;
};

struct Bridge {
  std::string header;
  std::string prefix = "oskel_";
  std::vector<Class> classes;
  std::vector<Function> functions;
};

std::string stemOf(const std::string& sourceName) {
  std::string s = sourceName;
  const std::size_t slash = s.find_last_of("/\\");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  const std::size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  return s.empty() ? "bridge" : s;
}

bool parseType(const std::string& text, Type* out, std::string* error) {
  std::string t = trim(text);
  out->isConst = false;
  if (t.compare(0, 6, "const ") == 0) {
    out->isConst = true;
    t = trim(t.substr(6));
  }
  if (t == "void") { out->kind = Type::Kind::Void; return true; }
  if (t == "bool") { out->kind = Type::Kind::Bool; return true; }
  if (t == "int" || t == "int32") { out->kind = Type::Kind::Int; return true; }
  if (t == "int64" || t == "long" || t == "long long") { out->kind = Type::Kind::Int64; return true; }
  if (t == "uint32" || t == "unsigned") { out->kind = Type::Kind::UInt32; return true; }
  if (t == "uint64" || t == "unsigned long" || t == "unsigned long long") {
    out->kind = Type::Kind::UInt64;
    return true;
  }
  if (t == "float") { out->kind = Type::Kind::Float; return true; }
  if (t == "double") { out->kind = Type::Kind::Double; return true; }
  if (t == "string" || t == "cstring") { out->kind = Type::Kind::String; return true; }
  if (t == "ptr" || t == "void*" || t == "void *") { out->kind = Type::Kind::Ptr; return true; }
  if (isIdent(t)) {
    out->kind = Type::Kind::Unknown;
    out->name = t;
    return true;
  }
  *error = "unknown type '" + text + "'";
  return false;
}

bool parseParams(const std::string& text, std::size_t open, std::size_t* close,
                 std::vector<Param>* params, std::string* error) {
  if (open >= text.size() || text[open] != '(') {
    *error = "expected '('";
    return false;
  }
  int depth = 0;
  std::size_t end = std::string::npos;
  for (std::size_t i = open; i < text.size(); ++i) {
    if (text[i] == '(') ++depth;
    if (text[i] == ')') {
      --depth;
      if (depth == 0) { end = i; break; }
    }
  }
  if (end == std::string::npos) {
    *error = "unterminated parameter list";
    return false;
  }
  *close = end;
  const std::string inside = trim(text.substr(open + 1, end - open - 1));
  if (inside.empty()) return true;
  std::size_t start = 0;
  while (start <= inside.size()) {
    const std::size_t comma = inside.find(',', start);
    const std::string part = trim(inside.substr(start, comma == std::string::npos
                                                          ? std::string::npos
                                                          : comma - start));
    const std::size_t colon = part.find(':');
    if (colon == std::string::npos) {
      *error = "parameter '" + part + "' needs 'name: type'";
      return false;
    }
    Param p;
    p.name = trim(part.substr(0, colon));
    if (!isIdent(p.name)) {
      *error = "invalid parameter name '" + p.name + "'";
      return false;
    }
    if (!parseType(part.substr(colon + 1), &p.type, error)) return false;
    params->push_back(p);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return true;
}

bool parseMethod(const std::string& line, Method* m, std::string* error) {
  std::string rest;
  if (line.compare(0, 4, "ctor") == 0 &&
      (line.size() == 4 || std::isspace(static_cast<unsigned char>(line[4])) || line[4] == '(')) {
    m->isCtor = true;
    rest = line.substr(4);
  } else if (line == "dtor") {
    m->isDtor = true;
    return true;
  } else if (line.compare(0, 6, "method") == 0 && line.size() > 6 &&
             std::isspace(static_cast<unsigned char>(line[6]))) {
    rest = trim(line.substr(6));
    const std::size_t open = rest.find('(');
    if (open == std::string::npos) {
      *error = "method needs a parameter list";
      return false;
    }
    m->name = trim(rest.substr(0, open));
    if (!isIdent(m->name)) {
      *error = "invalid method name '" + m->name + "'";
      return false;
    }
    std::size_t close = 0;
    if (!parseParams(rest, open, &close, &m->params, error)) return false;
    std::string tail = trim(rest.substr(close + 1));
    if (tail.empty() || tail[0] != ':') {
      *error = "method '" + m->name + "' needs ': returnType'";
      return false;
    }
    tail = trim(tail.substr(1));
    if (tail.size() >= 5 && tail.compare(tail.size() - 5, 5, "const") == 0 &&
        (tail.size() == 5 || std::isspace(static_cast<unsigned char>(tail[tail.size() - 6])))) {
      m->isConst = true;
      tail = trim(tail.substr(0, tail.size() - 5));
    }
    return parseType(tail, &m->ret, error);
  } else {
    *error = "expected ctor, dtor or method";
    return false;
  }

  const std::size_t open = rest.find('(');
  if (open == std::string::npos) {
    *error = "constructor needs a parameter list";
    return false;
  }
  std::size_t close = 0;
  if (!parseParams(rest, open, &close, &m->params, error)) return false;
  if (!trim(rest.substr(close + 1)).empty()) {
    *error = "constructor does not take a return type";
    return false;
  }
  return true;
}

bool parseClass(const std::vector<std::string>& lines, std::size_t* index,
                Class* cls, std::string* error) {
  const std::string line = lines[*index];
  if (line.compare(0, 6, "class ") != 0) {
    *error = "expected 'class Name {'";
    return false;
  }
  const std::string rest = trim(line.substr(6));
  const std::size_t brace = rest.find('{');
  if (brace == std::string::npos || !trim(rest.substr(brace + 1)).empty()) {
    *error = "expected 'class Name {' on one line";
    return false;
  }
  cls->name = trim(rest.substr(0, brace));
  if (!isIdent(cls->name)) {
    *error = "invalid class name '" + cls->name + "'";
    return false;
  }
  cls->cppName = cls->name;
  bool closed = false;
  for (++*index; *index < lines.size(); ++*index) {
    const std::string bodyLine = trim(lines[*index]);
    if (bodyLine.empty()) continue;
    if (bodyLine == "}") { closed = true; break; }
    std::string value;
    if (quotedDirective(bodyLine, "cpp", &value)) {
      if (!value.empty()) cls->cppName = value;
      continue;
    }
    Method m;
    if (!parseMethod(bodyLine, &m, error)) {
      *error = "class " + cls->name + ": " + *error;
      return false;
    }
    cls->methods.push_back(std::move(m));
  }
  if (!closed) {
    *error = "class " + cls->name + " is missing '}'";
    return false;
  }
  return true;
}

bool parseFunction(const std::string& line, Function* fn, std::string* error) {
  if (line.compare(0, 9, "function ") != 0) {
    *error = "expected 'function name(...): type'";
    return false;
  }
  const std::string rest = trim(line.substr(9));
  const std::size_t open = rest.find('(');
  if (open == std::string::npos) {
    *error = "function needs a parameter list";
    return false;
  }
  fn->name = trim(rest.substr(0, open));
  fn->cppName = fn->name;
  if (!isIdent(fn->name)) {
    *error = "invalid function name '" + fn->name + "'";
    return false;
  }
  std::size_t close = 0;
  if (!parseParams(rest, open, &close, &fn->params, error)) return false;
  std::string tail = trim(rest.substr(close + 1));
  if (tail.empty() || tail[0] != ':') {
    *error = "function '" + fn->name + "' needs ': returnType'";
    return false;
  }
  return parseType(tail.substr(1), &fn->ret, error);
}

bool parse(const std::string& source, Bridge* bridge, std::string* error) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= source.size()) {
    const std::size_t nl = source.find('\n', start);
    const std::string raw =
        source.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    lines.push_back(trim(stripComment(raw)));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (line.empty()) continue;
    std::string value;
    if (quotedDirective(line, "header", &value)) {
      if (bridge->header.empty()) bridge->header = value;
      continue;
    }
    if (quotedDirective(line, "prefix", &value)) {
      if (!value.empty()) bridge->prefix = value;
      continue;
    }
    if (line.compare(0, 6, "class ") == 0) {
      Class cls;
      if (!parseClass(lines, &i, &cls, error)) return false;
      bridge->classes.push_back(std::move(cls));
      continue;
    }
    if (line.compare(0, 9, "function ") == 0) {
      Function fn;
      if (!parseFunction(line, &fn, error)) return false;
      bridge->functions.push_back(std::move(fn));
      continue;
    }
    *error = "unknown directive '" + line + "'";
    return false;
  }

  if (bridge->header.empty()) {
    *error = "missing 'header \"file.h\"' directive";
    return false;
  }
  if (!isIdent(bridge->prefix)) {
    *error = "prefix '" + bridge->prefix + "' is not a C identifier prefix";
    return false;
  }

  std::unordered_set<std::string> classNames;
  for (Class& cls : bridge->classes) {
    if (!classNames.insert(cls.name).second) {
      *error = "duplicate class '" + cls.name + "'";
      return false;
    }
    if (cls.cppName.empty()) cls.cppName = cls.name;
  }
  std::unordered_map<std::string, std::string> cppNames;
  for (const Class& cls : bridge->classes) cppNames[cls.name] = cls.cppName;
  auto resolve = [&](Type* type) -> bool {
    if (type->kind != Type::Kind::Unknown) return true;
    const auto it = cppNames.find(type->name);
    if (it == cppNames.end()) {
      *error = "unknown type '" + type->name + "'";
      return false;
    }
    type->kind = Type::Kind::Class;
    type->cppName = it->second;
    return true;
  };

  for (Class& cls : bridge->classes) {
    bool hasCtor = false;
    bool hasDtor = false;
    std::unordered_set<std::string> methodNames;
    for (Method& m : cls.methods) {
      if (m.isCtor) {
        if (hasCtor) {
          *error = "class " + cls.name + " has more than one ctor";
          return false;
        }
        hasCtor = true;
      } else if (m.isDtor) {
        if (hasDtor) {
          *error = "class " + cls.name + " has more than one dtor";
          return false;
        }
        hasDtor = true;
      } else {
        if (m.name == "init" || m.name == "close" || m.name == "valid" ||
            m.name == "handle") {
          *error = "class " + cls.name + " method '" + m.name +
                   "' collides with the generated Oskel class member";
          return false;
        }
        if (!methodNames.insert(m.name).second) {
          *error = "class " + cls.name + " has duplicate method '" + m.name + "'";
          return false;
        }
        if (m.ret.kind != Type::Kind::Void) {
          if (!resolve(&m.ret)) return false;
          if (m.ret.kind == Type::Kind::Class) {
            *error = "class " + cls.name + " method '" + m.name +
                     "' returns a class handle, which the first bridge slice does not support";
            return false;
          }
          if (m.ret.kind == Type::Kind::String) {
            *error = "class " + cls.name + " method '" + m.name +
                     "' returns a string, which the first bridge slice does not support";
            return false;
          }
        }
      }
      for (Param& p : m.params) {
        if (!resolve(&p.type)) return false;
        if (p.type.kind == Type::Kind::Void) {
          *error = "class " + cls.name + " parameter '" + p.name + "' has type void";
          return false;
        }
      }
    }
    if (!hasCtor) {
      *error = "class " + cls.name + " needs a ctor";
      return false;
    }
    if (!hasDtor) {
      *error = "class " + cls.name + " needs a dtor";
      return false;
    }
  }
  for (Function& fn : bridge->functions) {
    if (!resolve(&fn.ret)) return false;
    if (fn.ret.kind == Type::Kind::Class || fn.ret.kind == Type::Kind::String) {
      *error = "function '" + fn.name + "' returns " +
               (fn.ret.kind == Type::Kind::Class ? "a class handle" : "a string") +
               ", which the first bridge slice does not support";
      return false;
    }
    for (Param& p : fn.params) {
      if (!resolve(&p.type)) return false;
      if (p.type.kind == Type::Kind::Void) {
        *error = "function '" + fn.name + "' parameter '" + p.name + "' has type void";
        return false;
      }
    }
  }

  std::unordered_set<std::string> symbols;
  auto claim = [&](const std::string& sym) -> bool {
    if (!symbols.insert(sym).second) {
      *error = "duplicate generated C symbol '" + sym + "'";
      return false;
    }
    return true;
  };
  for (const Class& cls : bridge->classes) {
    bool hasCtor = false;
    bool hasDtor = false;
    for (const Method& m : cls.methods) {
      if (m.isCtor) hasCtor = true;
      if (m.isDtor) hasDtor = true;
    }
    if (hasCtor && !claim(bridge->prefix + cls.name + "_new")) return false;
    if (hasDtor && !claim(bridge->prefix + cls.name + "_delete")) return false;
    for (const Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      if (!claim(bridge->prefix + cls.name + "_" + m.name)) return false;
    }
  }
  for (const Function& fn : bridge->functions) {
    if (!claim(bridge->prefix + fn.name)) return false;
  }
  return true;
}

std::string ffiType(const Type& t) {
  switch (t.kind) {
    case Type::Kind::Void: return "void";
    case Type::Kind::Bool: return "bool";
    case Type::Kind::Int: return "int32";
    case Type::Kind::Int64: return "int64";
    case Type::Kind::UInt32: return "uint32";
    case Type::Kind::UInt64: return "uint64";
    case Type::Kind::Float: return "float";
    case Type::Kind::Double: return "double";
    case Type::Kind::String: return "cstring";
    case Type::Kind::Ptr: return "ptr";
    case Type::Kind::Class: return "ptr";
    case Type::Kind::Unknown: break;
  }
  return "ptr";
}

std::string cType(const Type& t) {
  switch (t.kind) {
    case Type::Kind::Void: return "void";
    case Type::Kind::Bool: return "bool";
    case Type::Kind::Int: return "int32_t";
    case Type::Kind::Int64: return "int64_t";
    case Type::Kind::UInt32: return "uint32_t";
    case Type::Kind::UInt64: return "uint64_t";
    case Type::Kind::Float: return "float";
    case Type::Kind::Double: return "double";
    case Type::Kind::String: return "const char *";
    case Type::Kind::Ptr: return "void *";
    case Type::Kind::Class: return "void *";
    case Type::Kind::Unknown: break;
  }
  return "void *";
}

std::string oskelType(const Type& t) {
  switch (t.kind) {
    case Type::Kind::Void: return "void";
    case Type::Kind::Bool: return "bool";
    case Type::Kind::Int:
    case Type::Kind::Int64:
    case Type::Kind::UInt32:
    case Type::Kind::UInt64: return "int";
    case Type::Kind::Float: return "float";
    case Type::Kind::Double: return "double";
    case Type::Kind::String: return "string";
    case Type::Kind::Ptr: return "ptr";
    case Type::Kind::Class: return t.name;
    case Type::Kind::Unknown: break;
  }
  return "ptr";
}

std::string defaultValue(const Type& t) {
  switch (t.kind) {
    case Type::Kind::Bool: return "false";
    case Type::Kind::Int:
    case Type::Kind::Int64:
    case Type::Kind::UInt32:
    case Type::Kind::UInt64:
    case Type::Kind::Float:
    case Type::Kind::Double: return "0";
    case Type::Kind::Ptr:
    case Type::Kind::Class:
    case Type::Kind::String: return "nullptr";
    case Type::Kind::Void:
    case Type::Kind::Unknown: break;
  }
  return "0";
}

std::string cppArg(const Type& t, const std::string& name) {
  if (t.kind == Type::Kind::Class) {
    return std::string("static_cast<") + (t.isConst ? "const " : "") + t.cppName + " *>(" +
           name + ")";
  }
  return name;
}

std::string oskelArg(const Type& t, const std::string& name) {
  if (t.kind == Type::Kind::Class) return name + ".handle";
  return name;
}

std::string paramsText(const std::vector<Param>& params, std::string (*mapper)(const Type&)) {
  std::string out;
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0) out += ", ";
    out += params[i].name + ": " + mapper(params[i].type);
  }
  return out;
}

std::string cParamsText(const std::vector<Param>& params) {
  std::string out;
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0) out += ", ";
    out += cType(params[i].type) + " " + params[i].name;
  }
  return out;
}

std::string cppArgsText(const std::vector<Param>& params) {
  std::string out;
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0) out += ", ";
    out += cppArg(params[i].type, params[i].name);
  }
  return out;
}

std::string oskelArgsText(const std::vector<Param>& params) {
  std::string out;
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0) out += ", ";
    out += oskelArg(params[i].type, params[i].name);
  }
  return out;
}

std::string symbol(const Bridge& b, const Class& cls, const std::string& suffix) {
  return b.prefix + cls.name + "_" + suffix;
}

void emitModule(const Bridge& b, const std::string& sourceName, std::string* out) {
  const std::string stem = stemOf(sourceName);
  *out += "// Generated by bindgen --cxx from " +
          (sourceName.empty() ? "input" : sourceName) + ". Do not edit by hand.\n";
  *out += "// The C++ side lives in " + stem + "_shim.cpp; compile it with the same C++\n";
  *out += "// compiler and ABI as the library it bridges.\n\n";
  *out += "extern \"C\" {\n";
  for (const Class& cls : b.classes) {
    for (const Method& m : cls.methods) {
      if (m.isCtor) {
        *out += "  fn " + symbol(b, cls, "new") + "(" + paramsText(m.params, ffiType) +
                "): ptr;\n";
      } else if (m.isDtor) {
        *out += "  fn " + symbol(b, cls, "delete") + "(self: ptr): void;\n";
      } else {
        *out += "  fn " + symbol(b, cls, m.name) + "(self: ptr";
        if (!m.params.empty()) *out += ", " + paramsText(m.params, ffiType);
        *out += "): " + ffiType(m.ret) + ";\n";
      }
    }
  }
  for (const Function& fn : b.functions) {
    *out += "  fn " + b.prefix + fn.name + "(" + paramsText(fn.params, ffiType) +
            "): " + ffiType(fn.ret) + ";\n";
  }
  *out += "}\n\n";

  for (const Class& cls : b.classes) {
    *out += "class " + cls.name + " {\n";
    *out += "  handle: ptr;\n\n";
    for (const Method& m : cls.methods) {
      if (!m.isCtor) continue;
      *out += "  fn init(" + paramsText(m.params, oskelType) + ") {\n";
      *out += "    self.handle = " + symbol(b, cls, "new") + "(" +
              oskelArgsText(m.params) + ");\n";
      *out += "  }\n\n";
    }
    *out += "  fn valid(): bool {\n";
    *out += "    return self.handle != nil;\n";
    *out += "  }\n\n";
    *out += "  fn close() {\n";
    *out += "    if (self.handle != nil) {\n";
    *out += "      " + symbol(b, cls, "delete") + "(self.handle);\n";
    *out += "      self.handle = nil;\n";
    *out += "    }\n";
    *out += "  }\n\n";
    for (const Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      *out += "  fn " + m.name + "(" + paramsText(m.params, oskelType) + ")";
      if (m.ret.kind != Type::Kind::Void) *out += ": " + oskelType(m.ret);
      *out += " {\n";
      *out += "    ";
      if (m.ret.kind != Type::Kind::Void) *out += "return ";
      *out += symbol(b, cls, m.name) + "(self.handle";
      if (!m.params.empty()) *out += ", " + oskelArgsText(m.params);
      *out += ");\n";
      *out += "  }\n\n";
    }
    *out += "}\n\n";
  }

  for (const Function& fn : b.functions) {
    *out += "fn " + fn.name + "(" + paramsText(fn.params, oskelType) + ")";
    if (fn.ret.kind != Type::Kind::Void) *out += ": " + oskelType(fn.ret);
    *out += " {\n";
    *out += "  ";
    if (fn.ret.kind != Type::Kind::Void) *out += "return ";
    *out += b.prefix + fn.name + "(" + oskelArgsText(fn.params) + ");\n";
    *out += "}\n\n";
  }
}

void emitShimHeader(const Bridge& b, const std::string& sourceName, std::string* out) {
  *out += "// Generated by bindgen --cxx from " +
          (sourceName.empty() ? "input" : sourceName) + ". Do not edit by hand.\n";
  *out += "#pragma once\n\n#include <stdbool.h>\n#include <stdint.h>\n\n";
  *out += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
  for (const Class& cls : b.classes) {
    for (const Method& m : cls.methods) {
      if (m.isCtor) {
        *out += "void *" + symbol(b, cls, "new") + "(" + cParamsText(m.params) + ");\n";
      } else if (m.isDtor) {
        *out += "void " + symbol(b, cls, "delete") + "(void *self);\n";
      } else {
        *out += cType(m.ret) + " " + symbol(b, cls, m.name) + "(void *self";
        if (!m.params.empty()) *out += ", " + cParamsText(m.params);
        *out += ");\n";
      }
    }
  }
  for (const Function& fn : b.functions) {
    *out += cType(fn.ret) + " " + b.prefix + fn.name + "(" + cParamsText(fn.params) + ");\n";
  }
  *out += "\n#ifdef __cplusplus\n}\n#endif\n";
}

void emitShimSource(const Bridge& b, const std::string& sourceName, std::string* out) {
  const std::string stem = stemOf(sourceName);
  *out += "// Generated by bindgen --cxx from " +
          (sourceName.empty() ? "input" : sourceName) + ". Do not edit by hand.\n";
  *out += "// Exceptions are caught at this boundary; a throwing call returns its zero\n";
  *out += "// value (a constructor returns nullptr). The C++ ABI never crosses into Oskel.\n";
  *out += "#include \"" + b.header + "\"\n";
  *out += "#include \"" + stem + "_shim.h\"\n\n";
  *out += "#include <cstdint>\n\n";
  *out += "extern \"C\" {\n\n";
  for (const Class& cls : b.classes) {
    for (const Method& m : cls.methods) {
      if (m.isCtor) {
        *out += "void *" + symbol(b, cls, "new") + "(" + cParamsText(m.params) + ") {\n";
        *out += "  try {\n";
        *out += "    return static_cast<void *>(new " + cls.cppName + "(" +
                cppArgsText(m.params) + "));\n";
        *out += "  } catch (...) {\n    return nullptr;\n  }\n}\n\n";
      } else if (m.isDtor) {
        *out += "void " + symbol(b, cls, "delete") + "(void *self) {\n";
        *out += "  try {\n";
        *out += "    delete static_cast<" + cls.cppName + " *>(self);\n";
        *out += "  } catch (...) {\n  }\n}\n\n";
      } else {
        *out += cType(m.ret) + " " + symbol(b, cls, m.name) + "(void *self";
        if (!m.params.empty()) *out += ", " + cParamsText(m.params);
        *out += ") {\n";
        *out += "  try {\n";
        *out += "    ";
        if (m.ret.kind != Type::Kind::Void) *out += "return ";
        *out += "static_cast<" + std::string(m.isConst ? "const " : "") + cls.cppName +
                " *>(self)->" + m.name + "(" + cppArgsText(m.params) + ");\n";
        *out += "  } catch (...) {\n";
        if (m.ret.kind != Type::Kind::Void) *out += "    return " + defaultValue(m.ret) + ";\n";
        *out += "  }\n}\n\n";
      }
    }
  }
  for (const Function& fn : b.functions) {
    *out += cType(fn.ret) + " " + b.prefix + fn.name + "(" + cParamsText(fn.params) + ") {\n";
    *out += "  try {\n";
    *out += "    ";
    if (fn.ret.kind != Type::Kind::Void) *out += "return ";
    *out += fn.cppName + "(" + cppArgsText(fn.params) + ");\n";
    *out += "  } catch (...) {\n";
    if (fn.ret.kind != Type::Kind::Void) *out += "    return " + defaultValue(fn.ret) + ";\n";
    *out += "  }\n}\n\n";
  }
  *out += "}  // extern \"C\"\n";
}

}  // namespace

Result generate(const std::string& descriptor, const std::string& sourceName) {
  Result result;
  Bridge bridge;
  std::string error;
  if (!parse(descriptor, &bridge, &error)) {
    result.error = error;
    return result;
  }
  result.classes = static_cast<int>(bridge.classes.size());
  for (const Class& cls : bridge.classes) {
    result.methods += static_cast<int>(cls.methods.size());
  }
  result.functions = static_cast<int>(bridge.functions.size());
  emitModule(bridge, sourceName, &result.text);
  emitShimHeader(bridge, sourceName, &result.shimHeader);
  emitShimSource(bridge, sourceName, &result.shimSource);
  return result;
}

}  // namespace cxxbind
