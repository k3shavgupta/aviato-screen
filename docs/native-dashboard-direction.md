# Native dashboard direction

The user prefers returning to the native quote display and extending it with
scrollable Kite/Binance information, prioritizing low resource use.

The existing quote display and cached Kite summary are implemented. Kite is
currently unconfigured; Binance authenticated balances and input-driven pages
are not implemented. This is a proposed extension, not a completion claim.

Recommended layout: quotes/clock/weather as home, a Kite account page, and a
separate Binance account page with its wallet scope and valuation currency
clearly labelled. Default to spot for an initial Binance implementation only
after confirming that this covers the user's holdings. Avoid combining equity
cash, funded holdings, positions and crypto into an unexplained total.

Mouse wheel and arrow keys should move by rows/pages, with no continuous motion
or automatic ticker. The process should block on input and refresh deadlines;
repaint changed content only. Return to quotes after inactivity. Portfolio
refresh target: five minutes, with last-success time, stale states, backoff,
and paused polling when credentials require login. Keep the native clock's
existing minute cadence. Input must be limited to the selected display controls.

The local desktop is currently active and the native system service disabled.
`media/restore-native.sh` prepares the operational return to the existing native
display. It requires root, ends the desktop session, selects console boot,
and enables/restarts the native service. Run it from SSH after saving desktop
work. It does not uninstall Kodi or change other server service configuration.

Measure CPU, resident memory and package temperature under comparable server
load after restoration and again after portfolio/input integration. Historical
short idle measurements are not a guarantee of future power or temperature.
