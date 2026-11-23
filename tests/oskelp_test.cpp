// Tests for the `oskelp` tool library: the TOML subset, the manifest, the
// dependency graph and the lockfile. The tool is standalone, so this test links
// `oskelp_lib` only; the end-to-end CTest runner drives the real binary.

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

static oskelp::SemVer parseVersion(const std::string& text) {
  oskelp::SemVer version;
  check(oskelp::parseSemVer(text, version), "semver: parses a valid version");
  return version;
}

static void testSemver() {
  parseVersion("1.2.3");
  parseVersion("1.2.3-beta.1");
  parseVersion("1.2.3+build.5");
  parseVersion("1.2.3-beta.1+build.5");

  oskelp::SemVer ignored;
  check(!oskelp::parseSemVer("1.2", ignored), "semver: two parts are rejected");
  check(!oskelp::parseSemVer("1.2.3.4", ignored),
        "semver: four parts are rejected");
  check(!oskelp::parseSemVer("01.2.3", ignored),
        "semver: a leading zero is rejected");
  check(!oskelp::parseSemVer("1.2.3-01", ignored),
        "semver: a numeric prerelease leading zero is rejected");
  check(!oskelp::parseSemVer("1.2.3-", ignored),
        "semver: an empty prerelease is rejected");

  check(oskelp::compareSemVer(parseVersion("1.2.3"), parseVersion("1.2.4")) < 0,
        "semver: patch order");
  check(oskelp::compareSemVer(parseVersion("2.0.0"), parseVersion("1.9.9")) > 0,
        "semver: major order");
  check(oskelp::compareSemVer(parseVersion("1.2.3-beta"),
                              parseVersion("1.2.3")) < 0,
        "semver: a prerelease precedes its release");
  check(oskelp::compareSemVer(parseVersion("1.2.3-beta.10"),
                              parseVersion("1.2.3-beta.2")) > 0,
        "semver: numeric prerelease identifiers compare numerically");
  check(oskelp::compareSemVer(parseVersion("1.2.3-1"),
                              parseVersion("1.2.3-alpha")) < 0,
        "semver: a numeric identifier precedes an alphanumeric one");

  auto satisfies = [](const std::string& requirement,
                      const std::string& version) {
    return oskelp::satisfiesRequirement(requirement, parseVersion(version));
  };
  check(satisfies("1.2.3", "1.2.3"), "semver: exact matches");
  check(!satisfies("1.2.3", "1.2.4"), "semver: exact rejects a neighbour");
  check(satisfies("^1.2.3", "1.9.9"), "semver: caret within the major");
  check(!satisfies("^1.2.3", "2.0.0"), "semver: caret rejects the next major");
  check(!satisfies("^1.2.3", "1.2.2"), "semver: caret rejects an older patch");
  check(satisfies("^0.2.3", "0.2.9"), "semver: caret within the minor");
  check(!satisfies("^0.2.3", "0.3.0"), "semver: caret rejects the next minor");
  check(satisfies("^0.0.3", "0.0.3"), "semver: caret on a patch");
  check(!satisfies("^0.0.3", "0.0.4"),
        "semver: caret on a patch rejects the next patch");
  check(satisfies("~1.2.3", "1.2.9"), "semver: tilde within the minor");
  check(!satisfies("~1.2.3", "1.3.0"), "semver: tilde rejects the next minor");
  check(satisfies("~1.2", "1.2.0"), "semver: partial tilde");
  check(!satisfies("~1.2", "1.3.0"), "semver: partial tilde upper bound");
  check(satisfies(">=1.2.0 <2.0.0", "1.5.0"),
        "semver: a comparator conjunction");
  check(!satisfies(">=1.2.0 <2.0.0", "2.0.0"),
        "semver: the conjunction upper bound");
  check(satisfies(">= 1.2.0, < 2.0.0", "1.5.0"),
        "semver: spaces after the operator and commas");
  check(satisfies("1.2.*", "1.2.5"), "semver: a patch wildcard");
  check(!satisfies("1.2.*", "1.3.0"), "semver: a patch wildcard upper bound");
  check(satisfies("1.*", "1.9.9"), "semver: a minor wildcard");
  check(!satisfies("1.*", "2.0.0"), "semver: a minor wildcard upper bound");
  check(satisfies("*", "9.9.9"), "semver: a bare wildcard");
  check(satisfies("1.2.3-beta.1", "1.2.3-beta.1"),
        "semver: an exact prerelease");
  check(!satisfies("^1.2.3", "1.2.4-beta"),
        "semver: a range does not select a prerelease");

  check(oskelp::isValidRequirement(">=1.0.0 <2.0.0"),
        "semver: a comparator conjunction is valid");
  check(!oskelp::isValidRequirement("1.2.3 || 2.0.0"),
        "semver: disjunction is not supported");
  check(!oskelp::isValidRequirement(">=*"),
        "semver: a comparator on a wildcard is invalid");
  check(!oskelp::isValidRequirement(">="), "semver: a bare operator is invalid");
  check(!oskelp::isValidRequirement(""), "semver: an empty requirement is invalid");
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
            "[dependencies]\nutils = \"^1.2.3\"\n");
  oskelp::Manifest registry;
  std::string registryError;
  check(oskelp::loadManifest((packageDir / "oskel.toml").string(), registry,
                             registryError),
        "manifest: a registry dependency is accepted");
  auto registryDep = registry.package.dependencies.find("utils");
  check(registryDep != registry.package.dependencies.end() &&
            registryDep->second.isRegistry() &&
            registryDep->second.requirement == "^1.2.3",
        "manifest: the registry requirement is recorded");

  writeFile(packageDir / "oskel.toml",
            "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n\n"
            "[dependencies]\nutils = \"not a version\"\n");
  oskelp::Manifest badRequirement;
  std::string badRequirementError;
  check(!oskelp::loadManifest((packageDir / "oskel.toml").string(),
                              badRequirement, badRequirementError),
        "manifest: an invalid registry requirement is rejected");
  check(contains(badRequirementError, "invalid version requirement"),
        "manifest: the error names the requirement");

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

static void setRegistry(const std::string& value) {
#ifdef _WIN32
  _putenv_s("OSKEL_REGISTRY", value.c_str());
#else
  setenv("OSKEL_REGISTRY", value.c_str(), 1);
#endif
}

static void clearRegistry() {
#ifdef _WIN32
  _putenv_s("OSKEL_REGISTRY", "");
#else
  unsetenv("OSKEL_REGISTRY");
#endif
}

static void makeRegistryPackage(const fs::path& registry,
                                const std::string& name,
                                const std::string& version) {
  fs::path dir = registry / name / version;
  writeFile(dir / "oskel.toml",
            "[package]\nname = \"" + name + "\"\nversion = \"" + version +
                "\"\nentry = \"src/" + name +
                "/main.oskl\"\n\n[dependencies]\n");
  writeFile(dir / "src" / name / "main.oskl", "print(\"ok\");\n");
}

static void testRegistryResolution() {
  fs::path root = makeTempDir();
  fs::path registry = root / "registry";
  fs::path appDir = root / "app";
  makePackage(appDir, "app");
  makeRegistryPackage(registry, "utils", "0.1.0");
  makeRegistryPackage(registry, "utils", "0.2.0");
  makeRegistryPackage(registry, "utils", "0.2.1");
  makeRegistryPackage(registry, "utils", "1.0.0");

  oskelp::Manifest app;
  std::string error;
  check(oskelp::loadManifest((appDir / "oskel.toml").string(), app, error),
        "registry: loads the app");
  oskelp::Dependency utils;
  utils.name = "utils";
  app.package.dependencies["utils"] = utils;

  const char* previous = std::getenv("OSKEL_REGISTRY");
  std::string saved = previous == nullptr ? std::string() : previous;
  bool hadRegistry = previous != nullptr;
  setRegistry(registry.string());

  oskelp::Resolution resolution;
  auto resolve = [&](const std::string& requirement,
                     const std::map<std::string, std::string>& preferred = {}) {
    app.package.dependencies["utils"].requirement = requirement;
    resolution = oskelp::Resolution{};
    error.clear();
    return oskelp::resolveDependencies(app, resolution, error, preferred);
  };

  check(resolve("0.*"), "registry: resolves a range");
  const oskelp::ResolvedPackage* selected = resolution.find("utils");
  check(selected != nullptr && selected->version == "0.2.1",
        "registry: picks the highest satisfying version");
  check(selected != nullptr &&
            selected->source == "registry+utils@0.2.1",
        "registry: records the registry source");

  check(resolve("0.*", {{"utils", "0.1.0"}}),
        "registry: honours a lockfile pin");
  selected = resolution.find("utils");
  check(selected != nullptr && selected->version == "0.1.0",
        "registry: the pin wins over the highest version");

  check(resolve("0.*", {{"utils", "9.9.9"}}),
        "registry: ignores a pin that does not exist");
  selected = resolution.find("utils");
  check(selected != nullptr && selected->version == "0.2.1",
        "registry: a missing pin falls back to the highest version");

  check(resolve("~0.2.0"), "registry: resolves a tilde range");
  selected = resolution.find("utils");
  check(selected != nullptr && selected->version == "0.2.1",
        "registry: the tilde range selects the right minor");

  check(!resolve(">=9.0.0"), "registry: an unsatisfiable range fails");
  check(contains(error, "no version of 'utils' satisfies"),
        "registry: the error names the missing version");

  if (resolve("0.*")) {
    oskelp::Lockfile lock = oskelp::buildLockfile(resolution);
    check(oskelp::verifyLockfile(app, resolution, lock, error),
          "registry: the lockfile verifies");
    lock.packages["utils"].source = "registry+utils@0.1.0";
    check(!oskelp::verifyLockfile(app, resolution, lock, error),
          "registry: a changed source is rejected");
    check(contains(error, "lockfile source"),
          "registry: the error names the source");
  }

  clearRegistry();
  check(!resolve("^0.1.0"), "registry: an unset OSKEL_REGISTRY fails");
  check(contains(error, "OSKEL_REGISTRY"),
        "registry: the error names OSKEL_REGISTRY");

  if (hadRegistry) setRegistry(saved);
  else clearRegistry();
  fs::remove_all(root);
}

static void testLatestRegistryVersion() {
  fs::path root = makeTempDir();
  fs::path registry = root / "registry";
  makeRegistryPackage(registry, "utils", "0.1.0");
  makeRegistryPackage(registry, "utils", "0.2.0");
  makeRegistryPackage(registry, "utils", "0.2.1-beta.1");
  makeRegistryPackage(registry, "utils", "1.0.0");
  makeRegistryPackage(registry, "pre", "0.1.0-beta.1");

  const char* previous = std::getenv("OSKEL_REGISTRY");
  std::string saved = previous == nullptr ? std::string() : previous;
  bool hadRegistry = previous != nullptr;

  clearRegistry();
  std::string version;
  std::string error;
  check(!oskelp::latestRegistryVersion("utils", version, false, error),
        "latest: an unset registry fails");
  check(contains(error, "OSKEL_REGISTRY"),
        "latest: the error names OSKEL_REGISTRY");

  setRegistry(registry.string());
  check(oskelp::latestRegistryVersion("utils", version, false, error),
        "latest: reads the highest release");
  check(version == "1.0.0", "latest: prereleases do not hide a release");
  check(oskelp::latestRegistryVersion("utils", version, true, error),
        "latest: includePrerelease still picks the highest release");
  check(version == "1.0.0", "latest: a release outranks a prerelease");

  check(!oskelp::latestRegistryVersion("pre", version, false, error),
        "latest: a prerelease-only package needs an explicit version");
  check(contains(error, "prerelease"),
        "latest: the error names the prerelease");
  check(oskelp::latestRegistryVersion("pre", version, true, error),
        "latest: a prerelease is visible when allowed");
  check(version == "0.1.0-beta.1",
        "latest: includePrerelease returns the prerelease");

  check(!oskelp::latestRegistryVersion("missing", version, false, error),
        "latest: an unknown package fails");
  check(contains(error, "has no package 'missing'"),
        "latest: the error names the missing package");

  if (hadRegistry) setRegistry(saved);
  else clearRegistry();
  fs::remove_all(root);
}

static void testForcedResolution() {
  fs::path root = makeTempDir();
  fs::path registry = root / "registry";
  fs::path appDir = root / "app";
  makePackage(appDir, "app");
  makeRegistryPackage(registry, "utils", "0.1.0");
  makeRegistryPackage(registry, "utils", "0.2.0");
  makeRegistryPackage(registry, "utils", "0.2.1");
  makeRegistryPackage(registry, "utils", "0.3.0");

  oskelp::Manifest app;
  std::string error;
  check(oskelp::loadManifest((appDir / "oskel.toml").string(), app, error),
        "forced: loads the app");
  oskelp::Dependency utils;
  utils.name = "utils";
  app.package.dependencies["utils"] = utils;

  const char* previous = std::getenv("OSKEL_REGISTRY");
  std::string saved = previous == nullptr ? std::string() : previous;
  bool hadRegistry = previous != nullptr;
  setRegistry(registry.string());

  auto resolve = [&](const std::string& requirement,
                     const std::map<std::string, std::string>& forced) {
    app.package.dependencies["utils"].requirement = requirement;
    oskelp::Resolution resolution;
    error.clear();
    bool ok = oskelp::resolveDependencies(app, resolution, error, {}, forced);
    return std::make_pair(ok, resolution);
  };

  auto picked = resolve("^0.2.0", {{"utils", "0.2.0"}});
  check(picked.first, "forced: pins an existing satisfying version");
  const oskelp::ResolvedPackage* selected = picked.second.find("utils");
  check(selected != nullptr && selected->version == "0.2.0",
        "forced: the forced version wins over the highest");

  picked = resolve("^0.2.0", {{"utils", "0.1.0"}});
  check(!picked.first, "forced: a version outside the requirement fails");
  check(contains(error, "does not satisfy '^0.2.0'"),
        "forced: the error names the requirement");

  picked = resolve("^0.2.0", {{"utils", "0.2.9"}});
  check(!picked.first, "forced: a missing version fails");
  check(contains(error, "has no 'utils' version '0.2.9'"),
        "forced: the error names the missing version");

  picked = resolve("^0.2.0", {{"utils", "not-a-version"}});
  check(!picked.first, "forced: an invalid version fails");
  check(contains(error, "is not a semantic version"),
        "forced: the error names the invalid version");

  if (hadRegistry) setRegistry(saved);
  else clearRegistry();
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

static void testCycleFromInMemoryRoot() {
  fs::path root = makeTempDir();
  makePackage(root / "a", "a", {{"b", "../b"}});
  makePackage(root / "b", "b");

  oskelp::Manifest b;
  std::string error;
  check(oskelp::loadManifest((root / "b" / "oskel.toml").string(), b, error),
        "in-memory cycle: loads the root");

  // `oskelp add` resolves before it writes, so the root's in-memory manifest
  // already declares `a` while b's copy on disk does not declare it back.
  oskelp::Dependency a;
  a.name = "a";
  a.path = "../a";
  b.package.dependencies["a"] = a;

  oskelp::Resolution resolution;
  check(!oskelp::resolveDependencies(b, resolution, error),
        "in-memory cycle: a dependency back to the root is rejected");
  check(contains(error, "dependency cycle"),
        "in-memory cycle: the error names the cycle");
  check(contains(error, "b -> a -> b"),
        "in-memory cycle: the error shows the path");

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

static void testManifestEditing() {
  const std::string source =
      "[package]\n"
      "name = \"demo\"\n"
      "version = \"0.1.0\"\n"
      "\n"
      "[dependencies]\n"
      "alpha = { path = \"../alpha\" }\n"
      "\n"
      "[other]\n"
      "keep = true\n";

  oskelp::Dependency beta;
  beta.name = "beta";
  beta.path = "../beta";
  std::string edited;
  std::string error;
  check(oskelp::addDependencyToManifest(source, beta, edited, error),
        "edit: add to an existing section");
  check(contains(edited, "alpha = { path = \"../alpha\" }"),
        "edit: add keeps the existing dependency");
  check(contains(edited, "beta = { path = \"../beta\" }"),
        "edit: add writes the new dependency");
  check(edited.find("beta") < edited.find("[other]"),
        "edit: add stays inside [dependencies]");
  check(contains(edited, "keep = true"), "edit: add keeps the next section");

  oskelp::Dependency versioned;
  versioned.name = "gamma";
  versioned.path = "../gamma";
  versioned.version = "1.2.3";
  check(oskelp::addDependencyToManifest(source, versioned, edited, error),
        "edit: add with a version");
  check(contains(edited,
                 "gamma = { path = \"../gamma\", version = \"1.2.3\" }"),
        "edit: add writes the version");

  oskelp::Dependency registry;
  registry.name = "text";
  registry.requirement = "^1.0.0";
  check(oskelp::addDependencyToManifest(source, registry, edited, error),
        "edit: add a registry requirement");
  check(contains(edited, "text = \"^1.0.0\""),
        "edit: add writes the requirement as a string");

  const std::string withRegistry =
      "[dependencies]\n"
      "text = \"^1.0.0\"\n";
  check(oskelp::removeDependencyFromManifest(withRegistry, "text", edited, error),
        "edit: remove a registry requirement");
  check(edited.find("text =") == std::string::npos,
        "edit: remove drops the requirement line");

  const std::string noSection =
      "[package]\n"
      "name = \"demo\"\n"
      "version = \"0.1.0\"\n";
  check(oskelp::addDependencyToManifest(noSection, beta, edited, error),
        "edit: add creates the section");
  check(contains(edited, "[dependencies]\nbeta = { path = \"../beta\" }"),
        "edit: add creates the section at the end");

  std::string duplicate;
  check(!oskelp::addDependencyToManifest(edited, beta, duplicate, error),
        "edit: add refuses a duplicate");
  check(contains(error, "already declared"),
        "edit: duplicate error names the reason");

  check(oskelp::removeDependencyFromManifest(source, "alpha", edited, error),
        "edit: remove an existing dependency");
  check(!contains(edited, "alpha ="), "edit: remove deletes the line");
  check(contains(edited, "[dependencies]"), "edit: remove keeps the header");
  check(contains(edited, "keep = true"), "edit: remove keeps other sections");

  const std::string quoted =
      "[dependencies]\n"
      "\"alpha\" = { path = \"../alpha\" }\n"
      "'beta' = { path = \"../beta\" }\n";
  check(oskelp::removeDependencyFromManifest(quoted, "alpha", edited, error),
        "edit: remove a quoted key");
  check(!contains(edited, "alpha"), "edit: quoted key removed");
  std::string without;
  check(oskelp::removeDependencyFromManifest(edited, "beta", without, error),
        "edit: remove a literal-quoted key");
  check(!contains(without, "beta"), "edit: literal-quoted key removed");

  check(!oskelp::removeDependencyFromManifest(source, "missing", edited, error),
        "edit: remove refuses an absent dependency");
  check(contains(error, "not declared"),
        "edit: absent error names the reason");

  const std::string multiline =
      "[dependencies]\n"
      "alpha = { path = \"../alpha\",\n"
      "  version = \"1.0.0\" }\n";
  check(!oskelp::removeDependencyFromManifest(multiline, "alpha", edited, error),
        "edit: remove refuses a multi-line entry");
  check(contains(error, "spans multiple lines"),
        "edit: multi-line error names the reason");

  const std::string crlf =
      "[package]\r\n"
      "name = \"demo\"\r\n"
      "\r\n"
      "[dependencies]\r\n"
      "alpha = { path = \"../alpha\" }\r\n";
  check(oskelp::addDependencyToManifest(crlf, beta, edited, error),
        "edit: add to CRLF");
  check(contains(edited, "beta = { path = \"../beta\" }\r\n"),
        "edit: add keeps CRLF");
  check(oskelp::removeDependencyFromManifest(crlf, "alpha", without, error),
        "edit: remove from CRLF");
  check(contains(without, "[dependencies]\r\n"),
        "edit: remove keeps CRLF");
  check(!contains(without, "alpha ="), "edit: CRLF line removed");

  const std::string commented =
      "[dependencies] # dependencies\n"
      "# alpha is local\n"
      "alpha = { path = \"../alpha\" } # keep me\n";
  check(oskelp::addDependencyToManifest(commented, beta, edited, error),
        "edit: add under a commented header");
  check(contains(edited, "beta = { path = \"../beta\" }"),
        "edit: commented header add writes the dependency");
  check(contains(edited, "# alpha is local"),
        "edit: commented header keeps the comment");
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

  oskelp::Dependency registry;
  registry.name = "text";
  registry.requirement = "^1.0.0";
  package.dependencies["text"] = registry;
  const std::string withRegistry = oskelp::renderManifest(package);
  check(contains(withRegistry, "text = \"^1.0.0\""),
        "render: a registry dependency is a string");
  check(!contains(withRegistry, "text = { path"),
        "render: a registry dependency is not a path table");
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
  testSemver();
  testTomlBasics();
  testTomlEscapesAndErrors();
  testManifest();
  testFindManifest();
  testResolution();
  testRegistryResolution();
  testLatestRegistryVersion();
  testForcedResolution();
  testCycle();
  testCycleFromInMemoryRoot();
  testDuplicateLocation();
  testLockfile();
  testVerifyLockfile();
  testManifestEditing();
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
