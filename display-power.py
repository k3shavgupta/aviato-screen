#!/usr/bin/env python3
"""Keep the adaptive clock visible; let other profiles sleep overnight."""
from datetime import datetime
import fcntl
import os
import signal
import subprocess
import time
from pathlib import Path
from zoneinfo import ZoneInfo


def current_profile():
    try:
        with (Path.home() / ".config/aviato-screen/profile").open() as profile:
            return profile.readline(32).rstrip("\r\n")
    except OSError:
        return "minimal"


def display_should_be_on(now, profile="minimal"):
    return profile == "adaptive" or 8 <= now.astimezone(ZoneInfo("Asia/Kolkata")).hour < 23


def main():
    desired_on = display_should_be_on(datetime.now(ZoneInfo("Asia/Kolkata")), current_profile())
    blank = Path("/sys/class/graphics/fb0/blank")
    current_on = int(blank.read_text().strip()) == 0
    if current_on == desired_on:
        return
    descriptor = os.open("/dev/fb0", os.O_RDWR | os.O_CLOEXEC)
    try:
        # FBIOBLANK takes an integer, 0 to wake or 4 for VESA powerdown.
        fcntl.ioctl(descriptor, 0x4611, 0 if desired_on else 4)
    finally:
        os.close(descriptor)
    if (int(blank.read_text().strip()) == 0) != desired_on:
        raise RuntimeError("Display power state did not change")
    if desired_on:
        # fbcon restores the text console on wake, overwriting the dashboard.
        # Request a complete frame after that restoration has settled.
        time.sleep(1)
        pid = int(subprocess.check_output(
            ["systemctl", "show", "aviato-screen", "-p", "MainPID", "--value"], text=True))
        if pid > 0 and os.readlink(f"/proc/{pid}/exe") == "/home/keshav/aviato-screen/screen":
            os.kill(pid, signal.SIGUSR1)
    print("Display on" if desired_on else "Display off")


if __name__ == "__main__":
    main()
