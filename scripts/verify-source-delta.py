#!/usr/bin/env python3
"""Verify a candidate's exact patched files against a named baseline, without builds.

Apply the patch in a fresh temporary repository, then compare bytes with the
candidate. This catches git-apply invocations that silently skip paths when run
from a nested directory. It does not claim to audit files outside the patch and
any explicitly named --check-file paths. Neither input tree is modified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


def git(directory: pathlib.Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        raise ValueError(result.stderr.decode(errors="replace").strip())
    return result.stdout


def relative_file(value: str) -> pathlib.Path:
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or not path.parts or any(p in ("..", ".git") for p in path.parts):
        raise ValueError(f"unsafe patch path: {value!r}")
    return pathlib.Path(*path.parts)


def read_file(root: pathlib.Path, relative: pathlib.Path) -> bytes | None:
    path = root / relative
    # Reject symlink traversal, including links whose final target is absent.
    for part in (path, *path.parents):
        if part == root:
            break
        if part.is_symlink():
            raise ValueError(f"symlink is outside this verifier's file contract: {path}")
    if not path.exists():
        return None
    if not path.is_file():
        raise ValueError(f"not a regular file: {path}")
    return path.read_bytes()


def verify(baseline: pathlib.Path, candidate: pathlib.Path, patch: pathlib.Path,
           extra_files: list[str] | None = None) -> dict:
    baseline, candidate, patch = baseline.resolve(), candidate.resolve(), patch.resolve()
    if not baseline.is_dir() or not candidate.is_dir():
        raise ValueError("baseline and candidate must be existing source directories")
    patch_bytes = patch.read_bytes()
    with tempfile.TemporaryDirectory(prefix="source-delta-") as temporary:
        stage = pathlib.Path(temporary).resolve()
        git(stage, "init", "--quiet")
        if pathlib.Path(git(stage, "rev-parse", "--show-toplevel").decode().strip()).resolve() != stage:
            raise ValueError("temporary repository root was not selected")
        stats = git(stage, "apply", "--numstat", "-z", str(patch))
        paths = []
        for row in stats.split(b"\0"):
            if not row:
                continue
            fields = row.split(b"\t", 2)
            if len(fields) != 3:
                raise ValueError("unsupported patch path/rename format")
            paths.append(relative_file(fields[2].decode()))
        if not paths:
            raise ValueError("patch describes no files")
        checked = sorted(set(paths + [relative_file(p) for p in (extra_files or [])]))
        for path in checked:
            data = read_file(baseline, path)
            if data is not None:
                (stage / path).parent.mkdir(parents=True, exist_ok=True)
                (stage / path).write_bytes(data)
        git(stage, "apply", "--check", str(patch))
        git(stage, "apply", str(patch))
        report = []
        for path in checked:
            expected = read_file(stage, path)
            actual = read_file(candidate, path)
            if expected != actual:
                raise ValueError(f"candidate differs from baseline plus patch: {path}")
            report.append({"path": path.as_posix(), "exists": actual is not None,
                           "sha256": hashlib.sha256(actual).hexdigest() if actual is not None else None})
        return {"baseline": str(baseline), "candidate": str(candidate),
                "patch_sha256": hashlib.sha256(patch_bytes).hexdigest(),
                "verified_files": report,
                "scope": "patch paths and explicit check-file paths only"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=pathlib.Path, required=True)
    parser.add_argument("--candidate", type=pathlib.Path, required=True)
    parser.add_argument("--patch", type=pathlib.Path, required=True)
    parser.add_argument("--check-file", action="append", default=[])
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        report = verify(args.baseline, args.candidate, args.patch, args.check_file)
    except (ValueError, OSError, UnicodeError) as error:
        parser.exit(1, f"source delta verification failed: {error}\n")
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
