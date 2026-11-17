#pragma once

#include <string>

// The C-header binding generator (the `bindgen` tool).
//
// It reads a C header and writes Oskel `extern "C"` declarations, so a C
// library can be used from Oskel without spelling every signature by hand. It
// is a plain text transformation: it never compiles Oskel, never links the
// runtime and never touches the bytecode, the AST or the VM (DEVELOPMENT.md
// §3 stays untouched). The output is an ordinary Oskel module the compiler
// already accepts, so nothing about the language or the bytecode changes.
//
// The translation covers the FFI scalar vocabulary of PHASE 6.6 (pointers,
// `cstring`, the fixed-width/unsigned integers, `float`, `double`, `bool`,
// `void`). A declaration the vocabulary cannot express (a struct by value, a
// callback, `long double`, ...) is skipped with a `// skipped:` comment instead
// of emitting something the compiler would reject, and a C variadic is written
// commented out (it needs libffi and the System V AMD64 ABI).
namespace bindgen {

// The generated module and a small tally the CLI reports on stderr.
struct Result {
  // The Oskel source, ready to be written to a `.oskl` file.
  std::string text;
  // Number of `fn` declarations emitted as active code.
  int emitted = 0;
  // Number of declarations skipped (each one is a `// skipped:` comment in
  // `text`). A skipped declaration is not an error: it is a type outside the
  // FFI vocabulary, which the tool reports instead of mis-translating.
  int skipped = 0;
  // Number of variadic declarations emitted *commented out*. A C variadic
  // (`...`) needs libffi and the System V AMD64 ABI, and the compiler rejects
  // the declaration outright on a build without both, so the tool writes the
  // real signature as a comment: the module compiles everywhere, and the user
  // uncomments it on a build that supports it.
  int commented = 0;
};

// Translates `headerSource` into an Oskel module. `sourceName` is the header's
// name, used only in the leading comment of the output (empty becomes "input").
Result generate(const std::string& headerSource, const std::string& sourceName = "");

}  // namespace bindgen
