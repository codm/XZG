import contextlib
import importlib.util
import io
import itertools
import json
from pathlib import Path
import ssl
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

host = load('accept_host', ROOT/'tools/backhaul/accept_hardware.py')
host.MASTER, host.SATELLITE = '192.0.2.1', '192.0.2.2'
host.IEEES = ['00124b002e11424d', '00124b003cb89484']
host.SENSOR = '0xa4c13804c059ffff'
host.PSK = '01'*32

client_source = (ROOT/'tools/backhaul/czc_pair.py').read_text().split('\ndef acceptance(args, key):')[0]
ns = {'__name__': 'accept_worker'}
exec(client_source + '\n' + (ROOT/'tools/backhaul/accept_hardware_radio.py').read_text(), ns)


def settings(i):
    return dict(mode=i+1, psk=host.PSK, debug_mode=True, admin_port=7444, peer_port=7443,
                status=dict(own_ieee=host.IEEES[i], peer=True, uart_ok=True, radio_revision=20260917,
                            radio_protocol=2, peers=[dict(online=True, ieee=host.IEEES[1-i], ip=[host.MASTER, host.SATELLITE][1-i])]))


class Acceptance(unittest.TestCase):
    def setUp(self):
        self._globals = patch.dict(host.__dict__, MASTER='192.0.2.1', SATELLITE='192.0.2.2',
                                  IEEES=['00124b002e11424d', '00124b003cb89484'],
                                  SENSOR='0xa4c13804c059ffff', PSK='01'*32)
        self._globals.start()
        self.addCleanup(self._globals.stop)

    def test_discovered_identity_is_pinned_and_distinct(self):
        with patch.object(host, 'IEEES', [None, None]):
            host.pin_identity(0, '00:12:4B:00:00:00:00:01')
            with self.assertRaises(host.CheckFailed): host.pin_identity(1, host.IEEES[0])
            host.pin_identity(1, '00124b0000000002')
            with self.assertRaises(host.CheckFailed): host.pin_identity(0, '00124b0000000003')
            with self.assertRaises(host.CheckFailed): host.pin_identity(0, None)

    def test_full_requires_sensor_and_hosts_have_no_deployment_defaults(self):
        module = load('accept_defaults', ROOT/'tools/backhaul/accept_hardware.py')
        self.assertEqual((module.MASTER, module.SATELLITE, module.PSK, module.SENSOR), (None,)*4)
        self.assertEqual(module.IEEES, [None, None])
        for args in (['--mode', 'diagnose'], ['--mode', 'full', '--master', '192.0.2.1',
                     '--satellite', '192.0.2.2', '--psk', 'ab'*32]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                module.parse_args(args)

    def test_admin_tls_diagnostic_excludes_settings_and_secrets(self):
        value = dict(psk=host.PSK, status=dict(psk_tls_bytes=20400, admin_tls=dict(
            state='listening', failures=6, last_stage='setup', last_code=-0x7f00,
            open_free=79000, open_largest=21000, raw_client=False, psk=host.PSK, csrf='secret')))
        with patch.object(host, 'web_json', return_value=value):
            report = host.gateway_observation('fixture')
        self.assertEqual(report['admin_tls']['last_code'], -0x7f00)
        self.assertEqual(report['psk_tls_bytes'], 20400)
        self.assertNotIn(host.PSK, json.dumps(report))
        self.assertNotIn('secret', json.dumps(report))

    def test_diagnostic_http_filters_keys_and_handles_timeout(self):
        value = dict(boot='boot', version='17.5', wifi_rssi=-60,
                     heap=dict(free=10000, largest=5000, minimum=1000, psk=host.PSK), psk=host.PSK)
        with patch.object(host, 'web_json', return_value=value) as read:
            result = host.http_observation('fixture')
        self.assertEqual(result['http'], 'OK')
        read.assert_called_once_with('fixture', '/api/esp-update')
        self.assertNotIn(host.PSK, json.dumps(result))
        with patch.object(host, 'web_json', side_effect=TimeoutError('timed out')):
            result = host.http_observation('fixture')
        self.assertEqual((result['http'], result['error']), ('ERROR', 'TimeoutError'))

    def test_diagnose_detects_reboot_across_outage_and_never_changes_services(self):
        def observations(index):
            values = [dict(host=h, http='OK', ms=20, boot='initial') for h in (host.MASTER, host.SATELLITE)]
            if index == 1: values[0] = dict(host=host.MASTER, http='ERROR', ms=3000, error='TimeoutError')
            if index >= 2: values[0]['boot'] = 'restarted'
            return values
        output = io.StringIO()
        with patch.object(host, 'http_observations', side_effect=[observations(i) for i in range(5)]), \
             patch.object(host, 'advertised_addresses', side_effect=lambda h: dict(host=h, interfaces=[], candidates=[])), \
             patch.object(host, 'gateway_observation', return_value=dict(fault='none')), \
             patch.object(host, 'local_route', return_value={}), patch.object(host.time, 'sleep'), \
             patch.object(host, 'command') as command, patch.object(host, 'worker') as worker, \
             patch.object(host, 'reboot') as reboot, contextlib.redirect_stdout(output):
            self.assertEqual(host.diagnose(), 1)
        command.assert_not_called(); worker.assert_not_called(); reboot.assert_not_called()
        result = json.loads(output.getvalue().split('CZC_DIAG ', 1)[1])
        self.assertEqual(result['nodes'][0]['boot_changes'], 1)
        self.assertEqual(result['nodes'][0]['errors'], 1)
        self.assertEqual(result['nodes'][1]['ok'], 5)
        self.assertNotIn(host.PSK, output.getvalue())

    def test_diagnostic_filters_gateway_keys_and_bounds_discovery(self):
        cfg = settings(1)
        cfg.update(peer_host='192.0.2.1', token='TOKEN_MUST_NOT_APPEAR')
        cfg['status'].update(fault='peer_connect', connect_attempts=6, peer_tls_bytes=5000)
        with patch.object(host, 'web_json', return_value=cfg):
            result = host.gateway_observation(host.SATELLITE)
        self.assertEqual(result['peer_host'], '192.0.2.1')
        self.assertEqual(result['connect_attempts'], 6)
        self.assertNotIn(host.PSK, json.dumps(result))
        self.assertNotIn('TOKEN_MUST_NOT_APPEAR', json.dumps(result))
        interfaces = [
            dict(id='wifi', enabled=True, connected=True, ipv4='192.0.2.3', ipv6=[
                dict(address='fe80::1'), dict(address='2001:db8::160'), dict(address='2001:db8::161')]),
            dict(id='eth', enabled=True, connected=False, ipv4='192.0.2.2'),
            dict(id='ap', enabled=True, connected=True, ipv4='192.168.1.1')]
        with patch.object(host, 'web_json', return_value=dict(interfaces=interfaces)):
            result = host.advertised_addresses(host.SATELLITE)
        self.assertEqual(result['candidates'], [dict(host='192.0.2.3', interface='wifi', family=4),
                                                dict(host='2001:db8::160', interface='wifi', family=6)])
        for address in ('http://foreign.example', '255.255.255.255', '0.0.0.0', '127.0.0.1', '224.0.0.1', '::1', 'fe80::1%eth', None):
            with patch.object(host, 'web_json', return_value=dict(interfaces=[
                    dict(id='eth', enabled=True, connected=True, ipv4=address)])):
                self.assertEqual(host.advertised_addresses(host.SATELLITE)['candidates'], [])

    def test_diagnose_compares_addresses_preserves_mismatch_without_cookie_forwarding(self):
        def gateways(h):
            i = int(h == host.SATELLITE)
            return dict(own_ieee=host.IEEES[i], fault='none', peer=True)
        def networks(h):
            return dict(host=h, interfaces=[], candidates=[
                dict(host='2001:db8::' + ('1' if h == host.MASTER else '2'), interface='wifi', family=6)])
        base = [dict(host=h, http='OK', ms=20, boot='base') for h in (host.MASTER, host.SATELLITE)]
        counts = {}
        def aliases(h):
            self.assertNotIn(h, host.WEB_COOKIES)
            counts[h] = counts.get(h, 0) + 1
            # Initial mismatch remains visible after a later matching boot.
            return dict(host=h, http='OK', ms=2000 if h.endswith('2') else 25,
                        boot='other' if h.endswith('2') and counts[h] == 1 else 'base')
        output = io.StringIO()
        with patch.object(host, 'advertised_addresses', side_effect=networks), \
             patch.object(host, 'gateway_observation', side_effect=gateways), \
             patch.object(host, 'http_observations', return_value=base), \
             patch.object(host, 'http_observation', side_effect=aliases), \
             patch.object(host, 'local_route', return_value={}), patch.object(host.time, 'sleep'), \
             patch.object(host, 'command') as command, patch.object(host, 'worker') as worker, \
             patch.object(host, 'reboot') as reboot, contextlib.redirect_stdout(output):
            self.assertEqual(host.diagnose(), 1)
        command.assert_not_called(); worker.assert_not_called(); reboot.assert_not_called()
        result = json.loads(output.getvalue().split('CZC_DIAG ', 1)[1])
        self.assertEqual([a['identity'] for a in result['addresses']], ['SAME_BOOT', 'BOOT_MISMATCH'])
        self.assertEqual([a['ok'] for a in result['addresses']], [5, 5])
        self.assertEqual(result['addresses'][1]['max_ms'], 2000)
        self.assertNotIn(host.PSK, output.getvalue())

    def test_diagnostic_no_discovery_for_wrong_ieee_or_failed_api(self):
        with patch.object(host, 'web_json', side_effect=TimeoutError('not returned')):
            self.assertEqual(host.gateway_observation(host.MASTER), dict(error='TimeoutError'))
            self.assertEqual(host.advertised_addresses(host.MASTER)['error'], 'TimeoutError')
        with patch.object(host, 'advertised_addresses', side_effect=lambda h: dict(host=h, interfaces=[], candidates=[
                dict(host='2001:db8::99', interface='eth', family=6)])), \
             patch.object(host, 'gateway_observation', return_value=dict(own_ieee='bad')), \
             patch.object(host, 'http_observations', return_value=[
                 dict(host=h, http='OK', ms=20, boot='b') for h in (host.MASTER, host.SATELLITE)]), \
             patch.object(host, 'http_observation') as alias, patch.object(host.time, 'sleep'), \
             patch.object(host, 'local_route', return_value={}), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(host.diagnose(), 0)
        alias.assert_not_called()

    def test_direct_http_ignores_proxy_and_does_not_follow_redirects(self):
        import http.server
        import threading
        seen = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append(self.path)
                self.send_response(302 if self.path == '/redirect' else 200)
                self.send_header('Location', '/must-not-follow')
                self.end_headers()
                self.wfile.write(b'{"boot":"b"}')
            def log_message(self, *args): pass
        with http.server.HTTPServer(('127.0.0.1', 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            try:
                # A local HTTP fixture is the only network destination in this test.
                name = '127.0.0.1:' + str(server.server_port)
                with patch.dict(host.os.environ, http_proxy='http://127.0.0.1:1', no_proxy=''), \
                     patch.object(host.urllib.request, 'Request', wraps=host.urllib.request.Request) as request:
                    # web() receives literal IPv6 or unadorned host; inject fixture
                    # port through the URL constructor, not address classification.
                    original = request._mock_wraps
                    request.side_effect = lambda url, **kw: original(url.replace('fixture', name), **kw)
                    self.assertEqual(host.web_json('fixture', '/ok'), dict(boot='b'))
                    with self.assertRaises(host.urllib.error.HTTPError): host.web('fixture', '/redirect')
            finally:
                server.shutdown(); thread.join()
        self.assertEqual(seen, ['/ok', '/redirect'])

    def test_diagnose_reports_conflicting_advertisements_without_alias_probe(self):
        output = io.StringIO()
        with patch.object(host, 'advertised_addresses', side_effect=lambda h: dict(host=h, interfaces=[], candidates=[
                dict(host='2001:db8::99', interface='eth', family=6)])), \
             patch.object(host, 'gateway_observation', side_effect=lambda h: dict(
                 own_ieee=host.IEEES[int(h == host.SATELLITE)])), \
             patch.object(host, 'http_observations', return_value=[
                 dict(host=h, http='OK', ms=20, boot='b') for h in (host.MASTER, host.SATELLITE)]), \
             patch.object(host, 'http_observation') as alias, patch.object(host.time, 'sleep'), \
             patch.object(host, 'local_route', return_value={}), contextlib.redirect_stdout(output):
            self.assertEqual(host.diagnose(), 1)
        alias.assert_not_called()
        result = json.loads(output.getvalue().split('CZC_DIAG ', 1)[1])
        self.assertEqual(result['conflicting_advertisements'], ['2001:db8::99'])

    def test_diagnose_cli_does_not_require_key_compose_or_worker_source(self):
        with patch.object(host, 'diagnose', return_value=0) as run, \
             patch.object(host, 'command') as command, patch.dict(host.__dict__, RADIO_SOURCE=None):
            self.assertEqual(host.cli(['--mode', 'diagnose', '--master', host.MASTER, '--satellite', host.SATELLITE, '--compose-dir', '/nonexistent-czc-fixture']), 0)
        command.assert_not_called(); run.assert_called_once()

    def test_tls_failure_retains_stage_and_does_not_retry(self):
        accepted = contextlib.nullcontext()
        for fail_at, phase in ((0, 'valid_psk_before'), (1, 'valid_psk_after')):
            calls = [TimeoutError('handshake timeout')] if fail_at == 0 else [accepted, TimeoutError('handshake timeout')]
            with patch.dict(ns, connect=unittest.mock.Mock(side_effect=calls), denied_handshake=lambda *a: None), \
                 patch.object(ns['time'], 'sleep'):
                with self.assertRaises(ns['TlsProbeFailure']) as raised:
                    ns['check_tls_rejection']('fixture', b'x'*32, 'bad', 7444, b'y'*32)
                self.assertEqual(ns['connect'].call_count, fail_at+1)
            self.assertEqual(raised.exception.probe, dict(host='fixture', port=7444, phase=phase, error='TimeoutError'))

    def test_admin_readiness_recovers_transient_open_only(self):
        now, pauses = [100.0], []
        def sleep(seconds):
            pauses.append(seconds)
            now[0] += seconds
        client = object()
        factory = unittest.mock.Mock(side_effect=[ssl.SSLEOFError('booting'), ConnectionResetError(), client])
        with patch.dict(ns, Client=factory), patch.object(ns['time'], 'monotonic', side_effect=lambda: now[0]), \
             patch.object(ns['time'], 'sleep', side_effect=sleep):
            result, observed = ns['open_admin_after_reboot']('fixture', b'x'*32, 7444)
        self.assertIs(result, client)
        self.assertEqual(pauses, [2, 4])
        self.assertEqual(observed, dict(host='fixture', attempts=3, elapsed_ms=6000,
                                       errors=['SSLEOFError', 'ConnectionResetError']))
        self.assertTrue(all(call.kwargs['connect_deadline'] == 130 for call in factory.call_args_list))

    def test_admin_readiness_stays_bounded_and_rejects_fatal_tls(self):
        for exc, cost, retriable in ((ssl.SSLEOFError('closed'), 0, True),
                                    (TimeoutError('timeout'), 10, True),
                                    (ConnectionRefusedError(), 0, True),
                                    (ssl.SSLError('TLSV1_ALERT_UNKNOWN_PSK_IDENTITY'), 0, False),
                                    (ValueError('invalid configuration'), 0, False)):
            with self.subTest(error=type(exc).__name__):
                now, pauses = [0.0], []
                def sleep(seconds):
                    pauses.append(seconds)
                    now[0] += seconds
                def fail(*args, connect_deadline):
                    now[0] += min(cost, connect_deadline - now[0])
                    raise exc
                with patch.dict(ns, Client=unittest.mock.Mock(side_effect=fail)), \
                     patch.object(ns['time'], 'monotonic', side_effect=lambda: now[0]), \
                     patch.object(ns['time'], 'sleep', side_effect=sleep):
                    with self.assertRaises(ns['TlsProbeFailure']) as raised:
                        ns['open_admin_after_reboot']('satellite', b'x'*32, 7444)
                    count = ns['Client'].call_count
                self.assertLessEqual(now[0], 30)
                self.assertLessEqual(count, 8)
                self.assertEqual(count, len(pauses) + 1)
                self.assertEqual(count > 1, retriable)
                self.assertTrue(all(pause >= 2 for pause in pauses))
                probe = raised.exception.probe
                self.assertEqual((probe['host'], probe['phase'], probe['attempts']),
                                 ('satellite', 'admin_ready_after_reboot', count))
                self.assertEqual(probe['errors'], [type(exc).__name__] * count)

    def test_admin_transport_closes_owned_socket_and_bounds_handshake(self):
        for phase in ('tcp', 'wrap', 'handshake', 'deadline', 'success'):
            with self.subTest(phase=phase):
                raw, wrapped = unittest.mock.Mock(), unittest.mock.Mock()
                ctx = unittest.mock.Mock()
                ctx.wrap_socket.return_value = wrapped
                now = [0.0]
                def tcp(*args, timeout):
                    self.assertEqual(timeout, 5)
                    now[0] = 5 if phase == 'deadline' else 4
                    if phase == 'tcp': raise TimeoutError()
                    return raw
                if phase == 'wrap': ctx.wrap_socket.side_effect = ssl.SSLError('setup')
                if phase == 'handshake': wrapped.do_handshake.side_effect = ssl.SSLEOFError('closed')
                with patch.object(ns['socket'], 'create_connection', side_effect=tcp), \
                     patch.dict(ns, tls_context=lambda *args: ctx), \
                     patch.object(ns['time'], 'monotonic', side_effect=lambda: now[0]):
                    if phase == 'success':
                        self.assertIs(ns['connect']('fixture', b'x'*32, deadline=5), wrapped)
                    else:
                        with self.assertRaises(OSError):
                            ns['connect']('fixture', b'x'*32, deadline=5)
                if phase in ('handshake', 'success'):
                    self.assertEqual(wrapped.settimeout.call_args_list[0].args, (1,))
                    ctx.wrap_socket.assert_called_once_with(raw, server_hostname=None, do_handshake_on_connect=False)
                if phase in ('handshake', 'deadline'): wrapped.close.assert_called_once()
                if phase == 'wrap': raw.close.assert_called_once()
                if phase in ('tcp', 'deadline', 'wrap'): wrapped.do_handshake.assert_not_called()
                if phase == 'success':
                    wrapped.close.assert_not_called()
                    raw.close.assert_not_called()

    def test_worker_does_not_retry_admin_rpc_eof_after_open(self):
        master, satellite = unittest.mock.Mock(), unittest.mock.Mock()
        master.host, satellite.host = 'master', 'satellite'
        master.info.return_value = dict(ieee='0x1')
        satellite.info.side_effect = ssl.SSLEOFError('RPC closed')
        master.gateway.return_value = master.stats.return_value = {}
        factory = unittest.mock.Mock(side_effect=[master, satellite])
        cfg = dict(key='ab'*32, master='master', satellite='satellite', admin_port=7444,
                   operation='smoke', post_reboot=True)
        with patch.dict(ns, Client=factory), patch.object(ns['time'], 'sleep') as sleep, \
             contextlib.redirect_stdout(io.StringIO()):
            report = ns['radio_accept'](cfg)
        self.assertEqual(report['verdict'], 'FAIL')
        self.assertEqual(report['checks'], ['Готовность служебного TLS после перезагрузки'])
        self.assertEqual((report['tls']['host'], report['tls']['phase']), ('satellite', 'radio_info'))
        self.assertEqual(factory.call_count, 2)
        sleep.assert_not_called()
        for client in (master, satellite):
            client.register.assert_not_called()
            client.send.assert_not_called()
            client.close.assert_called_once()

    def test_initial_admin_open_is_not_retried(self):
        cfg = dict(key='ab'*32, master='master', satellite='satellite', admin_port=7444, operation='full')
        factory = unittest.mock.Mock(side_effect=ssl.SSLEOFError('closed'))
        with patch.dict(ns, Client=factory), patch.object(ns['time'], 'sleep') as sleep, \
             contextlib.redirect_stdout(io.StringIO()):
            report = ns['radio_accept'](cfg)
        self.assertEqual((report['verdict'], report['tls']['phase']), ('FAIL', 'admin_connect'))
        factory.assert_called_once()
        sleep.assert_not_called()

    def test_http_preflight_checks_ready_api_beside_idle_socket_without_key_dump(self):
        status = dict(boot='stable', version='test-build', state='idle', psk=host.PSK)
        closed = []
        @contextlib.contextmanager
        def idle(address, **kwargs):
            self.assertEqual(address[1], 80)
            self.assertEqual(kwargs['timeout'], 3)
            try: yield
            finally: closed.append(address[0])
        output = io.StringIO()
        with patch.object(host.socket, 'create_connection', side_effect=idle), patch.object(host.time, 'sleep'), \
             patch.object(host, 'web_json', return_value=status) as read, contextlib.redirect_stdout(output):
            results = host.http_preflight()
        self.assertEqual(closed, [host.MASTER, host.SATELLITE])
        self.assertEqual(read.call_count, 6)
        self.assertTrue(all(call.args[1] == '/api/esp-update' for call in read.call_args_list))
        self.assertTrue(all(item['result']=='PASS' and item['requests']==3 for item in results))
        self.assertNotIn(host.PSK, output.getvalue()+json.dumps(results))

    def test_http_preflight_rejects_slow_failed_rebooting_or_updating_api(self):
        good = dict(boot='stable', version='test-build', state='idle')
        for values, times in [([good], [0, 6.1]), ([TimeoutError('timed out')], None),
                              ([good, dict(good, boot='new')], None),
                              ([dict(good, state='uploading')], None), ([{}], None)]:
            with self.subTest(values=values), contextlib.ExitStack() as stack:
                connection = stack.enter_context(patch.object(host.socket, 'create_connection'))
                stack.enter_context(patch.object(host.time, 'sleep'))
                stack.enter_context(patch.object(host, 'web_json', side_effect=values))
                if times: stack.enter_context(patch.object(host.time, 'monotonic', side_effect=times))
                with self.assertRaisesRegex(host.CheckFailed, 'Приёмка радио не начата'):
                    host.http_preflight()
                connection.return_value.__exit__.assert_called_once()

    def test_http_failure_does_not_stop_services_or_reboot_devices(self):
        output = io.StringIO()
        with patch.object(host, 'command') as command, patch.object(host, 'compose_preflight'), \
             patch.object(host, 'http_preflight', side_effect=host.CheckFailed('HTTP stalled')), \
             patch.object(host, 'worker') as worker, patch.object(host, 'reboot') as reboot, \
             contextlib.redirect_stdout(output):
            self.assertEqual(host.main('full'), 1)
        self.assertEqual(command.call_args_list, [unittest.mock.call(host.COMPOSE+['version'])])
        worker.assert_not_called(); reboot.assert_not_called()
        self.assertIn('HTTP stalled', output.getvalue())

    def test_web_identity_and_no_keys_in_errors(self):
        nodes = [settings(0), settings(1)]
        with patch.object(host, 'web_json', side_effect=lambda ip: nodes[[host.MASTER, host.SATELLITE].index(ip)]):
            self.assertEqual(len(host.pair_snapshot()), 2)
            nodes[1]['status']['peers'][0]['ieee'] = host.IEEES[1]
            with self.assertRaisesRegex(host.CheckFailed, 'взаимного соединения'):
                host.pair_snapshot()
            nodes[1] = settings(1)
            nodes[1]['psk'] = '11'*32
            with self.assertRaises(host.CheckFailed) as err:
                host.pair_snapshot()
            self.assertNotIn(nodes[1]['psk'], str(err.exception))
            self.assertNotIn(host.PSK, str(err.exception))

    def test_ipv6_peer_addresses_are_numeric_and_require_actual_family(self):
        seeds = ('2001:db8::17', '2001:db8::9b')
        with patch.object(host, 'MASTER', seeds[0]), patch.object(host, 'SATELLITE', seeds[1]), patch.object(host, 'PEER_FAMILY', 6):
            nodes = [settings(0), settings(1)]
            for i, value in enumerate(nodes):
                value['status']['peers'][0].update(ip=host.ipaddress.ip_address(seeds[1-i]).exploded.upper(), af=6)
            with patch.object(host, 'web_json', side_effect=lambda h: nodes[seeds.index(h)]), patch.object(host, 'advertised_addresses') as discover:
                self.assertEqual(len(host.pair_snapshot()), 2)
                discover.assert_not_called()
                nodes[1]['status']['peers'][0]['af'] = 4
                with self.assertRaisesRegex(host.CheckFailed, 'должен использовать IPv6'):
                    host.pair_snapshot()
        self.assertEqual(host.normalized_ip('::FFFF:192.0.2.1'), '192.0.2.1')
        self.assertIsNone(host.normalized_ip('not-an-ip'))

    def test_control_address_may_differ_from_verified_peer_address(self):
        nodes = [settings(0), settings(1)]
        nodes[0]['status']['peers'][0]['ip'] = '2001:db8::9b'
        with patch.object(host, 'web_json', side_effect=lambda h: nodes[[host.MASTER, host.SATELLITE].index(h)]), \
             patch.object(host, 'advertised_addresses', return_value={'candidates':[{'host':'2001:db8::9b'}]}):
            self.assertEqual(len(host.pair_snapshot()), 2)
        with patch.object(host, 'web_json', side_effect=lambda h: nodes[[host.MASTER, host.SATELLITE].index(h)]), \
             patch.object(host, 'advertised_addresses', return_value={'candidates':[]}):
            with self.assertRaisesRegex(host.CheckFailed, 'адрес peer'):
                host.pair_snapshot()

    def test_link_local_peer_is_verified_without_probing_link_local_addresses(self):
        seeds = ('2001:db8:6716:a7eb:5e01:3bff:fecf:7a17', '2001:db8:6716:a7eb:6a25:ddff:fee9:429b')
        local = ('fe80::5e01:3bff:fecf:7a17', 'fe80::6a25:ddff:fee9:429b')
        for scoped in (False, True):
            with self.subTest(scoped=scoped), patch.object(host, 'MASTER', seeds[0]), patch.object(host, 'SATELLITE', seeds[1]), \
                 patch.object(host, 'PEER_FAMILY', 6):
                nodes = [settings(0), settings(1)]
                for i, cfg in enumerate(nodes):
                    cfg['status']['peers'][0].update(ip=local[1-i].upper() + ('%2' if scoped else ''), af=6)
                def web(address, path='/api/backhaul'):
                    self.assertIn(address, seeds)
                    i = seeds.index(address)
                    if path == '/api/backhaul':
                        return nodes[i]
                    self.assertEqual(path, '/api/network/status')
                    return dict(interfaces=[dict(id='eth', enabled=True, connected=True, ipv4='192.0.2.'+str(i+1),
                                                ipv6=[dict(address=local[i]), dict(address=seeds[i])])])
                with patch.object(host, 'web_json', side_effect=web), patch.object(host, 'http_observation') as probe:
                    self.assertEqual(len(host.pair_snapshot()), 2)
                probe.assert_not_called()

    def test_membership_includes_all_ipv6_but_diagnostic_probes_stay_bounded(self):
        network = dict(interfaces=[dict(id='eth', enabled=True, connected=True, ipv4='192.0.2.2', ipv6=[
            dict(address='fe80::2'), dict(address='2001:db8::2'), dict(address='2001:db8:1::2')]),
            dict(id='wifi', enabled=False, connected=True, ipv4='192.0.2.3', ipv6=[dict(address='fe80::3')])])
        with patch.object(host, 'web_json', return_value=network):
            inventory = host.advertised_addresses(host.SATELLITE)
        self.assertEqual([v['host'] for v in inventory['addresses']],
                         ['192.0.2.2', 'fe80::2', '2001:db8::2', '2001:db8:1::2'])
        self.assertEqual([v['host'] for v in inventory['candidates']], ['192.0.2.2', '2001:db8::2'])
        nodes = [settings(0), settings(1)]
        nodes[0]['status']['peers'][0]['ip'] = '2001:db8:1::2'
        with patch.object(host, 'web_json', side_effect=lambda h: nodes[[host.MASTER, host.SATELLITE].index(h)]), \
             patch.object(host, 'advertised_addresses', return_value=inventory):
            self.assertEqual(len(host.pair_snapshot()), 2)

    def test_unknown_peer_or_failed_inventory_is_diagnosed_without_mutations_or_secrets(self):
        nodes = [settings(0), settings(1)]
        nodes[0]['status']['peers'][0]['ip'] = 'FE80::BAD'
        for inventory, reason in ((dict(addresses=[dict(host='fe80::2')], candidates=[]), 'fe80::bad'),
                                  (dict(addresses=[], candidates=[], error='TimeoutError'), 'TimeoutError')):
            output = io.StringIO()
            with self.subTest(inventory=inventory), patch.object(host, 'web_json', side_effect=lambda h: nodes[[host.MASTER, host.SATELLITE].index(h)]), \
                 patch.object(host, 'advertised_addresses', return_value=inventory), patch.object(host, 'command') as command, \
                 patch.object(host, 'compose_preflight'), patch.object(host, 'http_preflight', return_value=[]), \
                 patch.object(host, 'worker') as worker, patch.object(host, 'reboot') as reboot, contextlib.redirect_stdout(output):
                self.assertEqual(host.main('full'), 1)
            command.assert_called_once_with(host.COMPOSE+['version'])
            worker.assert_not_called(); reboot.assert_not_called()
            self.assertIn(reason, output.getvalue())
            self.assertNotIn(host.PSK, output.getvalue())

    def test_admin_ipv4_is_same_interface_and_same_boot_without_config_writes(self):
        seeds = ('2001:db8::17', '2001:db8::9b')
        ips = ('192.0.2.17', '192.0.2.155')
        def discover(seed):
            return dict(candidates=[dict(host=seed, interface='eth', family=6),
                                    dict(host=ips[seeds.index(seed)], interface='eth', family=4),
                                    dict(host='192.0.2.99', interface='wifi', family=4)])
        def observed(address):
            return dict(http='OK', boot=str((seeds if address in seeds else ips).index(address)))
        with patch.object(host, 'MASTER', seeds[0]), patch.object(host, 'SATELLITE', seeds[1]), patch.object(host, 'ADMIN_FAMILY', 4), \
             patch.object(host, 'advertised_addresses', side_effect=discover), patch.object(host, 'http_observation', side_effect=observed), \
             patch.object(host, 'command') as command:
            self.assertEqual(host.admin_addresses(), list(ips))
            command.assert_not_called()
            def second_slaac(seed):
                inventory = discover(seed)['candidates']
                return dict(addresses=inventory,
                            candidates=[dict(host='2001:db8::99', interface='eth', family=6), inventory[1]])
            with patch.object(host, 'advertised_addresses', side_effect=second_slaac):
                self.assertEqual(host.admin_addresses(), list(ips))
            with patch.object(host, 'http_observation', side_effect=[dict(http='OK', boot='a'), dict(http='OK', boot='b')]):
                with self.assertRaisesRegex(host.CheckFailed, 'тот же CZC'):
                    host.admin_addresses()
            with patch.object(host, 'advertised_addresses', return_value={'candidates':[]}):
                with self.assertRaisesRegex(host.CheckFailed, 'того же интерфейса'):
                    host.admin_addresses()

    def test_full_skips_rf_challenge_but_keeps_auth_exchange_and_cleanup(self):
        for failed_phase in ('auth', 'broadcast'):
            with self.subTest(failed_phase=failed_phase):
                calls, clients, paused = [], [], [False, False]
                class Fake:
                    def __init__(self, address, key, port):
                        self.host=address; self.i=[host.MASTER,host.SATELLITE].index(address)
                        self.closed=False; self.events=[]; clients.append(self)
                    def info(self):
                        return dict(state=9 if self.i==0 else 7, short=self.i, ieee='0x'+host.IEEES[self.i], pan=42, channel=11, extpan='same')
                    def gateway(self):
                        return dict(role='master' if self.i==0 else 'satellite', debug=True, bootstrap=2, peer=not any(paused), peers_online=1, af=6, uart_ok=True)
                    def stats(self): return dict(online=1)
                    def register(self): calls.append(('register', self.i))
                    def pause(self, value):
                        paused[self.i]=value; calls.append(('pause', self.i, value))
                    def hold(self, value): calls.append(('hold', self.i, value))
                    def checked(self, cmd, data=b'', subsystem=31):
                        assert not self.closed
                        calls.append(('command', self.i, cmd, data, subsystem))
                    def close(self): self.closed=True; calls.append(('close', self.i))
                def exchange(sender, receiver, *args, **kw):
                    self.assertFalse(kw.get('negative'))
                    self.assertEqual(failed_phase, 'broadcast')
                    self.assertEqual(len([c for c in clients if not c.closed]), 2)
                    self.assertGreaterEqual(len([c for c in calls if c[0]=='auth']), 6)
                    calls.append(('exchange', sender.i, receiver.i, kw.get('broadcast', False)))
                    if kw.get('broadcast'):
                        self.assertTrue(kw['allow_rf_broadcast'])
                        raise AssertionError('stop at broadcast fixture')
                def auth(*args):
                    calls.append(('auth',))
                    if failed_phase == 'auth':
                        raise AssertionError('bad key accepted fixture')
                cfg=dict(key=host.PSK, master=host.MASTER, satellite=host.SATELLITE, admin_port=7444,
                         ieees=host.IEEES, operation='full', expect_family=6, peer_port=7443)
                with patch.dict(ns, Client=Fake, exchange=exchange, check_tls_rejection=auth), \
                     patch.object(ns['time'], 'sleep'), contextlib.redirect_stdout(io.StringIO()):
                    report=ns['radio_accept'](cfg)
                self.assertEqual(report['verdict'], 'FAIL')
                self.assertTrue(all(c.closed for c in clients))
                for i in range(2):
                    self.assertIn(('pause',i,False), calls)
                    self.assertIn(('command',i,4,bytes([240]),4), calls)
                self.assertEqual(report['rf_isolation_result'], 'NOT_RUN')
                self.assertFalse(report['rf_isolation'])
                self.assertNotIn(('pause', 0, True), calls)
                self.assertFalse(any(c[0]=='command' and c[2]==3 and c[4]==31 for c in calls))
                self.assertIn(('auth',), calls)
                if failed_phase == 'auth':
                    self.assertFalse(any(c[0]=='exchange' for c in calls))
                    self.assertEqual(report['failed_check'], 'TLS admin '+host.MASTER)
                else:
                    self.assertEqual(len(clients), 4)
                    self.assertEqual(report['deliveries'], 6)
                    self.assertEqual(report['failed_check'], 'Broadcast без дублей')

    def test_server_hello_and_positive_controls_required_for_tls_denial(self):
        body = b'\x03\x03' + bytes(32) + b'\x00\x13\x01\x00\x00\x06\x00\x2b\x00\x02\x03\x04'
        hello = b'\x02' + len(body).to_bytes(3, 'big') + body
        record = b'\x16\x03\x03' + len(hello).to_bytes(2, 'big') + hello
        self.assertTrue(ns['server_hello_seen'](record))
        for bad in (b'', record[:4], record[:-1], record[:-1]+b'\x03',
                    b'\x15\x03\x03\x00\x02\x02\x28'):
            self.assertFalse(ns['server_hello_seen'](bad))
        class Accepted:
            def __enter__(self): return self
            def __exit__(self, *args): pass
        calls = []
        with patch.dict(ns, connect=lambda *a: (calls.append('good') or Accepted()),
                        denied_handshake=lambda *a: calls.append('bad')), patch.object(ns['time'], 'sleep'):
            ns['check_tls_rejection']('unused', b'x'*32, 'bad', 1, b'y'*32)
        self.assertEqual(calls, ['good', 'bad', 'good'])
        for failure in (TimeoutError('busy'), AssertionError('EOF before ServerHello')):
            with patch.dict(ns, connect=lambda *a: Accepted(),
                            denied_handshake=lambda *a, e=failure: (_ for _ in ()).throw(e)), patch.object(ns['time'], 'sleep'):
                with self.assertRaises((AssertionError, OSError)):
                    ns['check_tls_rejection']('unused', b'x'*32, 'bad', 1, b'y'*32)

    def test_reboot_needs_changed_boot_before_link(self):
        ticks = iter([0, 1, 91])
        with patch.object(host.time, 'monotonic', side_effect=lambda: next(ticks)), patch.object(host.time, 'sleep'), \
             patch.object(host, 'web', return_value=b'ok'), patch.object(host, 'boot', return_value='same'), \
             patch.object(host, 'wait_link') as ready:
            with self.assertRaises(host.CheckFailed): host.reboot([host.MASTER])
            ready.assert_not_called()
        with patch.object(host, 'web', return_value=b'ok'), patch.object(host, 'boot', side_effect=['old', 'new']), \
             patch.object(host, 'wait_link') as ready:
            host.reboot([host.MASTER]); ready.assert_called_once()

    def run_host(self, full, failure=False, manual=True,
                 running=('mosquitto', 'zigbee2mqtt'), existing=('mosquitto', 'zigbee2mqtt'),
                 services=('mosquitto', 'zigbee2mqtt'), mode=None, stop_fails=False, family=4,
                 failure_operation=None, admin_recovery=None):
        calls = []
        active, containers = set(running), set(existing)
        def command(argv, **kwargs):
            calls.append(argv)
            service = argv[-1]
            if '--services' in argv: return '\n'.join(services)
            if '-q' in argv: return service+'-fixture' if service in containers else ''
            if argv[:2] == ['docker', 'inspect']:
                name = service.removesuffix('-fixture')
                return json.dumps(dict(Running=name in active, Restarting=False,
                                       StartedAt='2026-09-10T10:00:00Z', Health={'Status':'healthy'}))
            if 'stop' in argv:
                if not stop_fails: active.discard(service)
                return ''
            if 'up' in argv or 'start' in argv:
                containers.add(service); active.add(service)
                return ''
            if 'logs' in argv: return 'Zigbee2MQTT started!' if service in active else ''
            return '{}'
        def worker(config, broker):
            calls.append(['worker'])
            self.assertIn('mosquitto', active)
            self.assertNotIn('zigbee2mqtt', active)
            self.assertEqual(broker, 'mosquitto-fixture')
            self.assertNotIn('rf_ready', config)
            self.assertEqual(bool(config.get('post_reboot')), config['operation'] == 'smoke')
            if failure_operation is None or config['operation'] == failure_operation:
                if isinstance(failure, Exception): raise failure
                if failure: raise host.CheckFailed('injected failure')
            result = dict(verdict='PASS', checks=['test'], deliveries=1, resources={'live':0}, rf_isolation=False,
                        rf_isolation_result='NOT_RUN',
                        ip_family=family, network=[{'ieee':'fixture'}])
            if config['operation'] == 'smoke' and admin_recovery:
                result['admin_recovery'] = admin_recovery
            return result
        def sensor(report):
            if manual:
                for name in ('sensor_initial', 'sensor_without_satellite', 'sensor_recovery'):
                    report[name] = 'MANUAL_CONFIRMED'
        output = io.StringIO()
        with patch.object(host, 'command', side_effect=command), patch.object(host, 'pair_snapshot', return_value=[settings(0), settings(1)]), \
             patch.object(host, 'http_preflight', return_value=[]), \
             patch.object(host, 'boot', return_value='boot'), patch.object(host, 'ask', side_effect=AssertionError('unexpected input')), \
             patch.object(host, 'worker', side_effect=worker), patch.object(host, 'reboot'), \
             patch.object(host, 'wait_link'), patch.object(host, 'sensor_checks', side_effect=sensor), \
             patch.object(host, 'z2m_start', wraps=host.z2m_start) as start, contextlib.redirect_stdout(output):
            code = host.main(mode if mode is not None else ('full' if full else 'local'))
        self.assertNotIn(host.PSK, output.getvalue())
        result = json.loads(next(line[len('CZC_RESULT '):] for line in output.getvalue().splitlines() if line.startswith('CZC_RESULT ')))
        return code, result, start.call_count, calls

    def test_complete_result_requires_full_mode_and_physical_confirmation_without_rf_gate(self):
        code, result, _, _ = self.run_host(True)
        self.assertEqual((code, result['verdict']), (0, 'PASS_TWO_DEVICES_AND_SENSOR'))
        self.assertEqual(result['rf_isolation'], 'NOT_RUN')

    def test_reboot_admin_retries_are_retained_in_final_summary(self):
        observed = [dict(host='satellite', attempts=2, elapsed_ms=2000, errors=['SSLEOFError'])]
        code, result, _, calls = self.run_host(True, admin_recovery=observed)
        self.assertEqual(code, 0)
        self.assertEqual(result['admin_recovery'], [dict(reboot=name, nodes=observed)
                                                  for name in ('Satellite', 'Master', 'Оба CZC')])
        self.assertEqual(calls.count(['worker']), 4)
        self.assertIn('RF bypass between CZCs is not excluded', result['scope'])
        for full, manual in ((False, True), (True, False)):
            code, result, _, _ = self.run_host(full, manual=manual)
            self.assertEqual(code, 1); self.assertEqual(result['verdict'], 'INCOMPLETE')
            self.assertEqual(result['eight_satellites'], 'NOT_TESTED')

    def test_radio_failure_does_not_claim_rf_proof_or_run_remaining_reboots(self):
        failure=host.RadioCheckFailed(dict(failed_check='Unicast', reason='no confirm', rf_isolation_result='NOT_RUN'))
        code, result, starts, _ = self.run_host(True, failure=failure)
        self.assertEqual((code, result['verdict'], result['rf_isolation'], result['reboots'], starts), (1, 'FAIL', 'NOT_RUN', [], 1))
        failure=host.RadioCheckFailed(dict(failed_check='Обмен после перезагрузки', reason='no confirm', rf_isolation_result='NOT_RUN'))
        code, result, starts, _ = self.run_host(True, failure=failure, failure_operation='smoke')
        self.assertEqual((code, result['verdict'], result['rf_isolation'], starts), (1, 'FAIL', 'NOT_RUN', 1))

    def test_ipv6_result_uses_peer_transport_not_http_addresses(self):
        for family, expected in ((4, 'NOT_TESTED'), (6, 'PASS_BACKHAUL')):
            code, result, _, _ = self.run_host(True, family=family)
            self.assertEqual((code, result['ipv6']), (0, expected))

    def test_failed_worker_restores_z2m(self):
        code, result, starts, calls = self.run_host(True, failure=True)
        self.assertEqual(code, 1); self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(starts, 1)
        self.assertEqual(sum('stop' in call and 'zigbee2mqtt' in call for call in calls), 1)

    def test_services_may_be_stopped_or_containers_absent(self):
        for running, existing in ((('mosquitto',), ('mosquitto', 'zigbee2mqtt')),
                                  ((), ('mosquitto', 'zigbee2mqtt')), ((), ())):
            with self.subTest(running=running, existing=existing):
                code, result, starts, calls = self.run_host(True, running=running, existing=existing)
                self.assertEqual((code, result['verdict']), (0, 'PASS_TWO_DEVICES_AND_SENSOR'))
                self.assertEqual(starts, 1)
                first_worker = calls.index(['worker'])
                z2m_start = next(i for i, c in enumerate(calls) if c[-1] == 'zigbee2mqtt' and ('start' in c or 'up' in c))
                self.assertGreater(z2m_start, first_worker)
                for c in calls:
                    if 'up' in c:
                        for flag in ('--no-deps', '--no-recreate', '--no-build'):
                            self.assertIn(flag, c)
                        self.assertEqual(c[c.index('--pull')+1], 'never')
                    self.assertFalse(any(word in c for word in ('down', 'rm', '--force-recreate')))

    def test_failed_worker_leaves_previously_stopped_z2m_stopped(self):
        code, result, starts, calls = self.run_host(True, failure=True, running=())
        self.assertEqual((code, result['verdict']), (1, 'FAIL'))
        self.assertEqual(starts, 0)
        self.assertTrue(all(c[-1] != 'zigbee2mqtt' for c in calls if 'start' in c or 'up' in c))

    def test_missing_service_and_invalid_mode_do_not_start_services(self):
        for args, reason in (({'services': ('mosquitto',)}, 'отсутствуют службы: zigbee2mqtt'),
                             ({'mode': 'invalid'}, 'Неизвестный режим')):
            code, result, starts, calls = self.run_host(True, running=(), **args)
            self.assertEqual(code, 1)
            self.assertIn(reason, result['reason'])
            self.assertEqual(starts, 0)
            self.assertFalse(any(any(word in c for word in ('start', 'up', 'stop', 'worker')) for c in calls))

    def test_radio_cannot_start_if_z2m_remains_running(self):
        code, result, _, calls = self.run_host(True, stop_fails=True)
        self.assertEqual(code, 1)
        self.assertIn('Zigbee2MQTT не остановлен', result['reason'])
        self.assertNotIn(['worker'], calls)

    def test_z2m_readiness_uses_current_container_start(self):
        current = '2026-09-10T12:00:00Z'
        def logs(argv, **kwargs):
            self.assertEqual(argv[argv.index('--since')+1], current)
            return ''  # Earlier successful logs must not count for this start.
        with patch.object(host, 'service_start', return_value='z2m-fixture'), \
             patch.object(host, 'container_state', return_value={'Running': True, 'StartedAt': current}), \
             patch.object(host, 'command', side_effect=logs), patch.object(host.time, 'sleep'), \
             patch.object(host.time, 'monotonic', side_effect=[0, 1, 101]):
            with self.assertRaisesRegex(host.CheckFailed, 'не подтвердил запуск'):
                host.z2m_start()

    def test_physical_confirmations_read_stdin_without_controlling_tty(self):
        with patch.object(sys, 'stdin', io.StringIO('ДА\n')), \
             patch('builtins.open', side_effect=AssertionError('must not open /dev/tty')), \
             contextlib.redirect_stdout(io.StringIO()):
            host.confirmed('Проверено на экране')
        for text in ('', 'нет\n', '\n'):
            with patch.object(sys, 'stdin', io.StringIO(text)), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(host.CheckFailed):
                    host.confirmed('Проверено на экране')

    def test_cli_validates_parameters_without_printing_invalid_key(self):
        args = host.parse_args(['--master', host.MASTER, '--satellite', host.SATELLITE, '--mode', 'local', '--psk', 'AB'*32,
                                '--master-ieee', '00:12:4B:00:2E:11:42:4D', '--compose-dir', str(ROOT)])
        self.assertEqual(args.psk, 'ab'*32)
        self.assertEqual(args.master_ieee, host.IEEES[0])
        self.assertIsNone(args.sensor)
        for extra in (['--psk', 'bad-test-key'], ['--mode', 'invalid'], ['--master', host.SATELLITE]):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as err:
                host.parse_args(['--master', host.MASTER, '--satellite', host.SATELLITE, '--mode', 'local', '--psk', host.PSK] + extra)
            self.assertEqual(err.exception.code, 2)
            self.assertNotIn('bad-test-key', stderr.getvalue())
            self.assertNotIn(host.PSK, stderr.getvalue())

    def test_cli_passes_settings_and_compose_working_directory(self):
        module = load('accept_cli', ROOT/'tools/backhaul/accept_hardware.py')
        def run(mode):
            self.assertEqual(mode, 'local')
            self.assertEqual((module.MASTER, module.SATELLITE), ('192.0.2.1', '192.0.2.2'))
            self.assertEqual(module.PSK, 'ab'*32)
            self.assertEqual((module.ADMIN_FAMILY, module.PEER_FAMILY), (4, 6))
            self.assertIsNone(module.SENSOR)
            self.assertEqual(module.WEB_COOKIES[module.MASTER], 'session=example')
            self.assertEqual(module.COMPOSE, ['docker', 'compose', '-f', 'custom.yaml', '-p', 'fixture'])
            with patch.object(module.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'ok')) as call:
                module.command(module.COMPOSE + ['config', '--services'])
                self.assertEqual(call.call_args.kwargs['cwd'], str(ROOT))
                module.command(['docker', 'image', 'ls'])
                self.assertIsNone(call.call_args.kwargs['cwd'])
            return 0
        with patch.object(module, 'main', side_effect=run):
            code = module.cli(['--mode', 'local', '--master', '192.0.2.1', '--satellite', '192.0.2.2',
                               '--psk', 'ab'*32, '--compose-dir', str(ROOT), '--compose-file', 'custom.yaml',
                               '--project-name', 'fixture', '--master-cookie', 'session=example', '--admin-family', '4', '--peer-family', '6'])
        self.assertEqual(code, 0)

    def test_standalone_file_has_cli_and_can_read_redirected_stdin(self):
        script = ROOT/'.backhaul-tests/czc-two-device-acceptance.py'
        result = subprocess.run([sys.executable, str(script), '--help'], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd='/tmp', timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--mode {local,full,diagnose}', result.stdout)
        # runpy loads the standalone distribution away from the source tree.
        # The only stdin here is the operator response, not the Python source.
        code = ('import runpy, sys; m=runpy.run_path(sys.argv[1]); '
                'assert "def radio_accept(" in m["RADIO_SOURCE"]; m["confirmed"]("fixture")')
        result = subprocess.run([sys.executable, '-c', code, str(script)], input='ДА\n',
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd='/tmp', timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_manual_negative_control_cannot_pass_while_satellite_connected(self):
        with patch.object(host, 'confirmed'), patch.object(host, 'web_json', return_value={'status':{'peer':True,'peers_online':1}}):
            report = {}
            with self.assertRaises(host.CheckFailed): host.sensor_checks(report)
            self.assertTrue(report['restore_satellite_power'])
            self.assertNotIn('sensor_without_satellite', report)

    def test_worker_failure_cleans_only_reserved_endpoint_and_controls(self):
        clients = []
        class Fake:
            def __init__(self, host, key, port):
                self.host=host; self.i=len(clients); self.events=[]; self.calls=[]; clients.append(self)
            def info(self):
                return dict(state=9 if self.i==0 else 7, short=self.i, ieee='0x'+host.IEEES[self.i],
                            pan=42, channel=11, extpan='same')
            def gateway(self):
                return dict(role='master' if self.i==0 else 'satellite', debug=True, bootstrap=2, peer=True,
                            peers_online=1, af=4, uart_ok=True)
            def stats(self):
                return dict(online=1, pending=0, live=0, alloc=0, free=0, tx=0, rx=0, alloc_fail=0, queue_fail=0, overflow=0)
            def register(self): self.calls.append(('register', ns['EP']))
            def send(self, *args): raise RuntimeError('injected AF failure')
            def hold(self, value): self.calls.append(('hold', value))
            def pause(self, value): self.calls.append(('pause', value))
            def checked(self, cmd, data, subsystem): self.calls.append(('delete', data[0], subsystem))
            def close(self): self.calls.append(('close',))
        cfg = dict(key=host.PSK, master=host.MASTER, satellite=host.SATELLITE, admin_port=7444,
                   ieees=host.IEEES, operation='smoke')
        with patch.dict(ns, Client=Fake), contextlib.redirect_stdout(io.StringIO()):
            report=ns['radio_accept'](cfg)
        self.assertEqual(report['verdict'], 'FAIL')
        self.assertEqual(report['reason'], 'injected AF failure')
        for c in clients:
            self.assertEqual(c.calls[-4:], [('hold', False), ('pause', False), ('delete', 240, 4), ('close',)])

    def test_af_freshness_duplicate_metadata_and_tx_counter_checks(self):
        def frame(payload, source=0, mac=0):
            data = bytearray(20+len(payload))
            data[2:4] = ns['struct'].pack('<H', ns['CLUSTER'])
            data[4:6] = ns['struct'].pack('<H', source)
            data[6:8] = bytes([ns['EP'], ns['EP']]); data[16] = len(payload)
            data[17:17+len(payload)] = payload
            data[17+len(payload):19+len(payload)] = ns['struct'].pack('<H', mac)
            return 0x44, 0x81, bytes(data)
        class Fake:
            def __init__(self): self.events=[]; self.tx=self.rx=0
            def stats(self): return dict(tx=self.tx, rx=self.rx)
            def pump(self, timeout): pass
        for duplicate, wrong_mac, no_ethernet in ((False,False,False),(True,False,False),(False,True,False),(False,False,True)):
            sender, receiver = Fake(), Fake()
            def send(destination, payload):
                sender.tx += not no_ethernet; receiver.rx += not no_ethernet
                sender.events.append((0x44,0x80,bytes([0, ns['EP'],7])))
                receiver.events.extend([frame(payload, mac=123 if wrong_mac else 0)]*(2 if duplicate else 1))
                return 7
            sender.send = send
            with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
                if duplicate or wrong_mac or no_ethernet:
                    with self.assertRaises(AssertionError): ns['exchange'](sender, receiver, 0, 1)
                else: ns['exchange'](sender, receiver, 0, 1)

    def test_broadcast_rf_first_is_distinct_from_admission_or_queue_failure(self):
        class Fake:
            def __init__(self):
                self.events=[]
                self.values={key: 0 for key in ns['STATS']}
            def stats(self): return self.values.copy()
            def pump(self, timeout): pass
        cases = [
            # Wired admission works in either placement.
            (False, 1, 0, 1, 1, 0, 1, True),
            (True, 1, 0, 1, 1, 0, 1, True),
            # Suppression is allowed when the caller does not exclude RF overlap.
            (True, 0, 1, 1, 1, 0, 1, True),
            (False, 0, 1, 1, 1, 0, 1, False),
            # Merely receiving over RF, an unacknowledged duplicate, missing
            # wired TX, actual rejection and duplicate AF must still fail.
            (True, 0, 0, 1, 1, 0, 1, False),
            (True, 0, 1, 0, 1, 0, 1, False),
            (True, 0, 1, 1, 0, 0, 1, False),
            (True, 0, 1, 1, 1, 1, 1, False),
            (True, 1, 0, 1, 1, 0, 2, False),
        ]
        for local, rx, duplicate, accepted, tx, queue_fail, copies, passes in cases:
            with self.subTest(local=local, rx=rx, duplicate=duplicate, accepted=accepted, tx=tx, queue_fail=queue_fail, copies=copies):
                sender, receiver = Fake(), Fake()
                def send(destination, payload):
                    self.assertEqual(destination, 0xfffc)
                    sender.values.update(tx=tx, broadcast_tx=tx, accepted=accepted, failed=queue_fail)
                    receiver.values.update(rx=rx, duplicate=duplicate, queue_fail=queue_fail, reject=queue_fail)
                    data = bytearray(20+len(payload))
                    data[2:4] = ns['struct'].pack('<H', ns['CLUSTER'])
                    data[6:8] = bytes([ns['EP'], ns['EP']]); data[16] = len(payload)
                    data[17:17+len(payload)] = payload
                    receiver.events = [(0x44, 0x81, bytes(data))]*copies
                    return 7
                sender.send = send
                with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
                    if passes:
                        result = ns['exchange'](sender, receiver, 0, 1, broadcast=True, allow_rf_broadcast=local)
                        self.assertEqual(result['evidence']['wired_result'], 'admitted' if rx else 'duplicate_suppressed')
                    else:
                        with self.assertRaises(AssertionError):
                            ns['exchange'](sender, receiver, 0, 1, broadcast=True, allow_rf_broadcast=local)


class HeldExchange(unittest.TestCase):
    def fixture(self, statuses=(0xf0,), stuck=False, late=None):
        clock = {'now': 0.0}
        state = dict(held=False, paused=False, submitted=False, finished=False,
                     background=False, released=None)
        pending_samples, controls = [], []

        def frame(payload):
            data = bytearray(20+len(payload))
            data[2:6] = ns['struct'].pack('<HH', ns['CLUSTER'], 0)
            data[6:8] = bytes([ns['EP'], ns['EP']])
            data[16] = len(payload)
            data[17:17+len(payload)] = payload
            return 0x44, 0x81, bytes(data)

        def tick():
            if state['submitted'] and not state['finished'] and (state['paused'] or clock['now'] >= 1):
                state['finished'] = True
                # The challenged TX expires; a later broadcast is still held.
                state['background'] = True
                master.events.extend((0x44, 0x80, bytes([s, ns['EP'], 237])) for s in statuses)
                master.events.append((0x44, 0x80, bytes([0, ns['EP'], 18])))
            if state['released'] is not None and clock['now'] >= state['released'] + .12 and not stuck:
                state['background'] = False

        class Fake:
            def __init__(self, name):
                self.host, self.events = name, []
            def stats(self):
                tick()
                pending = int(state['submitted'] and not state['finished']) + int(state['background']) if self is master else 0
                if self is master:
                    pending_samples.append((state['held'], pending))
                values = {k: 0 for k in ns['STATS']}
                return dict(values, online=1, pending=pending)
            def pump(self, timeout):
                clock['now'] += timeout
                tick()
            def gateway(self):
                return dict(peer=not state['paused'])
            def send(self, destination, payload):
                state.update(submitted=True, payload=payload)
                return 237
            def hold(self, value):
                controls.append(('hold', value))
                state['held'] = value
                if not value and state['submitted'] and state['released'] is None:
                    state['pending_at_release'] = master.stats()['pending']
                    state['confirm_at_release'] = ns['confirmation'](master, 237)
                    state['released'] = clock['now']
                    if late == 'delivery':
                        satellite.events.append(frame(state['payload']))
                    elif late in ('success', 'duplicate'):
                        master.events.append((0x44, 0x80, bytes([0 if late == 'success' else 0xf0, ns['EP'], 237])))
            def pause(self, value):
                controls.append(('pause', value))
                state['paused'] = value
                tick()

        master, satellite = Fake('master'), Fake('satellite')
        return master, satellite, clock, state, pending_samples, controls

    def test_later_background_tx_drains_only_after_hold_is_released(self):
        for disconnect in (False, True):
            with self.subTest(disconnect=disconnect):
                master, satellite, clock, state, samples, controls = self.fixture()
                with patch.object(ns['time'], 'monotonic', side_effect=lambda: clock['now']):
                    ns['held_exchange'](master, satellite, 0x2b4d, disconnect)
                # Background TX remains pending after the challenged TX confirms with 0xF0.
                self.assertIn((True, 1), samples)
                self.assertEqual((state['pending_at_release'], state['confirm_at_release']), (1, [0xf0]))
                self.assertEqual(samples[-1], (False, 0))
                self.assertEqual(ns['confirmation'](master, 237), [0xf0])
                self.assertFalse(state['held'] or state['paused'])
                self.assertEqual(('pause', True) in controls, disconnect)

    def test_stuck_queue_after_release_is_bounded_failure_with_pending_evidence(self):
        for disconnect in (False, True):
            with self.subTest(disconnect=disconnect):
                master, satellite, clock, state, _, _ = self.fixture(stuck=True)
                with patch.object(ns['time'], 'monotonic', side_effect=lambda: clock['now']):
                    with self.assertRaisesRegex(ns['ExchangeFailure'], 'did not drain') as raised:
                        ns['held_exchange'](master, satellite, 0x2b4d, disconnect)
                evidence = raised.exception.evidence
                self.assertEqual(evidence['phase'], 'wait_pending_drain')
                self.assertEqual(evidence['queue'], dict(hold_active=False, pending_at_confirm=1, pending_after_release=1))
                self.assertEqual(evidence['aps_confirmations'], ['0xF0'])
                self.assertGreaterEqual(clock['now'] - state['released'], 8)
                self.assertLess(clock['now'] - state['released'], 8.1)
                self.assertFalse(state['held'] or state['paused'])

    def test_bad_missing_or_late_confirm_and_late_delivery_still_fail(self):
        cases = [((0,), None), ((0xf0, 0xf0), None), ((), 'success'),
                 ((0xf0,), 'success'), ((0xf0,), 'duplicate'), ((0xf0,), 'delivery')]
        for disconnect in (False, True):
            for statuses, late in cases:
                with self.subTest(disconnect=disconnect, statuses=statuses, late=late):
                    master, satellite, clock, state, _, _ = self.fixture(statuses=statuses, late=late)
                    with patch.object(ns['time'], 'monotonic', side_effect=lambda: clock['now']):
                        with self.assertRaises(ns['ExchangeFailure']) as raised:
                            ns['held_exchange'](master, satellite, 0x2b4d, disconnect)
                    evidence = raised.exception.evidence
                    self.assertFalse(state['held'] or state['paused'])
                    if not statuses:
                        self.assertEqual(evidence['phase'], 'wait_failed_confirm')
                        self.assertEqual(evidence['aps_confirmations'], [])
                        self.assertEqual(ns['confirmation'](master, 237), [0])
                    if late == 'delivery':
                        self.assertEqual(evidence['af_deliveries'], 1)
                    self.assertNotIn('payload', json.dumps(evidence))


class ExchangeDiagnostics(unittest.TestCase):
    def pair(self, statuses=(0,), source=0, destination=0x1df3, copies=1, late_confirm=False, broken_stats=False):
        class Fake:
            def __init__(self, host):
                self.host, self.events, self.calls = host, [], 0
                self.values = {k: 0 for k in ns['STATS']}
            def stats(self):
                self.calls += 1
                if self.calls > 1 and self is sender:
                    if late_confirm:
                        self.events.append((0x44, 0x80, bytes([0, ns['EP'], 255])))
                    if broken_stats:
                        raise ConnectionError('injected diagnostic failure')
                return self.values.copy()
            def pump(self, timeout): pass
        sender, receiver = Fake('master'), Fake('satellite')
        def send(dst, payload):
            self.assertEqual(dst, destination)
            sender.values.update(tx=1, accepted=1)
            receiver.values.update(rx=1, alloc=1, free=1)
            sender.events.extend((0x44, 0x80, bytes([s, ns['EP'], 255])) for s in statuses)
            data = bytearray(20+len(payload))
            data[2:6] = ns['struct'].pack('<HH', ns['CLUSTER'], source)
            data[6:8] = bytes([ns['EP'], ns['EP']]); data[16] = len(payload)
            data[17:17+len(payload)] = payload
            data[17+len(payload):19+len(payload)] = ns['struct'].pack('<H', source)
            receiver.events.extend([(0x44, 0x81, bytes(data))]*copies)
            return 255
        sender.send = send
        return sender, receiver

    def test_actual_failed_status_and_duplicates_survive_with_clean_ip_counters(self):
        for source, destination in ((0, 0x1df3), (0x1df3, 0)):
            for statuses, reason in (((0xb7,), 'ZApsNoAck'), ((0xe9,), 'ZMacNoACK'),
                                     ((0xf0,), 'ZMacTransactionExpired'), ((0x99,), 'unknown status'),
                                     ((0, 0), 'Duplicate'), ((0xb7, 0), 'Duplicate')):
                with self.subTest(source=source, statuses=statuses):
                    sender, receiver = self.pair(statuses, source, destination)
                    with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
                        with self.assertRaisesRegex(ns['ExchangeFailure'], reason) as raised:
                            ns['exchange'](sender, receiver, source, destination)
                    evidence = raised.exception.evidence
                    self.assertEqual(evidence['aps_confirmations'], ['0x%02X' % s for s in statuses])
                    self.assertEqual((evidence['source'], evidence['destination']), ('0x%04X' % source, '0x%04X' % destination))
                    self.assertEqual((evidence['transaction'], evidence['bytes'], evidence['af_deliveries']), (255, 32, 1))
                    self.assertEqual((evidence['sender']['accepted'], evidence['receiver']['rx']), (1, 1))
                    self.assertEqual(evidence['sender']['failed'], 0)
                    self.assertNotIn('payload', json.dumps(evidence))

    def test_missing_confirm_is_a_deadline_failure_even_if_diagnostics_receive_it(self):
        sender, receiver = self.pair(statuses=(), late_confirm=True)
        with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
            with self.assertRaisesRegex(ns['ExchangeFailure'], 'deadline') as raised:
                ns['exchange'](sender, receiver, 0, 0x1df3)
        self.assertEqual(raised.exception.evidence['af_deliveries'], 1)
        self.assertEqual(raised.exception.evidence['aps_confirmations'], [])
        self.assertEqual(raised.exception.evidence['phase'], 'wait_af_and_confirm')
        self.assertGreaterEqual(raised.exception.evidence['elapsed_ms'], 12000)
        self.assertEqual(ns['confirmation'](sender, 255), [0])

    def test_diagnostic_rpc_failure_does_not_hide_original_af_error(self):
        sender, receiver = self.pair(statuses=(0xb7,), broken_stats=True)
        with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
            with self.assertRaisesRegex(ns['ExchangeFailure'], 'ZApsNoAck') as raised:
                ns['exchange'](sender, receiver, 0, 0x1df3)
        self.assertEqual(raised.exception.evidence['sender'], dict(error='ConnectionError'))
        self.assertEqual(raised.exception.evidence['receiver']['rx'], 1)

    def test_no_delivery_duplicate_delivery_and_wrong_transaction_are_distinct(self):
        for copies in (0, 2):
            sender, receiver = self.pair(copies=copies)
            with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
                with self.assertRaises(ns['ExchangeFailure']) as raised:
                    ns['exchange'](sender, receiver, 0, 0x1df3)
            self.assertEqual(raised.exception.evidence['af_deliveries'], copies)
            self.assertEqual(raised.exception.evidence['aps_confirmations'], ['0x00'])
        sender, receiver = self.pair()
        send = sender.send
        def wrong_transaction(dst, payload):
            send(dst, payload)
            # An AF success for another transaction is never our success.
            sender.events = [(0x44, 0x80, bytes([0, ns['EP'], 17]))]
            return 255
        sender.send = wrong_transaction
        with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
            with self.assertRaises(ns['ExchangeFailure']) as raised:
                ns['exchange'](sender, receiver, 0, 0x1df3)
        self.assertEqual(raised.exception.evidence['aps_confirmations'], [])
        self.assertEqual(raised.exception.evidence['other_confirmations'], [dict(transaction=17, status='0x00')])

    def test_duplex_snapshots_both_directions_before_reading_stats(self):
        sender, receiver = self.pair(statuses=(), late_confirm=True)
        with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
            traces = [ns['ExchangeTrace'](sender, receiver, 0, 1, b'a', 'duplex'),
                      ns['ExchangeTrace'](receiver, sender, 1, 0, b'b', 'duplex')]
            traces[0].transaction = 255
            traces[1].transaction = 3
            sender.events.clear()
            with self.assertRaisesRegex(ns['ExchangeFailure'], 'duplex timeout') as raised:
                with ns['trace_exchanges'](traces):
                    raise TimeoutError('duplex timeout')
        self.assertEqual([e['aps_confirmations'] for e in raised.exception.evidence['transfers']], [[], []])
        self.assertEqual(ns['confirmation'](sender, 255), [0])

    def test_held_tx_requires_exact_failure_and_no_delivery(self):
        for statuses, copies, passes in (((0xf0,), 0, True), ((0,), 0, False),
                                          ((0xf0, 0xf0), 0, False), ((0xf0,), 1, False), ((), 0, False)):
            sender, receiver = self.pair(statuses=statuses, copies=copies)
            with patch.object(ns['time'], 'monotonic', side_effect=itertools.count(step=.1)):
                trace = ns['ExchangeTrace'](sender, receiver, 0, 0x1df3, b'abandoned', 'held_timeout')
                trace.send()
                if passes:
                    trace.validate(expected_status=0xf0, expected_deliveries=0)
                else:
                    with self.assertRaises(ns['ExchangeFailure']):
                        with ns['trace_exchanges']([trace]):
                            trace.validate(expected_status=0xf0, expected_deliveries=0)


if __name__ == '__main__':
    unittest.main()
