# TV integration verification — 26 October 2026

Implemented and installed: native framebuffer handover, RAM-backed shared BMP
frames with private portfolio content omitted, validated local portfolio summary
rendering, and the Python user collector. No real credentials were entered or
authenticated account calls performed. Collector state is `setup_required`.

Checks completed before installation:

- `make all test`: strict build, cache validation/freshness/privacy, exclusive
  media/shared native lock cooperation, atomic publication, unchanged-frame
  suppression, memory release and native handback. 110 incremental frames
  exactly match full paints, including growing/shrinking portfolio values.
- `make sanitize`: AddressSanitizer/UndefinedBehaviorSanitizer and leak checking
  pass for the native test suite.
- `python3 -m unittest discover -s tests`: 32 tests pass. Includes real HTTP idle
  callback timeout, synthetic authentication, quantity calculations, stale/error
  behavior, finite cache/expiry validation, private storage, and real subprocess
  wrapper/descendant lock lifetime.
- Day/evening/overnight offline previews generated successfully.
- Both systemd units validate; host's existing `spice-vdagent` unit emits an
  unrelated warning. Media setup shell syntax and addon ZIP build pass.
- Independent source review found no remaining concrete native integration
  blocker; real DRM/VT/video/audio operation remains unverified.

Installation replaces only the native `screen` executable and restarts its own
system service, then installs/enables the user `aviato-portfolio` service.
The previous native binary is in `backups/2026-10-26-tv-portfolio/screen`.

Post-installation checks: native service active with PID 148342 and collector
active with PID 148332. Runtime directory is mode 700; display lock and native
PID file are mode 600. A live simulated media owner acquired the exclusive lock,
woke the real renderer, and received a complete 6,220,854-byte 1920×1080 BMP.
Releasing ownership restored the same native process and removed the shared
frame. This verifies handover on the running host without claiming Kodi/DRM
playback. Collector reports `setup_required`, last update 0.

A 30-second `pidstat` sample at 22:01:49–22:02:19 IST measured native average
CPU at 0.03% of one core and RSS 18,048 KiB. The unconfigured collector measured
0.00% CPU at the tool's precision and RSS 26,584 KiB. These are short idle
observations, not Kodi playback or authenticated polling measurements.

Still pending: administrator Kodi package/session installation, real playback
and resource measurements, speaker selection/pairing, phone remote setup, and
Kite secret entry plus official login. Binance authenticated account collection
is not implemented. The media setup script does not start or enable Kodi.
Credentials and Kodi share a Linux user; separate-account isolation is absent.
No claim of zero heat or Kodi playback efficiency is supported by these tests.
