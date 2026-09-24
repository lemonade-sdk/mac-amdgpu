#!/usr/bin/env python3
"""Record native CPU samples or thread stacks; default is a dry run."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def command(args):
    if not 1 <= args.seconds <= 3600:
        raise ValueError("--seconds must be in [1, 3600]")
    if bool(args.attach) == bool(args.program):
        raise ValueError("choose --attach PID or a command after --")
    if args.attach is not None and args.attach <= 0:
        raise ValueError("--attach must be a positive PID")
    if args.attach and args.env:
        raise ValueError("--env only applies to a launched process")
    output = Path(args.output).expanduser().resolve()
    if args.tool == "sample":
        if not args.attach or args.program:
            raise ValueError("--tool sample requires --attach PID; it does not launch a target")
        if output.suffix != ".txt" or output.exists():
            raise ValueError("sample --output must be a new .txt path")
        return ["/usr/bin/sample", str(args.attach), str(args.seconds), "1",
                "-file", str(output)]
    if output.suffix != ".trace" or output.exists():
        raise ValueError("--output must be a new .trace path")
    result = ["xcrun", "xctrace", "record", "--template", "Time Profiler",
              "--time-limit", f"{args.seconds}s", "--output", str(output)]
    if args.attach:
        result += ["--attach", str(args.attach)]
    else:
        for value in args.env:
            if "=" not in value or not value.split("=", 1)[0]:
                raise ValueError("--env requires NAME=value")
            result += ["--env", value]
        result += ["--target-stdout", "-", "--launch", "--", *args.program]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="actually record; otherwise only print argv")
    parser.add_argument("--tool", choices=("xctrace", "sample"), default="xctrace")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--output", required=True)
    parser.add_argument("--attach", type=int)
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("program", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.program[:1] == ["--"]:
        args.program = args.program[1:]
    try:
        argv = command(args)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps({"argv": argv, "execute": args.run,
                      "measurement": ("thread-stack snapshots, including waiting threads; not CPU-time percentages"
                                      if args.tool == "sample" else "Instruments Time Profiler"),
                      "clock": "native host sampling; not correlated to rocprofmac GPU ticks"}), flush=True)
    if not args.run:
        return 0
    # Native tools own sampling and permissions. Never silently substitute a
    # different sampler, and never terminate an attached target.
    try:
        completed = subprocess.run(argv, timeout=args.seconds + 60, check=False)
    except subprocess.TimeoutExpired:
        print(f"{args.tool} exceeded recording deadline; output may be incomplete; target startup/retirement is unconfirmed", file=sys.stderr)
        return 3
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
