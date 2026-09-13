#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
# Exercise every ROM block using a deterministic full-size image, including
# sparse regions. This host test does not need a separately built radio binary.
python3 - <<'PY'
from pathlib import Path
image = bytes((i * 73 + 19) % 256 for i in range(720896))
Path('.backhaul-tests/radio-fixture.bin').write_bytes(image)
PY
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    g++ -std=c++11 -Wall -Wextra -Wno-sign-compare -Wno-unused-variable -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Itests/backhaul/cctools -Ilib/CCTools/src -Ilib/CzcBackhaul/src \
      tests/backhaul/test_cctools.cpp lib/CCTools/src/CCTools.cpp -o .backhaul-tests/test-cctools
    .backhaul-tests/test-cctools .backhaul-tests/radio-fixture.bin
  '
