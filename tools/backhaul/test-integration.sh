#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - <<'PY'
from pathlib import Path
s = Path('src/web.cpp').read_text()
version = s[s.index('static void apiCmdZbCheckFirmware(String &result)'):s.index('static void apiCmdZbLedToggle(String &result)')]
role = s[s.index('void changeZbMode(String fwMode)'):s.index('static void apiCmdDefault(String &result)')]
Path('.backhaul-tests/radio-role-under-test.inc').write_text(version + role)
PY
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
      -Itests/backhaul/rtos -Ilib/CzcBackhaul/src tests/backhaul/test_integration.cpp -lmbedcrypto -o .backhaul-tests/test-integration
    .backhaul-tests/test-integration
    python -c "from pathlib import Path; s=Path(\"src/web.cpp\").read_text(); Path(\".backhaul-tests/update-under-test.inc\").write_text(s[s.index(\"static const char *espUpdateState\"):s.index(\"// API strings\")]+s[s.index(\"void handleUpdateRequest()\"):s.index(\"// SSE transport:\")]+s[s.index(\"void progressFunc(\"):s.index(\"String fetchLatestEspFw(\")])"
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I.pio/libdeps/prod/ArduinoJson/src tests/backhaul/test_update.cpp -o .backhaul-tests/test-update
    .backhaul-tests/test-update
    python -c "from pathlib import Path; s=Path(\"src/zb.cpp\").read_text(); Path(\".backhaul-tests/radio-flash-under-test.inc\").write_text(s[s.index(\"bool eraseWriteZbFile(\"):s.index(\"float sendPercentageToFrontend(\")]); s=Path(\"src/radio_web_update.cpp\").read_text(); Path(\".backhaul-tests/radio-upload-under-test.inc\").write_text(\"\\n\".join(line for line in s.splitlines() if not line.startswith(\"#include\")))"
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I.pio/libdeps/prod/ArduinoJson/src -Ilib/CzcBackhaul/src \
      tests/backhaul/test_radio_upload.cpp -o .backhaul-tests/test-radio-upload
    .backhaul-tests/test-radio-upload
    python -c "from pathlib import Path; s=Path(\"src/zb.cpp\").read_text(); Path(\".backhaul-tests/radio-url-under-test.inc\").write_text(s[s.index(\"bool flashZigbeefromURL(\"):s.index(\"const char* downloadFirmwareFromGithub(\")])"
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      tests/backhaul/test_radio_url.cpp -o .backhaul-tests/test-radio-url
    .backhaul-tests/test-radio-url
  '
