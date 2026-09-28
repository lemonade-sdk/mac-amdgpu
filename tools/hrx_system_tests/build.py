#!/usr/bin/env python3
"""Build the HRX fixtures offline and optionally measure uncached compilation."""
import argparse
import json
import shlex
import shutil
import subprocess
import time

from common import REPO, SOURCES, add_paths, checked_run, digest, runtime_environment


def compiler_arguments(line, source, output):
    original = shlex.split(line)
    args = []
    iterator = iter(original)
    for value in iterator:
        if value in ("-MD", "-MMD"):
            continue
        if value in ("-MF", "-MT", "-MQ"):
            next(iterator)
            continue
        args.append(value)
    args[args.index("-o") + 1] = str(output)
    args[args.index("-c") + 1] = str(source)
    return args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_paths(parser, build=True)
    parser.add_argument("--skip-compile-measurements", action="store_true",
                        help="Build/link only; do not run the compiler benchmark or timed CLI compile loops.")
    args = parser.parse_args()
    folder = args.output_dir.resolve()
    build, runtime = args.lse_build.resolve(), args.runtime_dir.resolve()
    hrx_source, hrx_build = args.hrx_source.resolve(), args.hrx_build.resolve()
    llvm = args.llvm_bin.resolve()
    hrx_lib = hrx_build / "libhrx/src/libhrx"
    loom_lib = hrx_build / "loom/binding/c/libloomc.dylib"
    required = [build / "build.ninja", hrx_source / "libhrx/include/hrx_runtime.h",
                hrx_lib / "libhrx.dylib", loom_lib, runtime / "libhsa-runtime64.dylib",
                llvm / "clang", llvm / "clang++", llvm / "llvm-readelf", llvm / "llvm-objdump"]
    for path in required:
        if not path.is_file():
            parser.error(f"Missing prerequisite: {path}")
    lld = args.lld or (llvm / "ld.lld" if (llvm / "ld.lld").is_file() else shutil.which("ld.lld"))
    if not lld:
        parser.error("ld.lld was not found; pass --lld")
    folder.mkdir(parents=True, exist_ok=True)
    env = runtime_environment(runtime)
    lines = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "lse"], text=True).splitlines()
    main_line = next((line for line in lines if " -c " in line and "src/cli/main.cpp" in line), None)
    if main_line is None:
        parser.error("The LSE Ninja target does not expose its main.cpp compile command")
    compile_args = compiler_arguments(main_line, SOURCES / "compile.cpp", folder / "compile.o")
    link = shlex.split(lines[-1])
    start = next((i for i, value in enumerate(link) if value.endswith(("clang++", "g++"))), None)
    if start is None:
        parser.error("Cannot locate the LSE host compiler in its link command")
    link = link[start:]
    if "&&" in link:
        link = link[:link.index("&&")]
    main_object = next(value for value in link if value.endswith("CMakeFiles/lse.dir/src/cli/main.cpp.o"))
    link[link.index(main_object)] = str(folder / "compile.o")
    link[link.index("-o") + 1] = str(folder / "compile-bench")
    commands = {"compile_fixture": compile_args, "link_fixture": link, "native": {}}
    with (folder / "build.log").open("w") as log:
        checked_run(compile_args, cwd=build, env=env, log=log)
        checked_run(link, cwd=build, env=env, log=log)
        shutil.copy2(SOURCES / "latency.cl", folder / "latency.cl")
        clang = [llvm / "clang", "-target", "amdgcn-amd-amdhsa", "-mcpu=gfx1201", "-nogpulib",
                 "-x", "cl", "-cl-std=CL2.0", "-O2", "-c", folder / "latency.cl", "-o", folder / "latency.o"]
        code_link = [lld, "-shared", "--no-undefined", folder / "latency.o", "-o", folder / "latency.hsaco"]
        if args.skip_compile_measurements:
            checked_run(clang, env=env, log=log)
            checked_run(code_link, env=env, log=log)
        else:
            rows = []
            for trial in range(4):
                start_ns = time.perf_counter_ns()
                checked_run(clang, env=env, log=log)
                compiled_ns = time.perf_counter_ns()
                checked_run(code_link, env=env, log=log)
                stop_ns = time.perf_counter_ns()
                rows.append({"trial": trial, "compile_ms": (compiled_ns-start_ns)/1e6,
                             "link_ms": (stop_ns-compiled_ns)/1e6, "total_ms": (stop_ns-start_ns)/1e6})
            source = (folder / "latency.cl").read_text()
            (folder / "opencl-compile.json").write_text(json.dumps({
                "command": [str(v) for v in clang], "link_command": [str(v) for v in code_link],
                "trials": rows, "source_lines": len(source.splitlines()),
                "nonblank_lines": sum(bool(line.strip()) for line in source.splitlines()),
                "source_bytes": (folder / "latency.cl").stat().st_size,
                "code_bytes": (folder / "latency.hsaco").stat().st_size}, indent=2) + "\n")
        for name in ("native", "bandwidth-disjoint"):
            binary = folder / ("native-bench" if name == "native" else name)
            native = [llvm / "clang++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                      "-I" + str(hrx_source / "libhrx/include"), "-I" + str(REPO / "hsa/include"),
                      "-I" + str(REPO / "hsa/third_party/hsa/include"), SOURCES / (name + ".cpp"),
                      "-L" + str(hrx_lib), "-lhrx", "-Wl,-rpath," + str(hrx_lib),
                      "-L" + str(runtime), "-lhsa-runtime64", "-Wl,-rpath," + str(runtime), "-o", binary]
            commands["native"][name] = [str(v) for v in native]
            checked_run(native, env=env, log=log)
            checked_run(["install_name_tool", "-change", "@rpath/libhsa-runtime64.1.dylib",
                         "@rpath/libhsa-runtime64.dylib", binary], log=log)
            checked_run(["codesign", "--force", "--sign", "-", binary], log=log)
        for tool, name, flags in (("llvm-readelf", "latency.metadata", ["--notes"]),
                                  ("llvm-objdump", "latency.disasm", ["-d"])):
            with (folder / name).open("w") as output:
                checked_run([llvm / tool, *flags, folder / "latency.hsaco"], env=env, log=output)
    if not args.skip_compile_measurements:
        measure_env = dict(runtime_environment(runtime, trace=True), LSE_HRX_INT8="1")
        with (folder / "compile.log").open("w") as log:
            checked_run([folder / "compile-bench", folder], env=measure_env, log=log)
    files = [folder / "compile-bench", folder / "native-bench", folder / "bandwidth-disjoint",
             folder / "latency.hsaco", hrx_lib / "libhrx.dylib", loom_lib,
             runtime / "libhsa-runtime64.dylib"]
    metadata = {"paths": {"lse_build": str(build), "hrx_build": str(hrx_build),
                          "hrx_source": str(hrx_source), "runtime_dir": str(runtime), "llvm_bin": str(llvm)},
                "build_commands": commands, "compile_measurements_run": not args.skip_compile_measurements,
                "files": {str(path.resolve()): {"bytes": path.stat().st_size, "sha256": digest(path)} for path in files},
                "fixture_sources": {name: digest(SOURCES / name) for name in
                                    ("compile.cpp", "native.cpp", "bandwidth-disjoint.cpp", "latency.cl")}}
    (folder / "build-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Built fixtures: {folder}; no GPU work submitted.")


if __name__ == "__main__":
    main()
