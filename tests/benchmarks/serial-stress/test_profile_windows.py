"""Offline regression tests: never run WPR or touch a serial port."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("profile-windows.py")


def load_script():
    if not SCRIPT.exists():
        raise AssertionError("The stdlib Python profiling runner has not been implemented")
    spec = importlib.util.spec_from_file_location("profile_windows", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Child:
    pid = 1234

    def __init__(self, command, kwargs, scenario):
        self.base = Path(command[command.index("--profile-handshake") + 1])
        self.result = Path(command[7])
        self.kwargs = kwargs
        self.scenario = scenario
        self.returncode = None
        self.killed = False
        if scenario != "setup-timeout":
            Path(str(self.base) + ".ready").write_text("ready", encoding="ascii")

    def advance(self):
        if Path(str(self.base) + ".go").exists() and self.scenario != "input-timeout":
            Path(str(self.base) + ".done").write_text("done", encoding="ascii")

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        if not Path(str(self.base) + ".release").exists():
            raise AssertionError("Child must be released even if WPR stop fails")
        if self.scenario == "result-timeout" and not self.killed:
            raise subprocess.TimeoutExpired("benchmark", timeout)
        self.result.write_text(json.dumps({
            "pass": self.scenario != "incomplete",
            "driverQueueExclusionConfirmed": self.scenario != "unconfirmed-queue",
            "validFrames": 20, "missingFrames": 0,
        }), encoding="utf-8")
        self.returncode = -9 if self.killed else 0
        return self.returncode

    def kill(self):
        self.killed = True
        self.returncode = -9


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.module = load_script()
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / "captures"
        self.executable = Path(self.directory.name) / "serial stress.exe"
        self.executable.touch()
        self.children = []
        self.commands = []
        self.scenario = "success"
        self.clock = 0.0

    def run_command(self, command, **kwargs):
        self.commands.append(command)
        status = 0
        output = ""
        if command[:2] == ["wpr", "-status"]:
            output = "A recording is active" if self.scenario == "existing" else "WPR is not recording"
            if self.scenario == "status-failure":
                status = 1
        elif command[:2] == ["wpr", "-start"] and self.scenario == "start-failure":
            status = 1
        elif command[:2] == ["wpr", "-stop"] and self.scenario == "stop-failure":
            status = 2
        elif command[:2] == ["wpr", "-stop"] and self.scenario == "stop-timeout":
            raise subprocess.TimeoutExpired(command, 120)
        elif command[-1] == "--self-test" and self.scenario == "selftest-failure":
            status = 3
        return subprocess.CompletedProcess(command, status, output, "")

    def launch(self, command, **kwargs):
        self.commands.append(command)
        child = Child(command, kwargs, self.scenario)
        self.children.append(child)
        return child

    def sleep(self, seconds):
        self.clock += seconds
        for child in self.children:
            child.advance()

    def invoke(self, platform="win32"):
        arguments = ["--executable-path", str(self.executable),
                     "--output-directory", str(self.output), "--modes", "visible",
                     "--port-a", "COM13", "--port-b", "COM14",
                     "--setup-timeout", "0.2", "--input-timeout", "0.2",
                     "--result-timeout", "0.2"]
        with mock.patch.object(self.module.sys, "platform", platform), \
                mock.patch.object(self.module.subprocess, "run", side_effect=self.run_command), \
                mock.patch.object(self.module.subprocess, "Popen", side_effect=self.launch), \
                mock.patch.object(self.module.time, "monotonic", side_effect=lambda: self.clock), \
                mock.patch.object(self.module.time, "sleep", side_effect=self.sleep), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return self.module.main(arguments)

    def assert_clean_child(self):
        self.assertEqual(len(self.children), 1)
        child = self.children[0]
        self.assertTrue(Path(str(child.base) + ".release").exists())
        self.assertIsNotNone(child.returncode)
        self.assertTrue(child.kwargs["stdout"].closed)
        self.assertTrue(child.kwargs["stderr"].closed)
        self.assertNotIn("startupinfo", child.kwargs)

    def test_success_preserves_handshake_verification_and_selected_ports(self):
        self.assertEqual(self.invoke(), 0)
        self.assert_clean_child()
        launch = next(command for command in self.commands if "--profile-handshake" in command)
        self.assertEqual(launch[1:3], ["COM13", "COM14"])
        base = self.children[0].base
        for suffix in ("ready", "go", "done", "release"):
            self.assertTrue(Path(str(base) + "." + suffix).exists())
        self.assertEqual(sum(command[:2] == ["wpr", "-stop"] for command in self.commands), 1)

    def test_existing_recording_is_never_started_stopped_or_given_a_child(self):
        self.scenario = "existing"
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.commands, [["wpr", "-status"]])
        self.assertEqual(self.children, [])

    def test_failed_start_never_stops_someone_elses_recording(self):
        self.scenario = "start-failure"
        self.assertEqual(self.invoke(), 1)
        self.assertFalse(any(command[:2] == ["wpr", "-stop"] for command in self.commands))
        self.assert_clean_child()
        self.assertTrue(self.children[0].killed)

    def test_failed_status_is_not_authorization_to_start(self):
        self.scenario = "status-failure"
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.commands, [["wpr", "-status"]])
        self.assertEqual(self.children, [])

    def test_failed_stop_still_releases_and_reaps_child_and_closes_logs(self):
        self.scenario = "stop-failure"
        self.assertEqual(self.invoke(), 1)
        self.assert_clean_child()

    def test_stop_timeout_still_releases_and_reaps_child(self):
        self.scenario = "stop-timeout"
        self.assertEqual(self.invoke(), 1)
        self.assert_clean_child()

    def test_setup_timeout_cleans_up_without_stopping_wpr(self):
        self.scenario = "setup-timeout"
        self.assertEqual(self.invoke(), 1)
        self.assert_clean_child()
        self.assertTrue(self.children[0].killed)
        self.assertFalse(any(command[:2] == ["wpr", "-stop"] for command in self.commands))

    def test_input_timeout_stops_own_recording_and_kills_child(self):
        self.scenario = "input-timeout"
        self.assertEqual(self.invoke(), 1)
        self.assert_clean_child()
        self.assertTrue(self.children[0].killed)
        self.assertEqual(sum(command[:2] == ["wpr", "-stop"] for command in self.commands), 1)

    def test_result_timeout_kills_and_reaps_child(self):
        self.scenario = "result-timeout"
        self.assertEqual(self.invoke(), 1)
        self.assert_clean_child()
        self.assertTrue(self.children[0].killed)

    def test_completeness_or_unconfirmed_queue_rejects_capture(self):
        for scenario in ("incomplete", "unconfirmed-queue"):
            with self.subTest(scenario=scenario):
                self.children = []
                self.scenario = scenario
                self.assertEqual(self.invoke(), 1)
                self.assert_clean_child()

    def test_selftest_failure_does_not_launch_benchmark(self):
        self.scenario = "selftest-failure"
        self.assertEqual(self.invoke(), 1)
        self.assertEqual(self.children, [])
        self.assertFalse(any(command[:2] == ["wpr", "-start"] for command in self.commands))

    def test_non_windows_fails_before_process_or_output_side_effects(self):
        self.assertEqual(self.invoke(platform="linux"), 1)
        self.assertEqual(self.commands, [])
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
