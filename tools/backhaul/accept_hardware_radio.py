"""Hardware acceptance worker, combined with czc_pair.py by the standalone builder.
No provisioning, NV writes, resets, files, or connection to Z2M's endpoint 1.
"""

def server_hello_seen(records):
    """Require a TLS 1.3 ServerHello, not an occupied-port TCP close."""
    handshakes = bytearray()
    offset = 0
    while offset + 5 <= len(records):
        kind, major = records[offset], records[offset + 1]
        length = int.from_bytes(records[offset+3:offset+5], 'big')
        if major != 3 or length > 18432 or offset + 5 + length > len(records):
            break
        if kind == 20:  # Following handshake messages are encrypted.
            break
        if kind == 22:
            handshakes.extend(records[offset+5:offset+5+length])
        offset += 5 + length
    offset = 0
    while offset + 4 <= len(handshakes):
        length = int.from_bytes(handshakes[offset+1:offset+4], 'big')
        if offset + 4 + length > len(handshakes):
            break
        if handshakes[offset] == 2 and length >= 40:
            body = handshakes[offset+4:offset+4+length]
            if body[:2] != b'\x03\x03':
                return False
            pos = 35 + body[34]  # legacy session ID
            if pos + 5 > len(body) or body[pos:pos+3] != b'\x13\x01\x00':
                return False
            ext_length = int.from_bytes(body[pos+3:pos+5], 'big')
            pos += 5
            if pos + ext_length != len(body):
                return False
            while pos + 4 <= len(body):
                kind = int.from_bytes(body[pos:pos+2], 'big')
                size = int.from_bytes(body[pos+2:pos+4], 'big')
                pos += 4
                if pos + size > len(body):
                    return False
                if kind == 43:
                    return body[pos:pos+size] == b'\x03\x04'
                pos += size
            return False
        offset += 4 + length
    return False


def denied_handshake(host, key, identity, port):
    incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
    tls = tls_context(key, identity).wrap_bio(incoming, outgoing, server_side=False)
    records = bytearray()
    with socket.create_connection((host, port), timeout=5) as raw:
        end = time.monotonic() + 8
        while time.monotonic() < end:
            try:
                tls.do_handshake()
                raise AssertionError('Unauthorised TLS handshake accepted')
            except ssl.SSLWantReadError:
                pass
            except ssl.SSLError as exc:
                if 'ALERT' in (str(getattr(exc, 'reason', '')) + ' ' + str(exc)).upper():
                    return
                if isinstance(exc, ssl.SSLEOFError) and server_hello_seen(records):
                    return  # mbedTLS also closes on Finished authentication failure.
                raise AssertionError('TLS denial unproven: closed before ServerHello') from None
            while outgoing.pending:
                raw.sendall(outgoing.read())
            raw.settimeout(max(.01, min(3, end - time.monotonic())))
            chunk = raw.recv(4096)
            if not chunk:
                incoming.write_eof()
            else:
                records.extend(chunk)
                if len(records) > 32768:
                    raise AssertionError('Unexpected TLS transcript size')
                incoming.write(chunk)
        raise TimeoutError('TLS denial unproven: handshake deadline')


class TlsProbeFailure(AssertionError):
    def __init__(self, host, port, phase, exc):
        super().__init__(str(exc) or type(exc).__name__)
        self.probe = dict(host=host, port=port, phase=phase, error=type(exc).__name__)


def open_admin_after_reboot(host, key, port):
    # Only opening an admin connection may be retried. No radio commands or
    # negative authentication probes belong here. HTTP/peer readiness does not
    # imply readiness of the separate, single-client admin TLS listener.
    started = time.monotonic()
    deadline = started + 30
    errors = []
    transient = (ssl.SSLEOFError, ConnectionResetError, ConnectionAbortedError,
                 ConnectionRefusedError, BrokenPipeError, TimeoutError)
    for attempt in range(1, 9):
        try:
            client = Client(host, key, port, connect_deadline=deadline)
            return client, dict(host=host, attempts=attempt,
                                elapsed_ms=round((time.monotonic() - started) * 1000, 1),
                                errors=errors)
        except Exception as exc:
            errors.append(type(exc).__name__)
            remaining = deadline - time.monotonic()
            pause = min(6, 2 * attempt)
            if not isinstance(exc, transient) or attempt == 8 or remaining <= pause:
                failure = TlsProbeFailure(host, port, 'admin_ready_after_reboot', exc)
                failure.probe.update(attempts=attempt, errors=errors, budget_s=30,
                                     elapsed_ms=round((time.monotonic() - started) * 1000, 1))
                raise failure from None
            time.sleep(pause)


def check_tls_rejection(host, key, identity, port, good_key, good_identity='czc-admin-v1'):
    # Good PSK controls on the same listener bracket the negative challenge.
    # A busy port/EOF before ServerHello, connect error, or timeout is never PASS.
    phase = 'valid_psk_before'
    try:
        with connect(host, good_key, good_identity, port):
            pass
        time.sleep(.25)
        phase = 'wrong_psk' if key != good_key else 'wrong_identity'
        denied_handshake(host, key, identity, port)
        time.sleep(.25)
        phase = 'valid_psk_after'
        with connect(host, good_key, good_identity, port):
            pass
        time.sleep(.25)
    except Exception as exc:
        raise TlsProbeFailure(host, port, phase, exc) from None


def held_exchange(master, satellite, destination, disconnect=False):
    clients = [master, satellite]
    for c in clients:
        c.events.clear()
    master.hold(True)
    try:
        trace = ExchangeTrace(master, satellite, 0, destination, secrets.token_bytes(32),
                              'held_disconnect' if disconnect else 'held_timeout')
        trace.queue_observation = dict(hold_active=True)
        with trace_exchanges([trace]):
            tx = trace.send()
            trace.phase = 'wait_pending'
            wait_until(lambda: master.stats()['pending'] > 0, clients, 3)
            if disconnect:
                master.pause(True)
            trace.phase = 'wait_failed_confirm'
            wait_until(lambda: confirmation(master, tx), clients, 12)
            until = time.monotonic() + .4
            while time.monotonic() < until:
                for c in clients:
                    c.pump(.02)
            trace.validate(expected_status=0xf0, expected_deliveries=0)
            trace.queue_observation['pending_at_confirm'] = master.stats()['pending']

            # holdTx stops exports of ALL queued frames. A later broadcast can
            # remain pending after our transaction expires. Resume exports before
            # checking the shared queue, keeping this transaction's events intact.
            trace.phase = 'release_hold'
            master.hold(False)
            trace.queue_observation['hold_active'] = False
            if disconnect:
                master.pause(False)
            trace.phase = 'wait_pending_drain'
            deadline = time.monotonic() + 8
            while True:
                pending = master.stats()['pending']
                trace.queue_observation['pending_after_release'] = pending
                if not pending:
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError('Pending TX did not drain within 8 s after releasing hold')
                for c in clients:
                    c.pump(.03)
            trace.phase = 'wait_link_recovery'
            ready(clients)
            trace.phase = 'observe_after_release'
            until = time.monotonic() + .4
            while time.monotonic() < until:
                for c in clients:
                    c.pump(.02)
            # Resuming must not deliver the abandoned payload or produce another
            # confirmation (including a late success). Never clear its events here.
            trace.validate(expected_status=0xf0, expected_deliveries=0)
    finally:
        master.hold(False)
        master.pause(False)


def radio_accept(config):
    key = bytes.fromhex(config['key'])
    hosts = [config['master'], config['satellite']]
    clients, registered = [], []
    results = []
    active = 'connect'
    report = dict(verdict='FAIL', checks=results, rf_isolation=False, rf_isolation_result='NOT_RUN', deliveries=0)

    def check(name, fn):
        nonlocal active
        active = name
        print(name + ': ', end='', flush=True)
        fn()
        results.append(name)
        print('PASS', flush=True)

    def connect_nodes():
        for host in hosts:
            if config.get('post_reboot'):
                client, observation = open_admin_after_reboot(host, key, config['admin_port'])
                clients.append(client)
                if observation['attempts'] > 1:
                    report.setdefault('admin_recovery', []).append(observation)
            else:
                try:
                    clients.append(Client(host, key, config['admin_port']))
                except Exception as exc:
                    raise TlsProbeFailure(host, config['admin_port'], 'admin_connect', exc) from None

    def identities():
        info = []
        for c in clients:
            try:
                info.append(c.info())
            except OSError as exc:
                raise TlsProbeFailure(c.host, config['admin_port'], 'radio_info', exc) from None
        ready(clients)
        gate = [c.gateway() for c in clients]
        assert [g['role'] for g in gate] == ['master', 'satellite'], 'Incorrect Master/Satellite roles'
        assert [v['ieee'].lower().removeprefix('0x') for v in info] == config['ieees'], 'Unexpected radio IEEE'
        assert info[0]['state'] == 9 and info[0]['short'] == 0, 'Master network is not running'
        assert info[1]['state'] == 7 and 0 < info[1]['short'] < 0xfff8, 'Satellite is not a joined router'
        assert all(info[0][k] == info[1][k] for k in ('pan', 'channel', 'extpan')), 'Different Zigbee networks'
        assert all(g.get('debug') for g in gate), 'Enable Acceptance / debug mode on both CZCs'
        assert gate[1].get('bootstrap') == 2, 'Satellite IP bootstrap has not completed'
        assert clients[0].gateway()['peers_online'] == 1, 'Expected exactly one Satellite for this two-device test'
        clients[1].expected_mac = {0: 0}
        report['network'] = [{k: v[k] for k in ('ieee', 'short', 'pan', 'channel', 'extpan')} for v in info]
        report['ip_family'] = gate[1]['af']
        assert report['ip_family'] in (4, 6), 'Peer IP family is not known'
        if config.get('expect_family'):
            assert gate[1]['af'] == config['expect_family'], 'Peer TLS did not use the requested IP family'
        if config.get('baseline'):
            assert report['network'] == config['baseline'], 'Network identity changed after reboot'

    def register():
        for c in clients:
            registered.append(c)
            c.register()

    def transfer(i, j, size=32, **kwargs):
        info = report['network']
        if kwargs.get('broadcast'):
            # RF overlap is not excluded by this acceptance. A wired duplicate
            # still needs acknowledged suppression and exactly one AF delivery.
            kwargs['allow_rf_broadcast'] = True
        outcome = exchange(clients[i], clients[j], info[i]['short'], info[j]['short'], size, **kwargs)
        if kwargs.get('broadcast'):
            report.setdefault('broadcast', []).append(outcome['evidence']['wired_result'])
        if not kwargs.get('negative'):
            report['deliveries'] += 1

    def peer_auth():
        satellite = clients[1]
        satellite.pause(True)
        try:
            wait_until(lambda: not clients[0].gateway()['peer'], clients, 8)
            check_tls_rejection(hosts[0], bytes([key[0] ^ 1]) + key[1:], 'czc-peer-v2', config['peer_port'], key, 'czc-peer-v2')
            time.sleep(.25)
            check_tls_rejection(hosts[0], key, 'czc-admin-v1', config['peer_port'], key, 'czc-peer-v2')
        finally:
            satellite.pause(False)
        ready(clients)

    def duplex():
        for c in clients:
            c.events.clear()
        payloads = [secrets.token_bytes(32), secrets.token_bytes(32)]
        addresses = [v['short'] for v in report['network']]
        traces = [ExchangeTrace(clients[i], clients[1-i], addresses[i], addresses[1-i], payloads[i], 'duplex') for i in range(2)]
        with trace_exchanges(traces):
            tx = [t.send() for t in traces]
            wait_until(lambda: all(received(clients[1-i], payloads[i], addresses[i]) and confirmation(clients[i], tx[i]) for i in range(2)), clients)
            until = time.monotonic() + .4
            while time.monotonic() < until:
                for c in clients:
                    c.pump(.02)
            for t in traces:
                t.validate()
        report['deliveries'] += 2

    def held(disconnect):
        master, satellite = clients
        held_exchange(master, satellite, report['network'][1]['short'], disconnect)
        transfer(0, 1)

    def ownership():
        wait_until(lambda: all(not c.stats()['pending'] and not c.stats()['live'] for c in clients), clients, 8)
        after = [c.stats() for c in clients]
        assert all(a['alloc'] == a['free'] and all(a[k] == b[k] for k in ('alloc_fail', 'queue_fail', 'overflow')) for a, b in zip(after, before)), 'Memory or queue counters failed'
        assert all(c.gateway()['uart_ok'] and c.gateway()['peer'] and c.stats()['online'] for c in clients), 'UART or peer link not recovered'
        if config.get('expect_family'):
            assert clients[1].gateway()['af'] == config['expect_family'], 'Peer IP family changed during the test'
        report['resources'] = dict(live=0, pending=0, balanced=True)
        report['ethernet_frames'] = [{'tx': a['tx']-b['tx'], 'rx': a['rx']-b['rx']} for a, b in zip(after, before)]

    def admin_auth(i):
        # A node has one admin slot. Release our endpoint/session before probing
        # that listener, then restore them even if an authentication check fails.
        previous = clients[i]
        previous.checked(4, bytes([EP]), 4)
        registered.remove(previous)
        previous.close()
        time.sleep(.25)
        try:
            host = hosts[i]
            check_tls_rejection(host, bytes([key[0] ^ 1]) + key[1:], 'czc-admin-v1', config['admin_port'], key)
            time.sleep(.25)
            check_tls_rejection(host, key, 'czc-peer-v2', config['admin_port'], key)
            time.sleep(.25)
        finally:
            clients[i] = Client(hosts[i], key, config['admin_port'])
            clients[i].expected_mac = getattr(previous, 'expected_mac', {})
            registered.append(clients[i])
            clients[i].register()

    try:
        if config.get('post_reboot'):
            check('Готовность служебного TLS после перезагрузки', connect_nodes)
            check('Идентичности и общая сеть', identities)
        else:
            check('Идентичности и общая сеть', lambda: (connect_nodes(), identities()))
        before = [c.stats() for c in clients]
        check('Тестовый AF endpoint', register)
        if config['operation'] == 'full':
            print('RF-изоляция между CZC: NOT_RUN (проверка отключена)', flush=True)
            for i, host in enumerate(hosts):
                check('TLS admin ' + host, lambda i=i: admin_auth(i))
            check('PSK и identity межузлового TLS', peer_auth)
            for size in (16, 32, 80):
                check('Unicast туда/обратно, %s байт' % size, lambda size=size: (transfer(0, 1, size), transfer(1, 0, size)))
            check('Broadcast без дублей', lambda: (transfer(0, 1, broadcast=True), transfer(1, 0, broadcast=True)))
            check('Таймаут удержанного TX и восстановление', lambda: held(False))
            check('Обрыв при удержанном TX и восстановление', lambda: held(True))
            check('Пять одновременных обменов в обе стороны', lambda: [duplex() for _ in range(5)])
            check('Новая TLS-сессия и обмен', lambda: (clients[1].checked(5, subsystem=31), ready(clients), duplex()))
        else:
            check('Обмен после перезагрузки', duplex)
        check('Очереди, память и работоспособность UART', ownership)
        report['verdict'] = 'PASS'
    except Exception as exc:
        report.update(failed_check=active, reason=str(exc) or type(exc).__name__)
        if isinstance(exc, TlsProbeFailure):
            report['tls'] = exc.probe
        if isinstance(exc, ExchangeFailure):
            report['exchange'] = exc.evidence
        print('FAIL', flush=True)
        report['evidence'] = []
        for c in clients:
            try:
                report['evidence'].append(dict(host=c.host, gateway=c.gateway(), radio=c.info(), stats=c.stats()))
            except Exception as err:
                report['evidence'].append(dict(host=c.host, error=type(err).__name__))
    finally:
        for c in clients:
            try:
                c.hold(False)
                c.pause(False)
                if c in registered:
                    c.checked(4, bytes([EP]), 4)
            except Exception as exc:
                report['verdict'] = 'FAIL'
                report.setdefault('cleanup_errors', []).append(dict(host=c.host, reason=str(exc)))
            c.close()
    return report
