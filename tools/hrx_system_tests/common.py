"""Shared paths, subprocess handling and summary statistics."""
from pathlib import Path
import hashlib
import math
import os
import shutil
import statistics
import subprocess

REPO = Path(__file__).resolve().parents[2]
SOURCES = Path(__file__).resolve().parent
DEFAULT_OUTPUT = REPO / "build/benchmarks/hrx-system-tests"


def default_llvm():
    setting = os.environ.get("AMDGPU_LLVM_BIN")
    if setting:
        return Path(setting)
    for path in (Path("/opt/homebrew/opt/llvm@21/bin"), Path("/opt/homebrew/opt/llvm/bin")):
        if (path / "clang").is_file():
            return path
    compiler = shutil.which("clang")
    return Path(compiler).parent if compiler else Path("/usr/bin")


def add_paths(parser, *, build=False):
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT,
                        help="Artifacts directory (default: build/benchmarks/hrx-system-tests).")
    parser.add_argument("--runtime-dir", type=Path,
                        default=REPO / "build/hsa-code-cache-fix/runtime-default",
                        help="Directory containing the working libhsa-runtime64.dylib.")
    if build:
        parser.add_argument("--lse-build", type=Path,
                            default=REPO / "build/lse-macos-adapter",
                            help="Existing Ninja LSE build with the lse target and Loom enabled.")
        parser.add_argument("--hrx-build", type=Path,
                            default=REPO / "build/hrx-macos-adapter",
                            help="Existing HRX build containing libhrx and libloomc.")
        parser.add_argument("--hrx-source", type=Path,
                            default=REPO / "build/hrx-macos-source",
                            help="HRX source checkout containing libhrx/include/hrx_runtime.h.")
        parser.add_argument("--llvm-bin", type=Path, default=default_llvm(),
                            help="AMDGPU LLVM bin directory (default: AMDGPU_LLVM_BIN or Homebrew LLVM).")
        parser.add_argument("--lld", type=Path,
                            help="ld.lld executable; otherwise use LLVM bin directory or PATH.")


def runtime_environment(runtime_dir, *, trace=False):
    env = os.environ.copy()
    previous = env.get("DYLD_LIBRARY_PATH")
    env["DYLD_LIBRARY_PATH"] = str(runtime_dir) + (":" + previous if previous else "")
    fallback = env.get("DYLD_FALLBACK_LIBRARY_PATH", "/usr/local/lib:/usr/lib")
    z3 = Path("/opt/homebrew/Cellar/z3")
    env["DYLD_FALLBACK_LIBRARY_PATH"] = ":".join(
        [fallback] + [str(path) for path in z3.glob("*/lib")])
    if trace:
        env["DYLD_PRINT_LIBRARIES"] = "1"
    return env


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked_run(command, *, cwd=REPO, env=None, log=None):
    subprocess.run([str(value) for value in command], cwd=cwd, env=env, check=True,
                   stdout=log, stderr=subprocess.STDOUT if log else None)


def stats(values):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("No measured samples")

    def percentile(fraction):
        position = (len(ordered) - 1) * fraction
        lo, hi = math.floor(position), math.ceil(position)
        return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)

    return {"n": len(values), "min": ordered[0], "median": statistics.median(values),
            "p95": percentile(.95), "p99": percentile(.99), "max": ordered[-1],
            "mean": statistics.mean(values)}


def loaded_libraries(folder):
    libraries = {}
    for logfile in (folder / "compile.log", folder / "native.log"):
        if not logfile.is_file():
            continue
        for line in logfile.read_text(errors="replace").splitlines():
            if line.startswith("dyld[") and "> /" in line:
                path = Path(line.split("> ", 1)[1])
                if path.is_file() and path.name.startswith(("libhrx", "libloomc", "libhsa-runtime64")):
                    libraries[str(path.resolve())] = {"sha256": digest(path), "bytes": path.stat().st_size}
    return libraries
