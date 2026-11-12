#!/bin/bash
# Builds KnobRelay.app into ~/Applications and runs it as the LaunchAgent com.floatrx.knob-relay
# (at login, restarted on exit). The agent's relay.py also serves the knob on USB: stop the agent before
# flashing — `launchctl bootout gui/$(id -u)/com.floatrx.knob-relay` — and run this script again after.
# Usage: knob/mac/install.sh [--pair] [--usb]
#   --pair  forget the pinned knob and take the next one it sees
#   --usb   let the relay also serve the knob on USB. Off by default: USB is for debugging, and the test driver
#           (knob/test/drive.py) needs the port free
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HOME/Applications/KnobRelay.app"
LABEL=com.floatrx.knob-relay
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/knob-relay.log"

mkdir -p "$APP/Contents/MacOS"
cp "$HERE/Info.plist" "$APP/Contents/Info.plist"
swiftc -O -o "$APP/Contents/MacOS/KnobRelay" "$HERE/KnobRelay.swift"
codesign --force --sign - "$APP"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$APP/Contents/MacOS/KnobRelay</string>
    <string>$HERE/../relay.py</string>$( [[ " $* " == *" --pair "* ]] && echo "
    <string>--pair</string>" )
  </array>
  <key>EnvironmentVariables</key><dict><key>HOME</key><string>$HOME</string>$( [[ " $* " == *" --usb "* ]] || echo "
    <key>KNOB_RELAY_NO_SERIAL</key><string>1</string>" )</dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>5</integer>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict>
</plist>
PLIST
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "installed: $APP · agent $LABEL · log $LOG"
