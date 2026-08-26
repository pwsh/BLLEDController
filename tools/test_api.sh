#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/test_api.sh -- exercise every BLLED v3 HTTP endpoint against a real
# device (or tools/mock_server.py) and print the status code of each call.
#
#   ./tools/test_api.sh 10.0.42.33
#   ./tools/test_api.sh 10.0.42.33 admin secret       # with HTTP Basic auth
#   BLLED_WRITE=1 ./tools/test_api.sh 10.0.42.33      # also run the mutations
#   BLLED_DESTRUCTIVE=1 BLLED_WRITE=1 ./tools/test_api.sh ...  # + restart/reset
#
# By default only read-only requests and the harmless LED calls run.  Set
# BLLED_WRITE=1 for the config PUT / LED override tests, and BLLED_DESTRUCTIVE=1
# on top of that for restart and factory reset (which reboot the controller).
#
# Exit code 0 when every expected status matched.
# ---------------------------------------------------------------------------
set -u

HOST="${1:-}"
USER_="${2:-}"
PASS_="${3:-}"

if [ -z "$HOST" ]; then
  echo "usage: $0 <device-ip-or-host> [user] [password]" >&2
  exit 2
fi

BASE="http://$HOST"
AUTH=()
[ -n "$USER_" ] && AUTH=(-u "$USER_:$PASS_")

PASS_COUNT=0
FAIL_COUNT=0
BODY_FILE="$(mktemp)"
trap 'rm -f "$BODY_FILE"' EXIT

# check <expected-codes-csv> <label> <curl args...>
check() {
  local expect="$1" label="$2"
  shift 2
  local code
  code=$(curl -sS -o "$BODY_FILE" -w '%{http_code}' --max-time 20 "${AUTH[@]}" "$@" 2>/dev/null)
  local ok=1
  local IFS=,
  for e in $expect; do [ "$code" = "$e" ] && ok=0; done
  if [ $ok -eq 0 ]; then
    printf '  \033[32mok  \033[0m %-42s %s\n' "$label" "$code"
    PASS_COUNT=$((PASS_COUNT + 1))
  else
    printf '  \033[31mFAIL\033[0m %-42s %s (expected %s)\n' "$label" "$code" "$expect"
    head -c 200 "$BODY_FILE" | sed 's/^/         /'
    echo
    FAIL_COUNT=$((FAIL_COUNT + 1))
  fi
}

json() { echo "-HContent-Type: application/json"; }

echo "== BLLED API test against $BASE =="
echo
echo "-- read-only --"
check 200 "GET  /api/status"              "$BASE/api/status"
check 200 "GET  /api/config"              "$BASE/api/config"
check 200 "GET  /api/config/backup"       "$BASE/api/config/backup"
check 200 "GET  /api/info"                "$BASE/api/info"
check 200 "GET  /api/printers"            "$BASE/api/printers"
check 200 "GET  /api/wifi/scan"           "$BASE/api/wifi/scan"
check 404 "GET  /api/nonexistent"         "$BASE/api/nonexistent"
check 404 "GET  /api/stages (removed)"    "$BASE/api/stages"

echo
echo "-- secrets must never be echoed --"
if curl -sS --max-time 20 "${AUTH[@]}" "$BASE/api/config" | grep -qE '"(wifiPass|webPass|mqttExtPass)": ?"(\*{8})?"'; then
  printf '  \033[32mok  \033[0m %s\n' "GET /api/config masks the secrets"
  PASS_COUNT=$((PASS_COUNT + 1))
else
  printf '  \033[31mFAIL\033[0m %s\n' "GET /api/config did NOT mask the secrets"
  FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo
echo "-- removed legacy routes (must be 404) --"
check 404 "GET  /config.json"             "$BASE/config.json"
check 404 "GET  /factoryreset"            "$BASE/factoryreset"
check 404 "POST /submitConfig"            -X POST "$BASE/submitConfig"
check 404 "POST /submitWiFi"              -X POST "$BASE/submitWiFi"

echo
echo "-- legacy aliases (must still work) --"
check 200 "GET  /getConfig"               "$BASE/getConfig"
check 200 "GET  /configfile.json"         "$BASE/configfile.json"
check 200 "GET  /printerList"             "$BASE/printerList"

echo
echo "-- static assets --"
check 200,302 "GET  /"                    "$BASE/"
check 200 "GET  /app.js"                  "$BASE/app.js"
check 200 "GET  /style.css"               "$BASE/style.css"
check 200 "GET  /blled.svg"               "$BASE/blled.svg"
check 200 "GET  /favicon.png"             "$BASE/favicon.png"
check 200 "GET  /wifi"                    "$BASE/wifi"
check 200 "GET  /webserial"               "$BASE/webserial"

echo
echo "-- security headers --"
if curl -sSI --max-time 20 "${AUTH[@]}" "$BASE/app.js" | grep -qi 'x-content-type-options: *nosniff'; then
  printf '  \033[32mok  \033[0m %s\n' "X-Content-Type-Options: nosniff"
  PASS_COUNT=$((PASS_COUNT + 1))
else
  printf '  \033[31mFAIL\033[0m %s\n' "X-Content-Type-Options header missing"
  FAIL_COUNT=$((FAIL_COUNT + 1))
fi

echo
echo "-- input validation (must all be 400) --"
check 400 "POST /api/led bad hex"         -X POST "$(json)" -d '{"hex":"nope"}'          "$BASE/api/led"
check 400 "POST /api/led bad effect"      -X POST "$(json)" -d '{"hex":"#ff0000","effect":"disco"}' "$BASE/api/led"
check 400 "POST /api/led/mode bad mode"   -X POST "$(json)" -d '{"mode":"party"}'        "$BASE/api/led/mode"
check 400 "POST /api/led/brightness >100" -X POST "$(json)" -d '{"brightness":500}'      "$BASE/api/led/brightness"
check 400 "POST /api/action unknown"      -X POST "$(json)" -d '{"action":"selfdestruct"}' "$BASE/api/action"
check 400 "POST /api/action no \"on\""      -X POST "$(json)" -d '{"action":"chamberLight"}' "$BASE/api/action"
check 400 "PUT  /api/config unknown key"  -X PUT  "$(json)" -d '{"notAKey":1}'           "$BASE/api/config"

if [ "${BLLED_WRITE:-0}" != "1" ]; then
  echo
  echo "-- mutations skipped (set BLLED_WRITE=1 to run them) --"
else
  echo
  echo "-- LED control --"
  check 200 "POST /api/led (30 s orange)"   -X POST "$(json)" -d '{"hex":"#ff8800","effect":"solid","durationSec":30}' "$BASE/api/led"
  check 200 "POST /api/led (rgb+ww/cw)"     -X POST "$(json)" -d '{"r":0,"g":128,"b":255,"ww":40,"cw":40,"durationSec":10}' "$BASE/api/led"
  check 200 "POST /api/led/identify"        -X POST "$(json)" -d '{}' "$BASE/api/led/identify"
  check 200 "DELETE /api/led"               -X DELETE "$BASE/api/led"

  echo
  echo "-- config round-trip --"
  ORIG=$(curl -sS --max-time 20 "${AUTH[@]}" "$BASE/api/config" | tr -d '\n' | sed -n 's/.*"effectSpeed":\([0-9]*\).*/\1/p')
  ORIG=${ORIG:-5}
  check 200 "PUT  /api/config effectSpeed=7"  -X PUT "$(json)" -d '{"effectSpeed":7}' "$BASE/api/config"
  check 200 "PUT  /api/config restore ($ORIG)" -X PUT "$(json)" -d "{\"effectSpeed\":$ORIG}" "$BASE/api/config"
  check 200 "PUT  /api/config masked secret"  -X PUT "$(json)" -d '{"wifiPass":"********"}' "$BASE/api/config"

  echo
  echo "-- actions --"
  check 200 "POST /api/action pushall"        -X POST "$(json)" -d '{"action":"pushall","force":true}' "$BASE/api/action"
  check 200 "POST /api/action discover"       -X POST "$(json)" -d '{"action":"discover"}'  "$BASE/api/action"
  check 200 "POST /api/action rescanWifi"     -X POST "$(json)" -d '{"action":"rescanWifi"}' "$BASE/api/action"
  check 200 "POST /api/action reconnectMqtt"  -X POST "$(json)" -d '{"action":"reconnectMqtt"}' "$BASE/api/action"
  check 200 "POST /api/action chamberLight"   -X POST "$(json)" -d '{"action":"chamberLight","on":true}' "$BASE/api/action"

  echo
  echo "-- config backup / restore round-trip --"
  curl -sS --max-time 20 "${AUTH[@]}" "$BASE/api/config/backup" -o /tmp/blled_backup.json
  check 200 "POST /api/config/restore (backup)" -X POST -F "file=@/tmp/blled_backup.json" "$BASE/api/config/restore"
  echo '{"totally":"wrong"}' > /tmp/blled_bad.json
  check 400 "POST /api/config/restore (garbage)" -X POST -F "file=@/tmp/blled_bad.json" "$BASE/api/config/restore"
fi

if [ "${BLLED_DESTRUCTIVE:-0}" = "1" ] && [ "${BLLED_WRITE:-0}" = "1" ]; then
  echo
  echo "-- DESTRUCTIVE --"
  check 200 "POST /api/config/reset"          -X POST "$(json)" -d '{}' "$BASE/api/config/reset"
  echo "  (device is rebooting into setup mode)"
else
  echo
  echo "-- destructive tests skipped (BLLED_DESTRUCTIVE=1 BLLED_WRITE=1 to run) --"
fi

echo
echo "-- notes --"
cat <<'EOF'
  WebSocket:  websocat ws://HOST/ws     (or: python3 -c "import websocket")
              expect one /api/status object per second, plus immediate pushes
              on change; send {"cmd":"led","hex":"#00ff00"} / {"cmd":"clearLed"}
  OTA:        curl -u U:P -F "firmware=@.pio/build/esp32dev/firmware.bin" \
                   http://HOST/api/update      -> {"ok":true}, reboots
  External MQTT (when enabled):
              mosquitto_sub -h BROKER -v -t 'blled/#' -t 'homeassistant/+/blled_+/config'
              mosquitto_pub -h BROKER -t 'blled/BLLED/cmd' -m ON
              mosquitto_pub -h BROKER -t 'blled/BLLED/set' -m '{"hex":"#ff0000"}'
EOF

echo
echo "== $PASS_COUNT passed, $FAIL_COUNT failed =="
[ "$FAIL_COUNT" -eq 0 ]
