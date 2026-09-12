#pragma once

// The UI accepts 1..30 seconds. Apply the same bounds to NVS, old JSON
// migrations and POSTs; never let an invalid interval create a busy task.
inline unsigned webRefreshSeconds(long seconds) {
    return seconds < 1 ? 1 : seconds > 30 ? 30 : static_cast<unsigned>(seconds);
}
