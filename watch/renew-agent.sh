#!/bin/zsh
# Installs (or removes) the LaunchAgent that runs watch/renew.sh every 30 minutes, so Apex never runs out of
# its 7-day signature.
#
#   watch/renew-agent.sh install     # write ~/Library/LaunchAgents/com.floatrx.apex-renew.plist and load it
#   watch/renew-agent.sh uninstall   # unload and delete it
#   watch/renew-agent.sh status
#
# Log: ~/Library/Logs/apex-renew.log
set -euo pipefail
LABEL=com.floatrx.apex-renew
PLIST=$HOME/Library/LaunchAgents/$LABEL.plist
SCRIPT=$(cd "$(dirname "$0")" && pwd)/renew.sh
DOMAIN=gui/$(id -u)

case ${1:-status} in
  install)
    mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
    cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$SCRIPT</string></array>
  <key>StartInterval</key><integer>1800</integer>
  <key>RunAtLoad</key><true/>
  <key>EnvironmentVariables</key>
  <dict><key>PATH</key><string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin</string></dict>
  <key>StandardOutPath</key><string>$HOME/Library/Logs/apex-renew.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/apex-renew.log</string>
</dict>
</plist>
EOF
    plutil -lint "$PLIST" >/dev/null
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    launchctl bootstrap "$DOMAIN" "$PLIST"
    echo "loaded $LABEL — runs $SCRIPT every 30 minutes"
    ;;
  uninstall)
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    rm -f "${PLIST:?}"
    echo "removed $LABEL"
    ;;
  status)
    launchctl print "$DOMAIN/$LABEL" 2>/dev/null | grep -E 'state =|last exit code|run interval' || echo "$LABEL is not loaded"
    echo "signed until: $(cat "$HOME/.config/orca-deck/apex-signed-until" 2>/dev/null || echo unknown)"
    ;;
  *) echo "usage: $0 install|uninstall|status" >&2; exit 2 ;;
esac
