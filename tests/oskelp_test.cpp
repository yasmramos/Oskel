// Tests for the `oskelp` tool library: the TOML subset, the manifest, the
// dependency graph and the lockfile. The tool is standalone, so this test links
// `oskelp_lib` only; the end-to-end CTest runner drives the real binary.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "oskelp.h"

namespace fs = std::filesystem;

static int g_failures = 0;
static std::vector<fs::path> g_tempDirs;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

static bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

static fs::path makeTempDir() {
  static int counter = 0;
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path base = fs::temp_directory_path() /
                  ("oskelp-test-" + std::to_string(stamp) + "-" +
                   std::to_string(++counter));
  fs::remove_all(base);
  fs::create_directories(base);
  g_tempDirs.push_back(base);
  return base;
}

static void writeFile(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << text;
}

static std::string manifestText(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& dependencies = {}) {
  std::string text = "[package]\nname = \"" + name +
                     "\"\nversion = \"0.1.0\"\nentry = \"src/" + name +
                     "/main.oskl\"\n\n[dependencies]\n";
  for (const auto& [dependency, path] : dependencies) {
    text += dependency + " = { path = \"" + path + "\" }\n";
  }
  return text;
}

static void makePackage(
    const fs::path& dir, const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& dependencies = {}) {
  writeFile(dir / "oskel.toml", manifestText(name, dependencies));
  writeFile(dir / "src" / name / "main.oskl", "print(\"ok\");\n");
}

static void testTomlBasics() {
  const std::string source =
      "# a comment\n"
      "[package]\n"
      "name = \"demo\"\n"
      "version = \"0.1.0\"\n"
      "authors = [\"Ada\", \"Grace\"]\n"
      "entry = \"src/demo/main.oskl\"  # trailing comment\n"
      "\n"
      "[dependencies]\n"
      "utils = { path = \"../utils\", version = \"1.2.3\" }\n"
      "flag = true\n"
      "count = 7\n";
  oskelp::TomlDocument document;
  std::string error;
  check(oskelp::parseToml(source, document, error), "toml: parses the sample");
  check(error.empty(), "toml: no error on the sample");

  auto packageIt = document.values.find("package");
  check(packageIt != document.values.end() && packageIt->second.isTable(),
        "toml: [package] is a table");
  if (packageIt != document.values.end() && packageIt->second.isTable()) {
    const auto& table = packageIt->second.table;
    check(table.at("name").text == "demo", "toml: name is a string");
    check(table.at("authors").isArray() &&
              table.at("authors").array.size() == 2 &&
              table.at("authors").array[1].text == "Grace",
          "toml: authors is a string array");
  }

  auto dependenciesIt = document.values.find("dependencies");
  check(dependenciesIt != document.values.end() &&
            dependenciesIt->second.isTable(),
        "toml: [dependencies] is a table");
  if (dependenciesIt != document.values.end() &&
      dependenciesIt->second.isTable()) {
    const auto& table = dependenciesIt->second.table;
    check(table.at("utils").isTable() &&
              table.at("utils").table.at("path").text == "../utils",
          "toml: inline table holds path");
    check(table.at("utils").table.at("version").text == "1.2.3",
          "toml: inline table holds version");
    check(table.at("flag").isBoolean() && table.at("flag").boolean,
          "toml: boolean value");
    check(table.at("count").isInteger() && table.at("count").integer == 7,
          "toml: integer value");
  }
}

static void testTomlEscapesAndErrors() {
  oskelp::TomlDocument document;
  std::string error;
  check(oskelp::parseToml("[package]\nname = \"a\\\"b\"\n", document, error),
        "toml: escaped quote parses");
  check(document.values.at("package").table.at("name").text == "a\"b",
        "toml: escaped quote is decoded");

  oskelp::TomlDocument broken;
  std::string brokenError;
  check(!oskelp::parseToml("[package\nname = \"x\"\n", broken, brokenError),
        "toml: a missing ']' is rejected");
  check(contains(brokenError, "expected ']'"),
        "toml: the error names the missing bracket");

  oskelp::TomlDocument duplicate;
  std::string duplicateError;
  check(!oskelp::parseToml("[package]\nname = \"a\"\nname = \"b\"\n", duplicate,
                           duplicateError),
        "toml: a duplicate key is rejected");
  check(contains(duplicateError, "duplicate key"),
        "toml: the error names the duplicate key");
}

static void testManifest() {
  fs::path root = makeTempDir();
  fs::path packageDir = root / "demo";
  writeFile(packageDir / "oskel.toml",
            "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n"
            "authors = [\"Ada\"]\n\n[dependencies]\n");
  writeFile(packageDir / "src" / "demo" / "main.oskl", "print(\"ok\");\n");

  oskelp::Manifest manifest;
  std::string error;
  check(oskelp::loadManifest((packageDir / "oskel.toml").string(), manifest,
                             error),
        "manifest: loads a minimal manifest");
  check(manifest.package.name == "demo", "manifest: name");
  check(manifest.package.version == "0.1.0", "manifest: version");
  check(manifest.package.entry == "src/demo/main.oskl",
        "manifest: default entry is src/<name>/main.oskl");
  check(manifest.package.authors.size() == 1 &&
            manifest.package.authors[0] == "Ada",
        "manifest: authors");

  writeFile(packageDir / "oskel.toml",
            "[package]\nname = \"demo-name\"\nversion = \"0.1.0\"\n");
  oskelp::Manifest invalid;
  std::string invalidError;
  check(!oskelp::loadManifest((packageDir / "oskel.toml").string(), invalid,
                              invalidError),
        "manifest: a hyphenated name is rejected");
  check(contains(invalidError, "not a valid Oskel identifier"),
        "manifest: the error names the identifier rule");

  writeFile(packageDir / "oskel.toml",
            "[package]\nname = \"demo\"\nversion = \"0.1\"\n");
  oskelp::Manifest badVersion;
  std::string badVersionError;
  check(!oskelp::loadManifest((packageDir / "oskel.toml").string(), badVersion,
                              badVersionError),
        "manifest: a two-part version is rejected");
  check(contains(badVersionError, "major.minor.patch"),
        "manifest: the error names the version shape");

  writeFile(packageDir / "oskel.toml",
            "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n\n"
            "[dependencies]\nutils = \"1.2.3\"\n");
  oskelp::Manifest registry;
  std::string registryError;
  check(!oskelp::loadManifest((packageDir / "oskel.toml").string(), registry,
                             registryError),
        "manifest: a registry dependency is rejected for now");
  check(contains(registryError, "registry dependencies are not supported"),
        "manifest: the error explains the registry gap");

  writeFile(packageDir / "oskel.toml",
            "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n\n"
            "[dependencies]\n"
            "utils = { path = \"../utils\", version = \"1.2\" }\n");
  oskelp::Manifest badDependencyVersion;
  std::string badDependencyVersionError;
  check(!oskelp::loadManifest((packageDir / "oskel.toml").string(),
                              badDependencyVersion, badDependencyVersionError),
        "manifest: a malformed dependency version is rejected");
  check(contains(badDependencyVersionError, "major.minor.patch"),
        "manifest: the dependency version error names the shape");

  writeFile(packageDir / "oskel.toml",
            "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n\n"
            "[extra]\nkey = true\n");
  oskelp::Manifest warned;
  std::string warnedError;
  check(oskelp::loadManifest((packageDir / "oskel.toml").string(), warned,
                             warnedError),
        "manifest: an unknown section is not fatal");
  check(!warned.warnings.empty() &&
            contains(warned.warnings[0], "unknown section"),
        "manifest: an unknown section produces a warning");

  fs::remove_all(root);
}

static void testFindManifest() {
  fs::path root = makeTempDir();
  makePackage(root / "project", "project");
  fs::create_directories(root / "project" / "src" / "project" / "nested");

  std::string path;
  check(oskelp::findManifest((root / "project" / "src" / "project" / "nested")
                                 .string(),
                             path),
        "findManifest: walks up to the manifest");
  check(fs::equivalent(path, root / "project" / "oskel.toml"),
        "findManifest: returns the project manifest");

  fs::path outside = makeTempDir();
  std::string missing;
  check(!oskelp::findManifest(outside.string(), missing),
        "findManifest: no manifest in an unrelated tree");

  fs::remove_all(root);
  fs::remove_all(outside);
}

static void testResolution() {
  fs::path root = makeTempDir();
  makePackage(root / "base", "base");
  makePackage(root / "utils", "utils", {{"base", "../base"}});
  makePackage(root / "app", "app", {{"utils", "../utils"}});

  oskelp::Manifest app;
  std::string error;
  check(oskelp::loadManifest((root / "app" / "oskel.toml").string(), app,
                             error),
        "resolution: loads the root manifest");

  oskelp::Resolution resolution;
  check(oskelp::resolveDependencies(app, resolution, error),
        "resolution: resolves a path dependency tree");
  check(resolution.packages.size() == 3,
        "resolution: base, utils and the root");
  if (resolution.packages.size() == 3) {
    check(resolution.packages[0].name == "base",
          "resolution: a dependency of a dependency comes first");
    check(resolution.packages[1].name == "utils",
          "resolution: the direct dependency follows");
    check(resolution.packages[2].name == "app" &&
              resolution.packages[2].root,
          "resolution: the root comes last");
  }
  check(resolution.find("utils") != nullptr, "resolution: find() by name");

  oskelp::Lockfile lock = oskelp::buildLockfile(resolution);
  check(lock.packages.size() == 2, "lockfile: dependencies only");
  check(lock.packages.count("utils") == 1 &&
            lock.packages.at("utils").source == "path+../utils",
        "lockfile: the path source is recorded");

  fs::remove_all(root);
}

static void testCycle() {
  fs::path root = makeTempDir();
  makePackage(root / "a", "a", {{"b", "../b"}});
  makePackage(root / "b", "b", {{"a", "../a"}});

  oskelp::Manifest a;
  std::string error;
  check(oskelp::loadManifest((root / "a" / "oskel.toml").string(), a, error),
        "cycle: loads the root");
  oskelp::Resolution resolution;
  check(!oskelp::resolveDependencies(a, resolution, error),
        "cycle: a cycle is rejected");
  check(contains(error, "dependency cycle"),
        "cycle: the error names the cycle");
  check(contains(error, "a -> b -> a") || contains(error, "b -> a -> b"),
        "cycle: the error shows the path");

  fs::remove_all(root);
}

static void testDuplicateLocation() {
  fs::path root = makeTempDir();
  makePackage(root / "a1", "a");
  makePackage(root / "a2", "a");
  makePackage(root / "b", "b", {{"a", "../a2"}});
  makePackage(root / "app", "app", {{"a", "../a1"}, {"b", "../b"}});

  oskelp::Manifest app;
  std::string error;
  check(oskelp::loadManifest((root / "app" / "oskel.toml").string(), app,
                             error),
        "duplicate: loads the root");
  oskelp::Resolution resolution;
  check(!oskelp::resolveDependencies(app, resolution, error),
        "duplicate: two locations for one name are rejected");
  check(contains(error, "two different locations"),
        "duplicate: the error explains the conflict");

  fs::remove_all(root);
}

static void testLockfile() {
  oskelp::Lockfile lock;
  lock.packages["utils"].name = "utils";
  lock.packages["utils"].version = "0.1.0";
  lock.packages["utils"].source = "path+../utils";

  std::string text = oskelp::renderLockfile(lock);
  check(contains(text, "[packages.utils]"), "lockfile: renders a package table");
  check(contains(text, "source = \"path+../utils\""),
        "lockfile: renders the source");

  oskelp::Lockfile parsed;
  std::string error;
  check(oskelp::parseLockfile(text, parsed, error),
        "lockfile: round-trips through the parser");
  check(parsed.packages.size() == 1 &&
            parsed.packages.at("utils").version == "0.1.0" &&
            parsed.packages.at("utils").source == "path+../utils",
        "lockfile: the parsed entry matches");

  oskelp::Lockfile broken;
  std::string brokenError;
  check(!oskelp::parseLockfile("version = 2\n", broken, brokenError),
        "lockfile: an unsupported version is rejected");
  check(contains(brokenError, "unsupported lockfile version"),
        "lockfile: the error names the version");
}

static void testVerifyLockfile() {
  fs::path root = makeTempDir();
  makePackage(root / "utils", "utils");
  makePackage(root / "app", "app", {{"utils", "../utils"}});

  oskelp::Manifest app;
  std::string error;
  check(oskelp::loadManifest((root / "app" / "oskel.toml").string(), app,
                             error),
        "verify: loads the root");
  oskelp::Resolution resolution;
  check(oskelp::resolveDependencies(app, resolution, error),
        "verify: resolves the tree");

  oskelp::Lockfile lock = oskelp::buildLockfile(resolution);
  check(oskelp::verifyLockfile(app, resolution, lock, error),
        "verify: a matching lockfile is accepted");

  oskelp::Lockfile missing;
  std::string missingError;
  check(!oskelp::verifyLockfile(app, resolution, missing, missingError),
        "verify: an empty lockfile is rejected");
  check(contains(missingError, "missing 'utils'"),
        "verify: the error names the missing package");

  oskelp::Lockfile drift = lock;
  drift.packages["utils"].version = "9.9.9";
  std::string driftError;
  check(!oskelp::verifyLockfile(app, resolution, drift, driftError),
        "verify: a version drift is rejected");
  check(contains(driftError, "pins 'utils'"),
        "verify: the error explains the drift");

  oskelp::Lockfile extra = lock;
  extra.packages["ghost"].name = "ghost";
  extra.packages["ghost"].version = "0.1.0";
  extra.packages["ghost"].source = "path+../ghost";
  std::string extraError;
  check(!oskelp::verifyLockfile(app, resolution, extra, extraError),
        "verify: a stale entry is rejected");
  check(contains(extraError, "no longer depends on"),
        "verify: the error explains the stale entry");

  fs::remove_all(root);
}

static void testRenderManifest() {
  oskelp::Package package;
  package.name = "demo";
  package.version = "0.1.0";
  package.entry = "src/demo/main.oskl";
  package.authors = {"Ada"};
  const std::string text = oskelp::renderManifest(package);
  check(contains(text, "name = \"demo\""), "render: package name");
  check(contains(text, "entry = \"src/demo/main.oskl\""), "render: entry");
  check(contains(text, "authors = [\"Ada\"]"), "render: authors");
  check(contains(text, "[dependencies]"), "render: dependencies section");
}

static void testHelpers() {
  check(oskelp::normalizeSlashes("a\\b\\c") == "a/b/c",
        "helpers: normalizeSlashes");
  check(contains(oskelp::joinPath("a", "b"), "b"), "helpers: joinPath");

  fs::path root = makeTempDir();
  writeFile(root / "one.oskl", "print(1);\n");
  writeFile(root / "nested" / "two.oskl", "print(2);\n");
  writeFile(root / "nested" / "notes.txt", "ignore me\n");
  std::vector<std::string> files = oskelp::listSourceFiles(root.string());
  check(files.size() == 2, "helpers: listSourceFiles finds both modules");
  check(files.size() == 2 && files[0] < files[1],
        "helpers: listSourceFiles is sorted");
  fs::remove_all(root);
}

int main() {
  testTomlBasics();
  testTomlEscapesAndErrors();
  testManifest();
  testFindManifest();
  testResolution();
  testCycle();
  testDuplicateLocation();
  testLockfile();
  testVerifyLockfile();
  testRenderManifest();
  testHelpers();

  for (const fs::path& dir : g_tempDirs) {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  if (g_failures != 0) {
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("\nall oskelp tests passed\n");
  return 0;
}
