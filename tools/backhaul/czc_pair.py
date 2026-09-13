#!/usr/bin/env python3
"""CZC Master and 1..8 Satellites kit. Python >=3.13 (stdlib TLS PSK). Never prints the PSK."""
import argparse
import contextlib
import functools
import hashlib
import json
import operator
import os
from pathlib import Path
import secrets
import select
import socket
import ssl
import struct
import sys
from collections import deque
import time

REVISION = 20260917
STATS = 'tx accepted failed timeout overflow broadcast_tx rx alloc free live alloc_fail queue_fail duplicate reject confirm_retry'.split()
EP, PROFILE, CLUSTER = 240, 0xC105, 0xFCFE


def key_read(path):
    value = Path(path).read_text().strip()
    try:
        key = bytes.fromhex(value)
    except ValueError:
        raise ValueError('PSK file must contain 64 hexadecimal characters') from None
    if len(key) != 32:
        raise ValueError('PSK must be 32 bytes')
    return key


def tls_context(key, identity='czc-admin-v1', server=False):
    if not hasattr(ssl.SSLContext, 'set_psk_client_callback'):
        raise RuntimeError('Python 3.13+ with TLS PSK support required; use accept_hardware.py, which starts the Python 3.13 Docker worker')
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER if server else ssl.PROTOCOL_TLS_CLIENT)
    if not server:
        ctx.check_hostname = False
        # This is PSK authentication, NOT unauthenticated certificate TLS.
        ctx.verify_mode = ssl.CERT_NONE
        ctx.set_psk_client_callback(lambda hint: (identity, key))
    else:
        ctx.set_psk_server_callback(lambda offered: key if offered == identity else b'')
    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.options |= ssl.OP_NO_RENEGOTIATION
    return ctx


def connect(host, key, identity='czc-admin-v1', port=7444, *, deadline=None):
    def remaining():
        if deadline is None:
            return 5
        budget = deadline - time.monotonic()
        if budget <= 0:
            raise TimeoutError('Admin TLS connection deadline')
        return min(5, budget)

    raw = socket.create_connection((host.strip('[]'), port), timeout=remaining())
    wrapped = None
    try:
        wrapped = tls_context(key, identity).wrap_socket(raw, server_hostname=None,
                                                       do_handshake_on_connect=False)
        wrapped.settimeout(remaining())
        wrapped.do_handshake()
        wrapped.settimeout(5)
        return wrapped
    except BaseException:
        (wrapped if wrapped is not None else raw).close()
        raise


def packet(c0, c1, data=b''):
    if len(data) > 250:
        raise ValueError('MT payload too large')
    body = bytes([len(data), c0, c1]) + data
    return b'\xfe' + body + bytes([functools.reduce(operator.xor, body, 0)])


class Parser:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, chunk):
        self.buffer.extend(chunk)
        result = []
        while self.buffer:
            if self.buffer[0] != 0xFE:
                raise ValueError('MT framing lost')
            if len(self.buffer) < 2:
                break
            if self.buffer[1] > 250:
                raise ValueError('MT length exceeds limit')
            length = self.buffer[1] + 5
            if len(self.buffer) < length:
                break
            data = self.buffer[:length]
            del self.buffer[:length]
            if functools.reduce(operator.xor, data[1:], 0):
                raise ValueError('MT checksum mismatch')
            result.append((data[2], data[3], bytes(data[4:-1])))
        return result


class Client:
    def __init__(self, host, key, port=7444, *, connect_deadline=None):
        self.host, self.key = host, key
        self.socket = connect(host, key, port=port, deadline=connect_deadline)
        self.parser = Parser()
        self.events, self.replies = [], []
        self.transaction = secrets.randbelow(256)

    def close(self):
        self.socket.close()

    def pump(self, timeout=0.02):
        self.socket.settimeout(timeout)
        try:
            data = self.socket.recv(4096)
        except (TimeoutError, ssl.SSLWantReadError):
            return
        if not data:
            raise ConnectionError('TLS connection closed')
        for frame in self.parser.feed(data):
            (self.replies if frame[0] & 0xE0 == 0x60 else self.events).append(frame)
        if len(self.events) > 256 or len(self.replies) > 8:
            raise RuntimeError('Bounded host queue overflow; another controller or unexpected traffic')

    def request(self, cmd, data=b'', subsystem=1, timeout=3):
        self.socket.sendall(packet(0x20 | subsystem, cmd, data))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.replies:
                c0, c1, reply = self.replies.pop(0)
                if (c0, c1) != (0x60 | subsystem, cmd):
                    raise RuntimeError('Unexpected SRSP; UART session is not synchronized')
                return reply
            self.pump(min(0.1, max(0.001, deadline - time.monotonic())))
        raise TimeoutError(f'No SRSP: {subsystem:02x}/{cmd:02x}')

    def checked(self, cmd, data=b'', subsystem=1):
        reply = self.request(cmd, data, subsystem)
        if subsystem == 31 and cmd in (2, 3, 4, 5) and reply == b'\x1a':
            raise RuntimeError('Enable Acceptance / debug mode in the Role UI on both CZCs, then save and restart')
        if reply != b'\0':
            raise RuntimeError(f'Command {subsystem:02x}/{cmd:02x}: {reply.hex()}')

    def gateway(self):
        return json.loads(self.request(1, subsystem=31))

    def pause(self, value):
        self.checked(2, bytes([bool(value)]), 31)

    def hold(self, value):
        self.checked(4, bytes([bool(value)]), 31)

    def info(self):
        data = self.request(0xC0)
        if len(data) != 32 or data[:2] != b'\0\2' or struct.unpack_from('<I', data, 2)[0] != REVISION:
            raise RuntimeError('Radio firmware/protocol mismatch')
        return dict(revision=REVISION, short=struct.unpack_from('<H', data, 6)[0],
                    pan=struct.unpack_from('<H', data, 8)[0], channel=data[10], state=data[11],
                    ieee='0x' + data[12:20][::-1].hex(), extpan=data[20:28].hex(),
                    bound=data[28], online=data[29], peer=struct.unpack_from('<H', data, 30)[0])

    def stats(self):
        data = self.request(0xC6)
        if len(data) != 4 + 4 * len(STATS) or data[0]:
            raise RuntimeError('Radio statistics format')
        return dict(bound=data[1], online=data[2], pending=data[3],
                    **dict(zip(STATS, struct.unpack_from('<' + 'I' * len(STATS), data, 4))))

    def register(self):
        # A reserved acceptance endpoint; never removes Zigbee2MQTT's endpoint 1.
        self.request(4, bytes([EP]), 4)
        descriptor = struct.pack('<BHHBBBHBH', EP, PROFILE, 1, 1, 0, 1, CLUSTER, 1, CLUSTER)
        self.checked(0, descriptor, 4)

    def send(self, destination, payload):
        self.transaction = (self.transaction + 1) & 255
        options = 0 if destination >= 0xFFF8 else 0x10  # APS ACK for unicast.
        body = struct.pack('<HBBHBBBB', destination, EP, EP, CLUSTER, self.transaction, options, 30, len(payload)) + payload
        self.checked(1, body, 4)
        return self.transaction


def af(frame):
    c0, c1, data = frame
    if (c0, c1) != (0x44, 0x81):
        return None
    if len(data) < 20 or len(data) != 20 + data[16]:
        raise ValueError('Invalid AF event')
    return dict(cluster=struct.unpack_from('<H', data, 2)[0], source=struct.unpack_from('<H', data, 4)[0],
                source_ep=data[6], destination_ep=data[7], payload=data[17:17 + data[16]],
                mac_source=struct.unpack_from('<H', data, 17 + data[16])[0])


def wait_until(predicate, clients, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        for client in clients:
            client.pump(0.03)
    raise TimeoutError('Condition was not satisfied before deadline')


def ready(clients):
    wait_until(lambda: all(c.gateway()['peer'] and c.stats()['online'] for c in clients), clients, 20)


def received(client, payload, source):
    matches = []
    for frame in client.events:
        msg = af(frame)
        if msg and msg['payload'] == payload and msg['cluster'] == CLUSTER and msg['destination_ep'] == EP:
            if msg['source'] != source or msg['source_ep'] != EP or msg['mac_source'] != getattr(client,'expected_mac',{}).get(source,source):
                raise AssertionError('Payload arrived with incorrect source/MAC metadata')
            matches.append(msg)
    return len(matches)


def confirmation(client, transaction):
    return [data[0] for c0, c1, data in client.events if (c0, c1) == (0x44, 0x80) and
            len(data) == 3 and data[1:] == bytes([EP, transaction])]


class ExchangeFailure(AssertionError):
    def __init__(self, reason, evidence):
        super().__init__(reason)
        self.evidence = evidence


# Names from this radio's TI zcomdef.h. Keep unknown codes as hex rather than
# guessing whether a MAC/NWK/APS failure was an absent APS ACK.
AF_STATUS_NAMES = {0: 'ZSuccess', 0xb1: 'ZApsFail', 0xb7: 'ZApsNoAck',
                   0xbb: 'ZApsNotAuthenticated', 0xcc: 'ZNwkNoAck',
                   0xcd: 'ZNwkNoRoute', 0xe1: 'ZMacChannelAccessFailure',
                   0xe9: 'ZMacNoACK', 0xf0: 'ZMacTransactionExpired'}


class ExchangeTrace:
    """Capture a failed challenge before diagnostic RPCs can consume late events."""
    def __init__(self, sender, receiver, source, destination, payload, kind='unicast'):
        self.sender, self.receiver = sender, receiver
        self.source, self.destination, self.payload, self.kind = source, destination, payload, kind
        self.transaction = None
        self.queue_observation = None
        self.phase = 'send'
        self.before = [sender.stats(), receiver.stats()]
        self.started = time.monotonic()

    def send(self):
        try:
            self.transaction = self.sender.send(self.destination, self.payload)
        except Exception:
            self.transaction = getattr(self.sender, 'transaction', None)
            raise
        self.phase = 'wait_af_and_confirm'
        return self.transaction

    def snapshot(self):
        result = dict(kind=self.kind, phase=self.phase,
                      source='0x%04X' % self.source, destination='0x%04X' % self.destination,
                      sender_host=getattr(self.sender, 'host', None), receiver_host=getattr(self.receiver, 'host', None),
                      bytes=len(self.payload), endpoint=EP, transaction=self.transaction,
                      elapsed_ms=round((time.monotonic()-self.started)*1000, 1))
        if self.queue_observation is not None:
            result['queue'] = self.queue_observation.copy()
        # Do not serialize keys, payloads, or arbitrary UART traffic. Also keep
        # wrong-source matches so a metadata assertion cannot hide its evidence.
        deliveries = []
        for frame in self.receiver.events:
            try:
                msg = af(frame)
            except ValueError:
                result['malformed_af'] = result.get('malformed_af', 0) + 1
                continue
            if msg and msg['payload'] == self.payload and msg['cluster'] == CLUSTER and msg['destination_ep'] == EP:
                deliveries.append(dict(source='0x%04X' % msg['source'], source_ep=msg['source_ep'],
                                       mac_source='0x%04X' % msg['mac_source']))
        result['af_deliveries'] = len(deliveries)
        result['af_metadata'] = deliveries[:4]
        statuses = confirmation(self.sender, self.transaction) if self.transaction is not None else []
        result['aps_confirmations'] = ['0x%02X' % s for s in statuses]
        others = [dict(transaction=d[2], status='0x%02X' % d[0]) for c0, c1, d in self.sender.events
                  if (c0, c1) == (0x44, 0x80) and len(d) == 3 and d[1] == EP and d[2] != self.transaction]
        if others:
            result['other_confirmations_count'] = len(others)
            result['other_confirmations'] = others[-8:]
        return result

    def validate(self, expected_status=0, expected_deliveries=1):
        self.phase = 'validate'
        count = received(self.receiver, self.payload, self.source)
        assert count == expected_deliveries, 'AF delivery count: expected %s, got %s' % (expected_deliveries, count)
        statuses = confirmation(self.sender, self.transaction)
        if not statuses:
            raise AssertionError('Missing APS confirmation for transaction %s' % self.transaction)
        if len(statuses) != 1:
            raise AssertionError('Duplicate APS confirmations for transaction %s: %s' %
                                 (self.transaction, ', '.join('0x%02X' % s for s in statuses)))
        value = statuses[0]
        if (expected_status is None and value == 0) or (expected_status is not None and value != expected_status):
            raise AssertionError('APS confirmation: 0x%02X (%s); expected %s' %
                                 (value, AF_STATUS_NAMES.get(value, 'unknown status'),
                                  'failure' if expected_status is None else '0x%02X' % expected_status))

    def counters(self, after):
        return [{k: (a[k]-b[k]) & 0xffffffff for k in STATS if k in a and k in b}
                for a, b in zip(after, self.before)]


@contextlib.contextmanager
def trace_exchanges(traces):
    try:
        yield
    except Exception as exc:
        # Freeze ALL directions before issuing any RPC: a late confirmation
        # observed during diagnostics must never turn a deadline failure into PASS.
        evidence = [t.snapshot() for t in traces]
        after = {}
        for t in traces:
            for c in (t.sender, t.receiver):
                if c not in after:
                    try:
                        after[c] = c.stats()
                    except Exception as err:
                        after[c] = dict(error=type(err).__name__)
        for t, e in zip(traces, evidence):
            samples = [after[t.sender], after[t.receiver]]
            delta = t.counters(samples)
            for label, values, sample in zip(('sender', 'receiver'), delta, samples):
                e[label] = dict(error=sample['error']) if 'error' in sample else values
            e['counter_scope'] = 'Node deltas; may include background traffic and events after the failure snapshot'
        raise ExchangeFailure(str(exc) or type(exc).__name__, evidence[0] if len(evidence) == 1 else
                              dict(transfers=evidence)) from exc


def exchange(sender, receiver, source, destination, length=32, broadcast=False, negative=False,
             allow_rf_broadcast=False):
    sender.events.clear(); receiver.events.clear()
    payload = secrets.token_bytes(length)
    trace = ExchangeTrace(sender, receiver, source, 0xFFFC if broadcast else destination, payload,
                          'rf_isolation' if negative else 'broadcast' if broadcast else 'unicast')
    with trace_exchanges([trace]):
        return exchange_observed(trace, broadcast, negative, allow_rf_broadcast)


def exchange_observed(trace, broadcast, negative, allow_rf_broadcast):
    sender, receiver, payload, source = trace.sender, trace.receiver, trace.payload, trace.source
    transaction = trace.send()
    if negative:
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            for c in (sender, receiver):
                c.pump(0.05)
            if received(receiver, payload, source):
                raise AssertionError('RF isolation failed: native radio delivered the challenge with Ethernet forwarding disabled')
        trace.validate(expected_status=None, expected_deliveries=0)
        return
    wait_until(lambda: received(receiver, payload, source) and (broadcast or confirmation(sender, transaction)), [sender, receiver])
    # Observe duplicates, including an RF/broadcast retransmission after the first AF.
    trace.phase = 'observe_duplicates'
    deadline = time.monotonic() + (1.0 if broadcast else 0.25)
    while time.monotonic() < deadline:
        sender.pump(0.02); receiver.pump(0.02)
    if not broadcast:
        trace.validate()
    else:
        assert received(receiver, payload, source) == 1, 'Duplicate AF application delivery'
    trace.phase = 'counters'
    after = [sender.stats(), receiver.stats()]
    delta = trace.counters(after)
    evidence = dict(source='0x%04X' % source, destination='0x%04X' % trace.destination,
                    af_deliveries=1, sender=delta[0], receiver=delta[1])
    def require(condition, message):
        if not condition:
            raise AssertionError(message)
    require(delta[0]['tx'] > 0, 'No Ethernet TX evidence for the delivered challenge')
    if broadcast:
        require(delta[0]['broadcast_tx'] > 0, 'No Ethernet broadcast TX evidence')
        require(not any(d[k] for d in delta for k in ('queue_fail', 'alloc_fail', 'overflow', 'reject')) and
                not delta[0]['failed'], 'Broadcast admission or queue failure')
        # With radios in the same RF zone, the RF copy can reach NWK first and
        # be rebroadcast before the wired copy arrives. BH_DUPLICATE then means
        # successful suppression, not a failed IP leg. Isolated acceptance must
        # still prove a new wired admission and never uses this alternative.
        duplicate = allow_rf_broadcast and delta[1]['duplicate'] > 0 and delta[0]['accepted'] > 0
        require(delta[1]['rx'] > 0 or duplicate, 'No Ethernet broadcast admission or acknowledged duplicate suppression')
        evidence['wired_result'] = 'admitted' if delta[1]['rx'] > 0 else 'duplicate_suppressed'
    else:
        require(delta[1]['rx'] > 0, 'No Ethernet RX evidence for the delivered challenge')
    return dict(bytes=len(payload), evidence=evidence)


def reject_auth(host, key, identity='czc-admin-v1', port=7444):
    # A timeout/occupied port is NOT evidence that a bad PSK was rejected.
    try:
        with connect(host, key, identity, port) as conn:
            conn.sendall(packet(0x3F, 1)); conn.recv(1)
    except ssl.SSLError:
        return
    raise AssertionError('Unauthorised TLS client was not rejected by a TLS alert')
