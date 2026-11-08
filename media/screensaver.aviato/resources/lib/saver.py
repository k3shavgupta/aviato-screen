"""Kodi-independent refresh loop, intentionally leaves playback untouched."""
def display_loop(monitor, image, path, active):
    previous = None
    while active() and not monitor.abortRequested():
        try:
            stat = path.stat()
            current = (stat.st_ino, stat.st_mtime_ns, stat.st_size)
            if current != previous:
                image.setImage(str(path), useCache=False)
                previous = current
        except OSError:
            pass
        if monitor.waitForAbort(1.0):
            break
