#!/usr/bin/env python3
"""Build the native libraries the standard UI backend ships with.

The release artifacts must not ask the user to install SDL3 or Skia. This script
uses vcpkg to build a shared SDL3 (the engine loads it lazily) and a static Skia
(the shim links it into one self-contained shared object), then prints or writes
the CMake prefixes that make `find_package` pick both up.

The vcpkg checkout is pinned so a release is reproducible. The installed trees
live under the vcpkg root, and `VCPKG_BINARY_SOURCES` can point at a cache so CI
does not rebuild Skia on every run.
"""

from __future__ import annotations

import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VCPKG_REPOSITORY = "https://github.com/microsoft/vcpkg.git"
# Pinned vcpkg commit. Bump it deliberately, together with a full CI run: the
# Skia port's build flags are what make the shim self-contained.
VCPKG_COMMIT = "3cb3f66bb768e04b8af95737adaaec3b4a583842"


def run(command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    printable = " ".join(str(part) for part in command)
    print(f"+ {printable}", flush=True)
    subprocess.run(command, cwd=str(cwd) if cwd else None, env=env, check=True)


def platform_triplets() -> tuple[str, str, str]:
    """Return (SDL3 triplet, Skia triplet, Skia feature list)."""
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Linux" and machine in ("x86_64", "amd64"):
        return "x64-linux-sdl3", "x64-linux-skia", "core,freetype,fontconfig"
    if system == "Darwin" and machine in ("arm64", "aarch64"):
        return "arm64-osx-sdl3", "arm64-osx-skia", "core"
    if system == "Darwin" and machine in ("x86_64", "amd64"):
        return "x64-osx-sdl3", "x64-osx-skia", "core"
    if system == "Windows" and machine in ("amd64", "x86_64"):
        return "x64-windows-sdl3", "x64-windows-skia", "core"
    raise SystemExit(
        f"prepare_ui_deps: no UI dependency triplet for {system} {machine}"
    )


def vcpkg_executable(root: Path) -> Path:
    return root / ("vcpkg.exe" if os.name == "nt" else "vcpkg")


def ensure_vcpkg(root: Path, commit: str) -> Path:
    if not (root / ".git").exists():
        if root.exists() and any(root.iterdir()):
            raise SystemExit(
                f"prepare_ui_deps: {root} exists but is not a vcpkg checkout"
            )
        run(["git", "clone", "--filter=blob:none", VCPKG_REPOSITORY, str(root)])
    run(["git", "-C", str(root), "fetch", "--quiet", "--depth=1", "origin", commit])
    run(["git", "-C", str(root), "checkout", "--quiet", commit])
    executable = vcpkg_executable(root)
    if executable.exists():
        return executable

    if os.name == "nt":
        run(["cmd", "/c", str(root / "bootstrap-vcpkg.bat"), "-disableMetrics"], cwd=root)
    else:
        run([str(root / "bootstrap-vcpkg.sh"), "-disableMetrics"], cwd=root)
    if not executable.exists():
        raise SystemExit(f"prepare_ui_deps: vcpkg bootstrap did not create {executable}")
    return executable


def install(vcpkg: Path, root: Path, package: str, triplet: str, cache: Path | None) -> Path:
    env = os.environ.copy()
    env["VCPKG_BUILD_TYPE"] = "release"
    env.setdefault("VCPKG_MAX_CONCURRENCY", "2")
    if cache is not None:
        cache.mkdir(parents=True, exist_ok=True)
        env["VCPKG_BINARY_SOURCES"] = f"clear;files,{cache.as_posix()},readwrite"
    overlay = REPO_ROOT / "cmake" / "triplets"
    run(
        [
            str(vcpkg),
            "install",
            f"{package}:{triplet}",
            f"--overlay-triplets={overlay}",
            "--clean-after-build",
        ],
        cwd=root,
        env=env,
    )
    installed = root / "installed" / triplet
    if not installed.is_dir():
        raise SystemExit(f"prepare_ui_deps: vcpkg did not install {triplet}")
    return installed


def write_github_env(path: Path, prefixes: list[Path]) -> None:
    value = ";".join(str(prefix) for prefix in prefixes)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(f"CMAKE_PREFIX_PATH={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vcpkg-root",
        type=Path,
        default=REPO_ROOT / ".vcpkg",
        help="vcpkg checkout/build directory (default: .vcpkg)",
    )
    parser.add_argument(
        "--cache",
        type=Path,
        default=None,
        help="vcpkg binary cache directory",
    )
    parser.add_argument(
        "--github-env",
        type=Path,
        default=None,
        help="append CMAKE_PREFIX_PATH to this file (GitHub Actions $GITHUB_ENV)",
    )
    args = parser.parse_args()

    sdl_triplet, skia_triplet, skia_features = platform_triplets()
    root = args.vcpkg_root.resolve()
    cache = args.cache.resolve() if args.cache else None
    vcpkg = ensure_vcpkg(root, VCPKG_COMMIT)

    sdl_package = "sdl3[core,x11]" if platform.system() == "Linux" else "sdl3[core]"
    sdl_dir = install(vcpkg, root, sdl_package, sdl_triplet, cache)
    skia_dir = install(vcpkg, root, f"skia[{skia_features}]", skia_triplet, cache)

    prefixes = [sdl_dir, skia_dir]
    if args.github_env:
        write_github_env(args.github_env, prefixes)
    print("CMAKE_PREFIX_PATH=" + ";".join(str(prefix) for prefix in prefixes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
