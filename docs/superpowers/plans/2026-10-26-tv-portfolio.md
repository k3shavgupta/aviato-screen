# TV media mode and Kite portfolio implementation plan

**Goal:** Add a private Kite Connect portfolio summary and prepare Kodi media mode with phone/mouse control and Aviato as its screensaver.

**Architecture:** A Python standard-library collector performs official Kite authentication and account reads, atomically publishing a bounded private summary. C reads the summary once a minute. A separate Kodi integration shares the native renderer through a framebuffer ownership lock and RAM-backed BMP frames.

**Current status:** Native integration and user collector are installed and
verified; 32 Python tests, 110 pixel comparisons and sanitizer checks pass.
The live renderer passed a simulated media handover/recovery. Kodi artifacts
are prepared; packages/playback/Bluetooth/phone control and real Kite account
authentication remain pending. Phone control uses Kodi's built-in authenticated
HTTP control while Kodi runs; a custom phone start/stop relay was omitted.
Binance account integration remains outside the implemented Kite collector.
See `docs/tv-verification-2026-10-26.md` for current evidence.

## Global constraints

- Preserve existing server workloads and low-power dashboard behavior.
- Do not store credentials, tokens, or real portfolio values in source, tests, logs, or documentation.
- No order, transfer, or withdrawal endpoints. Kite session creation is the only permitted POST to its API.
- Cash, holdings, and positions remain separate; do not label their sum as net worth.
- Shared Kodi screensaver frames hide private financial values.
- Work in the current ignored application folder; preserve backups and do not change the unrelated home Git repository.
- This session has no administrator installation access. Build/test all artifacts and provide a concrete scoped host setup command; do not claim Kodi playback is verified without running it.

## Task 1: Kite collector and authentication (root agent)

- [ ] Test account calculations, empty accounts, unavailable/nonfinite prices, separate positions, expiry, failure retention, retry scheduling, private file modes, and credential-safe errors.
- [ ] Implement package `aviato_portfolio`: default config `~/.config/aviato-screen/kite.json`, cache `~/.local/state/aviato-screen/portfolio.json`, commands `configure`, `login`, `collect`, `status`.
- [ ] Official login uses API key and app secret to exchange the request token; callback listens only on 127.0.0.1:8766, validates a random state, disables access logs, and redirects away from token query parameters. The registered app callback must match `http://127.0.0.1:8766/kite/callback` (SSH forwarding supported).
- [ ] Read holdings, positions, and equity margins at 120-second intervals; use bounded responses, 8-second timeouts, 1/second request pacing, exponential backoff, and no retries on an expired/invalid token until a new token is saved.
- [ ] Install a user systemd collector service. Without credentials it publishes a setup-required state and sleeps, without network activity.

Cache contract (schema 1): `status` is `setup_required`, `login_required`, `ok`, or `stale`; `updated_at` is last successful Unix time or 0; optional numeric `holdings_value`, `cash`, `positions_pnl`, `holdings_pnl`; `unpriced` counts missing-price holdings; `currency` is `INR`. A missing price means `holdings_value` is null rather than understating the total. Preserve last successful values/timestamp on errors. Do not include symbols, account IDs, or credentials in this summary.

## Task 2: Media integration (delegated implementation)

- [ ] Implement `media/` launcher, authenticated local phone controller/setup, and a Kodi Python screensaver add-on that displays the shared BMP and wakes on input. Use only standard library plus Kodi's provided Python APIs.
- [ ] Native renderer contract: `/run/user/<uid>/aviato-screen/display.lock`; native process takes a nonblocking shared flock around framebuffer writes. Media launcher holds an exclusive flock for Kodi's lifetime, waking only the verified Aviato renderer with SIGUSR1 on acquisition/release. Frame path is `/run/user/<uid>/aviato-screen/screensaver.bmp`, atomically replaced and regenerated only when displayed content changes. No authenticated account requests in the add-on.
- [ ] Prepare scoped administrator install/uninstall artifacts for Kodi dependencies, a session/TTY suitable for GBM, and media service. No arbitrary remote shell commands. Default control binding is loopback; optional phone access binds only a selected private interface and requires authentication and CSRF protection for mutations.
- [ ] Tests must cover lock release/recovery when Kodi fails, secret-free configuration, input wakeup, and remote request authorization. Kodi cannot be run here until system dependencies/session setup are installed.

## Task 3: Native renderer integration (root agent)

- [ ] Read private portfolio cache into a dedicated small component; reject malformed/oversized data and validate all numbers/timestamps. Default missing cache produces no balances.
- [ ] Add a short private native portfolio line above public markets with explicit INR and last-update/stale/login states. Pixel tests cover longer/shorter values and cache state changes. No private line in night clock or shared Kodi frames.
- [ ] Add framebuffer flock handover and RAM rendering/BMP output while media holds the display. Ensure exit/failure resumes native output with a full redraw. Sleep normally; SIGUSR1 interrupts sleep.

## Task 4: Review, deployment, and handoff

- [ ] Run Python/C tests, strict builds, sanitizers, add-on package validation, independent review, and fix material findings.
- [ ] Atomically install the native binary and user collector after checks. Preserve backups and verify actual service state. Do not activate uninstalled Kodi mode.
- [ ] Document exact local secret/login steps, the root-only setup command, missing speaker/binance inputs, verification evidence, and measured/untested limits.
