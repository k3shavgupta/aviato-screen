# TV media mode and private portfolio display

Status: approved by the user; implementation underway. Kite Connect developer
app authentication was confirmed by the user.

## Existing hardware

The server is an Intel i3-7100 machine. Linux detects a powered Bluetooth
controller and an attached USB optical mouse. BlueZ, PipeWire, and WirePlumber
are running. PipeWire currently exposes only a dummy output, so speaker pairing
and the actual Bluetooth audio profile must be verified. Kodi is not installed.

The user selected music and local/network video as the primary media use.

## Recommended approach

Keep Linux and all existing server services. Add Kodi as the media interface
with hardware video decoding when supported. Use the existing mouse and Kodi's
phone web remote; Kore is the official Android remote. Run Bluetooth audio
through the same user's PipeWire session and verify reconnect after reboot.
Media playback consumes additional power; hardware support and real playback
measurements determine the result.

Sources: https://kodi.wiki/view/Web_interface and https://kodi.wiki/view/Kore

## Alternatives

- A minimal custom player plus a phone remote could be smaller but needs more
  custom navigation, library, and control work.
- Replacing the host OS or adding an Android VM would introduce migration or
  virtualization work and is unnecessary for local media and music.

## Display ownership and idle behavior

Exactly one renderer must own the display. The existing Aviato process maps
`/dev/fb0`; it cannot keep drawing behind Kodi on the same output. Before enabling
Kodi, implement an explicit handover or port Aviato into Kodi's screensaver API.
Prefer a Kodi screensaver integration with the same visual layout, since Kodi
already knows when music, video, and user input are active. Validate its idle
resource use before choosing it over an explicit launcher/display handover.

Mouse movement or phone commands dismiss the idle display. Video playback
suppresses it. For music, make idle visuals available while audio continues.
Server services remain independent throughout. Exiting media mode returns to
Aviato. Test display recovery when Kodi crashes, remote reconnect, speaker
reconnect, and startup without a network or speaker.

Phone control should be authenticated and available only on the local network
or the user's existing private remote-access network. Do not publish the remote
control port to the internet. Keep financial details hidden in shared media mode
and make portfolio visibility an explicit setting.

## Portfolio collector

Use a small server-side collector and one bounded cached summary shared with
the renderer. Refresh account snapshots every 1–5 minutes with retry backoff;
the display reads the cache without making its own authenticated requests.
Store tokens in a user-private credential file outside source and logs. The
phone and screen receive only the fields they need, never provider credentials.
No order placement, transfers, or withdrawals are part of this feature. Binance
keys should have only the reading permissions needed for account data.

Keep cash/funds, holdings market value, positions, and profit/loss distinct.
For Kite, use holdings, positions, and funds endpoints; avoid adding positions
to holdings without handling overlapping delivery positions. Display INR values.
For Binance, first select which wallets are in scope: spot balances are not a
complete total for futures, margin, funding, or Earn. Include both free and locked
spot amounts. Show valuation currency (for example USDT) explicitly; conversion
to INR needs a stated source/time and must not assume USDT equals USD or INR.
Unpriced assets remain visible as unpriced rather than silently counting as zero.

Show last successful update time and stale/disconnected/login-required states.
On timeout retain the previous snapshot with its timestamp. When Kite expires,
stop repeated account requests and offer the official login flow. Kite's normal
access token expires at 6 AM the next day; long-lived refresh tokens are available
only to certain approved platforms, so automatic daily login cannot be assumed.

Sources:

- https://kite.trade/docs/connect/v3/user/
- https://kite.trade/docs/connect/v3/portfolio/
- https://developers.binance.com/docs/binance-spot-api-docs/rest-api/account-endpoints

## Inputs needed for the next stage

- Bluetooth speaker in pairing mode and confirmation of which device to pair.
- Phone platform (the browser remote is cross-platform).
- Kite developer app setup and official account login; Binance reading-enabled
  credentials saved locally, with desired wallet coverage confirmed.
- Approval of the media/display handover design after its idle cost is measured.

Do not paste API secrets into the chat.
