import fcntl
import importlib.util
import os
from pathlib import Path
import tempfile
import runpy
import signal
import subprocess
import time
import sys
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj

class MediaTests(unittest.TestCase):
    def launcher(self):
        path = ROOT / 'media/launcher.py'
        self.assertTrue(path.exists(), 'media launcher is not implemented')
        return module('launcher', path)

    def test_lock_held_during_player_and_released_on_failure(self):
        mod = self.launcher()
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            def player(command):
                with open(runtime / 'display.lock', 'a') as lock:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
                raise FileNotFoundError('missing Kodi')
            with patch.object(mod, 'wake_native') as wake:
                with self.assertRaises(FileNotFoundError):
                    mod.run_player(['missing-kodi'], runtime, player)
                self.assertEqual(wake.call_count, 2)
            with open(runtime / 'display.lock', 'a') as lock:
                fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)

    def test_lock_survives_wrapper_exit_until_delayed_descendant_finishes(self):
        mod = self.launcher()
        self.assertTrue(hasattr(mod, 'supervise_player'), 'player needs descendant supervision')
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            wrapper = runtime / 'wrapper.py'
            wrapper.write_text("""import os, signal, time
from pathlib import Path
root = Path(__file__).parent
if os.fork() == 0:
    def stop(signum, frame):
        time.sleep(0.7)
        (root / 'done').touch()
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop)
    (root / 'ready').touch()
    while True: signal.pause()
signal.signal(signal.SIGTERM, lambda *args: os._exit(0))
while True: signal.pause()
""")
            code = ("import sys; from pathlib import Path; "
                    "sys.path.insert(0, sys.argv[1]); import launcher; "
                    "launcher.run_player([sys.executable, sys.argv[3]], Path(sys.argv[2]), launcher.supervise_player)")
            process = subprocess.Popen([sys.executable, '-c', code, str(ROOT / 'media'), str(runtime), str(wrapper)])
            try:
                deadline = time.monotonic() + 5
                while not (runtime / 'ready').exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue((runtime / 'ready').exists(), 'fake Kodi did not become ready')
                process.send_signal(signal.SIGTERM)
                time.sleep(0.15)
                self.assertIsNone(process.poll(), 'launcher exited before its descendant')
                with open(runtime / 'display.lock', 'a') as lock:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertTrue((runtime / 'done').exists())
                with open(runtime / 'display.lock', 'a') as lock:
                    fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)

    def test_unverified_pid_never_signalled(self):
        mod = self.launcher()
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            (runtime / 'native.pid').write_text(str(os.getpid()))
            with patch.object(mod.os, 'kill') as kill:
                mod.wake_native(runtime)
                kill.assert_not_called()

    def test_verified_pid_signalled(self):
        mod = self.launcher()
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            (runtime / 'native.pid').write_text(str(os.getpid()))
            previous = signal.signal(signal.SIGUSR1, lambda *_: None)
            try:
                with patch.object(mod, 'NATIVE_BINARY', Path('/proc/self/exe').resolve()), patch.object(mod.os, 'kill') as kill:
                    mod.wake_native(runtime)
                    kill.assert_called_once_with(os.getpid(), mod.signal.SIGUSR1)
            finally:
                signal.signal(signal.SIGUSR1, previous)

    def test_native_without_handover_signal_handler_never_signalled(self):
        mod = self.launcher()
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            (runtime / 'native.pid').write_text(str(os.getpid()))
            previous = signal.signal(signal.SIGUSR1, signal.SIG_DFL)
            try:
                with patch.object(mod, 'NATIVE_BINARY', Path('/proc/self/exe').resolve()), patch.object(mod.os, 'kill') as kill:
                    mod.wake_native(runtime)
                    kill.assert_not_called()
            finally:
                signal.signal(signal.SIGUSR1, previous)

    def test_main_refuses_player_until_native_handover_is_ready(self):
        mod = self.launcher()
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(mod, 'RUNTIME', Path(directory)), patch.object(mod, 'run_player') as player:
                self.assertEqual(mod.main(), 1)
                player.assert_not_called()

    def test_native_readiness_requires_renderer_to_open_the_same_lock(self):
        mod = self.launcher()
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            (runtime / 'native.pid').write_text(str(os.getpid()))
            (runtime / 'display.lock').touch(mode=0o600)
            previous = signal.signal(signal.SIGUSR1, lambda *_: None)
            try:
                with patch.object(mod, 'NATIVE_BINARY', Path('/proc/self/exe').resolve()):
                    self.assertFalse(mod.native_ready(runtime))
                    with open(runtime / 'display.lock', 'r'):
                        self.assertTrue(mod.native_ready(runtime))
            finally:
                signal.signal(signal.SIGUSR1, previous)

    def test_saver_refreshes_changes_and_exits_on_deactivation(self):
        path = ROOT / 'media/screensaver.aviato/resources/lib/saver.py'
        self.assertTrue(path.exists(), 'screensaver not implemented')
        saver = module('saver', path)
        class Image:
            def __init__(self): self.paths = []
            def setImage(self, path, useCache): self.paths.append((path, useCache))
        class Monitor:
            def abortRequested(self): return False
            def waitForAbort(self, seconds):
                self.calls += 1
                if self.calls == 1: self.path.write_bytes(b'changed')
                if self.calls == 3: self.active = False
                return False
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'screensaver.bmp'
            path.write_bytes(b'first')
            monitor = Monitor(); monitor.calls = 0; monitor.active = True; monitor.path = path
            image = Image()
            saver.display_loop(monitor, image, path, lambda: monitor.active)
            self.assertEqual(len(image.paths), 2)
            self.assertTrue(all(cache is False for _, cache in image.paths))
            self.assertEqual(monitor.calls, 3)

class KodiEntryTests(unittest.TestCase):
    def test_entry_dismisses_input_and_deactivation_without_player(self):
        events = []
        class Window:
            def getWidth(self): return 1920
            def getHeight(self): return 1080
            def addControl(self, image): events.append(('image', image.path))
            def show(self): pass
            def close(self): events.append(('close',))
        class Image:
            def __init__(self, x, y, w, h, path): self.path = path
        def loop(monitor, image, path, active):
            self.assertTrue(active())
            namespace = active.__globals__
            namespace['window'].onAction(object())
            self.assertFalse(active())
            namespace['window'].active = True
            monitor.onScreensaverDeactivated()
            self.assertFalse(active())
        api = types.SimpleNamespace(WindowDialog=Window, ControlImage=Image)
        monitor_api = types.SimpleNamespace(Monitor=type('Monitor', (), {}))
        fake_loop = types.SimpleNamespace(display_loop=loop)
        with patch.dict(sys.modules, xbmc=monitor_api, xbmcgui=api, saver=fake_loop):
            runpy.run_path(str(ROOT / 'media/screensaver.aviato/default.py'))
        backgrounds = [event[1] for event in events if event[0] == 'image']
        self.assertTrue(any(path.endswith('black.png') for path in backgrounds), 'saver needs opaque backing')
        self.assertGreaterEqual(events.count(('close',)), 3)

if __name__ == '__main__': unittest.main()
