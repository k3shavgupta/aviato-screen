#!/usr/bin/env python3
"""Serialize Kodi's display ownership with the native framebuffer renderer."""
import ctypes
import fcntl
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys

NATIVE_BINARY = Path(__file__).resolve().parents[1] / 'screen'
RUNTIME = Path('/run/user') / str(os.getuid()) / 'aviato-screen'


def cooperating_pid(runtime):
    try:
        with (runtime / 'native.pid').open() as file:
            pid = int(file.read(32).strip())
        if pid <= 1:
            return
        proc = Path('/proc') / str(pid)
        if proc.stat().st_uid != os.getuid():
            return
        if (proc / 'exe').resolve(strict=True) != NATIVE_BINARY.resolve(strict=True):
            return
        # Old Aviato binaries have no handler: SIGUSR1 would terminate them.
        for line in (proc / 'status').read_text().splitlines():
            if line.startswith('SigCgt:'):
                if int(line.split()[1], 16) & (1 << (signal.SIGUSR1 - 1)):
                    return pid
                return
    except (OSError, ValueError):
        pass


def wake_native(runtime):
    pid = cooperating_pid(runtime)
    if pid is not None:
        try: os.kill(pid, signal.SIGUSR1)
        except ProcessLookupError: pass


def native_ready(runtime):
    """Require the verified renderer to participate in this display lock."""
    pid = cooperating_pid(runtime)
    if pid is None:
        return False
    try:
        directory = runtime.lstat()
        lock = (runtime / 'display.lock').lstat()
        if (not stat.S_ISDIR(directory.st_mode) or directory.st_uid != os.getuid() or
                directory.st_mode & 0o077 or not stat.S_ISREG(lock.st_mode) or
                lock.st_uid != os.getuid() or lock.st_mode & 0o077 or lock.st_nlink != 1):
            return False
        for descriptor in (Path('/proc') / str(pid) / 'fd').iterdir():
            try: opened = descriptor.stat()
            except FileNotFoundError: continue
            if (opened.st_dev, opened.st_ino) == (lock.st_dev, lock.st_ino):
                return True
    except OSError:
        pass
    return False


def run_player(command, runtime, runner=subprocess.call):
    runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
    with open(runtime / 'display.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        wake_native(runtime)
        try:
            return runner(command)
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
            wake_native(runtime)


def supervise_player(command):
    """Reap the wrapper AND its orphaned descendants before display handback.

    Kodi's shell wrapper may exit its interrupted wait before kodi.bin exits.
    Linux subreaper adoption keeps those children waitable without polling /proc.
    A dedicated process group also forwards manual termination to the player.
    """
    libc = ctypes.CDLL(None, use_errno=True)
    previous_subreaper = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous_subreaper), 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'PR_GET_CHILD_SUBREAPER failed')
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'PR_SET_CHILD_SUBREAPER failed')
    child = None
    pending = []
    previous_handlers = {}
    def stop(signum, frame):
        if child is None:
            pending.append(signum)
        else:
            try:
                os.killpg(child.pid, signum)
            except ProcessLookupError:
                pass
    try:
        for signum in (signal.SIGTERM, signal.SIGINT):
            previous_handlers[signum] = signal.signal(signum, stop)
        child = subprocess.Popen(command, start_new_session=True)
        for signum in pending:
            stop(signum, None)
        result = None
        while True:
            try:
                pid, status = os.waitpid(-1, 0)
            except InterruptedError:
                continue
            except ChildProcessError:
                break
            if pid == child.pid:
                result = os.waitstatus_to_exitcode(status)
                child.returncode = result
        return result if result is not None else 1
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        libc.prctl(36, previous_subreaper.value, 0, 0, 0)


def main():
    runtime = RUNTIME
    if not native_ready(runtime):
        print('Media startup refused: native display handover is not ready.', file=sys.stderr)
        return 1
    return run_player(['/usr/bin/kodi', '--standalone', '--windowing=gbm'],
                      runtime, supervise_player)

if __name__ == '__main__':
    raise SystemExit(main())
