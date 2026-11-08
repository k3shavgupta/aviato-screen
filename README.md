# Aviato Screen

A native, low-power C dashboard writing directly to the Linux framebuffer. The
monitor keeps scanning the framebuffer while the application sleeps; no browser,
desktop compositor, or continuous animation loop is required.

## Build and verify

```sh
make
make test
make sanitize
make preview
```

Requires gcc, make, the existing Liberation Sans font, and libcurl's runtime
library. `CURL_LIB` can override the library location on another machine. Tests
use the real parser and renderer with an offline clock/HTTP transport. They check
refresh deadlines, retry backoff and recovery, retained values, bounded HTTP
responses, settings, and exact pixel equality of incremental and full frames.

Previews are BMP files under `build/`. `SCREEN_OFFLINE=1` suppresses network calls
only when `SCREEN_PREVIEW` is set. Offline previews show unavailable market and
weather data; tests exercise populated data too. Neither tests nor previews
touch `/dev/fb0`.

## Low-power settings

| Environment variable | Default and minimum | Maximum |
| --- | --- | --- |
| `SCREEN_CRYPTO_INTERVAL` | 60 seconds | 86400 seconds |
| `SCREEN_METALS_INTERVAL` | 300 seconds | 86400 seconds |
| `SCREEN_WEATHER_INTERVAL` | 900 seconds | 86400 seconds |
| `SCREEN_SYSTEM_INTERVAL` | 60 seconds | 86400 seconds |

Invalid values fall back to defaults. Failure doubles that source's retry delay,
up to an hour (or the configured interval when longer); success restores the
normal cadence. Last successfully fetched values remain visible during errors.
They are cached in memory and may be stale; no freshness indicator has been added.
The 00:00–07:00 IST clock theme pauses market polling and system sampling.

System values are sampled once per interval and the same values are compared
and drawn. Only changed clock, weather, market, or system regions repaint.
Startup, theme changes, and quote-layout changes paint the complete screen.
HTTP responses are capped at 256 KiB and HTTP errors are rejected. Weather uses
a persistent curl handle without shell launches or weather.json writes.

The clock uses IST and updates by the minute. Fetches remain synchronous with an
8-second timeout per request. Slow providers or multiple failed metal fallbacks
can delay a clock update; deadlines are recalculated after requests finish.

## Install and rollback

The active service runs as `keshav` with `Restart=always`. Save its PID before
replacing the executable. Build and test first, then atomically replace it:

```sh
make test all
screen_pid=$(systemctl show aviato-screen --property=MainPID --value)
test "$screen_pid" -gt 1
test "$(readlink /proc/"$screen_pid"/exe)" = /home/keshav/aviato-screen/screen
install -m 755 build/screen screen.new
mv -f screen.new screen
kill -TERM "$screen_pid"
```

Systemd restarts the display process after its configured delay. This does not
restart the machine or other server services. Check `systemctl status
aviato-screen` and `journalctl -u aviato-screen -n 20 --no-pager`; confirm a new
PID, framebuffer output, and paint messages. To restore the original, perform the
same atomic replacement using `backups/2026-10-26-low-power/screen` and terminate
only the verified Aviato PID. The backup also contains the original C source.

## Resource baseline

Before these changes, a 15-second sample measured 0.33% of one CPU core and
17,204 KiB resident memory. The CPU package sensor read approximately 47°C.
Those are short samples, not proof of heat caused by Aviato. Systemd's charged
memory differs from resident memory including shared libraries.

On successful daytime refreshes, primary market requests fall from approximately
960/hour to 84/hour (91.25% fewer), excluding weather, fallbacks, and failures.
Measure CPU with `pidstat -u -r -p <PID> 5 3`; compare longer samples under similar
server load for stronger conclusions. No zero-heat or fixed-temperature promise
is made.

## TV and portfolio extension

Native display handover and a cached Kite summary are implemented. A user
collector service is installed; without local credentials it reports
`setup_required` and makes no provider requests. The display reads its summary
once per system interval. Holdings, available cash, and positions P&L are shown
separately in INR, with a last-successful-update time and stale/login states.
Holdings are gross asset value, including funded MTF units, not net worth.
Balances are omitted from the overnight clock and all shared Kodi frames.
`SCREEN_SHOW_PORTFOLIO=0` hides the native line; `SCREEN_PORTFOLIO_PATH` can select
a different sanitized cache. Previews never read account data.

Kite setup from a terminal on this machine:

```sh
cd /home/keshav/aviato-screen
python3 -m aviato_portfolio configure
python3 -m aviato_portfolio login
python3 -m aviato_portfolio status
```

Enter the developer app's API key and API secret at the hidden local prompts.
Set its Redirect URL to `http://127.0.0.1:8766/kite/callback`, then open the
printed official login URL in a browser on this machine. For a browser on
another computer, forward its local port 8766 to the server with SSH before
logging in. Tokens normally expire at 06:00 IST; repeat the official login
when requested. No account access has been tested with real credentials.

The collector reads holdings, net positions and equity cash every 120 seconds,
with backoff and last-good retention. It supports no order endpoints. Its
configuration is in `~/.config/aviato-screen/kite.json` and sanitized cache in
`~/.local/state/aviato-screen/portfolio.json`, both private to the Linux user.
To stop it: `systemctl --user disable --now aviato-portfolio.service`.

See [media setup](media/README.md) for the prepared Kodi install command,
screensaver ZIP, phone control and Bluetooth pairing. Kodi is not installed;
playback, phone control and Bluetooth audio are unverified. Authenticated
Binance account balances are not implemented; public Binance prices remain
separate from an account portfolio.

See [the security and resource review](docs/security-and-resource-review-2026-10-26.md)
for the earlier resource measurements and credential safeguards. Its native
handover status predates this implementation; see the updated
[TV integration verification](docs/tv-verification-2026-10-26.md).

## Ambient clock and local agent display

Installed 5 November 2026 for the connected 1920×1080 monitor. The original profile was `minimal`, an alias for `focus`. The current profile
in `~/.config/aviato-screen/profile` is `adaptive`.
`SCREEN_PROFILE=legacy`, `focus`, `album`, `adaptive`, or `ambient` overrides it when
starting the executable.

Ambient mode draws a large IST clock, a geometric riverside scene with four
daily palettes, CPU/memory readings and a Codex status area. The clock redraws
once per minute. Lifecycle hooks wake the renderer when local status changes;
system readings refresh every minute. Only changed regions repaint. Ambient mode skips weather, ETH, metals and
portfolio collectors inside the renderer. It polls only BTC/USDT from Binance
once per minute, with a two-second timeout and a 2 KiB response cap. Failures
retain the previous price, visibly mark it stale and back off retries. The
existing local quote collection rotates every ten minutes with no requests. Other server services run separately.

Codex status comes from the private `~/.cache/aviato-screen/agents.json` cache.
Its expiry prevents old events from appearing current. Shared media frames hide
agent status. Previews never read the live agent cache. See
[agent integration](docs/agent-status.md) for hooks, trust, verification and recovery.

Build and check with `make all test sanitize`. For an offline ambient preview,
run `SCREEN_PROFILE=ambient SCREEN_OFFLINE=1 SCREEN_FORCE_HOUR=18
SCREEN_PREVIEW=build/ambient.bmp ./build/screen` on one shell line. Physical
layout is tuned to the current 1080p output. Smaller framebuffers are clipped
safely but need a layout adjustment for full readability.

## Focus screen, quote card, and profiles

The `adaptive` profile, activated 8 November 2026, changes with the room:

- During daytime playback, a 640×640 album cover sits beside the title, artist,
  and album. Time and date stay compact, with weather and Bitcoin in the footer.
- While idle, a large clock and a quote share the screen. Quotes rotate every
  ten minutes. There is no empty music card or machine/agent status row.
- From 23:00 to 08:00 IST, a dim clock and date appear on black. Night takes
  priority over playback; the existing overnight network polling pause remains.

Missing artwork uses a vinyl placeholder while bounded retries continue.
Failed Spotify polls retain the previous track with “Last known playback”; an
idle outage is labeled “Spotify unavailable”. Weather and Bitcoin retain their
last known values and indicate stale data. Artwork is decoded once at 640px;
older profiles draw it at their original 260px size. Only changed screen regions
repaint, and every layout transition clears the previous composition.

To preview the idle or night layout without touching the display, run
`SCREEN_PROFILE=adaptive SCREEN_OFFLINE=1 SCREEN_FORCE_HOUR=14
SCREEN_PREVIEW=build/adaptive-idle.bmp ./build/screen` on one shell line; use
`SCREEN_FORCE_HOUR=23` for night. Add `SCREEN_SPOTIFY_JPEG=/path/to/cover.jpg`
for a daytime artwork preview. Populated offline visual fixtures are available
through `make preview-adaptive COVER=/path/to/cover.jpg`.

The pre-adaptive executable, profile, and changed source files are preserved in
`backups/2026-11-08-adaptive-before/`. To revert only the layout, set the profile
file back to `minimal` and restart the display using the procedure above.

The `focus` profile keeps the large clock and top weather/Bitcoin row, then uses
the open area beside Spotify for a locally stored quote. Quotes rotate at
10-minute boundaries in Asia/Kolkata. `minimal` remains an alias for `focus`.
The `album` profile uses the same quiet layout and takes its accent color from a
single downsampled sample of the current cover. `ambient` keeps the geometric
clock scene and shows the small face only while Codex or T3/Grok is working,
waiting, done, or in error. `legacy` keeps the earlier day/night dashboard.

Set `adaptive`, `focus`, `album`, `ambient`, or `legacy` in
`~/.config/aviato-screen/profile`, then restart only `aviato-screen` to switch.
For example, run `printf 'album\n' > ~/.config/aviato-screen/profile` and
`sudo systemctl restart aviato-screen`. `SCREEN_PROFILE` overrides the saved
profile for a one-off run or preview. Quotes are local, not fetched from X. The
catalog keeps each author and a source label beside the short text.

Spotify status still polls every 20 seconds while music is playing and every
45 seconds while idle so playback can be noticed without keeping a network
request running. Artwork is decoded once per changed cover, supports common
JPEG and PNG/BMP/TGA formats, and is cached privately at
`~/.cache/aviato-screen/spotify-cover.bin`. A failed download or decode retries
with bounded backoff; the vinyl mark stays visible while art is missing.

The existing lifecycle hook adapter now signals the renderer after updating
its private status file. The renderer redraws only the affected region; it does
not record prompts, keystrokes, or microphone activity. Typing and speaking
states need support from the client app and are not inferred from these hooks.

The original executable, source, Makefile, test and README are saved under
`backups/2026-11-05-ambient/`. To restore the previous display, copy that saved
`screen` to `screen.new`, atomically rename it to `screen`, remove the ambient
profile (or restore the backed-up profile if present), and terminate only the
PID whose executable is `/home/keshav/aviato-screen/screen`. The system service
restarts it automatically. Hook removal is covered in the agent integration doc.

A 60-second sample after deployment measured 0.03% of one CPU core and
9,528 KiB resident memory. These short measurements do not establish the
monitor's power draw or long-term thermal effect. Screen power scheduling is enabled as described below.

With BTC and quotes enabled, a three-minute live sample measured 0.03% of one
CPU core and 15,876 KiB resident memory. Live framebuffer inspection confirmed
quote rotation at 15:45 IST and BTC price/time updates across successive minutes.
The sensor read 48°C during observation and 38°C at the final check. Other
workloads and cooling affect those readings. The quote/price implementation
passed 279 exact full-versus-incremental frame comparisons and sanitizer checks.


## Overnight monitor schedule

The `adaptive` profile keeps the monitor awake overnight to show its dim clock.
Other profiles sleep from 23:00 until 08:00 Asia/Kolkata every day. The enabled
user timer `aviato-display-power.timer` checks once per minute and after the
user manager starts. It only calls framebuffer power control when the state
needs changing. User lingering is enabled, so the schedule runs after reboot
without an interactive login. A restarted display or manual wake during the
sleep period is corrected within one minute for those other profiles. The timer
reads the saved profile on each check, so switching back to `minimal` restores
the previous overnight power-off behavior.

`display-power.py` uses the framebuffer VESA powerdown command. The live test
confirmed connector DPMS Off and then On, and both time boundaries passed.
To disable the schedule, run
`systemctl --user disable --now aviato-display-power.timer`. This stops future
changes and leaves the current monitor state intact.

After wake, the power controller waits one second for the text console restore,
then signals the verified native display PID to repaint the full frame. This
prevents the console overwriting static artwork while only changing regions
reappear. A live sleep/wake test confirmed the complete dashboard was restored.
