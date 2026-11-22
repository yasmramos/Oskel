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

enum class ReturnPolicy { Owned, Borrowed, Smart, Value };
enum class ReleasePolicy { Delete, Unref, None };

struct Type {
  enum class Kind { Unknown, Void, Bool, Int, Int64, UInt32, UInt64, Float, Double, String, Ptr, Class };
  Kind kind = Kind::Unknown;
  std::string name;     // the Oskel class name
  std::string cppName;  // the qualified C++ class name, for a class handle
  bool isConst = false; // `const Class` parameter
  bool isRef = false;   // `Class&` / `const Class&` parameter
};

struct Param {
  std::string name;
  Type type;
};

struct Method {
  std::string name;
  std::vector<Param> params;
  Type ret;
  ReturnPolicy retPolicy = ReturnPolicy::Owned;
  bool retPolicySet = false;
  bool isCtor = false;
  bool isDtor = false;
  bool isConst = false;
};

struct Class {
  std::string name;
  std::string cppName;
  std::vector<Method> methods;
  bool hasCtor = false;
  bool hasDtor = false;
  ReleasePolicy release = ReleasePolicy::Delete;
  bool releaseSet = false;
};

struct Function {
  std::string name;
  std::string cppName;
  std::vector<Param> params;
  Type ret;
  ReturnPolicy retPolicy = ReturnPolicy::Owned;
  bool retPolicySet = false;
};

struct Object {
  std::string name;
  std::vector<Function> functions;
};

struct Bridge {
  std::string header;
  std::string prefix = "oskel_";
  std::vector<Class> classes;
  std::vector<Object> objects;
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
  out->isRef = false;
  if (t.compare(0, 6, "const ") == 0) {
    out->isConst = true;
    t = trim(t.substr(6));
  }
  if (!t.empty() && t.back() == '&') {
    out->isRef = true;
    t = trim(t.substr(0, t.size() - 1));
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

bool takeSuffixWord(std::string* tail, const std::string& word) {
  if (*tail == word) {
    tail->clear();
    return true;
  }
  if (tail->size() > word.size() &&
      tail->compare(tail->size() - word.size(), word.size(), word) == 0 &&
      std::isspace(static_cast<unsigned char>((*tail)[tail->size() - word.size() - 1]))) {
    *tail = trim(tail->substr(0, tail->size() - word.size()));
    return true;
  }
  return false;
}

bool extractCppDirective(std::string* tail, std::string* cppName, std::string* error) {
  const std::string marker = "cpp \"";
  const std::size_t pos = tail->find(marker);
  if (pos == std::string::npos) return true;
  if (pos != 0 && !std::isspace(static_cast<unsigned char>((*tail)[pos - 1]))) {
    *error = "cpp must be a separate word";
    return false;
  }
  const std::size_t open = pos + marker.size();
  const std::size_t close = tail->find('"', open);
  if (close == std::string::npos) {
    *error = "cpp directive is missing its closing quote";
    return false;
  }
  if (!cppName->empty()) {
    *error = "duplicate cpp directive";
    return false;
  }
  *cppName = tail->substr(open, close - open);
  tail->erase(pos, close - pos + 1);
  *tail = trim(*tail);
  if (tail->find(marker) != std::string::npos) {
    *error = "duplicate cpp directive";
    return false;
  }
  return true;
}

bool parseReturnTail(const std::string& text, bool allowConst, bool allowCpp,
                     Type* ret, bool* isConst, ReturnPolicy* policy, bool* policySet,
                     std::string* cppName, std::string* error) {
  std::string tail = trim(text);
  if (allowCpp && !extractCppDirective(&tail, cppName, error)) return false;
  while (!tail.empty()) {
    if (takeSuffixWord(&tail, "const")) {
      if (!allowConst) {
        *error = "const is only valid on a method";
        return false;
      }
      if (*isConst) {
        *error = "duplicate const";
        return false;
      }
      *isConst = true;
      continue;
    }
    ReturnPolicy parsed;
    bool matched = true;
    if (takeSuffixWord(&tail, "borrow")) {
      parsed = ReturnPolicy::Borrowed;
    } else if (takeSuffixWord(&tail, "owned")) {
      parsed = ReturnPolicy::Owned;
    } else if (takeSuffixWord(&tail, "sp")) {
      parsed = ReturnPolicy::Smart;
    } else if (takeSuffixWord(&tail, "value")) {
      parsed = ReturnPolicy::Value;
    } else {
      matched = false;
    }
    if (!matched) break;
    if (*policySet) {
      *error = "duplicate return policy";
      return false;
    }
    *policy = parsed;
    *policySet = true;
  }
  if (tail.empty()) {
    *error = "missing return type";
    return false;
  }
  if (!parseType(tail, ret, error)) return false;
  if (ret->isRef) {
    *error = "a return type cannot be a reference";
    return false;
  }
  if (*policySet && ret->kind != Type::Kind::Unknown && ret->kind != Type::Kind::Class) {
    *error = "a return policy is only valid for a class return";
    return false;
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
    std::string ignoredCpp;
    return parseReturnTail(tail.substr(1), true, false, &m->ret, &m->isConst,
                           &m->retPolicy, &m->retPolicySet, &ignoredCpp, error);
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
    if (quotedDirective(bodyLine, "release", &value)) {
      if (cls->releaseSet) {
        *error = "class " + cls->name + " has more than one release policy";
        return false;
      }
      if (value == "delete") {
        cls->release = ReleasePolicy::Delete;
      } else if (value == "unref") {
        cls->release = ReleasePolicy::Unref;
      } else if (value == "none") {
        cls->release = ReleasePolicy::None;
      } else {
        *error = "class " + cls->name + " has unknown release policy '" + value + "'";
        return false;
      }
      cls->releaseSet = true;
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
  bool ignoredConst = false;
  std::string cppName;
  if (!parseReturnTail(tail.substr(1), false, true, &fn->ret, &ignoredConst,
                       &fn->retPolicy, &fn->retPolicySet, &cppName, error)) {
    return false;
  }
  fn->cppName = cppName.empty() ? fn->name : cppName;
  return true;
}

bool parseObject(const std::vector<std::string>& lines, std::size_t* index,
                 Object* object, std::string* error) {
  const std::string line = lines[*index];
  if (line.compare(0, 7, "object ") != 0) {
    *error = "expected 'object Name {'";
    return false;
  }
  const std::string rest = trim(line.substr(7));
  const std::size_t brace = rest.find('{');
  if (brace == std::string::npos || !trim(rest.substr(brace + 1)).empty()) {
    *error = "expected 'object Name {' on one line";
    return false;
  }
  object->name = trim(rest.substr(0, brace));
  if (!isIdent(object->name)) {
    *error = "invalid object name '" + object->name + "'";
    return false;
  }
  bool closed = false;
  for (++*index; *index < lines.size(); ++*index) {
    const std::string bodyLine = trim(lines[*index]);
    if (bodyLine.empty()) continue;
    if (bodyLine == "}") { closed = true; break; }
    if (bodyLine.compare(0, 9, "function ") != 0) {
      *error = "object " + object->name + " only accepts function entries";
      return false;
    }
    Function fn;
    if (!parseFunction(bodyLine, &fn, error)) {
      *error = "object " + object->name + ": " + *error;
      return false;
    }
    object->functions.push_back(std::move(fn));
  }
  if (!closed) {
    *error = "object " + object->name + " is missing '}'";
    return false;
  }
  return true;
}

bool validate(Bridge* bridge, std::string* error) {
  if (bridge->header.empty()) {
    *error = "missing 'header \"file.h\"' directive";
    return false;
  }
  if (!isIdent(bridge->prefix)) {
    *error = "prefix '" + bridge->prefix + "' is not a C identifier prefix";
    return false;
  }

  std::unordered_map<std::string, Class*> classMap;
  std::unordered_set<std::string> classNames;
  for (Class& cls : bridge->classes) {
    if (!classNames.insert(cls.name).second) {
      *error = "duplicate class '" + cls.name + "'";
      return false;
    }
    if (cls.cppName.empty()) cls.cppName = cls.name;
    classMap[cls.name] = &cls;
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

  // First pass: constructors, destructors, release policy, method names and
  // type resolution. The class map is complete before class returns are
  // checked, so a factory may return a class declared later in the descriptor.
  for (Class& cls : bridge->classes) {
    std::unordered_set<std::string> methodNames;
    for (Method& m : cls.methods) {
      if (m.isCtor) {
        if (cls.hasCtor) {
          *error = "class " + cls.name + " has more than one ctor";
          return false;
        }
        cls.hasCtor = true;
      } else if (m.isDtor) {
        if (cls.hasDtor) {
          *error = "class " + cls.name + " has more than one dtor";
          return false;
        }
        cls.hasDtor = true;
      } else {
        if (m.name == "init" || m.name == "close" || m.name == "valid" ||
            m.name == "handle" || m.name == "owned") {
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
          if (m.ret.kind == Type::Kind::String) {
            *error = "class " + cls.name + " method '" + m.name +
                     "' returns a string, which the bridge does not support";
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
        if (p.type.isRef && p.type.kind != Type::Kind::Class) {
          *error = "class " + cls.name + " parameter '" + p.name +
                   "' is a reference to a non-class type";
          return false;
        }
      }
    }
    if (cls.hasDtor) {
      if (cls.releaseSet) {
        *error = "class " + cls.name + " has both dtor and release";
        return false;
      }
      cls.release = ReleasePolicy::Delete;
      cls.releaseSet = true;
    } else if (!cls.releaseSet) {
      cls.release = ReleasePolicy::Delete;
      cls.releaseSet = true;
    }
    if (cls.hasCtor && cls.release == ReleasePolicy::None) {
      *error = "class " + cls.name + " has a ctor but no release policy";
      return false;
    }
  }

  auto validateClassReturn = [&](const Type& ret, ReturnPolicy policy, bool policySet,
                                 const std::string& context) -> bool {
    if (ret.kind != Type::Kind::Class) {
      if (policySet) {
        *error = context + " has a return policy but does not return a class";
        return false;
      }
      return true;
    }
    const auto it = classMap.find(ret.name);
    if (it == classMap.end()) {
      *error = context + " returns unknown class '" + ret.name + "'";
      return false;
    }
    const Class& target = *it->second;
    if (target.hasCtor) {
      *error = context + " returns class '" + ret.name +
               "', which has a ctor; a class return needs an adopt-only class";
      return false;
    }
    if (policy == ReturnPolicy::Value && target.release != ReleasePolicy::Delete) {
      *error = context + " returns class '" + ret.name +
               "' by value, which needs release \"delete\"";
      return false;
    }
    if (policy != ReturnPolicy::Borrowed && target.release == ReleasePolicy::None) {
      *error = context + " owns a class '" + ret.name +
               "' whose release policy is \"none\"";
      return false;
    }
    return true;
  };

  // Second pass: class returns. A class return is adopt-only, because the
  // generated wrapper calls `Target(handle, owned)` rather than a C++ ctor.
  for (Class& cls : bridge->classes) {
    for (Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      if (m.ret.kind == Type::Kind::Class) {
        if (!validateClassReturn(m.ret, m.retPolicy, m.retPolicySet,
                                 "class " + cls.name + " method '" + m.name + "'")) {
          return false;
        }
      } else if (m.retPolicySet) {
        *error = "class " + cls.name + " method '" + m.name +
                 "' has a return policy but does not return a class";
        return false;
      }
    }
  }

  std::unordered_set<std::string> functionNames;
  for (Function& fn : bridge->functions) {
    if (!functionNames.insert(fn.name).second) {
      *error = "duplicate function '" + fn.name + "'";
      return false;
    }
    if (classNames.count(fn.name) != 0) {
      *error = "function '" + fn.name + "' collides with a class";
      return false;
    }
    if (!resolve(&fn.ret)) return false;
    if (fn.ret.kind == Type::Kind::String) {
      *error = "function '" + fn.name + "' returns a string, which the bridge does not support";
      return false;
    }
    if (!validateClassReturn(fn.ret, fn.retPolicy, fn.retPolicySet, "function '" + fn.name + "'")) {
      return false;
    }
    for (Param& p : fn.params) {
      if (!resolve(&p.type)) return false;
      if (p.type.kind == Type::Kind::Void) {
        *error = "function '" + fn.name + "' parameter '" + p.name + "' has type void";
        return false;
      }
      if (p.type.isRef && p.type.kind != Type::Kind::Class) {
        *error = "function '" + fn.name + "' parameter '" + p.name +
                 "' is a reference to a non-class type";
        return false;
      }
    }
  }

  std::unordered_set<std::string> objectNames;
  for (Object& object : bridge->objects) {
    if (!objectNames.insert(object.name).second) {
      *error = "duplicate object '" + object.name + "'";
      return false;
    }
    if (classNames.count(object.name) != 0) {
      *error = "object '" + object.name + "' collides with a class";
      return false;
    }
    if (functionNames.count(object.name) != 0) {
      *error = "object '" + object.name + "' collides with a function";
      return false;
    }
    std::unordered_set<std::string> names;
    for (Function& fn : object.functions) {
      if (!names.insert(fn.name).second) {
        *error = "object " + object.name + " has duplicate function '" + fn.name + "'";
        return false;
      }
      if (!resolve(&fn.ret)) return false;
      if (fn.ret.kind == Type::Kind::String) {
        *error = "object " + object.name + " function '" + fn.name +
                 "' returns a string, which the bridge does not support";
        return false;
      }
      if (!validateClassReturn(fn.ret, fn.retPolicy, fn.retPolicySet,
                               "object " + object.name + " function '" + fn.name + "'")) {
        return false;
      }
      for (Param& p : fn.params) {
        if (!resolve(&p.type)) return false;
        if (p.type.kind == Type::Kind::Void) {
          *error = "object " + object.name + " function '" + fn.name + "' parameter '" +
                   p.name + "' has type void";
          return false;
        }
        if (p.type.isRef && p.type.kind != Type::Kind::Class) {
          *error = "object " + object.name + " function '" + fn.name + "' parameter '" +
                   p.name + "' is a reference to a non-class type";
          return false;
        }
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
    if (cls.hasCtor && !claim(bridge->prefix + cls.name + "_new")) return false;
    if (cls.release == ReleasePolicy::Delete &&
        !claim(bridge->prefix + cls.name + "_delete")) return false;
    if (cls.release == ReleasePolicy::Unref &&
        !claim(bridge->prefix + cls.name + "_unref")) return false;
    for (const Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      if (!claim(bridge->prefix + cls.name + "_" + m.name)) return false;
    }
  }
  for (const Function& fn : bridge->functions) {
    if (!claim(bridge->prefix + fn.name)) return false;
  }
  for (const Object& object : bridge->objects) {
    for (const Function& fn : object.functions) {
      if (!claim(bridge->prefix + object.name + "_" + fn.name)) return false;
    }
  }
  return true;
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
    if (line.compare(0, 7, "object ") == 0) {
      Object object;
      if (!parseObject(lines, &i, &object, error)) return false;
      bridge->objects.push_back(std::move(object));
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
  return validate(bridge, error);
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
    const std::string cast = std::string("static_cast<") + (t.isConst ? "const " : "") +
                             t.cppName + " *>(" + name + ")";
    return t.isRef ? "*" + cast : cast;
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

std::string newSymbol(const Bridge& b, const Class& cls) {
  return b.prefix + cls.name + "_new";
}

std::string releaseSymbol(const Bridge& b, const Class& cls) {
  if (cls.release == ReleasePolicy::Delete) return b.prefix + cls.name + "_delete";
  if (cls.release == ReleasePolicy::Unref) return b.prefix + cls.name + "_unref";
  return "";
}

std::string methodSymbol(const Bridge& b, const Class& cls, const std::string& name) {
  return b.prefix + cls.name + "_" + name;
}

std::string functionSymbol(const Bridge& b, const Function& fn) {
  return b.prefix + fn.name;
}

std::string objectSymbol(const Bridge& b, const Object& object, const Function& fn) {
  return b.prefix + object.name + "_" + fn.name;
}

std::string classReturnExpr(const Type& ret, const std::string& call, ReturnPolicy policy) {
  return ret.name + "(" + call + ", " +
         (policy == ReturnPolicy::Borrowed ? "false" : "true") + ")";
}

void emitModule(const Bridge& b, const std::string& sourceName, std::string* out) {
  const std::string stem = stemOf(sourceName);
  *out += "// Generated by bindgen --cxx from " +
          (sourceName.empty() ? "input" : sourceName) + ". Do not edit by hand.\n";
  *out += "// The C++ side lives in " + stem + "_shim.cpp; compile it with the same C++\n";
  *out += "// compiler and ABI as the library it bridges.\n\n";
  *out += "extern \"C\" {\n";
  for (const Class& cls : b.classes) {
    if (cls.hasCtor) {
      for (const Method& m : cls.methods) {
        if (!m.isCtor) continue;
        *out += "  fn " + newSymbol(b, cls) + "(" + paramsText(m.params, ffiType) +
                "): ptr;\n";
      }
    }
    if (cls.release != ReleasePolicy::None) {
      *out += "  fn " + releaseSymbol(b, cls) + "(self: ptr): void;\n";
    }
    for (const Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      *out += "  fn " + methodSymbol(b, cls, m.name) + "(self: ptr";
      if (!m.params.empty()) *out += ", " + paramsText(m.params, ffiType);
      *out += "): " + ffiType(m.ret) + ";\n";
    }
  }
  for (const Function& fn : b.functions) {
    *out += "  fn " + functionSymbol(b, fn) + "(" + paramsText(fn.params, ffiType) +
            "): " + ffiType(fn.ret) + ";\n";
  }
  for (const Object& object : b.objects) {
    for (const Function& fn : object.functions) {
      *out += "  fn " + objectSymbol(b, object, fn) + "(" +
              paramsText(fn.params, ffiType) + "): " + ffiType(fn.ret) + ";\n";
    }
  }
  *out += "}\n\n";

  for (const Class& cls : b.classes) {
    *out += "class " + cls.name + " {\n";
    *out += "  handle: ptr;\n";
    if (cls.hasCtor) {
      *out += "  owned: bool = true;\n\n";
      for (const Method& m : cls.methods) {
        if (!m.isCtor) continue;
        *out += "  fn init(" + paramsText(m.params, oskelType) + ") {\n";
        *out += "    self.handle = " + newSymbol(b, cls) + "(" + oskelArgsText(m.params) +
                ");\n";
        *out += "  }\n\n";
      }
    } else {
      *out += "  owned: bool;\n\n";
      *out += "  fn init(handle: ptr, owned: bool) {\n";
      *out += "    self.handle = handle;\n";
      *out += "    self.owned = owned;\n";
      *out += "  }\n\n";
    }
    *out += "  fn valid(): bool {\n";
    *out += "    return self.handle != nil;\n";
    *out += "  }\n\n";
    *out += "  fn close() {\n";
    *out += "    if (self.handle != nil) {\n";
    if (cls.release != ReleasePolicy::None) {
      *out += "      if (self.owned) {\n";
      *out += "        " + releaseSymbol(b, cls) + "(self.handle);\n";
      *out += "      }\n";
    }
    *out += "      self.handle = nil;\n";
    *out += "    }\n";
    *out += "  }\n\n";
    for (const Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      *out += "  fn " + m.name + "(" + paramsText(m.params, oskelType) + ")";
      if (m.ret.kind != Type::Kind::Void) *out += ": " + oskelType(m.ret);
      *out += " {\n";
      *out += "    ";
      const std::string call = methodSymbol(b, cls, m.name) + "(self.handle";
      const std::string args = m.params.empty() ? "" : ", " + oskelArgsText(m.params);
      if (m.ret.kind == Type::Kind::Class) {
        *out += "return " + classReturnExpr(m.ret, call + args + ")", m.retPolicy) + ";\n";
      } else if (m.ret.kind != Type::Kind::Void) {
        *out += "return " + call + args + ");\n";
      } else {
        *out += call + args + ");\n";
      }
      *out += "  }\n\n";
    }
    *out += "}\n\n";
  }

  for (const Object& object : b.objects) {
    *out += "let " + object.name + " = {\n";
    for (const Function& fn : object.functions) {
      *out += "  " + fn.name + ": (" + paramsText(fn.params, oskelType) + ") => ";
      const std::string call = objectSymbol(b, object, fn) + "(" + oskelArgsText(fn.params) + ")";
      if (fn.ret.kind == Type::Kind::Class) {
        *out += classReturnExpr(fn.ret, call, fn.retPolicy) + ",\n";
      } else if (fn.ret.kind != Type::Kind::Void) {
        *out += call + ",\n";
      } else {
        *out += "{ " + call + "; },\n";
      }
    }
    *out += "};\n\n";
  }

  for (const Function& fn : b.functions) {
    *out += "fn " + fn.name + "(" + paramsText(fn.params, oskelType) + ")";
    if (fn.ret.kind != Type::Kind::Void) *out += ": " + oskelType(fn.ret);
    *out += " {\n";
    *out += "  ";
    const std::string call = functionSymbol(b, fn) + "(" + oskelArgsText(fn.params) + ")";
    if (fn.ret.kind == Type::Kind::Class) {
      *out += "return " + classReturnExpr(fn.ret, call, fn.retPolicy) + ";\n";
    } else if (fn.ret.kind != Type::Kind::Void) {
      *out += "return " + call + ";\n";
    } else {
      *out += call + ";\n";
    }
    *out += "}\n\n";
  }
}

void emitCallBody(const Type& ret, ReturnPolicy policy, const std::string& call,
                  std::string* out) {
  if (ret.kind == Type::Kind::Void) {
    *out += "    " + call + ";\n";
    return;
  }
  if (ret.kind == Type::Kind::Class) {
    if (policy == ReturnPolicy::Smart) {
      *out += "    auto result = " + call + ";\n";
      *out += "    return static_cast<void *>(result.release());\n";
    } else if (policy == ReturnPolicy::Value) {
      *out += "    auto result = " + call + ";\n";
      *out += "    return static_cast<void *>(new " + ret.cppName + "(result));\n";
    } else {
      *out += "    return static_cast<void *>(const_cast<" + ret.cppName + " *>(" +
              call + "));\n";
    }
    return;
  }
  *out += "    return " + call + ";\n";
}

void emitCatch(const Type& ret, std::string* out) {
  *out += "  } catch (...) {\n";
  if (ret.kind == Type::Kind::Class) {
    *out += "    return nullptr;\n";
  } else if (ret.kind != Type::Kind::Void) {
    *out += "    return " + defaultValue(ret) + ";\n";
  }
  *out += "  }\n}\n\n";
}

void emitShimHeader(const Bridge& b, const std::string& sourceName, std::string* out) {
  *out += "// Generated by bindgen --cxx from " +
          (sourceName.empty() ? "input" : sourceName) + ". Do not edit by hand.\n";
  *out += "#pragma once\n\n#include <stdbool.h>\n#include <stdint.h>\n\n";
  *out += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
  for (const Class& cls : b.classes) {
    for (const Method& m : cls.methods) {
      if (m.isCtor) {
        *out += "void *" + newSymbol(b, cls) + "(" + cParamsText(m.params) + ");\n";
      }
    }
    if (cls.release != ReleasePolicy::None) {
      *out += "void " + releaseSymbol(b, cls) + "(void *self);\n";
    }
    for (const Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      *out += cType(m.ret) + " " + methodSymbol(b, cls, m.name) + "(void *self";
      if (!m.params.empty()) *out += ", " + cParamsText(m.params);
      *out += ");\n";
    }
  }
  for (const Function& fn : b.functions) {
    *out += cType(fn.ret) + " " + functionSymbol(b, fn) + "(" + cParamsText(fn.params) +
            ");\n";
  }
  for (const Object& object : b.objects) {
    for (const Function& fn : object.functions) {
      *out += cType(fn.ret) + " " + objectSymbol(b, object, fn) + "(" +
              cParamsText(fn.params) + ");\n";
    }
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
        *out += "void *" + newSymbol(b, cls) + "(" + cParamsText(m.params) + ") {\n";
        *out += "  try {\n";
        *out += "    return static_cast<void *>(new " + cls.cppName + "(" +
                cppArgsText(m.params) + "));\n";
        *out += "  } catch (...) {\n    return nullptr;\n  }\n}\n\n";
      }
    }
    if (cls.release != ReleasePolicy::None) {
      *out += "void " + releaseSymbol(b, cls) + "(void *self) {\n";
      *out += "  try {\n";
      if (cls.release == ReleasePolicy::Unref) {
        *out += "    static_cast<" + cls.cppName + " *>(self)->unref();\n";
      } else {
        *out += "    delete static_cast<" + cls.cppName + " *>(self);\n";
      }
      *out += "  } catch (...) {\n  }\n}\n\n";
    }
    for (const Method& m : cls.methods) {
      if (m.isCtor || m.isDtor) continue;
      *out += cType(m.ret) + " " + methodSymbol(b, cls, m.name) + "(void *self";
      if (!m.params.empty()) *out += ", " + cParamsText(m.params);
      *out += ") {\n";
      *out += "  try {\n";
      const std::string call = "static_cast<" + std::string(m.isConst ? "const " : "") +
                               cls.cppName + " *>(self)->" + m.name + "(" +
                               cppArgsText(m.params) + ")";
      emitCallBody(m.ret, m.retPolicy, call, out);
      emitCatch(m.ret, out);
    }
  }
  for (const Function& fn : b.functions) {
    *out += cType(fn.ret) + " " + functionSymbol(b, fn) + "(" + cParamsText(fn.params) +
            ") {\n";
    *out += "  try {\n";
    emitCallBody(fn.ret, fn.retPolicy, fn.cppName + "(" + cppArgsText(fn.params) + ")", out);
    emitCatch(fn.ret, out);
  }
  for (const Object& object : b.objects) {
    for (const Function& fn : object.functions) {
      *out += cType(fn.ret) + " " + objectSymbol(b, object, fn) + "(" +
              cParamsText(fn.params) + ") {\n";
      *out += "  try {\n";
      emitCallBody(fn.ret, fn.retPolicy, fn.cppName + "(" + cppArgsText(fn.params) + ")", out);
      emitCatch(fn.ret, out);
    }
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
  for (const Object& object : bridge.objects) {
    result.functions += static_cast<int>(object.functions.size());
  }
  emitModule(bridge, sourceName, &result.text);
  emitShimHeader(bridge, sourceName, &result.shimHeader);
  emitShimSource(bridge, sourceName, &result.shimSource);
  return result;
}

}  // namespace cxxbind
