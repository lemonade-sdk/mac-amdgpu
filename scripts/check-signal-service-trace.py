#!/usr/bin/env python3
"""Verify backend/lifetime evidence from the explicit hardware lifecycle tool log."""
import re
import sys
from pathlib import Path


def validate(text):
    live = set()
    completed = set()
    reasons = set()
    ready = 0
    fallback = 0
    for line in text.splitlines():
        if match := re.search(r'signal-mailbox: ready queue=(\d+)', line):
            queue = int(match[1])
            assert queue not in live, 'queue mapped again before retirement'
            live.add(queue)
            ready += 1
        elif match := re.search(r'signal-mailbox: backend=mailbox first-request-completed queue=(\d+)', line):
            queue = int(match[1])
            assert queue in live, 'completion without ready queue'
            completed.add(queue)
        elif match := re.search(r'signal-mailbox: retired reason=(\S+) queue=(\d+) completed-requests=(\d+) completion=(\S+)', line):
            reason, queue, count, completion = match[1], int(match[2]), int(match[3]), match[4]
            assert queue in live and queue in completed, 'retirement without proven request'
            assert count > 0 and completion == 'confirmed', 'unconfirmed completion'
            live.remove(queue)
            completed.remove(queue)
            reasons.add(reason)
        elif 'signal-mailbox: backend=one-shot fallback-completed' in line:
            fallback += 1
        assert 'signal-mailbox: retirement-failed' not in line, 'failed retirement'
    assert ready >= 4 and not live and not completed, 'missing startup, restart, or final retirement'
    assert {'idle', 'public-queue', 'shutdown'} <= reasons, 'missing idle/reclaim/shutdown path'
    assert fallback > 0, 'reserved one-shot fallback unproven'
    # Keep historical opt-in qualification logs readable after default promotion.
    assert ('PASS GPU-mediated HSA signal service:' in text or
            'PASS opt-in HSA signal service:' in text), 'lifecycle tool did not pass'
    return ready, fallback


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: check-signal-service-trace.py hardware-lifecycle.log')
    try:
        ready, fallback = validate(Path(sys.argv[1]).read_text())
    except (AssertionError, OSError) as error:
        raise SystemExit(f'FAIL trace proof: {error}')
    print(f'PASS backend trace: {ready} mailbox lifetimes retired, idle/public-queue/shutdown observed, {fallback} completed reserved fallback(s)')
