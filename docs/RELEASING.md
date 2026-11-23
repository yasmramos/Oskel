# Releasing Oskel

This document is the checklist for cutting a release. The release itself is
**tag-driven**: pushing a `v*` tag is what publishes it. There is no manual
upload step.

## 1. Version

`0.2.0` is the single source of truth for the version in `include/version.h`
(`OSKEL_VERSION`) and `CMakeLists.txt` (`project(oskel VERSION 0.2.0 ...)`).
`src/main.cpp` reads the header, so the binary's `--version`, the installers and
the package names can never drift. The CMake configure step **fails** when the
two disagree, and the `version_consistency` test checks the built binary prints
the value.

Before a release, confirm the version:

```sh
grep '^project(' CMakeLists.txt
./build/oskel --version        # must match
```

The CI derives the version from the tag when releasing (`v0.1.0` → `0.1.0`) and
from `CMakeLists.txt` on a branch push or pull request, so a branch build is
packaged as `0.2.0` too.

## 2. Pre-flight

From a clean checkout of `develop`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
(cd build && ctest --output-on-failure)   # all tests must pass
```

- The suite is green on Linux, Windows and macOS; the CI runs all three.
- The AOT execution tests (`oskel.build_binary`) run on Linux x86-64 only and
  skip themselves elsewhere.
- The hosted-mode tests build and run on Linux and Windows x86-64, and on an
  x86-64 macOS host. The arm64 macOS runner builds the x86-64 Mach-O image and
  checks its libSystem dependency but does not run it (that needs Rosetta).
- Update `CHANGELOG.md`: move the entries under `[Unreleased]` into a new
  `[x.y.z] - YYYY-MM-DD` section and leave `[Unreleased]` empty.

## 3. Cut the release

The release branch model is `develop` → `main` (see `CONVENTIONS.md` §2).
Shipping means the code that is tagged is on `main`:

```sh
git checkout main
git pull --rebase origin main
git merge --no-ff develop        # or open and merge a release/x.y.z PR
git push origin main

git tag -a v0.2.0 -m "Oskel 0.2.0"
git push origin v0.2.0
```

The tag push triggers `.github/workflows/artifacts.yml`, which:

1. builds and tests on Linux, Windows and macOS;
2. packages the engine (`oskel` + `share/oskel/std`), the Windows installer,
   the Linux `.deb`/`.rpm`, the macOS `.pkg`/`.dmg` and the VS Code extension;
3. creates the GitHub Release with `--generate-notes` and a `SHA256SUMS` file.

Branch pushes and pull requests stop at the artifacts; only a tag publishes.

Do **not** create the release in the GitHub UI first. The workflow creates it
from the tag, and it now reuses an existing release instead of failing, but a
UI-created release (a pre-release, or one with hand-written notes) is a second
source of truth for the same tag. Let the tag create it.

When the release already exists, the workflow edits it before uploading: the
title is set to the tag, and the pre-release/latest flags are derived from the
tag itself, so a `-` in the name (`v0.2.0-rc1`) keeps it a pre-release and every
other tag becomes a final release marked "Latest". This exists because a
hand-made `v0.2.0` release was published as a pre-release, which hides it from
the "Latest" slot and from `gh release download` defaults even though its assets
are correct.

## 4. Verify

- Watch the run: `gh run watch` (or the Actions tab).
- Check that the release is final and is the "Latest" one. A pre-release flag
  survives an upload and silently hides a final release from the "Latest" slot:

  ```sh
  gh release view v0.2.0 --json isPrerelease,isDraft,assets \
    -q '.isPrerelease, .isDraft, (.assets | length)'
  gh api repos/yasmramos/Oskel/releases/latest -q '.tag_name'
  ```

  A final release prints `false`, `false`, `10` and then its own tag.
- Check the release assets:

  ```sh
  gh release view v0.2.0
  ```

- Download and verify the checksums:

  ```sh
  gh release download v0.2.0
  sha256sum -c SHA256SUMS
  ```

- Smoke-test one engine archive:

  ```sh
  tar -xzf oskel-linux-x64.tar.gz -C /tmp/oskel-check
  /tmp/oskel-check/bin/oskel --version
  # `-e` takes a statement, so the trailing `;` is required.
  /tmp/oskel-check/bin/oskel -e 'print("ok");'
  # the compiler ships beside the engine and must emit a working binary
  /tmp/oskel-check/bin/oskelc --version
  printf 'print("ok");\n' > /tmp/hello.oskl
  /tmp/oskel-check/bin/oskelc /tmp/hello.oskl -o /tmp/hello
  /tmp/hello
  ```

## 5. Pre-release tags

A pre-release tag (`v0.1.0-rc1`) works the same way. The packaging scripts
handle the per-format quirks:

- **RPM** splits `Version` and `Release` at the first `-`, and `Version` may
  not contain one, so `0.1.0-rc1` becomes `Version: 0.1.0` +
  `Release: 0.rc1`.
- **deb** rewrites `-` as `~` in its control file so the pre-release sorts
  before the final `0.1.0`. GitHub rewrites `~` to `.` in asset names, so the
  uploaded file name uses `.` while the control file keeps `~`.

## 6. Dry-run the pipeline

To exercise the whole release path without shipping anything, push a throwaway
tag, confirm the assets and `sha256sum -c SHA256SUMS`, then delete the release
**and** the tag:

```sh
git tag v0.1.0-smoketest && git push origin v0.1.0-smoketest
# ... verify ...
gh release delete v0.1.0-smoketest --yes
git push origin :refs/tags/v0.1.0-smoketest
git tag -d v0.1.0-smoketest
```

A failed run can leave a draft release behind; delete that too. Do not leave
test tags or drafts.

## 7. After the release

- Bump the version in `CMakeLists.txt` and `editors/vscode/package.json` for
  the next development cycle if the release closes a line.
- Merge the release branch back into `develop` so the changelog entry and the
  version bump are not lost.
