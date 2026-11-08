# Low-power release verification

Installed on 2026-10-26 at approximately 19:34 IST.

## Checks

- The original regression failed because crypto was fetched again after six
  seconds. It now passes with the default 60-second interval.
- `make test` passes refresh, failure backoff/recovery, last-good-value retention,
  malformed/truncated/null weather, array-length checks, response size/overflow
  bounds, settings, deadline selection, and minute-boundary catch-up tests.
- All 104 incremental frames match the real full-frame renderer byte for byte,
  including day/night transitions and changed/unavailable data.
- `make all` passes with `-O2 -Wall -Wextra -Werror`.
- `make sanitize` passes with AddressSanitizer, UndefinedBehaviorSanitizer, and
  leak detection enabled.
- `make preview` produces all three offline themes. A separate live-data preview
  successfully fetched weather and all four market prices.
- An independent review identified weather parsing and minute-boundary issues;
  both were fixed and re-reviewed with no further material findings.
- An offline preview traced with strace showed no network calls, child-process
  creation, weather.json access, or framebuffer access.

## Live service

Only the Aviato process was restarted. Systemd launched PID 111143, confirmed
framebuffer output at 1920 x 1080, and logged successful full and partial paints.
A capture of the actual framebuffer was visually inspected. The original source
and executable are saved under `backups/2026-10-26-low-power/`.

The installed and built executable SHA-256 values match:

`8e30a4cd130c008f10870e40ce00dd60ddc8ae31b647f45cbc27cef4ed878e72`

## Measurements

The post-install `pidstat` sample covered 75 seconds (19:35:00–19:36:15 IST).
Average process CPU use was **0.03% of one core**, with resident memory
**17,252 KiB** (approximately 16.8 MiB). The earlier 15-second baseline was
0.33% CPU and 17,204 KiB resident memory. These are short, unequal-duration
samples under changing server load, so they establish low observed overhead,
not a controlled percentage improvement or a temperature reduction.

During the post-install observation, weather.json retained its pre-install
modification time and size. Successful daytime primary market requests are
scheduled at approximately 84/hour, compared with 960/hour previously; the
cadence is covered by controlled-clock tests. Fallbacks and failures vary.

Raw samples: `build/live-resource-sample.txt`. Display capture:
`build/active-display.png`.

## Remaining scope

The media/phone/Bluetooth integration and private portfolio collector are
proposed in `docs/tv-and-portfolio-design.md`. Kodi is not installed, no speaker
was paired, and no private financial account was connected. Existing other
server services and system configuration were not changed.
