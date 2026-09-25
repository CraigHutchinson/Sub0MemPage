#!/usr/bin/env python3
"""Sub0MemPage development loop: build, test, sanitize, mutate, benchmark, report -- one command each.

Policy: docs/DEVELOPMENT_WORKFLOW.md. Gates: docs/perf/kpi_gates.json. Shape borrowed from Sub0Llm's
scripts/run_perf_suite.py (contention gate, interleaved arms, history + report), which learned its rules
the hard way; read Sub0Llm's docs/OPTIMIZATION_PROCESS.md for why each rule exists.

Stages (each independently runnable; `check` = test + sanitize + mutate + arm):

  test      Release build + CTest, warnings as errors. Windows (MSVC) and Linux (GCC, via WSL on a
            Windows host). Per-suite check counts are compared EXACTLY against kpi_gates.json.
  sanitize  Linux ASan+UBSan and TSan builds, every test repeated, any report fails the stage.
  mutate    Applies each mutant in scripts/mutants.json to a copy of include/ and requires the named
            test to fail (or hang, killed by timeout). A surviving mutant means a gate is vacuous.
  arm       Cross-builds for aarch64 (cmake/toolchains/aarch64-linux-gnu.cmake) and runs under
            qemu-aarch64 user-mode emulation: correctness only, never perf. No cross toolchain/qemu ->
            SKIP, never PASS. ASan/TSan are attempted; a sanitizer qemu-user cannot support is recorded
            as SKIP with the real emulator error, not as a library defect (see docs/DEVELOPMENT_WORKFLOW.md).
  bench     Contention-gated microbenchmarks; with --baseline REF, interleaved A/B against the library
            headers at REF. Appends docs/perf/perf_history.jsonl and rewrites docs/perf/perf_report.md.

Typical use:

  python scripts/dev.py check                       # before every commit
  python scripts/dev.py bench --baseline main       # before claiming a perf change
  python scripts/dev.py bench --aa                  # measure the noise floor (same binary twice)

On a Windows host, Linux stages re-invoke this script inside WSL (`--native-only`); on Linux everything
is native and the Windows stages report "skipped", never "passed".
"""
from __future__ import annotations

import argparse
import hashlib
import datetime
import io
import json
import os
import pathlib
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
GATES = ROOT / "docs" / "perf" / "kpi_gates.json"
HISTORY = ROOT / "docs" / "perf" / "perf_history.jsonl"
REPORT = ROOT / "docs" / "perf" / "perf_report.md"
MUTANTS = ROOT / "scripts" / "mutants.json"
IS_WINDOWS = platform.system() == "Windows"
WSL_DISTRO = os.environ.get("SUB0MEMPAGE_WSL_DISTRO", "Ubuntu-24.04")

# Background load above this makes a timing unattributable (Sub0Llm measured 2x variance under sibling
# load on this host). Named tools are checked separately: they are the usual culprits.
MAX_BACKGROUND_LOAD_PCT = 5.0
CONTENTION_RE = re.compile(r"^(cl|link|msbuild|ninja|cmake|clang\+*|clang-cl|cc1plus|g\+\+|ld|sub0llm\S*)(\.exe)?$", re.I)

# name -> (extra cmake args, environment for tests). Linux-only configs carry sanitizer flags.
SAN_ENV = {
    "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1:abort_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    "TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1",
}
CONFIGS = {
    "msvc-release": (["-DCMAKE_BUILD_TYPE=Release"], {}),
    "gcc-release": (["-DCMAKE_BUILD_TYPE=Release"], {}),
    "gcc-asan": (["-DCMAKE_BUILD_TYPE=Debug",
                  "-DCMAKE_CXX_FLAGS=-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer",
                  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"], SAN_ENV),
    "gcc-tsan": (["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_FLAGS=-O1 -g -fsanitize=thread",
                  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"], SAN_ENV),
}

# aarch64 cross-build via cmake/toolchains/aarch64-linux-gnu.cmake, run through qemu-user. Correctness
# only -- an emulated timing is never a measurement of the target (AGENTS.md rule 4/8), so no perf here.
ARM_TOOLCHAIN = ROOT / "cmake" / "toolchains" / "aarch64-linux-gnu.cmake"
ARM_CONFIGS = {
    "arm-release": (["-DCMAKE_BUILD_TYPE=Release"], {}),
    "arm-asan": (["-DCMAKE_BUILD_TYPE=Debug",
                  "-DCMAKE_CXX_FLAGS=-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer",
                  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"], SAN_ENV),
    "arm-tsan": (["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_FLAGS=-O1 -g -fsanitize=thread",
                  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"], SAN_ENV),
}


# --- process helpers -----------------------------------------------------------------------------------

def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def run(cmd: list[str], cwd: pathlib.Path | None = None, env: dict | None = None,
        timeout: float = 1800) -> subprocess.CompletedProcess:
    merged = {**os.environ, **(env or {})}
    return subprocess.run(cmd, cwd=cwd, env=merged, capture_output=True, text=True, timeout=timeout,
                          encoding="utf-8", errors="replace")


def build_root() -> pathlib.Path:
    """Out-of-tree on Linux (ext4, not the 9p Windows mount); under build/dev on Windows."""
    if IS_WINDOWS:
        return ROOT / "build" / "dev"
    # Keyed by checkout: CMake refuses a build dir configured from another source tree, and several
    # checkouts (worktrees, parallel agents) share one cache directory.
    key = hashlib.sha1(str(ROOT.resolve()).encode()).hexdigest()[:8]
    return pathlib.Path(os.environ.get("XDG_CACHE_HOME", pathlib.Path.home() / ".cache")) / "sub0mempage-dev" / key


def linux_cxx() -> str | None:
    for candidate in ("g++-15", "g++-14", "g++"):
        if shutil.which(candidate):
            return candidate
    return None


def wsl_path(path: pathlib.Path) -> str:
    drive, rest = path.drive.rstrip(":").lower(), path.as_posix()[2:]
    return f"/mnt/{drive}{rest}"


def wsl_available() -> bool:
    if not IS_WINDOWS or not shutil.which("wsl"):
        return False
    probe = run(["wsl", "-d", WSL_DISTRO, "--exec", "true"], timeout=120)
    return probe.returncode == 0


# --- build + ctest ---------------------------------------------------------------------------------------

def configure_and_build(name: str, extra: list[str], benchmarks: bool = False,
                        directory: str | None = None) -> tuple[pathlib.Path, str | None]:
    args, _ = CONFIGS[name]
    build = build_root() / (directory or name)
    cmake = ["cmake", "-S", str(ROOT), "-B", str(build), *args, *extra,
             f"-DSUB0MEMPAGE_BUILD_BENCHMARKS={'ON' if benchmarks else 'OFF'}"]
    env = {}
    if not IS_WINDOWS:
        cxx = linux_cxx()
        if cxx is None:
            return build, "no C++ compiler found"
        env["CXX"] = cxx
        cmake += ["-G", "Ninja"] if shutil.which("ninja") else []
    configured = run(cmake, env=env)
    if configured.returncode != 0:
        return build, "configure failed:\n" + configured.stdout[-3000:] + configured.stderr[-3000:]
    built = run(["cmake", "--build", str(build), "--config", "Release" if "release" in name else "Debug",
                 "--parallel"])
    if built.returncode != 0:
        errors = [line for line in (built.stdout + built.stderr).splitlines() if re.search(r"error|warning", line)]
        return build, "build failed:\n" + "\n".join(errors[:40])
    return build, None


def run_ctest(name: str, build: pathlib.Path, repeat: int = 1) -> dict:
    """Runs CTest verbosely and extracts each suite's `N checks, M failures` line."""
    _, env = CONFIGS[name]
    cmd = ["ctest", "--test-dir", str(build), "-V", "-C", "Release" if "release" in name else "Debug"]
    if repeat > 1:
        cmd += ["--repeat", f"until-fail:{repeat}"]
    started = time.monotonic()
    result = run(cmd, env=env, timeout=3600)
    names = dict(re.findall(r"^\s*Start\s+(\d+):\s+(\S+)", result.stdout, re.M))
    checks = {}
    for number, total, failures in re.findall(r"^(\d+): (\d+) checks, (\d+) failures", result.stdout, re.M):
        checks[names.get(number, number)] = {"checks": int(total), "failures": int(failures)}
    sanitizer = re.findall(r"(ERROR: AddressSanitizer|WARNING: ThreadSanitizer|runtime error:|ERROR: LeakSanitizer)",
                           result.stdout + result.stderr)
    return {"config": name, "ok": result.returncode == 0 and not sanitizer, "returncode": result.returncode,
            "suites": checks, "sanitizer_reports": len(sanitizer), "seconds": round(time.monotonic() - started, 1),
            "tail": "" if result.returncode == 0 else (result.stdout + result.stderr)[-4000:]}


# --- stages ----------------------------------------------------------------------------------------------

def stage_build_test(configs: list[str], repeat: int = 1) -> list[dict]:
    results = []
    for name in configs:
        log(f"[{name}] configure + build")
        build, error = configure_and_build(name, [])
        if error:
            results.append({"config": name, "ok": False, "error": error})
            log(f"[{name}] {error}")
            continue
        log(f"[{name}] ctest" + (f" x{repeat}" if repeat > 1 else ""))
        outcome = run_ctest(name, build, repeat)
        results.append(outcome)
        log(f"[{name}] {'PASS' if outcome['ok'] else 'FAIL'} {outcome['suites']}")
    return results


def stage_mutate() -> list[dict]:
    """Every mutant must be killed: its named test fails, crashes, or hangs past the timeout."""
    cxx = linux_cxx()
    if IS_WINDOWS or cxx is None:
        return [{"id": "*", "ok": False, "skipped": "mutation runs on the Linux toolchain only"}]
    mutants = json.loads(MUTANTS.read_text(encoding="utf-8"))["mutants"]
    results = []
    with tempfile.TemporaryDirectory(prefix="sub0mempage-mut-") as tmp:
        work = pathlib.Path(tmp)
        for mutant in mutants:
            include = work / "include"
            shutil.rmtree(include, ignore_errors=True)
            shutil.copytree(ROOT / "include", include)
            target = include / "sub0mempage" / mutant["file"]
            text = target.read_text(encoding="utf-8")
            if mutant["find"] not in text:
                results.append({"id": mutant["id"], "ok": False, "error": "mutation site not found (stale mutant)"})
                continue
            target.write_text(text.replace(mutant["find"], mutant["replace"], 1), encoding="utf-8")
            binary = work / "mutant"
            compiled = run([cxx, "-std=c++23", "-O1", f"-I{include}", f"-I{ROOT / 'testing' / 'include'}", f"-I{ROOT / 'tests'}",
                            str(ROOT / "tests" / mutant["test"]), str(ROOT / "tests" / "allocation_counter.cpp"),
                            "-o", str(binary), "-pthread"])
            if compiled.returncode != 0:
                results.append({"id": mutant["id"], "ok": False, "error": "mutant did not compile: "
                                + compiled.stderr[-500:]})
                continue
            try:
                ran = run([str(binary)], timeout=30)
                killed, how = ran.returncode != 0, f"exit {ran.returncode}"
            except subprocess.TimeoutExpired:
                killed, how = True, "hang (timeout)"
            results.append({"id": mutant["id"], "ok": killed, "detected_by": how})
            log(f"[mutate] {mutant['id']}: {'killed' if killed else 'SURVIVED'} ({how})")
    return results


def arm_tools_available() -> tuple[bool, str]:
    missing = [tool for tool in ("aarch64-linux-gnu-g++", "qemu-aarch64") if not shutil.which(tool)]
    if missing:
        return False, ("missing " + ", ".join(missing) + " (apt-get install -y g++-aarch64-linux-gnu qemu-user)")
    return True, ""


def configure_and_build_arm(name: str) -> tuple[pathlib.Path, str | None]:
    args, _ = ARM_CONFIGS[name]
    build = build_root() / name
    cmake = ["cmake", "-S", str(ROOT), "-B", str(build), f"-DCMAKE_TOOLCHAIN_FILE={ARM_TOOLCHAIN}", *args,
             "-DSUB0MEMPAGE_BUILD_BENCHMARKS=OFF"]
    if shutil.which("ninja"):
        cmake += ["-G", "Ninja"]
    configured = run(cmake)
    if configured.returncode != 0:
        return build, "configure failed:\n" + configured.stdout[-3000:] + configured.stderr[-3000:]
    built = run(["cmake", "--build", str(build), "--parallel"])
    if built.returncode != 0:
        errors = [line for line in (built.stdout + built.stderr).splitlines() if re.search(r"error|warning", line)]
        return build, "build failed:\n" + "\n".join(errors[:40])
    return build, None


def run_ctest_arm(name: str, build: pathlib.Path) -> dict:
    """Like run_ctest, but the emulator's own crash signatures count as a real failure, and a
    sanitizer that qemu-user cannot support (see stage_arm) is distinguished from a defect it caught."""
    _, env = ARM_CONFIGS[name]
    started = time.monotonic()
    result = run(["ctest", "--test-dir", str(build), "-V"], env=env, timeout=600)
    names = dict(re.findall(r"^\s*Start\s+(\d+):\s+(\S+)", result.stdout, re.M))
    checks = {}
    for number, total, failures in re.findall(r"^(\d+): (\d+) checks, (\d+) failures", result.stdout, re.M):
        checks[names.get(number, number)] = {"checks": int(total), "failures": int(failures)}
    emulator_broken = re.findall(r"(qemu: uncaught target signal|FATAL: ThreadSanitizer: unsupported)",
                                 result.stdout + result.stderr)
    sanitizer = re.findall(r"(ERROR: AddressSanitizer|WARNING: ThreadSanitizer|runtime error:|ERROR: LeakSanitizer)",
                           result.stdout + result.stderr)
    return {"config": name, "ok": result.returncode == 0 and not sanitizer and not emulator_broken,
            "emulator_broken": bool(emulator_broken), "returncode": result.returncode, "suites": checks,
            "seconds": round(time.monotonic() - started, 1), "tail": (result.stdout + result.stderr)[-3000:]}


def stage_arm() -> list[dict]:
    """Cross-builds for aarch64 (cmake/toolchains/aarch64-linux-gnu.cmake, Ubuntu's
    g++-aarch64-linux-gnu) and runs the suite under qemu-aarch64 user-mode emulation. Correctness
    only: an emulated timing is never a measurement of the target, so no perf numbers are taken here
    (AGENTS.md rule 4/8). Missing cross toolchain or qemu -> a single SKIP row, never a silent PASS.
    ASan/UBSan and TSan are attempted; qemu-user is known not to guarantee either (shadow-memory and
    VMA-layout assumptions the emulator does not satisfy) -- a failure there is recorded as SKIP with
    the real emulator output, not conflated with a library defect. Only arm-release's plain build/test
    (which does exercise this library's actual code under emulation) can fail this stage."""
    available, reason = arm_tools_available()
    if not available:
        return [{"config": "arm", "ok": True, "skipped": reason}]
    results: list[dict] = []
    build, error = configure_and_build_arm("arm-release")
    if error:
        results.append({"config": "arm-release", "ok": False, "error": error})
        log(f"[arm-release] {error}")
    else:
        outcome = run_ctest_arm("arm-release", build)
        results.append(outcome)
        log(f"[arm-release] {'PASS' if outcome['ok'] else 'FAIL'} {outcome['suites']}")
    for name in ("arm-asan", "arm-tsan"):
        build, error = configure_and_build_arm(name)
        if error:
            results.append({"config": name, "ok": True, "skipped": "did not build under the cross "
                            "toolchain: " + error[:500]})
            log(f"[{name}] SKIP (build): {error[:200]}")
            continue
        outcome = run_ctest_arm(name, build)
        if outcome["ok"]:
            results.append(outcome)
            log(f"[{name}] PASS (works under qemu-user) {outcome['suites']}")
        elif outcome["emulator_broken"] and not outcome["suites"]:
            # Only a crash before any suite reported counts as "qemu cannot run this sanitizer"; once
            # a test body has run, a crash is signal and falls through to FAIL below.
            results.append({"config": name, "ok": True,
                            "skipped": f"unsupported under qemu-user: {outcome['tail'][-600:]}"})
            log(f"[{name}] SKIP: unsupported under qemu-user emulation")
        else:
            # returned cleanly (not an emulator crash signature) but a test genuinely failed/reported --
            # that is real signal even under emulation, so it counts.
            results.append(outcome)
            log(f"[{name}] FAIL {outcome['suites']}")
    return results


def delegate_to_wsl(stage: str, extra: list[str]) -> dict:
    """Runs this script's Linux half inside WSL and returns its JSON result."""
    out = ROOT / "build" / "dev" / f"wsl-{stage}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["wsl", "-d", WSL_DISTRO, "--exec", "python3", wsl_path(ROOT / "scripts" / "dev.py"), stage,
           "--native-only", "--json-out", wsl_path(out), *extra]
    log(f"[wsl] {stage} (delegated to {WSL_DISTRO})")
    proc = subprocess.run(cmd, stderr=sys.stderr, stdout=subprocess.DEVNULL, timeout=7200)
    if not out.exists():
        return {"error": f"WSL delegation failed (exit {proc.returncode})"}
    return json.loads(out.read_text(encoding="utf-8"))


# --- contention + benchmarks -----------------------------------------------------------------------------

def background_load_pct(samples: int = 5) -> float | None:
    """Sustained total CPU load; None = unknown, which is never treated as idle."""
    try:
        if IS_WINDOWS:
            ps = ("$s = Get-Counter '\\Processor(_Total)\\% Processor Time' -SampleInterval 1 "
                  f"-MaxSamples {samples}; ($s.CounterSamples | Measure-Object CookedValue -Average).Average")
            out = run(["powershell", "-NoProfile", "-Command", ps], timeout=samples + 60).stdout
            return float(out.strip().splitlines()[-1])
        def snap():
            fields = [int(v) for v in pathlib.Path("/proc/stat").read_text().splitlines()[0].split()[1:]]
            return sum(fields), fields[3] + fields[4]
        total0, idle0 = snap()
        time.sleep(samples)
        total1, idle1 = snap()
        return 100.0 * (1 - (idle1 - idle0) / max(1, total1 - total0))
    except Exception:
        return None


def contention() -> dict:
    if IS_WINDOWS:
        listing = run(["tasklist", "/fo", "csv", "/nh"]).stdout
        names = [line.split('","')[0].strip('"') for line in listing.splitlines() if line]
    else:
        names = run(["ps", "-eo", "comm="]).stdout.split()
    named = sorted({n for n in names if CONTENTION_RE.match(n)})
    return {"named_processes": named, "background_load_pct": background_load_pct()}


def run_bench(binary: pathlib.Path, samples: int) -> dict[str, dict]:
    proc = run([str(binary), "--samples", str(samples)], timeout=600)
    if proc.returncode != 0:
        raise RuntimeError(f"{binary} failed: {proc.stderr[-1000:]}")
    return {b["name"]: b for b in json.loads(proc.stdout)["benchmarks"]}


def git(*args: str) -> str:
    return run(["git", *args], cwd=ROOT).stdout.strip()


def baseline_binary(ref: str) -> pathlib.Path:
    """Builds the CURRENT benchmark source against the library headers at `ref`, so the A/B isolates
    library changes. Fails loudly if the bench no longer compiles against the older API."""
    sha = git("rev-parse", "--short=12", ref)
    if not sha:
        raise RuntimeError(f"unknown git ref {ref}")
    headers = build_root() / f"baseline-{sha}" / "include"
    if not headers.exists():
        archive = subprocess.run(["git", "archive", "--format=tar", sha, "include"], cwd=ROOT, capture_output=True)
        if archive.returncode != 0:
            raise RuntimeError(f"git archive {sha} failed")
        with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
            tar.extractall(headers.parent, filter="data")
    build, error = configure_and_build("msvc-release" if IS_WINDOWS else "gcc-release",
                                       [f"-DSUB0MEMPAGE_BENCH_LIBRARY_INCLUDE={headers.as_posix()}"],
                                       benchmarks=True, directory="bench")
    if error:
        raise RuntimeError(f"baseline {sha} does not build with the current bench (API changed?):\n{error}")
    moved = build_root() / f"bench-baseline-{sha}"
    shutil.rmtree(moved, ignore_errors=True)
    shutil.copytree(bench_exe(build).parent, moved)
    return moved / bench_exe(build).name


def bench_exe(build: pathlib.Path) -> pathlib.Path:
    name = "sub0mempage-bench" + (".exe" if IS_WINDOWS else "")
    for candidate in (build / "bench" / "Release" / name, build / "bench" / name):
        if candidate.exists():
            return candidate
    raise RuntimeError(f"no benchmark binary under {build}")


def stage_bench(args) -> dict:
    gates = json.loads(GATES.read_text(encoding="utf-8"))
    load = contention()
    contended = bool(load["named_processes"]) or load["background_load_pct"] is None \
        or load["background_load_pct"] > MAX_BACKGROUND_LOAD_PCT
    log(f"[bench] contention: named={load['named_processes']} load={load['background_load_pct']}")
    if contended and not args.allow_contention:
        return {"ok": False, "error": "host is contended; a timing now is meaningless (see "
                "docs/DEVELOPMENT_WORKFLOW.md). Retry when idle, or --allow-contention for an advisory run.",
                "contention": load}

    config = "msvc-release" if IS_WINDOWS else "gcc-release"
    build, error = configure_and_build(config, ["-DSUB0MEMPAGE_BENCH_LIBRARY_INCLUDE="], benchmarks=True,
                                       directory="bench")
    if error:
        return {"ok": False, "error": error}
    current = build_root() / "bench-current"
    shutil.rmtree(current, ignore_errors=True)
    shutil.copytree(bench_exe(build).parent, current)
    arms = {"current": current / bench_exe(build).name}
    if args.aa:
        arms["current-again"] = arms["current"]
    elif args.baseline:
        arms = {"baseline": baseline_binary(args.baseline), **arms}

    runs: dict[str, dict[str, list[dict]]] = {arm: {} for arm in arms}
    order = list(arms)
    for round_index in range(args.rounds):
        rotated = order[round_index % len(order):] + order[:round_index % len(order)]  # cancel order effects
        for arm in rotated:
            for name, bench in run_bench(arms[arm], args.samples).items():
                runs[arm].setdefault(name, []).append(bench)
        log(f"[bench] round {round_index + 1}/{args.rounds} done")

    summary: dict[str, dict] = {}
    for arm, benches in runs.items():
        for name, rows in benches.items():
            medians = [r["ns_per_op"] for r in rows]
            med = statistics.median(medians)
            summary.setdefault(name, {})[arm] = {
                "median_ns": round(med, 2), "runs": [round(m, 2) for m in medians],
                "spread_pct": round(100 * (max(medians) - min(medians)) / med, 1),
                "allocations": sum(r["allocations"] for r in rows)}

    gate_rows = evaluate_bench_gates(summary, gates, list(arms))
    record = {
        "label": args.label or ("aa" if args.aa else f"vs-{args.baseline}" if args.baseline else "snapshot"),
        "utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git": git("rev-parse", "--short=12", "HEAD"), "dirty": bool(git("status", "--porcelain")),
        "host": platform.node(), "platform": platform.platform(), "config": config,
        "rounds": args.rounds, "samples": args.samples, "contention": load, "advisory": contended,
        "arms": list(arms), "results": summary, "gates": gate_rows}
    HISTORY.parent.mkdir(parents=True, exist_ok=True)
    with HISTORY.open("a", encoding="utf-8") as history:
        history.write(json.dumps(record) + "\n")
    write_report(record)
    hard_fail = any(g["status"] == "FAIL" and g["severity"] == "hard" for g in gate_rows)
    return {"ok": not hard_fail or contended, "record": record}


def evaluate_bench_gates(summary: dict, gates: dict, arms: list[str]) -> list[dict]:
    rows = []
    by_id = {g["id"]: g for g in gates["gates"]}
    allocs = sum(arm["allocations"] for bench in summary.values() for arm in bench.values())
    rows.append({"id": "G-ALLOC", "severity": by_id["G-ALLOC"]["severity"], "detail": "timed-region allocations",
                 "value": allocs, "status": "PASS" if allocs == 0 else "FAIL"})
    if len(arms) == 2:
        base, cand = arms
        limit = by_id["G-PERF"]["threshold"]
        for name, per_arm in summary.items():
            ratio = per_arm[cand]["median_ns"] / per_arm[base]["median_ns"]
            rows.append({"id": "G-PERF", "severity": by_id["G-PERF"]["severity"],
                         "detail": f"{name}: {cand} / {base}", "value": round(ratio, 3),
                         "status": "PASS" if ratio <= limit else "FAIL"})
    return rows


def write_report(record: dict) -> None:
    lines = [f"# Sub0MemPage performance report", "",
             f"Generated {record['utc']} -- label `{record['label']}` -- git `{record['git']}`"
             f"{' (dirty)' if record['dirty'] else ''} -- `{record['config']}` on `{record['host']}`", "",
             f"Contention: named `{record['contention']['named_processes']}`, background "
             f"{record['contention']['background_load_pct']}%"
             + (" -- **ADVISORY: contended run, not evidence**" if record["advisory"] else ""), "",
             "## Gate panel", "", "| Gate | Detail | Value | Status |", "|---|---|---:|---|"]
    lines += [f"| `{g['id']}` | {g['detail']} | {g['value']} | {g['status']} |" for g in record["gates"]]
    lines += ["", f"## Microbenchmarks ({record['rounds']} interleaved rounds x {record['samples']} samples; "
              "ns/op, median of per-run medians)", ""]
    arms = record["arms"]
    lines += ["| Benchmark | " + " | ".join(f"{a} (spread)" for a in arms) + " | Runs |",
              "|---|" + "---:|" * len(arms) + "---|"]
    for name, per_arm in record["results"].items():
        cells = [f"{per_arm[a]['median_ns']} ({per_arm[a]['spread_pct']}%)" for a in arms]
        runs = "; ".join(f"{a}: {', '.join(str(r) for r in per_arm[a]['runs'])}" for a in arms)
        lines.append(f"| {name} | " + " | ".join(cells) + f" | {runs} |")
    lines += ["", "History: `perf_history.jsonl`. Policy: `docs/DEVELOPMENT_WORKFLOW.md`.", ""]
    REPORT.write_text("\n".join(lines), encoding="utf-8")


# --- orchestration ---------------------------------------------------------------------------------------

def check_counts(results: list[dict], gates: dict) -> list[str]:
    """G-SUITE: every suite's check count must equal the recorded one. A changed count nobody can
    attribute is an unexplained behaviour change; an intended one is re-recorded with --accept-counts."""
    expected = next(g for g in gates["gates"] if g["id"] == "G-SUITE")["threshold"]
    problems = []
    for result in results:
        for suite, counts in result.get("suites", {}).items():
            want = expected.get(suite)
            if want is not None and counts["checks"] != want:
                problems.append(f"{result['config']}: {suite} ran {counts['checks']} checks, gate expects {want}")
    return problems


def accept_counts(results: list[dict]) -> None:
    gates = json.loads(GATES.read_text(encoding="utf-8"))
    suite_gate = next(g for g in gates["gates"] if g["id"] == "G-SUITE")
    for result in results:
        for suite, counts in result.get("suites", {}).items():
            suite_gate["threshold"][suite] = counts["checks"]
    GATES.write_text(json.dumps(gates, indent=2) + "\n", encoding="utf-8")
    log(f"[gates] G-SUITE re-recorded: {suite_gate['threshold']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stage", choices=["test", "sanitize", "mutate", "arm", "check", "bench"])
    parser.add_argument("--native-only", action="store_true", help="do not delegate Linux stages to WSL")
    parser.add_argument("--json-out", help="write the stage result as JSON (used by WSL delegation)")
    parser.add_argument("--accept-counts", action="store_true", help="re-record G-SUITE check counts")
    parser.add_argument("--repeat", type=int, default=3, help="sanitizer test repetitions")
    parser.add_argument("--baseline", help="bench: git ref whose library headers form the baseline arm")
    parser.add_argument("--aa", action="store_true", help="bench: run the same binary as both arms (noise floor)")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--label")
    parser.add_argument("--allow-contention", action="store_true", help="bench: record an ADVISORY run")
    args = parser.parse_args()

    gates = json.loads(GATES.read_text(encoding="utf-8"))
    outcome: dict = {"stage": args.stage}
    stages = ["test", "sanitize", "mutate", "arm"] if args.stage == "check" else [args.stage]
    delegate = IS_WINDOWS and not args.native_only
    have_wsl = delegate and wsl_available()

    for stage in stages:
        if stage == "bench":
            outcome["bench"] = stage_bench(args)
            continue
        native: list = []
        if stage == "test":
            native = stage_build_test(["msvc-release"] if IS_WINDOWS else ["gcc-release"])
        elif stage == "sanitize" and not IS_WINDOWS:
            native = stage_build_test(["gcc-asan", "gcc-tsan"], repeat=args.repeat)
        elif stage == "mutate" and not IS_WINDOWS:
            native = stage_mutate()
        elif stage == "arm" and not IS_WINDOWS:
            native = stage_arm()
        linux: list = []
        if delegate:
            if have_wsl:
                linux = delegate_to_wsl(stage, ["--repeat", str(args.repeat)]).get(stage, [])
            else:
                linux = [{"config": f"linux-{stage}", "ok": False, "skipped": "no WSL distro available"}]
        outcome[stage] = native + linux

    if args.json_out:
        pathlib.Path(args.json_out).write_text(json.dumps(outcome, indent=2), encoding="utf-8")
        return 0

    test_like = [r for s in ("test", "sanitize") for r in outcome.get(s, [])]
    if args.accept_counts:
        accept_counts(test_like)
        gates = json.loads(GATES.read_text(encoding="utf-8"))
    problems = check_counts(test_like, gates)
    failed = [f"{stage}: {r.get('config') or r.get('id')} -- {r.get('error') or r.get('skipped') or 'failed'}"
              for stage in ("test", "sanitize", "mutate", "arm") for r in outcome.get(stage, []) if not r.get("ok")]
    if "bench" in outcome and not outcome["bench"].get("ok"):
        failed.append(f"bench: {outcome['bench'].get('error', 'hard gate failed; see docs/perf/perf_report.md')}")

    log("\n== summary ==")
    for stage in ("test", "sanitize", "mutate", "arm"):
        for r in outcome.get(stage, []):
            # skipped takes precedence over ok: some skips (e.g. a sanitizer qemu-user cannot run)
            # carry ok=True so they do not fail `check`, and must still print as SKIP, never PASS.
            state = "SKIP" if r.get("skipped") else ("PASS" if r.get("ok") else "FAIL")
            detail = r.get("suites") or r.get("detected_by") or r.get("skipped") or r.get("error", "")
            log(f"  {stage:9} {str(r.get('config') or r.get('id')):28} {state}  {detail}")
    if "bench" in outcome and "record" in outcome["bench"]:
        for g in outcome["bench"]["record"]["gates"]:
            log(f"  bench     {g['id']:8} {g['detail'][:60]:60} {g['value']}  {g['status']}")
        log(f"  report: {REPORT.relative_to(ROOT)}")
    for p in problems + failed:
        log(f"  PROBLEM: {p}")
    return 1 if problems or failed else 0


if __name__ == "__main__":
    sys.exit(main())
