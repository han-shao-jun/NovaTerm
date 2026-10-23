"""Manual regression checks for display leases and child cleanup; no desktop changes."""
import subprocess
import json
from pathlib import Path
import runpy
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

from gui_display_guard import DisplayGuard, run_visible


class DisplayGuardTests(unittest.TestCase):
    def setUp(self):
        self.bus = Mock()
        self.saver = Mock()
        self.saver.GetActive.return_value = False
        self.saver.Inhibit.return_value = 11
        self.power = Mock()
        self.power.Inhibit.return_value = 22
        fake = types.SimpleNamespace(SessionBus=lambda: self.bus,
            Interface=lambda proxy, name: self.saver if name == 'org.freedesktop.ScreenSaver' else self.power)
        self.modules = patch.dict(sys.modules, dbus=fake)
        self.modules.start()
        self.addCleanup(self.modules.stop)

    def test_body_failure_releases_both_leases(self):
        with patch('gui_display_guard.subprocess.run'), patch(
                'gui_display_guard.subprocess.check_output', return_value='Monitor is On'):
            with self.assertRaisesRegex(ValueError, 'sample failed'):
                with DisplayGuard():
                    raise ValueError('sample failed')
        self.saver.UnInhibit.assert_called_once_with(11, timeout=5)
        self.power.UnInhibit.assert_called_once_with(22, timeout=5)
        self.bus.close.assert_called_once()

    def test_partial_acquisition_releases_first_lease(self):
        self.power.Inhibit.side_effect = RuntimeError('power service rejected')
        with patch('gui_display_guard.subprocess.run') as wake:
            with self.assertRaisesRegex(RuntimeError, 'power service rejected'):
                with DisplayGuard():
                    self.fail('failed acquisition must not enter the sample')
        self.saver.UnInhibit.assert_called_once_with(11, timeout=5)
        self.bus.close.assert_called_once()
        wake.assert_not_called()

    def test_locked_desktop_is_rejected_before_wake_or_lease(self):
        self.saver.GetActive.return_value = True
        with patch('gui_display_guard.subprocess.run') as wake:
            with self.assertRaisesRegex(RuntimeError, 'desktop is locked'):
                with DisplayGuard():
                    self.fail('locked desktop must not enter the sample')
        self.saver.Inhibit.assert_not_called()
        self.power.Inhibit.assert_not_called()
        wake.assert_not_called()
        self.bus.close.assert_called_once()

    def test_visibility_loss_stops_the_real_child(self):
        class Visibility:
            checks = 0
            def check(self):
                self.checks += 1
                if self.checks > 1:
                    raise RuntimeError('visibility lost')
        started = []
        real_popen = subprocess.Popen
        def start(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            started.append(child)
            return child
        with tempfile.TemporaryFile() as log, patch('gui_display_guard.subprocess.Popen', side_effect=start):
            with self.assertRaisesRegex(RuntimeError, 'visibility lost'):
                run_visible([sys.executable, '-c', 'import time; time.sleep(60)'], log, Visibility(), timeout=5)
        self.assertEqual(len(started), 1)
        self.assertIsNotNone(started[0].poll(), 'rejected sample must not leave a child running')

    def test_dependency_setup_failure_is_recorded(self):
        guard = DisplayGuard()
        with patch.dict(sys.modules, dbus=None):
            with self.assertRaises(ModuleNotFoundError):
                guard.__enter__()
        self.assertTrue(guard.description.get('aborted'))
        self.assertIn('dbus', guard.description.get('error', ''))

    def test_initial_locked_failure_is_saved_in_new_output(self):
        self.saver.GetActive.return_value = True
        runner = runpy.run_path(str(Path(__file__).with_name('run-gui-matrix.py')))
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'new-output'
            with patch.object(sys, 'argv', ['run-gui-matrix.py', '--keep-display-on', '--output', str(output)]):
                with self.assertRaisesRegex(RuntimeError, 'desktop is locked'):
                    runner['main']()
            self.assertTrue((output / 'display-guard.json').exists())
            result = json.loads((output / 'display-guard.json').read_text())
            self.assertTrue(result['aborted'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
