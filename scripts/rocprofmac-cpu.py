#!/usr/bin/env python3
"""Record native CPU samples with Instruments; default is a dry run."""
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
                      "clock": "Instruments CPU timeline; not correlated to rocprofmac GPU ticks"}), flush=True)
    if not args.run:
        return 0
    # xctrace owns sampling and permissions. Do not silently substitute wall time
    # or suppress authorization prompts if native recording cannot start.
    try:
        completed = subprocess.run(argv, timeout=args.seconds + 60, check=False)
    except subprocess.TimeoutExpired:
        print("xctrace exceeded recording deadline; trace may be incomplete", file=sys.stderr)
        return 3
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
