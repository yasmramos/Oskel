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
// The translation covers the FFI vocabulary of PHASE 6.6: pointers, `cstring`,
// the fixed-width/unsigned integers, `float`, `double`, `bool`, `void`, a
// `struct` by value, a callback (`fn(P...): R`) and a C `enum`. A struct is
// emitted only when the runtime's own layout check accepts it (a union, an
// array field, a bitfield and a nested struct are reported instead, because the
// Oskel `struct` is a map of whole scalars). A declaration the vocabulary
// cannot express is skipped with a `// skipped:` comment instead of emitting
// something the compiler would reject.
//
// A declaration this *build* cannot marshal -- a C variadic, which needs libffi
// and the System V AMD64 ABI -- is written commented out: the module compiles
// everywhere and the real signature is there to uncomment on a build that
// supports it. A struct whose layout `ffi::buildStructLayout` rejects is not
// commented out but reported as skipped, because no build could accept it.
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
  // Number of declarations emitted *commented out* because this build cannot
  // marshal them: a C variadic (`...`) needs libffi and the System V AMD64 ABI,
  // and the compiler rejects the declaration outright on a build without both,
  // so the tool writes the real signature as a comment: the module compiles
  // everywhere, and the user uncomments it on a build that supports it.
  int commented = 0;
  // Number of `struct` declarations emitted before the `extern "C"` block.
  int structs = 0;
  // Number of `enum` members emitted as module constants.
  int constants = 0;
};

// Translates `headerSource` into an Oskel module. `sourceName` is the header's
// name, used only in the leading comment of the output (empty becomes "input").
Result generate(const std::string& headerSource, const std::string& sourceName = "");

}  // namespace bindgen
