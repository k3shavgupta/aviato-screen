# Aviato security and resource review — 26 October 2026

Historical review, before the TV integration was installed. Native handover and
summary rendering have since been implemented and tested; the user collector is
installed in setup-required mode. See [current verification](tv-verification-2026-10-26.md).

## Running state and measurements

The native `aviato-screen` service is active as `keshav`. Kodi is not installed,
the portfolio user service is not installed/active, and the known Aviato Kite
configuration, portfolio cache, and media runtime directory were absent when
checked. No credentials were requested or read, and no authenticated provider
requests were made during this review. Binance is used only for public ticker
prices in the current C renderer; an authenticated Binance collector does not
exist in this application.

`pidstat -u -r -p 111143 10 6` measured 0.03% of one CPU core over 60 seconds,
from approximately 20:38:38–20:39:38 IST. RSS remained 17,252 KiB. An earlier
15-second sample measured 0.07%. A separate initial CPU-package sensor reading
was 50°C. These are short observations on a running server, not an attribution
of temperature to Aviato or a guarantee of zero heat.

Native rendering already skips unchanged frames, refreshes the clock by the
minute, and sleeps between deadlines. The Kodi addon waits one second between
metadata checks and reloads an image only when its identity/mtime/size changes.
This loop does not establish Kodi's actual GUI/GPU power consumption.

## Repairs

- Credential/cache reads and writes now open each directory with `O_NOFOLLOW`
  and use directory descriptors for file operations. Replacing an ancestor with
  a symlink cannot redirect the final rename. Private directories are mode 700;
  files are created mode 600. Reads reject symlinks, hardlinked files, nonregular
  files, foreign owners, permissive modes, and oversized data. Writes remain
  atomic and sync both the file and its directory. Symlinked config trees are
  deliberately rejected; use a real private directory.
- Local credential entry refuses `getpass`'s echoing fallback when hidden
  terminal input is unavailable. Credentials belong in the local prompt, not
  chat, command arguments, source, or environment variables.
- The CLI disables process dumping with Linux `PR_SET_DUMPABLE(0)` before
  loading credentials and sets both core limits to zero. This host pipes dumps
  to Apport, which can ignore `RLIMIT_CORE` alone. The prepared service units
  also set `LimitCORE=0`; the media unit sets a private umask and
  `NoNewPrivileges=true`. These unit changes are artifacts, not installed units.
- Media startup now fails unless the verified native renderer catches SIGUSR1
  and has the same private display lock open. The current native binary does
  not satisfy this contract, so Kodi startup is refused. Wake-up signals are
  never sent to a renderer without its handler, avoiding termination of the old
  binary. This check is a readiness guard, not a completed handover protocol.

Existing protections reviewed: Kite requests use a fixed HTTPS provider host
with certificate verification, no redirects, a read endpoint allowlist, bounded
responses/timeouts, and retry backoff. The sole permitted POST is session
creation. Login callbacks bind to loopback, validate random state, disable
request logging, and send no-store/no-referrer headers. Published summaries
allowlist fields and omit credentials and account identifiers. No app phone
remote listener is installed. An assignment-pattern scan of 26 app text files
found no potential literal credentials outside dummy test values; this is not
a comprehensive secrets scanner or an audit of unrelated server applications.

## Verification and boundaries

Regression tests reproduced unsafe parent-symlink writes, hardlinked credential
reads, echoing input fallback, dumpable credential processes, media startup
without handover, and unsafe SIGUSR1 delivery before the fixes. The Python
suite covers these plus the original authentication/calculation and media-loop
behavior. The C suite includes refresh/backoff tests and 104 incremental frames
compared exactly with full paints. Run:

```sh
make all test sanitize
python3 -m unittest discover -s tests -p 'test_*.py' -v
systemd-analyze --user verify aviato_portfolio/aviato-portfolio.service
systemd-analyze verify media/aviato-media.service
python3 -m compileall -q aviato_portfolio media/launcher.py media/screensaver.aviato
bash -n media/setup-admin.sh
python3 media/build-addon.py
```

The unit verifier emitted an unrelated existing warning about
`spice-vdagent.service`'s `[Install]` section. Neither unit was installed or
started. Original app files are preserved in
`backups/2026-10-26-security/`. The running native binary and unrelated services
were not replaced or restarted.

Before enabling Kodi, complete the native framebuffer lock/offscreen-frame
protocol and verify crash recovery and real idle/music/video CPU/GPU load on
this hardware. Keep the native display as the idle default until those pass.
Kodi documents its changed-region rendering options in its
[official settings reference](https://kodi.wiki/view/Advancedsettings.xml).

Separate Linux accounts would provide an additional boundary between Kodi and
the collector's secret files. Mode 600 protects against other users; it does not protect against
Kodi addons or other processes running as `keshav`. Root/host compromise is also
outside this protection. Only a sanitized summary should cross to the renderer;
shared frames must omit balances as well as credentials. That account isolation
remains unimplemented; private-summary rendering has since been added.

An eventual Binance account integration needs a separate reading-only key with
trading/transfers/withdrawals disabled and trusted-IP restrictions. These account
permissions cannot be checked here because no Binance account integration or
credentials were configured. See Binance's
[API-key guide](https://www.binance.com/en/support/faq/detail/360002502072).

This is an application review, not a guarantee that secrets cannot leak or a
review of every listener, backup, container, or program on the server.
