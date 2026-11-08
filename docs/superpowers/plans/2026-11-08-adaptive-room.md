# Adaptive Room Display Implementation Plan

**Goal:** Implement the user-selected playback, idle, and night layouts.

**Architecture:** Add an adaptive profile using existing minimal collectors.
Keep its renderer separate and compare region signatures for incremental paint.
Retain higher-resolution artwork while drawing old profiles at their old size.

**Tech Stack:** C, stb_truetype, stb_image, libcurl, Linux framebuffer.

## Constraints

1920×1080 output; 23:00–08:00 IST night priority; no new dependencies; no
animation or local status in adaptive mode; no changes to existing profiles.

## Tasks

- [x] Integrate `display-power.py` with the saved profile: keep adaptive awake
  overnight, preserving the existing sleep schedule for every other profile.
  Verify all hour boundaries and saved-profile lookup in `tests/test_display_power.py`.

- [x] Add regression tests in `tests/test-screen.c` for adaptive profile loading,
  byte-identical incremental transitions, large cover rendering, skipped hidden
  updates, long metadata, and clipped canvases. Run `make test` and observe the
  new profile assertion fail before implementation.
- [x] Add `PROFILE_ADAPTIVE` and persisted state in `aviato-screen.c`; recognize
  `adaptive` in `ambient-screen.h`; dispatch minimal rendering to the new
  `adaptive-screen.h`. Disable adaptive blink scheduling in `minimal-screen.h`.
- [x] Extend `spotify-screen.h` to retain 640px decoded artwork with a sized
  blitter, preserving 260px drawing for the old profiles.
- [x] Implement header/body/footer painters and signatures; clear all regions
  when idle/playback/night changes. Use full metadata in the body signature.
- [x] Run `make all test sanitize`. Render populated idle/playback/night fixtures
  with the real painter and inspect PNGs, including long titles and missing art.
- [x] Update README profile instructions. Activate through the documented atomic
  executable replacement; verify the systemd PID, logs, and framebuffer. Keep
  the pre-change executable and profile for rollback.
