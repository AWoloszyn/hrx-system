# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class RocmEnvironmentTest(unittest.TestCase):
    def probe_command(self, native_script: str, **options) -> list[str]:
        script = (
            "import signal, sys; "
            "from build_tools.ci.rocm_environment import "
            "run_probe, handle_termination; "
            "signal.signal(signal.SIGTERM, handle_termination); "
            f"sys.exit(run_probe([sys.executable, '-c', {native_script!r}], "
            f"**{options!r}))"
        )
        return [sys.executable, "-u", "-c", script]

    def test_success_and_native_errors_preserve_exit_status(self):
        for native_script, expected in (
            ("print('native probe succeeded')", 0),
            ("import sys; sys.exit(23)", 23),
            ("import os, signal; os.kill(os.getpid(), signal.SIGUSR1)", 138),
        ):
            with self.subTest(status=expected):
                result = subprocess.run(
                    self.probe_command(native_script),
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    result.returncode, expected, result.stdout + result.stderr
                )
                self.assertNotIn("capturing process state", result.stdout)

    def test_timeout_captures_native_wait_state_before_termination(self):
        result = subprocess.run(
            self.probe_command(
                "import os, signal; print(f'NATIVE_PID={os.getpid()}', flush=True); "
                "signal.pause()",
                timeout_seconds=2,
                kill_after_seconds=1,
                diagnostic_delay_seconds=1,
            ),
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 124, result.stdout + result.stderr)
        native_id = result.stdout.split("NATIVE_PID=", 1)[1].splitlines()[0]
        self.assertIn(f"/proc/{native_id}/status:", result.stdout)
        self.assertIn(f"/proc/{native_id}/task/{native_id}/wchan:", result.stdout)
        self.assertIn("Recent kernel warnings and errors", result.stdout)

    def test_native_process_ignoring_term_requires_kill(self):
        result = subprocess.run(
            self.probe_command(
                "import signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "print('ignoring TERM', flush=True); signal.pause()",
                timeout_seconds=2,
                kill_after_seconds=1,
                diagnostic_delay_seconds=1,
            ),
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertIn("ignoring TERM", result.stdout)
        self.assertEqual(result.returncode, 137, result.stdout + result.stderr)

    def test_cancellation_reaches_native_child(self):
        process = subprocess.Popen(
            self.probe_command(
                "import os, signal; print(os.getpid(), flush=True); signal.pause()",
                kill_after_seconds=1,
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            # The child explicitly announces readiness before cancellation.
            native_id = int(process.stdout.readline())
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate()
            self.assertEqual(process.returncode, 143, stdout + stderr)
            try:
                status = Path(f"/proc/{native_id}/status").read_text()
            except FileNotFoundError:
                pass
            else:
                # A container's init may not have reaped the terminated child.
                self.assertIn("State:\tZ", status)
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate()

    def test_kernel_log_access_failure_is_explicit(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            dmesg = Path(temporary_dir) / "dmesg"
            dmesg.write_text(
                "#!/bin/sh\necho 'kernel log permission denied'\nexit 13\n"
            )
            dmesg.chmod(0o755)
            result = subprocess.run(
                [
                    sys.executable,
                    "build_tools/ci/rocm_environment.py",
                    "--diagnose",
                    str(os.getpid()),
                ],
                env={**os.environ, "PATH": temporary_dir},
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("kernel log permission denied", result.stdout)
            self.assertIn("Kernel log unavailable (dmesg exit 13)", result.stdout)

    def test_stalled_diagnostics_are_bounded_separately(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            dmesg = Path(temporary_dir) / "dmesg"
            dmesg.write_text(
                "#!/bin/sh\ntrap '' TERM\necho 'stalled kernel reader'\nexec sleep 60\n"
            )
            dmesg.chmod(0o755)
            result = subprocess.run(
                self.probe_command(
                    "import signal; signal.pause()",
                    timeout_seconds=2,
                    kill_after_seconds=1,
                    diagnostic_delay_seconds=1,
                ),
                env={
                    **os.environ,
                    "PATH": temporary_dir + os.pathsep + os.environ["PATH"],
                },
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 124, result.stdout + result.stderr)
            self.assertIn("Process diagnostics incomplete", result.stdout)


if __name__ == "__main__":
    unittest.main()
