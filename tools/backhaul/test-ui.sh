#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export npm_config_cache="$PWD/.cache/backhaul/npm"
npm install --prefix .cache/backhaul/ui jsdom@24.1.3 --silent
node --check src/websrc/js/functions.js
node tests/backhaul/test_ui.cjs
node tests/backhaul/test_network_ui.cjs
node tests/backhaul/test_zigbee_popup.cjs

node tests/backhaul/test_esp_popup.cjs
