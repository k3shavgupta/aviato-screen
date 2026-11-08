#!/bin/bash
# Return this host to its existing native display and console boot target.
# Run from SSH: stopping the display manager closes the local desktop session.
set -euo pipefail
if [[ $EUID != 0 ]]; then
  echo 'Run from SSH: sudo bash /home/keshav/aviato-screen/media/restore-native.sh' >&2
  exit 1
fi
test -x /home/keshav/aviato-screen/screen
test -c /dev/fb0
test "$(systemctl show aviato-screen.service -p LoadState --value)" = loaded
echo 'Switching to native Aviato. The local desktop session will close.'
systemctl daemon-reload
systemctl enable aviato-screen.service
systemctl set-default multi-user.target
if [[ $(systemctl show aviato-media.service -p LoadState --value) = loaded ]]; then
  systemctl stop aviato-media.service
fi
if [[ $(systemctl show display-manager.service -p LoadState --value) = loaded ]]; then
  systemctl stop display-manager.service
fi
chvt 1
systemctl restart aviato-screen.service
systemctl is-active aviato-screen.service
echo 'Native service started. Allow its startup network requests to finish before the first frame.'
echo 'To restore the desktop: sudo systemctl set-default graphical.target; sudo systemctl start display-manager.service'
