"""Optional X11 display lease for visible GPU sampling; no persistent settings."""
import subprocess


class DisplayGuard:
    def __init__(self):
        self.bus = None
        self.leases = []
        self.checks = 0
        self.description = dict(mode='X11 monitor-on + unlocked session; temporary D-Bus inhibition',
                                persistent_settings_changed=False, released=False)

    def __enter__(self):
        try:
            import dbus  # Optional distribution package, needed only for this flag.
            self.bus = dbus.SessionBus()
            self.saver = dbus.Interface(self.bus.get_object(
                'org.freedesktop.ScreenSaver', '/ScreenSaver'), 'org.freedesktop.ScreenSaver')
            if bool(self.saver.GetActive(timeout=5)):
                raise RuntimeError('X11 desktop is locked: unlock it before visible GUI sampling')
            interfaces = [self.saver, dbus.Interface(self.bus.get_object(
                'org.freedesktop.PowerManagement.Inhibit', '/org/freedesktop/PowerManagement/Inhibit'),
                'org.freedesktop.PowerManagement.Inhibit')]
            for interface in interfaces:
                cookie = interface.Inhibit('NovaTerm GUI benchmark',
                    'Visible GPU sampling requires an active display', timeout=5)
                self.leases.append((interface, cookie))
            subprocess.run(['xset', 'dpms', 'force', 'on'], check=True, capture_output=True, timeout=5)
            self.check()
            return self
        except BaseException as error:
            self.__exit__(type(error), error, error.__traceback__)
            raise

    def check(self):
        if bool(self.saver.GetActive(timeout=5)):
            raise RuntimeError('X11 desktop locked during sampling; preserve and reject this run')
        state = subprocess.check_output(['xset', 'q'], text=True, timeout=5)
        if 'Monitor is On' not in state:
            raise RuntimeError('X11 monitor is not On; preserve and reject this run')
        self.checks += 1

    def __exit__(self, exc_type, exc_value, traceback):
        errors = []
        for interface, cookie in reversed(self.leases):
            try:
                interface.UnInhibit(cookie, timeout=5)
            except Exception as error:
                errors.append(str(error))
        self.leases.clear()
        if self.bus is not None:
            self.bus.close()  # Connection loss also revokes any remaining lease.
            self.bus = None
        self.description.update(released=True, checks=self.checks, release_errors=errors,
                                aborted=exc_type is not None,
                                error=str(exc_value) if exc_value is not None else None)
        return False


def run_visible(command, log, guard, timeout=180):
    """Check once per second; abort a child if visibility/power state changes."""
    import time
    guard.check()
    deadline = time.monotonic() + timeout
    with subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT) as process:
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(command, timeout)
                try:
                    code = process.wait(timeout=min(1, remaining))
                except subprocess.TimeoutExpired:
                    guard.check()
                    continue
                guard.check()
                return code
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
