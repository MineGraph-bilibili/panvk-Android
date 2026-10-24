#!/bin/sh
# P9: true cold second-launch. Two processes, both must WSI-pass, PID_A != PID_B.
# Usage: ANDROID_SERIAL=<serial> ./scripts/run-cold-second-launch.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERIAL="${ANDROID_SERIAL:-Y5WWBMJVOZSK4HU8}"
PKG="org.panvk.loadertest"
ACT="${PKG}/.MainActivity"
ADB_BIN="${ADB:-/opt/android-sdk/platform-tools/adb}"
ADB="${ADB_BIN} -s ${SERIAL}"
OUT="${ROOT}/validation/g615-v11-csf/beta2-cold-second-launch-2026-09-19.txt"
APK="${ROOT}/tests/android-loader-app/panvk-loader-test.apk"
[ -f "$APK" ] || APK="${ROOT}/dist/panvk-loader-test.apk"
[ -f "$APK" ] || { echo "missing APK" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

$ADB wait-for-device
$ADB shell /system/bin/input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
$ADB shell /system/bin/wm dismiss-keyguard >/dev/null 2>&1 || true
$ADB shell /system/bin/svc power stayon true >/dev/null 2>&1 || true
$ADB shell /system/bin/am force-stop "$PKG" >/dev/null 2>&1 || true
$ADB install -r "$APK" >/dev/null

wait_wsi() {
  i=0
  while [ "$i" -lt 90 ]; do
    dump="$TMP/lc"
    $ADB logcat -d -s PanVKLoaderApp >"$dump" 2>/dev/null || true
    if grep -q "Gate H: PASS Vulkan WSI" "$dump"; then
      return 0
    fi
    if grep -Eq "FAIL |Gate H FAIL|FATAL EXCEPTION" "$dump"; then
      return 1
    fi
    sleep 1
    i=$((i + 1))
  done
  return 1
}

run_once() {
  label="$1"
  $ADB logcat -c >/dev/null 2>&1 || true
  $ADB shell /system/bin/am force-stop "$PKG" >/dev/null 2>&1 || true
  sleep 1
  $ADB shell /system/bin/input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
  $ADB shell /system/bin/wm dismiss-keyguard >/dev/null 2>&1 || true
  $ADB shell /system/bin/am start -W -n "$ACT" >/dev/null
  if ! wait_wsi; then
    $ADB logcat -d -s PanVKLoaderApp >"$TMP/${label}.log" 2>/dev/null || true
    echo "FAIL ${label}: no WSI PASS" >&2
    return 1
  fi
  $ADB logcat -d -s PanVKLoaderApp >"$TMP/${label}.log" 2>/dev/null || true
  pid="$(grep -o 'PID_SELF=[0-9]*' "$TMP/${label}.log" | head -n1 | cut -d= -f2)"
  if [ -z "$pid" ]; then
    pid="$($ADB shell /system/bin/pidof "$PKG" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
  fi
  [ -n "$pid" ] || { echo "FAIL ${label}: no PID" >&2; return 1; }
  grep -q "Gate H: PASS Vulkan WSI" "$TMP/${label}.log" || { echo "FAIL ${label}: WSI missing in dump" >&2; return 1; }
  grep -q "PRESENT_FRAMES=300/300" "$TMP/${label}.log" || { echo "FAIL ${label}: PRESENT_FRAMES != 300/300" >&2; return 1; }
  echo "$pid"
}

PID_FIRST="$(run_once first)" || { echo "COLD_SECOND_LAUNCH=fail FIRST" >&2; exit 1; }
$ADB shell /system/bin/am force-stop "$PKG" >/dev/null 2>&1 || true
sleep 2
PID_SECOND="$(run_once second)" || { echo "COLD_SECOND_LAUNCH=fail SECOND" >&2; exit 1; }
$ADB shell /system/bin/am force-stop "$PKG" >/dev/null 2>&1 || true
$ADB shell /system/bin/svc power stayon false >/dev/null 2>&1 || true

STATUS=fail
COLD=fail
SECOND_WSI=fail
if [ "$PID_FIRST" != "$PID_SECOND" ] && [ -n "$PID_FIRST" ] && [ -n "$PID_SECOND" ]; then
  COLD=pass
  SECOND_WSI=pass
  STATUS=pass
fi

{
  echo "device=Poco X6 Pro serial=${SERIAL} package=${PKG}"
  echo "date=2026-09-19"
  echo "test=cold second-launch (P9)"
  echo "sequence=install,launch A,WSI>=60,destroy,force-stop,launch B,reload,WSI>=60,clean exit"
  echo "PID_FIRST=${PID_FIRST}"
  echo "PID_SECOND=${PID_SECOND}"
  echo "COLD_RELAUNCH=${COLD}"
  echo "SECOND_WSI_RUN=${SECOND_WSI}"
  echo "COLD_SECOND_LAUNCH=${STATUS}"
  echo
  echo "===== RUN A pid=${PID_FIRST} ====="
  cat "$TMP/first.log"
  echo
  echo "===== RUN B pid=${PID_SECOND} ====="
  cat "$TMP/second.log"
} >"$OUT"

echo "COLD_SECOND_LAUNCH=${STATUS}"
echo "PID_FIRST=${PID_FIRST}"
echo "PID_SECOND=${PID_SECOND}"
echo "COLD_RELAUNCH=${COLD}"
echo "SECOND_WSI_RUN=${SECOND_WSI}"
echo "LOG=${OUT}"
[ "$STATUS" = pass ]
