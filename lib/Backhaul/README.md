# Framed peer transport

Portable C++11 framing and bounded session state used by `src/backhaul.cpp`.
The caller supplies an authenticated byte stream, radio admission callback and
monotonic clock. This library allocates no heap memory and has no Arduino or
socket dependency.

Sessions use epochs and sequence numbers to reject stale confirmations, a
four-message window, admission results, heartbeats and transaction deadlines.
Admission means the remote radio queued the frame; APS delivery is confirmed
separately by the Zigbee stack.

See [configuration and protocol](../../docs/backhaul.md) and
[tests](../../tests/backhaul/README.md).
