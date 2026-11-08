#!/bin/bash
# Reviewed, deliberately manual one-time host setup. Does not start/enable Kodi.
set -euo pipefail
if [[ $EUID != 0 ]]; then echo 'Run: sudo bash /home/keshav/aviato-screen/media/setup-admin.sh' >&2; exit 1; fi
source_dir=/home/keshav/aviato-screen/media
unit=/etc/systemd/system/aviato-media.service
[[ $(id -u keshav) = 1000 ]] || { echo 'Expected keshav uid 1000; review unit first.' >&2; exit 1; }
[[ ! -e "$unit" ]] || { echo "Refusing to overwrite $unit" >&2; exit 1; }
# apt may add its own service dependencies; no existing configuration is rewritten here.
# This host has working IPv4 but no IPv6 default route. Keep these settings
# scoped to this installer; never continue using old lists after a failed update.
apt_options=(-o Acquire::ForceIPv4=true -o Acquire::http::Timeout=15
             -o Acquire::https::Timeout=15 -o Acquire::Retries=1)
if ! apt-get "${apt_options[@]}" --error-on=any update; then
  printf '%s\n' 'Repository refresh failed. Check connectivity and rerun this script; Kodi installation was not attempted.' >&2
  exit 1
fi
apt-get "${apt_options[@]}" install kodi kodi-bin libpam-systemd kbd
install -m 0644 "$source_dir/aviato-media.service" "$unit"
systemctl daemon-reload
printf '%s\n' 'Installed on-demand unit. No services enabled or started.'
