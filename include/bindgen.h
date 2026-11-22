#pragma once

#include <string>

#include "ffi_types.h"

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
// A declaration the selected destination cannot marshal -- a C variadic, which
// needs a target with a variadic FFI path, or a struct by value on an ABI whose
// register thunks cannot express it -- is written commented out: the module
// compiles everywhere and the real signature is there to uncomment on a target
// that supports it. Both questions are put to the compiler's own
// `ffi::validateSignature`, asked once with every ABI capability on (is the
// shape in the vocabulary at all?) and once with the destination's (can it
// marshal the shape?), so the tool cannot drift from the compiler. A struct
// whose layout `ffi::buildStructLayout` rejects is reported as skipped instead,
// because no target could accept it.
namespace bindgen {

// The C data model the header was written for. `long` and `wchar_t` are the
// only common types whose width differs between the two 64-bit models: LP64
// (Linux, macOS) has a 64-bit `long` and a 32-bit `wchar_t`, while LLP64
// (Windows) has a 32-bit `long` and a 16-bit `wchar_t`. `long long` is 64-bit
// in both, so it never depends on the target.
enum class Target { Lp64, Llp64 };

struct Options {
  Target target = Target::Lp64;
  // The destination ABI for the emitted declarations. `Build` (the default)
  // uses the host's capabilities; `--target llp64` implies `Win64`, because
  // that data model is Windows, and `--abi` overrides it for cross-generation.
  ffi::FfiAbi abi = ffi::FfiAbi::Build;
};

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
  // Number of declarations emitted *commented out* because the selected
  // destination cannot marshal them: a C variadic needs a target with a
  // variadic FFI path, and a struct by value needs the System V AMD64 ABI. The
  // tool writes the real signature as a comment with the compiler's own reason:
  // the module compiles everywhere, and the user uncomments it on a target that
  // supports it. A declaration the *vocabulary* cannot express at all is
  // counted as `skipped`, not here.
  int commented = 0;
  // Number of `struct` declarations emitted before the `extern "C"` block.
  int structs = 0;
  // Number of `enum` members emitted as module constants.
  int constants = 0;
  // Number of object-like `#define` macros emitted as module constants. A
  // function-like macro or a replacement that is not a single literal is not a
  // constant and is not counted.
  int macroConstants = 0;
};

// Translates `headerSource` into an Oskel module. `sourceName` is the header's
// name, used only in the leading comment of the output (empty becomes "input").
Result generate(const std::string& headerSource, const std::string& sourceName = "",
                const Options& options = {});

}  // namespace bindgen
