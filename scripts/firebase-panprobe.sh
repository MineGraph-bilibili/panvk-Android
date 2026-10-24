#!/bin/sh
# Run PanProbe as a Firebase Test Lab Game Loop test (one physical device, no retries).
# Usage: scripts/firebase-panprobe.sh <model> <api> [apk]
#   e.g. scripts/firebase-panprobe.sh a14 34
# Models: gcloud firebase test android models list. Spark plan: 5 physical runs/day.
# The app (MainActivity, TEST_LOOP intent) runs autorun "all" with the bundled
# driver, uploads to panvk-upload, writes JSON to the Game Loop result file and
# logcat tag PanProbeGameLoop, then finish()es.
# Pull results afterwards:
#   gsutil ls gs://<bucket>/panprobe/<model>-<api>-<stamp>/        (bucket printed by gcloud)
#   gsutil cp -r gs://<bucket>/panprobe/<model>-<api>-<stamp>/ ./ft-results/
#   (game_loop_results/*.json and logcat live under <model>-<api>-*/)
set -eu
[ $# -ge 2 ] || { sed -n '2,3p' "$0"; exit 2; }
MODEL=$1 API=$2
APK=${3:-apps/panvk-test/app/build/outputs/apk/debug/app-debug.apk}
[ -f "$APK" ] || { echo "no APK: $APK" >&2; exit 1; }
exec gcloud firebase test android run \
  --project panvk-device-test \
  --type game-loop \
  --app "$APK" \
  --device "model=$MODEL,version=$API" \
  --timeout 15m \
  --results-dir "panprobe/$MODEL-$API-$(date +%Y%m%d-%H%M%S)"
