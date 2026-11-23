#pragma once

#include <string>
#include <vector>

#include "bytecode_io.h"
#include "compiler.h"
#include "frontend/module_resolver.h"
#include "frontend/parser.h"
#include "native/target.h"

// Shared plumbing for the two entry points of the toolchain.
//
// `oskel` (the development engine) and `oskelc` (the host compiler) are two
// front ends of one language: they parse and compile with the same code and
// differ only in what they do with the bytecode. `oskel` runs it on the VM and
// offers the REPL; `oskelc` lowers it to a native image. Keeping the shared
// part here is what makes the split a second `main` instead of a fork
// (DEVELOPMENT.md §2, toolchain boundary). Each binary supplies its own `main`
// and links this file; nothing else includes this header.

// The version both binaries print (`include/version.h` is the only source).
extern const char *kVersion;

// Directory of the standard library shipped with the toolchain. `std.*`
// imports resolve here; `$OSKEL_STDLIB` overrides the search.
std::string standardLibraryDir(const std::string& argv0);

// Installed-packages root: `$OSKEL_PACKAGES`, else `$HOME/.oskel/packages`.
std::string packagesDir();

// Resolver for a program named `name`: imports resolve from the project root,
// the directory of the file being compiled (or the working directory for
// `-e`/stdin).
ModuleResolver resolverFor(const std::string& name, const std::string& argv0);

// The module loader the compiler uses for `import` declarations.
Compiler::ImportLoader makeImportLoader(ModuleResolver resolver);

// Builds the scanner the parser uses to make a module's enums visible at parse
// time (PHASE 6.9 across modules). It resolves and parses each module once,
// caching the result, so `import { Color } from geom;` and `geom.Color.RED`
// are recognized before the compiler loads the module. A module that cannot be
// read here is reported by the compiler with its own diagnostic.
Parser::EnumScanner makeEnumScanner(ModuleResolver resolver);

// The AOT target `--target` names, and the host's own target.
bool parseTargetName(const std::string& name, Target& out);
Target hostTarget();

// The link mode `--mode` names.
bool parseLinkModeName(const std::string& name, LinkMode& out);

// How a hosted object is closed (PHASE 10.1): `Host` drives the host C
// compiler, `Own` uses the project's own linker (`native/link.*`). The choice
// is a command-line flag, never language syntax.
enum class Linker { Host, Own };
bool parseLinkerName(const std::string& name, Linker& out);
const char* linkerName(Linker linker);

// The C compiler the hosted link drives (PHASE 7f.4), from `$CC` or `cc`.
std::string hostedLinkerCommand();

// Quotes a path for a POSIX shell.
std::string shellQuote(const std::string& path);

// Links a relocatable object into a hosted executable with the host C
// compiler. `linkLibs` are library names (`SDL3`, passed as `-l<name>`) or
// paths (the bundled `libSDL3.so`/`SDL3.lib`), which is how a program's
// `extern "C"` symbols in a third-party library resolve. `linkObjects` are
// extra object files or `ar` archives passed to the compiler driver as
// additional inputs (PHASE 10.2); `runPaths` become rpath entries on POSIX.
// Returns 0 on success, 65 when the toolchain is missing or the link fails --
// never a half-linked image.
int linkHostedObject(const std::string& objectPath, const std::string& output,
                     const Target& target, const std::string& tool,
                     const std::vector<std::string>& linkLibs,
                     const std::vector<std::string>& libraryPaths,
                     const std::vector<std::string>& linkObjects,
                     const std::vector<std::string>& runPaths);

// Compiles `source` (resolving imports) and writes a native binary to
// `output`. Returns 0 on success, 65 for compile errors and 74 for a write
// failure. `tool` names the binary in diagnostics (`oskel build` or `oskelc`).
// `linker` selects how a hosted object is closed and `emitObject` stops after
// writing that object (the input of `oskelld`). `linkLibs` and `linkObjects`
// reach the hosted linker; both are empty in freestanding mode. When
// `autoHostedForUi` is set, a program that imports a native UI backend selects
// the hosted mode and the bundled libraries itself; an explicit mode still
// wins.
int buildBinary(const std::string& source, const std::string& name,
                const std::string& output, const std::string& argv0,
                const Target& target, LinkMode mode, Linker linker,
                bool emitObject, bool autoHostedForUi,
                const std::vector<std::string>& linkLibs,
                const std::vector<std::string>& libraryPaths,
                const std::vector<std::string>& linkObjects,
                const std::string& tool);

// Compiles a `.oskc` file (the already-compiled form of a program) to a native
// binary. The file carries the FFI capabilities it was validated against, and
// this path checks them against the destination before lowering, so a chunk
// written on one machine builds on another or fails with a message -- never a
// mis-compiled binary. Returns 0 on success, 65 for a lowering error, 66 for an
// unreadable or malformed file and 74 for a write failure.
int buildBinaryFromBytecode(const std::string& path, const std::string& output,
                            const Target& target, LinkMode mode, Linker linker,
                            bool emitObject, const std::vector<std::string>& linkLibs,
                            const std::vector<std::string>& libraryPaths,
                            const std::vector<std::string>& linkObjects,
                            const std::string& tool);

// Reads a `.oskc` image, checks its FFI capabilities against `destination` and
// rebuilds the chunk. `needs` receives the requirement bitset the header
// records (`kOskcNeedsSdl`, `kOskcNeedsSkia`), so the AOT path can link the same
// UI backend the source path would. Returns true on success; prints a
// diagnostic and returns false otherwise.
bool loadBytecodeImage(const std::string& path,
                       const ffi::AbiCapabilities& destination, Bytecode& chunk,
                       std::uint32_t& needs, const std::string& tool);

// Compiles `source` to a `.oskc` image at `output` (`oskelc -c`). Validates the
// FFI surface against `kAotAbiCapabilities`, the fixed surface the format
// records, so the file does not depend on how the writing engine was built.
// Returns 0 on success, 65 for a compile error and 74 for a write failure.
int emitBytecodeFile(const std::string& source, const std::string& name,
                     const std::string& output, const std::string& argv0,
                     const std::string& tool);

// Parses the `.oskc` command line (`<file.oskl> [-o <out>]`) and writes the
// image. Drives `oskelc -c`.
int runBytecodeCommand(const std::vector<std::string>& arguments,
                       const std::string& argv0, const std::string& tool,
                       const std::string& usage);

// Parses the AOT command line
// (`<file.oskl> [-o <out>] [--target T] [--mode M] [--link-lib L]`) and writes
// the binary. Shared by `oskel build` and by `oskelc`, so the two cannot drift:
// `oskel build` is a thin alias over this function.
//
// `tool` is the name used in diagnostics and in the usage line (`oskel build`
// or `oskelc`).
int runBuildCommand(const std::vector<std::string>& arguments,
                    const std::string& argv0, const std::string& tool,
                    const std::string& usage);
