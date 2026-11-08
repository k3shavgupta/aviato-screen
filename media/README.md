# On-demand Kodi media

Prepared integration, **hardware playback unverified**. Kodi is not installed on
this host; no seat session is active and `/dev/tty1` is root-owned. No packages,
system media services, Bluetooth pairings, or Kodi settings were changed by this work.
The native renderer now implements display locking, SIGUSR1 wakeup, and shared
frames. The launcher checks that the renderer has its handler and lock open
before starting Kodi. Native handover tests pass; real Kodi/DRM restoration
still needs the installation and playback checks below.
The current Ubuntu apt index offers Kodi 21.3 with GBM library dependencies;
actual GBM backend, DRM/session access, codecs and HDMI/BT output must be tested
on the host after installation. Native Aviato remains the idle default.

## Install and start

Review `media/setup-admin.sh` and `media/aviato-media.service` first. They are
specific to keshav (uid 1000), this checkout, and unused tty8. Then run:

```sh
sudo bash /home/keshav/aviato-screen/media/setup-admin.sh
python3 /home/keshav/aviato-screen/media/build-addon.py
sudo systemctl start aviato-media.service
journalctl -u aviato-media.service -b --no-pager
```

This deliberately requires your administrator command: no passwordless privilege
rules or phone start/stop relay are installed. The unit creates a PAM login on
tty8 and switches to it, letting logind grant seat devices. Kodi runs as keshav,
using that user's existing PipeWire session. It does not enable Kodi on boot,
change the default target, stop server services, or overwrite Kodi configuration.
Do not use tty8 for another login. If a desktop/compositor already owns DRM,
resolve the seat conflict locally before launch. No root Kodi fallback is used.

The installer uses IPv4, 15-second HTTP/HTTPS timeouts and one retry per request.
It stops if any repository refresh fails, instead of proceeding with old package
lists. These options apply only to this script, without changing global network
or APT settings. After a download/network failure, restore connectivity and rerun
the same setup command; the unit is installed only after packages succeed.

To return to the native default:

```sh
sudo systemctl stop aviato-media.service
sudo chvt 1
```

If startup fails, inspect the journal and Kodi's `~/.kodi/temp/kodi.log`.
The launcher releases `display.lock` even if Kodi fails, so Aviato can resume.
It uses Linux child-subreaper adoption and waits for the Kodi wrapper plus all
adopted player descendants before handback. TERM/INT are forwarded to a dedicated
process group; an interrupted shell-wrapper wait cannot release ownership early.
If Linux subreaper setup fails, media startup fails closed.
Do not delete the lock file while either renderer runs.
The runtime directory must have mode 700 and its lock file mode 600.

## Aviato screensaver

In Kodi Settings → Add-ons, enable installation from ZIP when prompted and
install `media/dist/screensaver.aviato-1.0.0.zip`. Existing profiles are retained.
In Settings → Interface → Screensaver select **Aviato** and set an idle timeout.
For music, disable “Use visualization if playing audio” and “Use dim if paused”
if you want this saver in those states. Start music and wait for the timeout.

The addon reads only `/run/user/1000/aviato-screen/screensaver.bmp`. The native
process renders this private-safe frame while Kodi holds the display lock; no
credentials, private balances or caches belong in it. The addon polls metadata
once per second while active and reloads only changed files with image caching
disabled. Any input or Kodi saver deactivation closes it. It never stops music.
If no frame exists it shows black. Videos normally inhibit screensavers.

## Media, mouse and phone

Kodi's GUI accepts the attached mouse/keyboard. Add local music/video sources
through Music/Videos → Files → Add source. Add authenticated SMB/NFS or explicit
network URLs there; keep credentials in Kodi's profile, never dashboard frames.
Streaming-service/DRM support depends on separate addons and provider support.

For a phone on the same trusted home LAN, enable Kodi Settings → Services →
Control → “Allow remote control via HTTP”, set a unique username/password,
and connect Kore or Kodi's web UI to the host's LAN address/port shown there.
Keep external application control off unless required by your chosen remote.
Do not forward the port through the router or expose it to the internet. HTTP
credentials are not encrypted: use only your trusted LAN or an SSH tunnel.
The remote works only while Kodi is active; use the administrator start command
above first. No additional network listener is shipped by this integration.
Kodi and the collector currently run as the same Linux user. Private file modes
protect against other users, but do not isolate credentials from Kodi addons.
Separate-account isolation is not implemented.

## Bluetooth audio

Run these as **keshav**, not root. Put the speaker/headphones in pairing mode:

```text
bluetoothctl
power on
agent on
default-agent
scan on
# substitute the actual address below
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
connect AA:BB:CC:DD:EE:FF
scan off
quit
```

Then run `wpctl status` and `wpctl set-default ID` with the real speaker sink ID.
In Kodi Settings → System → Audio choose the PipeWire/PulseAudio default output
and disable passthrough for Bluetooth. Reconnect or reselect the output if Kodi
started before pairing. Bluetooth adapter, BlueZ, PipeWire and WirePlumber were
observed active, but **no device was paired and audio is untested**.

## Verification performed

`python3 -m unittest discover -s tests -p test_media.py` exercises lock lifetime,
failed-player release, rejection of unrelated PIDs, verified native signalling,
wrapper/descendant termination ordering, and addon refresh/deactivation with a fake monitor. This is not a hardware test.
