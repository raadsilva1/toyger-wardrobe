#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "error: root privileges are required. run with doas/sudo." >&2
  exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

./build.sh

install -d -m 0755 /usr/local/bin
install -m 0755 toyger-wardrobe /usr/local/bin/toyger-wardrobe

install -d -m 0755 /etc/toyger-wardrobe
if [ -f /etc/toyger-wardrobe/apps.json ]; then
  install -m 0644 sample-apps.json /etc/toyger-wardrobe/apps.json.sample
  echo "info: kept existing /etc/toyger-wardrobe/apps.json" >&2
  echo "info: installed sample to /etc/toyger-wardrobe/apps.json.sample" >&2
else
  install -m 0644 sample-apps.json /etc/toyger-wardrobe/apps.json
  echo "info: installed default config to /etc/toyger-wardrobe/apps.json" >&2
fi

echo "installed: /usr/local/bin/toyger-wardrobe"
