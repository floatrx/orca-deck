#!/bin/zsh
# Keeps Apex signed on the watch. A free Personal Team signs for 7 days, and rebuilding does not renew it:
# Xcode reuses the cached profile until it expires. Run every 30 minutes by the LaunchAgent
# com.floatrx.apex-renew (watch/renew.plist.template); by hand: watch/renew.sh [--force]
#
# 1. The watch's signature (~/.config/orca-deck/apex-signed-until, written by install.sh) is good for more than
#    RENEW_DAYS → nothing to do.
# 2. The cached Apex profile expires within RENEW_DAYS too → back it up and remove it, so Xcode makes a new one.
# 3. Build and install without launching Apex (APEX_NO_LAUNCH): a sleeping watch fails, the next run retries.
# 4. Telegram (~/.claude/tg-send.sh): "renewed until …", or — under a day left and still failing — a nudge to
#    raise the wrist, at most every NUDGE_HOURS.
set -uo pipefail
cd "$(dirname "$0")"

RENEW_DAYS=${APEX_RENEW_DAYS:-2}
NUDGE_HOURS=3
STATE=$HOME/.config/orca-deck
PROFILES="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
PROFILE_NAME="iOS Team Provisioning Profile: com.floatrx.orcadeck.apex"
TELEGRAM=$HOME/.claude/tg-send.sh
now=$(date +%s)

log() { print -r -- "$(date '+%F %T') $*"; }
epoch() { date -j -u -f '%Y-%m-%dT%H:%M:%SZ' "$1" +%s 2>/dev/null || echo 0; }
notify() { [[ -x $TELEGRAM ]] && "$TELEGRAM" "$1" >/dev/null 2>&1; }

signed=$(cat "$STATE/apex-signed-until" 2>/dev/null || true)
left=$(( $(epoch "${signed:-1970-01-01T00:00:00Z}") - now ))
if [[ ${1:-} != --force && $left -gt $(( RENEW_DAYS * 86400 )) ]]; then
  exit 0  # quiet: this runs every 30 minutes
fi
log "the watch is signed until ${signed:-unknown} — renewing"

# A cached profile that will not outlive the renewal window goes to the backup, so Xcode creates a fresh one.
for file in "$PROFILES"/*.mobileprovision(N); do
  plist=$(security cms -D -i "$file" 2>/dev/null) || continue
  [[ $(plutil -extract Name raw -o - - <<<"$plist" 2>/dev/null) == "$PROFILE_NAME" ]] || continue
  expires=$(plutil -extract ExpirationDate raw -o - - <<<"$plist" 2>/dev/null)
  if (( $(epoch "$expires") - now < RENEW_DAYS * 86400 )); then
    mkdir -p "${STATE:?}/old-profiles"
    cp "$file" "${STATE:?}/old-profiles/" && rm -f "${file:?}" && log "retired the cached profile $(basename "$file") (expires $expires)"
  fi
done

APEX_NO_LAUNCH=1 ./install.sh
code=$?
if (( code == 0 )); then
  signed=$(cat "$STATE/apex-signed-until" 2>/dev/null)
  log "renewed: the watch is signed until $signed"
  notify "⌚️ <b>Apex</b> renewed on the watch — signed until <code>$signed</code>"
  rm -f "${STATE:?}/apex-nudged-at"
  exit 0
fi

# install.sh exits 1 when the watch refused the app; anything else is the build (signing, Xcode, the network).
if (( code == 1 )); then
  log "the install failed (the watch is probably asleep) — the next run retries"
else
  log "the build failed (exit $code) — see the lines above; the next run retries"
fi
if (( left < 86400 || code != 1 )); then
  nudged=$(cat "$STATE/apex-nudged-at" 2>/dev/null || echo 0)
  if (( now - nudged > NUDGE_HOURS * 3600 )); then
    if (( code == 1 )); then
      notify "⌚️ <b>Apex</b> stops launching at <code>${signed:-?}</code> — raise your wrist and unlock the watch; the reinstall retries every 30 min"
    else
      notify "⌚️ <b>Apex</b> renewal: the build failed (exit $code) — see ~/Library/Logs/apex-renew.log"
    fi
    print -r -- "$now" > "$STATE/apex-nudged-at"
  fi
fi
exit 1
