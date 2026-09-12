#!/usr/bin/env bash
# Run after build.sh -e prod -e debug to check both linked variants.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p .backhaul-tests
python3 tools/backhaul/prepare-peer-tls.py --check
python3 -m unittest discover -s tests/backhaul -p test_tls_generation.py
python3 tools/backhaul/check-build-variants.py
docker build -q -t czc-backhaul-tests:1 -f tests/backhaul/Dockerfile tests/backhaul
for suite in native tls admin-tls debug-gate controller status integration reconnect network web-events stock-uart cctools ui; do
  if bash "tools/backhaul/test-$suite.sh" > ".backhaul-tests/$suite.log" 2>&1; then
    echo "$suite: PASS"
  else
    echo "$suite: FAIL"
    tail -60 ".backhaul-tests/$suite.log"
    exit 1
  fi
done
python3 tools/backhaul/make_acceptance_script.py
python3 -m unittest discover -s tests/backhaul -p test_hardware_acceptance.py
