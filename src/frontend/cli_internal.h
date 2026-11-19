#pragma once

#include <string>
#include <vector>

#include "compiler.h"
#include "frontend/module_resolver.h"
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

// The AOT target `--target` names, and the host's own target.
bool parseTargetName(const std::string& name, Target& out);
Target hostTarget();

// The link mode `--mode` names.
bool parseLinkModeName(const std::string& name, LinkMode& out);

// The C compiler the hosted link drives (PHASE 7f.4), from `$CC` or `cc`.
std::string hostedLinkerCommand();

// Quotes a path for a POSIX shell.
std::string shellQuote(const std::string& path);

// Links a relocatable object into a hosted executable with the host C
// compiler. Returns 0 on success, 65 when the toolchain is missing or the link
// fails -- never a half-linked image.
int linkHostedObject(const std::string& objectPath, const std::string& output,
                     const std::string& tool);

// Compiles `source` (resolving imports) and writes a native binary to
// `output`. Returns 0 on success, 65 for compile errors and 74 for a write
// failure. `tool` names the binary in diagnostics (`oskel build` or `oskelc`).
int buildBinary(const std::string& source, const std::string& name,
                const std::string& output, const std::string& argv0,
                const Target& target, LinkMode mode, const std::string& tool);

// Parses the AOT command line (`<file.oskl> [-o <out>] [--target T] [--mode M]`)
// and writes the binary. Shared by `oskel build` and by `oskelc`, so the two
// cannot drift: `oskel build` is a thin alias over this function.
//
// `tool` is the name used in diagnostics and in the usage line (`oskel build`
// or `oskelc`).
int runBuildCommand(const std::vector<std::string>& arguments,
                    const std::string& argv0, const std::string& tool,
                    const std::string& usage);
