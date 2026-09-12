#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p .cache/backhaul/pio .backhaul-tests
export npm_config_cache="$PWD/.cache/backhaul/npm"
(cd tools/webfilesbuilder && npm install --silent && npx gulp xzg)
docker run --rm --cap-drop ALL --security-opt no-new-privileges --pids-limit 512 --memory 4g --cpus 4 \
  --user "$(id -u):$(id -g)" -v "$PWD:/workspace" -w /workspace \
  -e HOME=/workspace/.cache/backhaul -e TMPDIR=/workspace/.cache/backhaul \
  -e PLATFORMIO_CORE_DIR=/workspace/.cache/backhaul/pio -e CZC_WEB_PREBUILT=1 \
  python:3.12-slim sh -c '
    test -x .cache/backhaul/venv/bin/pio || {
      python -m venv .cache/backhaul/venv
      .cache/backhaul/venv/bin/pip -q install platformio==6.1.18
    }
    .cache/backhaul/venv/bin/pio run "$@"
  ' sh "$@"
