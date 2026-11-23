#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// `oskelp`: the project and package manager for Oskel.
//
// It is a standalone tool (like `bindgen`): it links no engine code and drives
// `oskel`/`oskelc` as child processes. The language, the bytecode, the VM and
// the AOT backend are untouched. The only compiler-side change is that
// `resolverFor` honours `OSKEL_MODULE_ROOT`, so a package can declare `src/` as
// its module root instead of the entry file's directory.
//
// The project model:
//
//   oskel.toml                 the manifest
//   oskel.lock                 the generated lockfile
//   src/<package>/<module>.oskl
//   tests/<name>.oskl
//   target/<name>              the native binary
//   .oskel/deps/<package>/...  dependencies, materialised for the compiler
//
// Every package lives under `src/<package>/`, and an import is always
// `<package>.<module>` (or `std.<module>`). `oskelp` runs the compiler with
// `OSKEL_MODULE_ROOT=<root>/src` and `OSKEL_PACKAGES=<root>/.oskel/deps`.
namespace oskelp {

// Exit codes follow the toolchain convention; 101 is the cargo-style "some
// tests failed".
enum ExitCode : int {
  kOk = 0,
  kFailure = 1,
  kUsage = 64,
  kCompileError = 65,
  kIoError = 66,
  kRuntimeError = 70,
  kTestsFailed = 101,
};

// ---------------------------------------------------------------------------
// TOML subset

struct TomlValue {
  enum class Type { String, Integer, Boolean, Array, Table };

  Type type = Type::String;
  std::string text;
  long long integer = 0;
  bool boolean = false;
  std::vector<TomlValue> array;
  std::map<std::string, TomlValue> table;

  bool isString() const { return type == Type::String; }
  bool isInteger() const { return type == Type::Integer; }
  bool isBoolean() const { return type == Type::Boolean; }
  bool isArray() const { return type == Type::Array; }
  bool isTable() const { return type == Type::Table; }
};

struct TomlDocument {
  std::map<std::string, TomlValue> values;
};

// Parses the supported TOML subset: comments, `[a]`/`[a.b]` sections, basic
// strings with the usual escapes, integers, booleans, arrays and inline tables.
// On failure returns false and sets `error` to "<line>:<column>: message".
bool parseToml(const std::string& source, TomlDocument& out, std::string& error);

// ---------------------------------------------------------------------------
// Semantic versions

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string prerelease;  // empty for a release
};

// Parses `major.minor.patch[-prerelease][+build]`. Build metadata is ignored.
bool parseSemVer(const std::string& text, SemVer& out);

// Orders two parsed versions, prereleases before their release.
int compareSemVer(const SemVer& a, const SemVer& b);

// True when `version` satisfies `requirement`. Supported requirements are an
// exact version, `^x.y.z`, `~x.y.z`, `>=`, `>`, `<=`, `<`, `=`, `*`/`x`
// wildcards and a comma- or space-separated conjunction of those. A
// prerelease only matches an exact requirement for that prerelease.
bool satisfiesRequirement(const std::string& requirement,
                          const SemVer& version);

// True when the requirement is syntactically supported (not necessarily
// satisfiable by any published version).
bool isValidRequirement(const std::string& requirement);

// ---------------------------------------------------------------------------
// Manifest

struct Dependency {
  std::string name;
  std::string path;         // empty for a registry dependency
  std::string version;      // optional exact version for a path dependency
  std::string requirement;  // registry version requirement

  bool isRegistry() const { return path.empty(); }
};

struct Package {
  std::string name;
  std::string version;
  std::string entry;  // relative to the manifest directory
  std::vector<std::string> authors;
  std::map<std::string, Dependency> dependencies;
};

struct Manifest {
  Package package;
  std::string path;                   // absolute path to oskel.toml
  std::string dir;                    // absolute directory of the manifest
  std::vector<std::string> warnings;  // unknown keys and sections
};

bool loadManifest(const std::string& path, Manifest& out, std::string& error);

// Walks up from `startDir` looking for `oskel.toml`. Returns false when no
// manifest is found; `manifestPath` is the absolute path when it is.
bool findManifest(const std::string& startDir, std::string& manifestPath);

// Canonical text for a fresh manifest.
std::string renderManifest(const Package& package);

// Edits a manifest's text in place, preserving comments, blank lines and line
// endings. `addDependencyToManifest` inserts the entry at the end of
// `[dependencies]` (creating the section when missing) and refuses a duplicate;
// `removeDependencyFromManifest` deletes the entry and refuses one that spans
// multiple lines, because deleting a partial line would corrupt the file.
bool addDependencyToManifest(const std::string& source,
                             const Dependency& dependency, std::string& out,
                             std::string& error);
bool removeDependencyFromManifest(const std::string& source,
                                  const std::string& name, std::string& out,
                                  std::string& error);

// ---------------------------------------------------------------------------
// Lockfile

struct LockedPackage {
  std::string name;
  std::string version;
  std::string source;  // "path+../utils"
};

struct Lockfile {
  int version = 1;
  std::map<std::string, LockedPackage> packages;
};

bool parseLockfile(const std::string& source, Lockfile& out, std::string& error);
std::string renderLockfile(const Lockfile& lock);

// ---------------------------------------------------------------------------
// Dependency graph

struct ResolvedPackage {
  std::string name;
  std::string version;
  std::string source;  // "path+../utils"
  std::string dir;     // absolute package directory
  bool root = false;
};

struct Resolution {
  std::vector<ResolvedPackage> packages;  // dependencies first, root last

  const ResolvedPackage* find(const std::string& name) const;
};

// Resolves every dependency, recursively. A path dependency is read from its
// directory; a registry dependency is picked from `$OSKEL_REGISTRY`, preferring
// `preferred[name]` when it still satisfies the requirement. `forced[name]`
// pins a registry package to exactly that version: it is an error when the
// version is missing from the registry or does not satisfy the requirement, so
// `oskelp update --precise` never falls back silently. Detects cycles, two
// different locations for the same package name, and a dependency whose
// declared name disagrees with its manifest.
bool resolveDependencies(
    const Manifest& root, Resolution& out, std::string& error,
    const std::map<std::string, std::string>& preferred = {},
    const std::map<std::string, std::string>& forced = {});

// The highest version of `name` published in `$OSKEL_REGISTRY`, as a
// `major.minor.patch[-prerelease]` string. With `includePrerelease == false`,
// only releases count; a package that has only prereleases fails with a
// message telling the user to pass an explicit version.
bool latestRegistryVersion(const std::string& name, std::string& version,
                           bool includePrerelease, std::string& error);

// True when `<registry>/<name>/<version>/.yanked` exists. A yanked version is
// hidden from new selections (`add`, `update`, `outdated`) but a lockfile pin
// that already names it keeps working, so a build does not break when a
// publisher withdraws a release.
bool isRegistryVersionYanked(const std::string& name, const std::string& version,
                             const std::string& registry);

Lockfile buildLockfile(const Resolution& resolution);

// Compares a loaded lockfile with a fresh resolution. On failure sets `error`
// to the exact reason (missing entry, version drift, moved path, changed
// registry source, stale entry).
bool verifyLockfile(const Manifest& root, const Resolution& resolution,
                    const Lockfile& lock, std::string& error);

// ---------------------------------------------------------------------------
// Commands
//
// Each returns an exit code and writes its diagnostics to stderr. `argv0` is
// the path of the `oskelp` executable, used to find its sibling `oskel` and
// `oskelc`.

struct BuildOptions {
  std::string target;  // empty: the host
  std::string mode;    // empty: the compiler default (freestanding)
  bool locked = false;
};

int commandNew(const std::string& directory, const std::string& name,
               const std::string& vcs, const std::string& argv0);
int commandInit(const std::string& directory, const std::string& name,
                const std::string& vcs, const std::string& argv0);
// `add` edits the manifest, then re-resolves and refreshes the lockfile and
// `.oskel/deps`; it never runs the compiler. With `--path`, `version` is an
// optional exact assertion for the path dependency. Without `--path`, `version`
// is the registry requirement and `name` is required.
int commandAdd(const std::string& manifestPath, const std::string& name,
               const std::string& path, const std::string& version);
int commandRemove(const std::string& manifestPath, const std::string& name);
// Copies `oskel.toml` and `src/` into `<registry>/<name>/<version>`. `registry`
// is `--registry`, or `$OSKEL_REGISTRY` when empty.
int commandPublish(const std::string& manifestPath, const std::string& registry);
// Marks a published version as yanked (`--undo` clears the marker). `registry`
// is `--registry`, or `$OSKEL_REGISTRY` when empty. Yanking an already yanked
// version and un-yanking a live one are successful no-ops, so the command is
// idempotent.
int commandYank(const std::string& name, const std::string& version,
                const std::string& registry, bool undo);
// Re-resolves ignoring the lockfile pin for `name` (or for every package when
// `name` is empty), writes the lockfile and materialises the new tree. A
// non-empty `precise` pins `name` to exactly that registry version and fails
// when it is missing, is not a registry dependency, or does not satisfy the
// manifest requirement.
int commandUpdate(const std::string& manifestPath, const std::string& name,
                  const std::string& precise);
// Prints each registry dependency whose pinned version is behind the registry.
// The target is the highest version still allowed by a direct dependency's
// requirement; when a newer version exists but needs a manifest change, the
// line says so instead of hiding it.
int commandOutdated(const std::string& manifestPath);
int commandBuild(const std::string& manifestPath, const BuildOptions& options,
                 const std::string& argv0);
int commandRun(const std::string& manifestPath, const BuildOptions& options,
               bool native, const std::vector<std::string>& args,
               const std::string& argv0);
int commandCheck(const std::string& manifestPath, const BuildOptions& options,
                 const std::string& argv0);
int commandTest(const std::string& manifestPath, const BuildOptions& options,
                bool native, const std::string& argv0);
int commandClean(const std::string& manifestPath, const std::string& argv0);
int commandTree(const std::string& manifestPath, const std::string& argv0);

// ---------------------------------------------------------------------------
// Helpers (exposed for the unit tests)

std::string absolutePath(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
std::string normalizeSlashes(const std::string& path);

// Every `.oskl` file under `dir`, recursively, sorted by path.
std::vector<std::string> listSourceFiles(const std::string& dir);

// Runs `argv` with the extra environment in `env`, in `workingDir`. Returns the
// child's exit code, or 127 when it could not be started.
int runProcess(const std::vector<std::string>& argv,
               const std::vector<std::pair<std::string, std::string>>& env,
               const std::string& workingDir);

// Finds an executable next to `oskelp` first, then on PATH.
bool findExecutable(const std::string& argv0, const std::string& name,
                    std::string& out);

// Like `findExecutable`, with the `$OSKEL`/`$OSKELC` overrides.
bool findTool(const std::string& argv0, const std::string& name, std::string& out);

}  // namespace oskelp
