# Aviato Screen low-power proposal

Date: 2026-10-26. Status: approved, implemented, and installed in the running service.

## Goal

Keep the existing always-on dashboard responsive while minimizing its CPU work,
memory use, network requests, and disk writes. An Android TV launcher, application
runtime, remote input, and video playback are separate scope decisions.

## Evidence from the running system

- The application is a native C process writing directly to `/dev/fb0` at
  1920 x 1080. It has no browser or desktop rendering dependency.
- A 15-second `pidstat` sample showed average CPU use of 0.33% of one core and
  resident memory of 17,204 KiB (approximately 16.8 MiB, including shared libraries).
- Systemd reported approximately 5.3 MiB charged to the service and 17.4 seconds
  of accumulated CPU time since its 17:32 start. These memory accounting methods
  measure different things.
- The CPU package sensor read approximately 47 degrees C. A single reading
  cannot attribute heat to the display process or establish a temperature trend.
- Source and logs show crypto polling every 5 seconds, two primary metals
  requests every 30 seconds, and weather polling every 10 minutes.
- Any changed signature triggers a complete screen clear and repaint, including
  rerasterizing text. Temperature, load, and memory changes enter that signature
  on each loop even if the clock and displayed prices have not changed.
- Network calls run synchronously. Metal failures can invoke fallback requests;
  logs include overdue deadlines. No progressive retry backoff is present.
- Weather fetching launches a shell and curl process and rewrites weather.json.

## Options

1. **Low-power dashboard (recommended):** retain the native display path, slow
   background data refresh, redraw only affected regions, and back off failed
   requests. Best fit when low resource use matters more than live prices.
2. **Preserve live-price cadence:** optimize redraws and failure handling but
   retain the 5-second crypto refresh. Preserves freshness with smaller savings
   in network work and wakeups.
3. **TV launcher:** add navigation, apps, and remote input. Requires a separate
   interaction design and typically adds work beyond this dashboard's needs.

## Recommended design

- Keep the current appearance, clock, weather, market values, and night themes.
- Default refresh intervals: crypto 60 seconds, metals 300 seconds, weather
  900 seconds, and system readings 60 seconds. Make the intervals configurable
  with validated minimum values. Clock changes remain aligned to each minute.
- Under successful requests, daytime primary market polling falls from roughly
  960 requests per hour to 84, excluding weather, retries, and fallbacks.
- Sleep until the next actual deadline. Recalculate time after network work so
  expired deadlines do not cause needless immediate wakeups or stale clock draws.
- Repaint only the region whose displayed content changed. Use the same sampled
  system readings for comparison and drawing. Reserve full redraws for startup,
  theme transitions, and changes that affect the whole layout.
- Retain last good values during failures and progressively increase retry
  delays with a cap. Show unavailable data when no successful value exists.
- Reuse an in-process curl handle for weather; keep responses bounded and avoid
  repeated shell launches and unnecessary cache writes.
- Avoid adding a browser, animation loop, or continuously running render thread.
- Do not alter other server workloads or global CPU/fan settings as part of
  this change.

## Validation

Check scheduler behavior, failure backoff, unchanged-content rendering, and
minute/theme transitions with controlled inputs. Build with compiler warnings
enabled and compare day and night previews. Measure the updated live service's
CPU, memory, redraw frequency, and request cadence against the baseline with
comparable sampling. Longer temperature measurements must account for other
server activity and room conditions; zero heat is not a credible guarantee.

## Deployment constraint

The system service is owned by root and passwordless sudo is unavailable.
Deployment used an atomic replacement of the executable followed by SIGTERM to
the verified Aviato process, which belongs to this user. Systemd's existing
Restart=always policy started the verified replacement. No service configuration
or other server workloads were changed. Original source and executable remain
in backups/2026-10-26-low-power/.
