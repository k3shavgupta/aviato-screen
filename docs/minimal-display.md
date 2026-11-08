# Aviato minimal display

The physical 1920×1080 monitor uses the existing native framebuffer service. The minimal profile shows the IST clock and date, a direct Coinbase BTC/USD spot quote, Kanpur weather, safe agent status or local daylight information, CPU temperature, load, memory and free disk space. It does not need a browser, desktop, web server, graphics animation or additional package.

## Refresh and power behavior

- Clock: once per minute; only changed screen regions repaint.
- BTC/USD: every 300 seconds, bounded two-second HTTP request; last good value remains with source, update time and a stale warning after failure.
- Kanpur weather: every 1800 seconds, with unavailable/stale handling.
- Machine and existing safe agent summary: every 60 seconds.
- Network and machine collection pause outside 08:00–23:00 Asia/Kolkata. A request already in flight can finish shortly after the boundary, within its timeout.
- The existing `aviato-display-power.timer` applies actual monitor VESA powerdown at 23:00 and wake at 08:00, checking idempotently once a minute. It signals a complete redraw after wake. The server itself stays awake.

The service is `aviato-screen.service`. The selected profile lives in `/home/keshav/.config/aviato-screen/profile` and contains `minimal`. The power schedule is `/home/keshav/aviato-screen/display-power.py`. Existing ambient/dashboard profiles, media handover and other applications remain available.

## Checks

```sh
systemctl status aviato-screen --no-pager
systemctl --user status aviato-display-power.timer --no-pager
cat /sys/class/drm/card1-DP-1/dpms
```

Source and test work is retained in `/home/keshav/Projects/aviato-playground/monitor-implementation`, with a baseline snapshot and the approved design under `docs/superpowers/`. Run `make test all sanitize` and `python3 -m unittest discover -s tests -p 'test_display_power.py' -v` there.

## Recovery

The original display files, executable, profile and power timer are saved in `/home/keshav/aviato-screen/backups/minimal-usd-20261106-225325`. Its generated `rollback.py` restores those exact files atomically, reloads the user timer and restarts only the verified display process through its existing systemd restart policy. Run it as the ordinary `keshav` user if rollback is needed. It does not change Tailscale, Docker, DNS or the server's suspend configuration.

Short CPU/RSS and temperature observations are saved separately as deployment verification. They establish the observed process overhead, not a long-term heat guarantee or monitor wall-power measurement.
