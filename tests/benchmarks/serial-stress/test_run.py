"""Serial matrix orchestration tests; never open a serial port or launch Qt."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SOURCE = Path(__file__).with_name("run.py")


class RunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = None
        if SOURCE.is_file():
            spec = importlib.util.spec_from_file_location("serial_matrix_runner", SOURCE)
            cls.runner = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.runner)

    def execute(self, switches=(), codes=(), platform="linux", environment=None):
        self.assertIsNotNone(self.runner, "Python serial matrix runner must exist")
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        output = Path(directory.name) / "results with spaces"
        calls = []
        remaining = iter(codes)

        def fake_run(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, next(remaining, 0))

        env = {"PATH": "existing-path", "QT_QPA_PLATFORM": "offscreen"}
        if environment:
            env.update(environment)
        with mock.patch.object(self.runner.sys, "platform", platform), \
                mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(self.runner.subprocess, "run", side_effect=fake_run), \
                contextlib.redirect_stdout(io.StringIO()) as stdout, \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            before = dict(os.environ)
            result = self.runner.main([
                "--output-directory", str(output), "--executable-path", "fixture with spaces",
                *switches,
            ])
            self.assertEqual(dict(os.environ), before)
        self.assertTrue(output.is_dir())
        self.assertEqual(calls[0][0], ["fixture with spaces", "--self-test"])
        return result, [command for command, _ in calls[1:]], calls, stdout.getvalue(), stderr.getvalue()

    def case_arguments(self, command):
        self.assertEqual(command[0], "fixture with spaces")
        self.assertEqual(Path(command[7]).suffix, ".json")
        return command[1:7] + command[8:]

    def test_default_matrix_preserves_all_36_cases_and_order(self):
        result, commands, _, stdout, _ = self.execute()
        expected = []
        for baud in ("1000000", "3000000", "5000000", "8000000"):
            for first, second in (("COM1", "COM2"), ("COM2", "COM1")):
                for size in ("65536", "1048576", "8388608"):
                    expected.append([first, second, baud, "20", "visible", "paced", "--bytes", size])
                expected.append([first, second, baud, "20", "hidden", "paced", "--bytes", "8388608"])
        for first, second in (("COM1", "COM2"), ("COM2", "COM1")):
            for pacing in ("paced", "burst"):
                expected.append([first, second, "8000000", "20", "visible", pacing, "--bytes", "67108864"])
        self.assertEqual([self.case_arguments(command) for command in commands], expected)
        self.assertEqual(result, 0)
        self.assertIn("failures=0", stdout)

    def test_all_special_scenarios_preserve_arguments(self):
        cases = {
            "--smoke": (2, ["1000000", "1", "hidden", "paced"], [], "smoke-hidden.json"),
            "--diagnostics": (4, ["8000000", "20", "visible", "burst"], ["--bytes", "67108864", "--deferred-verify"], "8000000-COM1-COM2-visible-deferred-burst-67108864.json"),
            "--chunk-controls": (6, ["8000000", "20", "visible", "burst"], ["--bytes", "8388608", "--deferred-verify", "--tx-chunk", "4096"], "8000000-COM1-COM2-visible-deferred-burst-8388608-chunk4096.json"),
            "--long-cases": (2, ["8000000", "20", "visible", "paced"], ["--bytes", "67108864"], "8000000-COM1-COM2-visible-paced-67108864.json"),
            "--timing-controls": (4, ["8000000", "20", "raw", "paced"], ["--bytes", "67108864"], "8000000-COM1-COM2-raw-paced-67108864.json"),
            "--stable-raw-case": (1, ["8000000", "20", "raw", "paced"], ["--bytes", "67108864", "--stable-timers"], "8000000-COM1-COM2-raw-stable-paced-67108864.json"),
            "--stable-terminal-case": (1, ["8000000", "20", "visible", "paced"], ["--bytes", "67108864", "--stable-timers", "--deferred-verify"], "8000000-COM1-COM2-visible-stable-deferred-paced-67108864.json"),
            "--driver-queue-probe": (1, ["8000000", "1", "visible", "paced"], ["--bytes", "1048576", "--driver-rx-queue", "262144", "--stable-timers", "--deferred-verify"], "queue-probe.json"),
            "--large-queue-cases": (6, ["8000000", "20", "visible", "paced"], ["--bytes", "67108864", "--driver-rx-queue", "262144", "--stable-timers", "--deferred-verify"], "8000000-COM1-COM2-visible-stable-deferred-paced-67108864-rx262144.json"),
        }
        for switch, (count, primary, extra, filename) in cases.items():
            with self.subTest(switch=switch):
                result, commands, _, _, _ = self.execute([switch])
                self.assertEqual(result, 0)
                self.assertEqual(len(commands), count)
                if switch == "--chunk-controls":
                    self.assertEqual(commands[0][1:3], ["--probe", "COM1"])
                    self.assertEqual(Path(commands[0][3]).name, "driver-COM1.json")
                    self.assertEqual(commands[1][1:3], ["--probe", "COM2"])
                    commands = commands[2:]
                self.assertEqual(commands[0][1:7], ["COM1", "COM2", *primary])
                self.assertEqual(commands[0][8:], extra)
                self.assertEqual(Path(commands[0][7]).name, filename)
                if switch in ("--diagnostics", "--chunk-controls", "--large-queue-cases"):
                    modes = ["visible", "hidden", "raw"] if switch == "--large-queue-cases" else ["visible", "raw"]
                    self.assertEqual([(command[1], command[2], command[5]) for command in commands],
                                     [(a, b, mode) for a, b in (("COM1", "COM2"), ("COM2", "COM1")) for mode in modes])
                    for command in commands:
                        self.assertEqual(command[8:], extra)
                if switch == "--timing-controls":
                    self.assertEqual([command[5] for command in commands], ["raw", "visible", "raw", "visible"])
                    self.assertEqual([command[10:] for command in commands], [[], ["--stable-timers"], ["--stable-timers"], ["--stable-timers", "--deferred-verify"]])

    def test_precedence_and_legacy_aliases(self):
        result, commands, _, _, _ = self.execute(["-Smoke", "-Diagnostics", "-StableTerminalCase", "-DriverQueueProbe"])
        self.assertEqual(result, 0)
        self.assertEqual(len(commands), 1)
        self.assertEqual(Path(commands[0][7]).name, "queue-probe.json")

    def test_self_test_failure_prevents_matrix(self):
        result, commands, _, _, stderr = self.execute(["--smoke"], [4])
        self.assertNotEqual(result, 0)
        self.assertEqual(commands, [])
        self.assertIn("self-test failed", stderr)

    def test_counts_failures_and_continues(self):
        result, commands, _, stdout, _ = self.execute(["--smoke"], [0, 2, 7])
        self.assertEqual(result, 2)
        self.assertEqual(len(commands), 2)
        self.assertIn("failures=2", stdout)

    def test_queue_rejection_stops_large_queue_matrix(self):
        result, commands, _, _, stderr = self.execute(["--large-queue-cases"], [0, 1, 7])
        self.assertNotEqual(result, 0)
        self.assertEqual(len(commands), 2)
        self.assertIn("Driver queue enlargement rejected", stderr)

    def test_child_environment_is_portable_and_preserves_explicit_backend(self):
        _, commands, calls, _, _ = self.execute(
            ["--smoke", "--qt-prefix", "/opt/custom Qt", "--port-a", "/dev/ttyUSB0", "--port-b", "/dev/ttyUSB1"],
            environment={"NOVATERM_RHI_API": "vulkan"})
        child = calls[0][1]["env"]
        self.assertEqual(child["PATH"], str(Path("/opt/custom Qt") / "bin") + os.pathsep + "existing-path")
        self.assertEqual(child["QT_PLUGIN_PATH"], str(Path("/opt/custom Qt") / "plugins"))
        self.assertEqual(child["NOVATERM_RHI_API"], "vulkan")
        self.assertNotIn("QT_QPA_PLATFORM", child)
        self.assertEqual(child["QT_WIDGETS_RHI"], "1")
        self.assertEqual(commands[0][1:3], ["/dev/ttyUSB0", "/dev/ttyUSB1"])

    def test_non_windows_defaults_do_not_force_windows_qt_or_backend(self):
        _, _, calls, _, _ = self.execute(["--smoke"])
        child = calls[0][1]["env"]
        self.assertEqual(child["PATH"], "existing-path")
        self.assertNotIn("NOVATERM_RHI_API", child)
        self.assertNotIn("QT_PLUGIN_PATH", child)

    def test_serial_device_names_cannot_create_nested_result_paths(self):
        _, commands, _, _, _ = self.execute(["--long-cases", "--port-a", "/dev/ttyUSB0", "--port-b", "/dev/serial/by-id/usb:device"])
        for command in commands:
            filename = Path(command[7])
            self.assertEqual(filename.parent.name, "results with spaces")
            self.assertNotIn(":", filename.name)
        self.assertEqual(commands[0][1:3], ["/dev/ttyUSB0", "/dev/serial/by-id/usb:device"])

    def test_launch_failure_is_reported_without_traceback(self):
        self.assertIsNotNone(self.runner, "Python serial matrix runner must exist")
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(self.runner.subprocess, "run", side_effect=FileNotFoundError("missing fixture")), \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            result = self.runner.main(["--smoke", "--output-directory", directory])
        self.assertNotEqual(result, 0)
        self.assertIn("missing fixture", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
