#pragma once

#include <string>

// The C++ bridge generator (`bindgen --cxx`).
//
// Skia and most modern libraries expose a C++ API, not a C one. Parsing their
// headers directly is not viable: `bindgen` is a C text transformer, and a C++
// ABI is compiler-, platform- and library-specific. The bridge keeps the Oskel
// FFI on the C ABI and moves the C++ ABI into a generated translation unit:
//
//   descriptor (.oskelbind) + C++ header
//        -> <stem>_shim.h      C declarations for the thunks
//        -> <stem>_shim.cpp    C++ thunks compiled with the library
//        -> <stem>.oskl        Oskel module with classes and wrappers
//
// The descriptor is a small, line-oriented file. It is the ownership/signature
// contract the C++ compiler checks when the generated shim is compiled; a
// signature that drifts from the real header fails that compile instead of
// producing a wrong ABI call. A class becomes an Oskel `class` whose `handle`
// field is the opaque C++ object; the generated `close` method releases it.
namespace cxxbind {

// The three generated files plus a tally for the CLI summary.
struct Result {
  // The Oskel module.
  std::string text;
  // The C header the shim source includes (for the C++ library's declarations).
  std::string shimHeader;
  // The C++ thunks. Compile it with the same C++ compiler and ABI as the
  // library it bridges.
  std::string shimSource;
  int classes = 0;
  int methods = 0;
  int functions = 0;
  // Non-empty when the descriptor is invalid. The CLI reports it and exits 65;
  // no partial output is written.
  std::string error;
};

// Translates `descriptor` into the module and shim. `sourceName` is used in the
// generated comments and to derive the `#include` guard.
Result generate(const std::string& descriptor, const std::string& sourceName = "");

}  // namespace cxxbind
