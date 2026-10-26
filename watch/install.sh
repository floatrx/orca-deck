#!/bin/zsh
# Rebuilds Apex and installs it on the paired Apple Watch — the weekly reinstall a free Personal Team needs
# (its provisioning profile lives 7 days; xcodebuild renews it). Then waits for the app's hello in the bridge
# trail, which proves the watch reaches the bridge.
#
#   watch/install.sh                  # bridge on the Mac's LAN address (configure.sh's default)
#   watch/install.sh http://host:7353 # another bridge URL
#   APEX_WATCH_UDID=<udid> watch/install.sh
#   APEX_CONFIGURATION=Debug watch/install.sh   # default Release: -O makes SwiftUI's per-frame work several
#                                               # times cheaper on the watch (Debug is -Onone)
#   APEX_NO_LAUNCH=1 watch/install.sh           # install only: do not bring Apex to the front (watch/renew.sh)
#
# On success it records the installed signature's expiry in ~/.config/orca-deck/apex-signed-until (ISO 8601);
# watch/renew.sh reads it to decide when to reinstall.
#
# The watch must be awake: raise the wrist and keep it unlocked until the hello line appears.
set -euo pipefail
cd "$(dirname "$0")"

TRAIL=/tmp/orca-deck-bridge.log
BUILD=/tmp/apex-dd
CONFIGURATION=${APEX_CONFIGURATION:-Release}

watch=${APEX_WATCH_UDID:-}
if [[ -z $watch ]]; then
  devices=$(mktemp)
  xcrun devicectl list devices --json-output "$devices" >/dev/null 2>&1
  watch=$(jq -r '[.result.devices[] | select(.hardwareProperties.deviceType == "appleWatch"
                   and .hardwareProperties.reality == "physical")][0].hardwareProperties.udid // empty' "$devices")
  rm -f "${devices:?}"
fi
[[ -n $watch ]] || { echo "no paired Apple Watch found (xcrun devicectl list devices)"; exit 1; }

./configure.sh "$@"
echo "building…"
xcodebuild -project Apex.xcodeproj -scheme Apex -configuration "$CONFIGURATION" -destination 'generic/platform=watchOS' \
  -allowProvisioningUpdates -derivedDataPath "$BUILD" -quiet build
app="$BUILD/Build/Products/$CONFIGURATION-watchos/Apex.app"

expires=$(security cms -D -i "$app/embedded.mobileprovision" 2>/dev/null \
  | plutil -extract ExpirationDate raw -o - - 2>/dev/null || echo "?")
echo "signed until $expires"

start=$(wc -l < "$TRAIL" 2>/dev/null || echo 0)
log=/tmp/apex-install.log
# A sleeping watch refuses the connection: retry, telling the user to raise the wrist.
for attempt in 1 2 3; do
  echo "installing on $watch (attempt $attempt — keep the watch awake and unlocked)…"
  if xcrun devicectl device install app --device "$watch" "$app" > "$log" 2>&1; then
    installed=1
    break
  fi
  echo "  $(grep -m1 'ERROR' "$log" || echo 'install failed')"
  (( attempt < 3 )) && { echo "  raise the wrist, unlock the watch — retrying in 10 s"; sleep 10; }
done
[[ -n ${installed:-} ]] || { echo "the watch did not take the app — full log: $log"; exit 1; }
[[ $expires != "?" ]] && print -r -- "$expires" > "$HOME/.config/orca-deck/apex-signed-until"
[[ -n ${APEX_NO_LAUNCH:-} ]] && { echo "installed (not launched) — signed until $expires"; exit 0; }
xcrun devicectl device process launch --terminate-existing --device "$watch" com.floatrx.orcadeck.apex >/dev/null

# A watch that just woke can take a minute to reconnect through the iPhone.
echo "waiting for the watch to reach the bridge (up to 90 s)…"
for _ in {1..90}; do
  hello=$(tail -n +$((start + 1)) "$TRAIL" 2>/dev/null | grep -m1 'client log from .*: Apex .* online' || true)
  [[ -n $hello ]] && { echo "$hello"; exit 0; }
  sleep 1
done
echo "installed, but no hello in $TRAIL within 90 s — open Apex on the watch; is the iPhone nearby and the bridge running?"
exit 2
