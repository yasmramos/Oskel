#!/usr/bin/env python3
"""Benchmark runner for the Oskel suite.

Each benchmark is a small program written once per language, all of them doing
the same work and printing the same checksum, so every implementation is
verified before it is timed. The runner:

1. takes the development VM's output as the reference,
2. runs every implementation once and rejects the ones whose output differs (a
   benchmark that computes something else is not a benchmark),
3. times the survivors (one warm-up plus N repetitions) and reports the median
   wall-clock of the whole process, start-up included -- the `startup` row is
   the baseline to subtract,
4. prints a Markdown table plus the environment it was measured on.

Oskel itself is measured four ways: the VM with its JIT, the VM with
`--no-jit`, the native binary `oskel build` emits, and the WebAssembly module
under Node. The other languages are the reference points.

Usage:

    python3 benchmarks/run.py                 # everything that is installed
    python3 benchmarks/run.py --only fib,arrays --reps 7
    python3 benchmarks/run.py --output docs/benchmarks.md

`$OSKEL_EXE` overrides the engine path (default `build/oskel`). Missing runtimes
are reported as `n/a` and named in the footnotes, never silently dropped.
"""

import argparse
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
WASM_RUNNER = REPO / "tests" / "run_wasm.cjs"
BENCHMARKS = ("startup", "fib", "arrays", "strings")
DEFAULT_BASELINE = "oskel-aot"
TIMEOUT = 300.0


class Unavailable(Exception):
    """The implementation cannot run here: a runtime is missing."""


class Broken(Exception):
    """The implementation is installed but failed, or printed something else."""


class Implementation:
    """One column of the table: how to build the program and how to run it."""

    def __init__(self, name, prepare, argv_for, version):
        self.name = name
        self.prepare = prepare    # (stem, workdir) -> None, may raise
        self.argv_for = argv_for  # (stem, workdir) -> list[str], may raise
        self.version = version    # command whose first line names the runtime


def which(*names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def first_line(argv):
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return ""
    text = (out.stdout or out.stderr).strip().splitlines()
    return text[0].strip() if text else ""


def find_oskel():
    env = os.environ.get("OSKEL_EXE")
    if env:
        path = Path(env)
        if not path.exists():
            raise SystemExit(f"run.py: OSKEL_EXE='{env}' does not exist")
        return path
    for candidate in (REPO / "build" / "oskel", REPO / "build" / "oskel.exe"):
        if candidate.exists():
            return candidate
    found = which("oskel")
    if found:
        return Path(found)
    raise SystemExit(
        "run.py: no oskel found; build the engine (cmake --build build) or set OSKEL_EXE")


def oskel_impls(oskel, have_node, want_wasm):
    """The four Oskel columns, plus the reasons a few of them may be absent."""

    def prepare_nothing(stem, workdir):
        pass

    def prepare_binary(stem, workdir):
        proc = subprocess.run([str(oskel), "build", str(HERE / f"{stem}.oskl"),
                               "-o", str(workdir / stem)],
                              capture_output=True, text=True, timeout=TIMEOUT)
        if proc.returncode != 0:
            detail = proc.stderr.strip().splitlines()
            raise Broken(detail[-1] if detail else "oskel build failed")

    def prepare_wasm(stem, workdir):
        if not have_node:
            raise Unavailable("node is not installed")
        if not want_wasm:
            raise Unavailable("--no-wasm")
        if not WASM_RUNNER.exists():
            raise Unavailable(f"{WASM_RUNNER.relative_to(REPO)} is missing")
        proc = subprocess.run([str(oskel), "build", "--target", "wasm32-freestanding-wasm",
                               str(HERE / f"{stem}.oskl"), "-o", str(workdir / f"{stem}.wasm")],
                              capture_output=True, text=True, timeout=TIMEOUT)
        if proc.returncode != 0:
            detail = proc.stderr.strip().splitlines()
            raise Broken(detail[-1] if detail else "oskel build --target wasm32 failed")

    return [
        Implementation("oskel-vm", prepare_nothing,
                       lambda stem, workdir: [str(oskel), str(HERE / f"{stem}.oskl")],
                       [str(oskel), "--version"]),
        Implementation("oskel-nojit", prepare_nothing,
                       lambda stem, workdir: [str(oskel), "--no-jit", str(HERE / f"{stem}.oskl")],
                       None),
        Implementation("oskel-aot", prepare_binary,
                       lambda stem, workdir: [str(workdir / stem)], None),
        Implementation("oskel-wasm", prepare_wasm,
                       lambda stem, workdir: [which("node"), str(WASM_RUNNER),
                                              str(workdir / f"{stem}.wasm")],
                       None),
    ]


def cc_impls():
    """The C reference point, built with a GCC/Clang-style driver (`$CC`)."""
    cc = os.environ.get("CC") or which("cc", "gcc", "clang")
    if not cc:
        return []

    def prepare_c(stem, workdir):
        proc = subprocess.run([cc, "-O2", "-o", str(workdir / f"{stem}_c"),
                               str(HERE / f"{stem}.c")],
                              capture_output=True, text=True, timeout=TIMEOUT)
        if proc.returncode != 0:
            detail = proc.stderr.strip().splitlines()
            raise Broken(detail[-1] if detail else "cc failed")

    return [Implementation("c", prepare_c,
                           lambda stem, workdir: [str(workdir / f"{stem}_c")],
                           [cc, "--version"])]


def interpreter_impls():
    impls = [Implementation("python", lambda stem, workdir: None,
                            lambda stem, workdir: [sys.executable, str(HERE / f"{stem}.py")],
                            [sys.executable, "--version"])]
    node = which("node")
    if node:
        impls.append(Implementation("node", lambda stem, workdir: None,
                                    lambda stem, workdir: [node, str(HERE / f"{stem}.js")],
                                    [node, "--version"]))
    lua = which("lua5.4", "lua5.3", "lua")
    if lua:
        impls.append(Implementation("lua", lambda stem, workdir: None,
                                    lambda stem, workdir: [lua, str(HERE / f"{stem}.lua")],
                                    [lua, "-v"]))
    luajit = which("luajit")
    if luajit:
        impls.append(Implementation("luajit", lambda stem, workdir: None,
                                    lambda stem, workdir: [luajit, str(HERE / f"{stem}.lua")],
                                    [luajit, "-v"]))
    return impls


def run_once(argv):
    started = time.perf_counter()
    proc = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT)
    return proc, (time.perf_counter() - started) * 1000.0


def cpu_name():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown CPU"


def measure(args, impls):
    """Returns ({bench: {impl: median_ms}}, {(bench, impl): reason})."""
    times = {}
    failures = {}
    for bench in args.only:
        reference_proc, _ = run_once([str(args.oskel), str(HERE / f"{bench}.oskl")])
        if reference_proc.returncode != 0:
            raise SystemExit(f"run.py: the VM failed on '{bench}':\n{reference_proc.stderr}")
        reference = reference_proc.stdout.strip()
        times[bench] = {}
        for impl in impls:
            try:
                impl.prepare(bench, args.workdir)
                argv = impl.argv_for(bench, args.workdir)
                proc, _ = run_once(argv)
                if proc.returncode != 0:
                    detail = (proc.stderr or proc.stdout).strip().splitlines()
                    raise Broken(detail[-1] if detail else f"exit {proc.returncode}")
                if proc.stdout.strip() != reference:
                    raise Broken(f"output {proc.stdout.strip()!r} != {reference!r}")
                samples = [run_once(argv)[1] for _ in range(args.warmup + args.reps)]
                times[bench][impl.name] = statistics.median(samples[args.warmup:])
            except (Unavailable, Broken) as error:
                failures[(bench, impl.name)] = str(error)
                print(f"run.py: {bench}/{impl.name}: {error}", file=sys.stderr)
    return times, failures


def markdown(args, impls, times, failures):
    names = [impl.name for impl in impls]
    started = time.strftime("%Y-%m-%d")
    lines = [
        "<!-- Generated by `python3 benchmarks/run.py --output docs/benchmarks.md`; do not edit by hand. -->",
        "",
        f"Median wall-clock of {args.reps} runs after {args.warmup} warm-up run(s), in ms, whole",
        "process included; the `startup` row is the process baseline to subtract. `fail` means the",
        "implementation is installed and its output was wrong or it crashed -- the footnote names",
        "the reason -- and `n/a` that its runtime is not installed here.",
        "",
        f"- measured: {started} on {cpu_name()}, {platform.system()} {platform.release()}",
        f"- engine: `{args.oskel}`",
        "",
        "| benchmark | " + " | ".join(names) + " |",
        "| --- | " + " | ".join("---:" for _ in names) + " |",
    ]
    for bench in args.only:
        cells = []
        for name in names:
            if name in times[bench]:
                cells.append(f"{times[bench][name]:.1f}")
            elif (bench, name) in failures:
                cells.append("fail")
            else:
                cells.append("n/a")
        lines.append(f"| {bench} | " + " | ".join(cells) + " |")

    if args.baseline in names:
        others = [name for name in names if name != args.baseline]
        lines += ["", f"Same numbers relative to `{args.baseline}` (lower is better):", "",
                  "| benchmark | " + " | ".join(others) + " |",
                  "| --- | " + " | ".join("---:" for _ in others) + " |"]
        for bench in args.only:
            base = times[bench].get(args.baseline)
            cells = []
            for name in others:
                value = times[bench].get(name)
                if value is not None and base:
                    cells.append(f"x{value / base:.2f}")
                elif (bench, name) in failures:
                    cells.append("fail")
                else:
                    cells.append("n/a")
            lines.append(f"| {bench} | " + " | ".join(cells) + " |")

    lines += ["", "Commands:", ""]
    for impl in impls:
        try:
            sample = " ".join(impl.argv_for(args.only[0], args.workdir))
        except (Unavailable, Broken):
            sample = "(build step)"
        sample = sample.replace(str(args.workdir), "<build>")
        lines.append(f"- `{impl.name}`: `{sample}`")

    notes = [f"- `{bench}/{name}`: {reason}" for (bench, name), reason in sorted(failures.items())]
    if notes:
        lines += ["", "Not measured:", ""] + notes
    versions = [f"- `{impl.name}`: {first_line(impl.version)}" for impl in impls if impl.version]
    if versions:
        lines += ["", "Runtimes:", ""] + versions
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Run the Oskel benchmark suite.")
    parser.add_argument("--only", default=",".join(BENCHMARKS),
                        help=f"comma-separated benchmarks (default: {','.join(BENCHMARKS)})")
    parser.add_argument("--reps", type=int, default=5, help="timed runs per cell (default: 5)")
    parser.add_argument("--warmup", type=int, default=1, help="unmeasured runs per cell (default: 1)")
    parser.add_argument("--baseline", default=DEFAULT_BASELINE,
                        help=f"column the ratio table divides by (default: {DEFAULT_BASELINE})")
    parser.add_argument("--output", help="write the Markdown here instead of stdout")
    parser.add_argument("--no-wasm", action="store_true", help="skip the WebAssembly column")
    parser.add_argument("--list", action="store_true", help="list the columns and exit")
    args = parser.parse_args()

    args.only = [name.strip() for name in args.only.split(",") if name.strip()]
    unknown = [name for name in args.only if name not in BENCHMARKS]
    if unknown:
        raise SystemExit(f"run.py: unknown benchmark(s) {unknown}; known: {list(BENCHMARKS)}")
    if args.reps < 1:
        raise SystemExit("run.py: --reps must be at least 1")
    args.oskel = find_oskel()

    impls = (oskel_impls(args.oskel, bool(which("node")), not args.no_wasm)
             + cc_impls() + interpreter_impls())
    if args.list:
        for impl in impls:
            print(impl.name)
        return 0

    with tempfile.TemporaryDirectory(prefix="oskel-bench-") as directory:
        args.workdir = Path(directory)
        times, failures = measure(args, impls)
        report = markdown(args, impls, times, failures)
    if args.output:
        target = Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(report)
        print(f"run.py: wrote {target}")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
