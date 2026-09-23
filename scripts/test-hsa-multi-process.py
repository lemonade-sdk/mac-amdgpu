#!/usr/bin/env python3
"""Verify that one HSA client's queue teardown preserves another client's session."""
from pathlib import Path
import subprocess
import threading

root = Path(__file__).resolve().parents[1]
output = root / "build/tests"
output.mkdir(parents=True, exist_ok=True)
command = [str(root / "build/hsa/mac-hsa-queue-test"), "--shared-session",
           str(output / "hsa-code-object.hsaco")]
processes, readers, ready, lines = [], [], [], []


def start():
    index = len(processes)
    process = subprocess.Popen(command, cwd=root, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1)
    processes.append(process)
    ready.append(threading.Event())
    lines.append([])

    def read():
        with (output / f"hsa-multi-process-{index + 1}.log").open("w") as log:
            for line in process.stdout:
                lines[index].append(line)
                log.write(line)
                log.flush()
                print(f"client {index + 1}: {line}", end="", flush=True)
                if line.startswith("READY:"):
                    ready[index].set()
        ready[index].set()  # Wake the controller on early failure too.

    reader = threading.Thread(target=read, daemon=True)
    readers.append(reader)
    reader.start()
    if not ready[index].wait(40) or not any(s.startswith("READY:") for s in lines[index]):
        raise RuntimeError(f"client {index + 1} did not create its queues")
    return process


try:
    first = start()
    second = start()
    if first.poll() is not None or second.poll() is not None:
        raise RuntimeError("clients did not hold queues concurrently")
    first.stdin.write("R\n")
    first.stdin.flush()
    if first.wait(timeout=40) != 0:
        raise RuntimeError("first client's workload or teardown failed")
    if second.poll() is not None:
        raise RuntimeError("second client exited before its workload was released")
    print("First client exited; running second client's existing queues", flush=True)
    second.stdin.write("R\n")
    second.stdin.flush()
    if second.wait(timeout=40) != 0:
        raise RuntimeError("remaining client's workload or teardown failed")
    for reader in readers:
        reader.join(timeout=5)
    for result in lines:
        if not any(s.startswith("PASS: persistent AQL wraparound") for s in result):
            raise RuntimeError("a client did not report complete verification")
    print("PASS: two processes, four concurrent queues, independent teardown and continued dispatch")
finally:
    for process in processes:
        if process.stdin:
            try:
                process.stdin.close()
            except BrokenPipeError:
                pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
    for reader in readers:
        reader.join(timeout=5)
