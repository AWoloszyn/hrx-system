#!/usr/bin/env python3
# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Runs the native ROCm probe and records a stalled process before termination."""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path


def print_proc_file(path: Path) -> str:
    try:
        with path.open() as source:
            text = source.read(8193)
        print(f"{path}:\n{text[:8192].rstrip()}", flush=True)
        if len(text) > 8192:
            print("File snapshot truncated after 8192 characters.", flush=True)
        return text
    except OSError as error:
        # Containers may deny stack access, and a thread may exit mid-snapshot.
        print(f"{path}: unavailable: {error}", flush=True)
        return ""


def diagnose_process(process_id: int) -> None:
    """Reads procfs without creating another client of the stalled GPU driver."""
    print(f"Kernel: {os.uname().release}", flush=True)
    pending = [process_id]
    visited = set()
    while pending and len(visited) < 64:
        current_id = pending.pop()
        if current_id in visited:
            continue
        visited.add(current_id)
        process_path = Path(f"/proc/{current_id}")
        print_proc_file(process_path / "status")
        try:
            tasks = sorted((process_path / "task").iterdir())
        except OSError as error:
            print(f"{process_path}/task: unavailable: {error}", flush=True)
            continue
        for task in tasks[:256]:
            print_proc_file(task / "comm")
            print_proc_file(task / "wchan")
            print_proc_file(task / "stack")
            children = print_proc_file(task / "children")
            pending.extend(int(child) for child in children.split())
        if len(tasks) > 256:
            print("Thread snapshot truncated after 256 threads.", flush=True)
    if pending:
        print("Process snapshot truncated after 64 processes.", flush=True)

    print("Recent kernel warnings and errors (last 200 lines):", flush=True)
    try:
        result = subprocess.run(
            ["dmesg", "--level=err,warn", "--since=-2min"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        print("\n".join(result.stdout.splitlines()[-200:]), flush=True)
        if result.returncode:
            print(
                f"Kernel log unavailable (dmesg exit {result.returncode}).", flush=True
            )
    except OSError as error:
        print(f"Kernel log unavailable: {error}", flush=True)


def run_probe(
    command: list[str],
    *,
    timeout_seconds: float = 30,
    kill_after_seconds: float = 5,
    diagnostic_delay_seconds: float = 25,
) -> int:
    # GNU timeout remains the independent supervisor. Diagnostics cannot delay
    # its TERM/KILL deadlines, even when reading kernel state stalls as well.
    process = subprocess.Popen(
        [
            "timeout",
            f"--kill-after={kill_after_seconds}s",
            f"{timeout_seconds}s",
            *command,
        ],
        start_new_session=True,
    )
    try:
        try:
            process.wait(timeout=diagnostic_delay_seconds)
        except subprocess.TimeoutExpired:
            print(
                f"ROCm probe still running after {diagnostic_delay_seconds:g}s; "
                "capturing process state before termination.",
                flush=True,
            )
            result = subprocess.run(
                [
                    "timeout",
                    "--kill-after=1s",
                    "3s",
                    sys.executable,
                    "-u",
                    __file__,
                    "--diagnose",
                    str(process.pid),
                ],
                start_new_session=True,
                check=False,
            )
            if result.returncode:
                print(
                    f"Process diagnostics incomplete (exit {result.returncode}).",
                    flush=True,
                )
        returncode = process.wait()
        return returncode if returncode >= 0 else 128 - returncode
    finally:
        if process.poll() is None:
            # Cancellation must reach both the supervisor and its native child.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                # The process group exited between poll and killpg.
                process.wait()
            try:
                process.wait(timeout=kill_after_seconds)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    # The supervisor's own kill deadline may have fired first.
                    pass
                process.wait()


def handle_termination(signum: int, frame: object) -> None:
    raise SystemExit(128 + signum)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diagnose", type=int, metavar="PID")
    args = parser.parse_args()
    if args.diagnose is not None:
        diagnose_process(args.diagnose)
        return 0

    for tool in ("rocminfo", "timeout"):
        if not shutil.which(tool):
            print(f"::error::Required ROCm preflight tool {tool} was not found.")
            return 1
    runner_name = os.environ.get("RUNNER_NAME", "unknown")
    print(f"rocminfo path: {shutil.which('rocminfo')}", flush=True)
    print(f"Checking ROCm hardware on runner {runner_name} (timeout: 30s).", flush=True)
    signal.signal(signal.SIGTERM, handle_termination)
    try:
        status = run_probe(["rocminfo"])
    except KeyboardInterrupt:
        return 130
    if status == 124:
        print(f"::error::ROCm hardware probe exceeded 30s on runner {runner_name}.")
    elif status == 137:
        print(f"::error::ROCm hardware probe required SIGKILL on runner {runner_name}.")
    elif status:
        print(
            f"::error::ROCm hardware probe failed with exit code {status} on "
            f"runner {runner_name} before GPU tests."
        )
    return status


if __name__ == "__main__":
    sys.exit(main())
