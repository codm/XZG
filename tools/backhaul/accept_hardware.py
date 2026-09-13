"""Host-side acceptance. The standalone Python distribution embeds its worker.
Run with --help for CLI parameters. No configuration changes or NV resets.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import select
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

MASTER = None
SATELLITE = None
IEEES = [None, None]
SENSOR = None
PSK = None
COMPOSE = ['docker', 'compose']
COMPOSE_DIR = None
IMAGE = 'python:3.13-slim-bookworm'
# Set from --master-cookie/--satellite-cookie when the web UI requires login.
WEB_COOKIES = {}
TEST_REVISION = '20260917.12'
ADMIN_FAMILY = 0
PEER_FAMILY = 0


class NoWebRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class CheckFailed(Exception):
    pass


class RadioCheckFailed(CheckFailed):
    def __init__(self, report):
        super().__init__(report.get('failed_check', 'Радио') + ': ' + report.get('reason', 'FAIL'))
        self.report = report


def command(argv, timeout=30, required=True):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout,
                            cwd=COMPOSE_DIR if argv[:len(COMPOSE)] == COMPOSE else None)
    if required and result.returncode:
        raise CheckFailed('Команда не выполнена: ' + ' '.join(argv[:4]) + '; проверьте Docker/Compose.')
    return result.stdout.strip()


def ask(message):
    print('\n' + message, flush=True)
    try:
        reply = input('> ')
    except (EOFError, OSError):
        raise CheckFailed('Нет ввода для подтверждения. Запустите Python-файл в обычном терминале; '
                          'для автоматической проверки без датчика используйте --mode local.') from None
    return reply.strip().lower()


def confirmed(message):
    if ask(message + '\nВведите ДА только если всё перечисленное подтверждено; иначе НЕТ.') not in ('да', 'yes'):
        raise CheckFailed('Физическая проверка не подтверждена.')


def web(host, path):
    headers = {'Cache-Control': 'no-cache'}
    if WEB_COOKIES.get(host):
        headers['Cookie'] = WEB_COOKIES[host]
    address = '[' + host + ']' if ':' in host else host
    request = urllib.request.Request('http://' + address + path, headers=headers)
    try:
        # These are direct device requests, including IPv6 literals. Environment
        # proxies and redirects must not change the measured path or receive cookies.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoWebRedirect())
        with opener.open(request, timeout=3) as response:
            body = response.read(32769)
            if len(body) > 32768:
                raise CheckFailed('Слишком большой ответ web API ' + host)
            return body
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            option = '--master-cookie' if host == MASTER else '--satellite-cookie'
            raise CheckFailed('Web UI ' + host + ' требует входа; передайте cookie через ' + option + '.') from None
        raise


def web_json(host, path='/api/backhaul'):
    value = json.loads(web(host, path))
    if not isinstance(value, dict):
        raise CheckFailed('Некорректный JSON web API ' + host)
    return value


def normalized_ieee(value):
    return str(value).lower().removeprefix('0x').replace(':', '')


def normalized_ip(value, without_scope=False):
    if not isinstance(value, str):
        return None
    try:
        address = ipaddress.ip_address(value)
        address = getattr(address, 'ipv4_mapped', None) or address
        # Scope IDs belong to the reporting machine. Ignore them only when
        # comparing addresses from the two already identified CZCs, never when
        # constructing an HTTP/TLS destination on the machine running this test.
        if without_scope and address.version == 6:
            address = ipaddress.IPv6Address(address.packed)
        return str(address)
    except (ValueError, TypeError):
        return None


def admin_addresses():
    """An explicitly selected control family does not change the peer transport."""
    result = []
    for seed in (MASTER, SATELLITE):
        address = seed
        if ADMIN_FAMILY and ipaddress.ip_address(seed).version != ADMIN_FAMILY:
            discovered = advertised_addresses(seed)
            if discovered.get('error'):
                raise CheckFailed('Не удалось прочитать адреса интерфейсов у %s: %s' % (seed, discovered['error']))
            inventory = discovered.get('addresses', discovered['candidates'])
            interface = next((v['interface'] for v in inventory
                              if normalized_ip(v['host'], without_scope=True) == normalized_ip(seed, without_scope=True)), None)
            matches = [v['host'] for v in discovered['candidates']
                       if interface and v['interface'] == interface and v['family'] == ADMIN_FAMILY]
            if len(matches) != 1:
                raise CheckFailed('Не найден IPv%s того же интерфейса у %s' % (ADMIN_FAMILY, seed))
            address = matches[0]
            # Do not send the seed's login cookie to a discovered address.
            a, b = http_observation(seed), http_observation(address)
            if a['http'] != 'OK' or b['http'] != 'OK' or a.get('boot') != b.get('boot'):
                raise CheckFailed('Не подтверждён тот же CZC по служебному адресу ' + address)
        result.append(address)
    if result[0] == result[1]:
        raise CheckFailed('Одинаковые служебные адреса у двух CZC')
    return result


def http_observation(host):
    started = time.monotonic()
    result = dict(host=host)
    try:
        status = web_json(host, '/api/esp-update')
        if not isinstance(status.get('boot'), str) or not status['boot']:
            raise CheckFailed('Ответ не содержит boot ID CZC')
        result['http'] = 'OK'
        for key in ('boot', 'version', 'state', 'wifi_connected', 'wifi_rssi'):
            if key in status:
                result[key] = status[key]
        heap = status.get('heap', {})
        result['heap'] = {k: heap[k] for k in ('free', 'largest', 'minimum') if k in heap}
    except (OSError, ValueError, CheckFailed) as exc:
        result.update(http='ERROR', error=type(exc).__name__)
    result['ms'] = round((time.monotonic() - started) * 1000, 1)
    return result


def http_observations():
    # Two fixed destinations, one request each. A missing Master must not
    # delay measuring the Satellite by another socket timeout.
    with ThreadPoolExecutor(max_workers=2) as pool:
        return list(pool.map(http_observation, (MASTER, SATELLITE)))


def gateway_observation(host):
    """Only operational fields; never return the Role API's PSK or CSRF token."""
    try:
        value = web_json(host)
        state = value.get('status', {})
        result = {k: value[k] for k in ('mode', 'peer_host', 'ipv6', 'debug_mode') if k in value}
        result.update({k: state[k] for k in (
            'own_ieee', 'fault', 'last_peer_error', 'peer', 'peers_online', 'af',
            'connect_attempts', 'retry_ms', 'peer_tls_bytes', 'psk_tls_bytes', 'uart_ok') if k in state})
        if isinstance(state.get('admin_tls'), dict):
            result['admin_tls'] = {k: state['admin_tls'][k] for k in (
                'state', 'failures', 'last_stage', 'last_code', 'open_free', 'open_largest', 'raw_client') if k in state['admin_tls']}
        result['peers'] = [{k: p[k] for k in ('ieee', 'ip', 'online', 'fault', 'af') if k in p}
                           for p in state.get('peers', [])[:8] if isinstance(p, dict)]
        return result
    except (OSError, ValueError, CheckFailed, TypeError, AttributeError) as exc:
        return dict(error=type(exc).__name__)


def advertised_addresses(host):
    """Active address inventory plus a smaller list of HTTP probe targets."""
    result = dict(host=host, interfaces=[], addresses=[], candidates=[])
    try:
        value = web_json(host, '/api/network/status')
        for interface in value.get('interfaces', [])[:3]:
            if not isinstance(interface, dict) or interface.get('id') not in ('wifi', 'eth'):
                continue
            item = {k: interface[k] for k in ('id', 'enabled', 'connected') if k in interface}
            result['interfaces'].append(item)
            if interface.get('enabled') is not True or interface.get('connected') is not True:
                continue
            values = [interface.get('ipv4')]
            values += [v.get('address') for v in interface.get('ipv6', [])[:16] if isinstance(v, dict)]
            # Membership must include link-local and additional SLAAC addresses.
            # Only active probes are capped at one routable IP per family; no
            # probe uses a link-local scope ID learned from a different machine.
            families = set()
            for value in values:
                literal = normalized_ip(value)
                if not literal:
                    continue
                ip = ipaddress.ip_address(literal)
                if (ip.is_unspecified or ip.is_multicast or ip.is_loopback or
                        ip == ipaddress.ip_address('255.255.255.255')):
                    continue
                item = dict(host=str(ip), interface=interface['id'], family=ip.version)
                if item not in result['addresses']:
                    result['addresses'].append(item)
                if ip.is_link_local or ip.version in families:
                    continue
                families.add(ip.version)
                result['candidates'].append(item)
    except (OSError, ValueError, CheckFailed, TypeError, AttributeError) as exc:
        result['error'] = type(exc).__name__
    return result


def local_route(host):
    """Read only the route and cached neighbor for this literal IP; no scans."""
    if sys.platform == 'darwin':
        commands = [('route', ['/sbin/route', '-n', 'get', host]),
                    ('neighbor', ['/usr/sbin/arp', '-n', host])] if ':' not in host else [
                    ('route', ['/sbin/route', '-n', 'get', '-inet6', host])]
    elif sys.platform.startswith('linux'):
        commands = [('route', ['ip', 'route', 'get', host]),
                    ('neighbor', ['ip', 'neigh', 'show', 'to', host])]
    else:
        return dict(host=host, state='UNAVAILABLE')
    result = dict(host=host)
    for name, argv in commands:
        try:
            proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, timeout=2)
            lines = proc.stdout.strip().splitlines()
            if name == 'route' and sys.platform == 'darwin':
                lines = [line.strip() for line in lines if any(
                    line.strip().startswith(k + ':') for k in ('destination', 'gateway', 'interface', 'flags'))]
            result[name] = dict(code=proc.returncode, text='\n'.join(lines)[:400])
        except (OSError, subprocess.TimeoutExpired) as exc:
            result[name] = dict(error=type(exc).__name__)
    return result


def diagnose():
    print('CZC: диагностика ' + TEST_REVISION + '; IPv4/IPv6 и состояние канала, 5 замеров. '
          'Настройки, службы и питание не меняются.', flush=True)
    seeds = (MASTER, SATELLITE)
    with ThreadPoolExecutor(max_workers=2) as pool:
        networks = list(pool.map(advertised_addresses, seeds))
        gateways = list(pool.map(gateway_observation, seeds))
    targets = {h: [dict(host=h, interface='requested', family=ipaddress.ip_address(h).version)] for h in seeds}
    owners = {h: h for h in seeds}
    conflicts = set()
    for index, h in enumerate(seeds):
        seen = {h}
        # Do not discover further targets from a different device or login page.
        try:
            pin_identity(index, gateways[index].get('own_ieee'))
        except CheckFailed:
            continue
        if normalized_ieee(gateways[index].get('own_ieee')) == IEEES[index]:
            for candidate in networks[index]['candidates']:
                other = owners.setdefault(candidate['host'], h)
                if other != h:
                    conflicts.add(candidate['host'])
                    continue
                if candidate['host'] == h:
                    targets[h][0] = candidate
                if candidate['host'] not in seen and candidate['host'] not in seeds:
                    targets[h].append(candidate)
                    seen.add(candidate['host'])
    for h in seeds:
        targets[h][1:] = [v for v in targets[h][1:] if v['host'] not in conflicts]
    nodes = {h: dict(targets[h][0], ok=0, errors=0, boot_changes=0, max_ms=0) for h in seeds}
    aliases = {v['host']: dict(v, node=h, ok=0, errors=0, max_ms=0, ms=[], identity='UNVERIFIED')
               for h in seeds for v in targets[h][1:]}
    previous = {}
    route_hosts = list(nodes) + list(aliases)
    routes = [local_route(h) for h in route_hosts]
    gateway_changes = {h: [gateways[i]] for i, h in enumerate(seeds)}
    def aliases_and_gateway(h):
        # Sequential within each CZC, parallel across the two devices. Do not
        # forward the seed's login cookie to addresses learned from its API.
        return ([http_observation(v['host']) for v in targets[h][1:]], gateway_observation(h))
    for index in range(5):
        observations = http_observations()
        for value in observations:
            host = value['host']
            node = nodes[host]
            node['max_ms'] = max(node['max_ms'], value['ms'])
            if value['http'] == 'OK':
                node['ok'] += 1
                if host in previous and previous[host] != value['boot']:
                    node['boot_changes'] += 1
                previous[host] = value['boot']
                node['last_response'] = value
            else:
                node['errors'] += 1
                node['last_error'] = value
        with ThreadPoolExecutor(max_workers=2) as pool:
            extra = list(pool.map(aliases_and_gateway, seeds))
        for h, (values, gateway) in zip(seeds, extra):
            if gateway != gateway_changes[h][-1]:
                gateway_changes[h].append(gateway)
            for value in values:
                item = aliases[value['host']]
                item['max_ms'] = max(item['max_ms'], value['ms'])
                item['ms'].append(value['ms'])
                if value['http'] == 'OK':
                    item['ok'] += 1
                    reference = next(v for v in observations if v['host'] == h)
                    if reference['http'] == 'OK':
                        identity = 'SAME_BOOT' if value['boot'] == reference['boot'] else 'BOOT_MISMATCH'
                        # A mismatch can be another device, stale address or reboot;
                        # preserve it even if a later observation matches again.
                        if item['identity'] != 'BOOT_MISMATCH':
                            item['identity'] = identity
                    item['heap'] = value.get('heap', {})
                else:
                    item['errors'] += 1
                    item['last_error'] = value['error']
        print('[%s/5] ' % (index+1) + ' · '.join(v['host'] + ': ' + v['http'] +
              ' (%.0f мс)' % v['ms'] for v in observations), flush=True)
        if index != 4:
            time.sleep(2)
    for value in aliases.values():
        print('  ' + value['interface'] + '/IPv' + str(value['family']) + ' ' + value['host'] +
              ': %s/5 ответов, максимум %.0f мс; %s' % (value['ok'], value['max_ms'], value['identity']), flush=True)
    unstable = (bool(conflicts) or any(n['errors'] or n['boot_changes'] for n in nodes.values()) or
                any(v['errors'] or v['identity'] == 'BOOT_MISMATCH' for v in aliases.values()))
    gateway_summary = []
    for h, values in gateway_changes.items():
        summary = dict(host=h, status=values[-1])
        summary['faults'] = list(dict.fromkeys(v['fault'] for v in values if 'fault' in v))
        counts = [v['connect_attempts'] for v in values if isinstance(v.get('connect_attempts'), int)]
        if counts:
            summary['attempts_first_last'] = [counts[0], counts[-1]]
        gateway_summary.append(summary)
    result = dict(schema='czc-network-diagnostic-2', test_revision=TEST_REVISION,
                  verdict='OBSERVED_INTERRUPTION' if unstable else 'HTTP_RESPONDING',
                  nodes=list(nodes.values()), addresses=list(aliases.values()),
                  conflicting_advertisements=sorted(conflicts),
                  interfaces=[dict(host=n['host'], interfaces=n['interfaces'], **({'error': n['error']} if 'error' in n else {})) for n in networks],
                  gateway=gateway_summary,
                  route_before=routes, route_after=[local_route(h) for h in route_hosts],
                  scope='Direct HTTP and existing gateway status only; no new TLS sessions or radio tests; '
                        'IPv6 HTTP does not certify IPv6 backhaul; BOOT_MISMATCH is not proof of an IP conflict')
    print('CZC_DIAG ' + json.dumps(result, ensure_ascii=False), flush=True)
    return 1 if unstable else 0


def http_preflight():
    """Exercise the idle-browser regression before changing any device/service state."""
    results = []
    for host in (MASTER, SATELLITE):
        elapsed = []
        version = None
        previous_boot = None
        try:
            # A browser can open TCP before it has an HTTP request to send.
            # Keep one such socket while making three real API requests.
            with socket.create_connection((host, 80), timeout=3):
                time.sleep(.1)
                for _ in range(3):
                    started = time.monotonic()
                    status = web_json(host, '/api/esp-update')
                    duration = time.monotonic() - started
                    elapsed.append(round(duration * 1000, 1))
                    if duration >= 3:
                        raise CheckFailed('Ответ API занял %.2f с при лимите 3 с' % duration)
                    if not isinstance(status.get('boot'), str) or not status['boot'] or not isinstance(status.get('version'), str):
                        raise CheckFailed('API не вернул версию и идентификатор запуска')
                    if previous_boot is not None and previous_boot != status['boot']:
                        raise CheckFailed('CZC перезапустился во время проверки HTTP')
                    if status.get('state') != 'idle':
                        raise CheckFailed('Обновление ESP32 не завершено; дождитесь обычной работы UI')
                    version, previous_boot = status['version'], status['boot']
                    time.sleep(.1)
        except (OSError, ValueError, CheckFailed) as exc:
            raise CheckFailed('HTTP ' + host + ': ' + (str(exc) or type(exc).__name__) +
                              '. Приёмка радио не начата, службы не изменены.') from None
        results.append(dict(host=host, result='PASS', requests=3, max_ms=max(elapsed), version=version))
        print('HTTP ' + host + ': PASS (3 запроса, максимум %.1f мс)' % max(elapsed), flush=True)
    return results


def pin_identity(index, value):
    identity = normalized_ieee(value)
    if not re.fullmatch(r'[0-9a-f]{16}', identity) or identity in ('0'*16, 'f'*16):
        raise CheckFailed('Invalid radio IEEE address')
    if IEEES[index] is None:
        if identity == IEEES[1-index]:
            raise CheckFailed('Master and Satellite have the same IEEE address')
        IEEES[index] = identity
    if identity != IEEES[index]:
        raise CheckFailed('Radio IEEE changed or differs from the expected address')
    return identity


def pair_snapshot(require_link=True):
    nodes = []
    for i, host in enumerate((MASTER, SATELLITE)):
        cfg = web_json(host)
        st = cfg.get('status', {})
        if i == 1 and cfg.get('mode') == 0:
            raise CheckFailed('Satellite ' + host + ' в Off. Выберите Satellite во вкладке «Роль», '
                              'укажите Master ' + MASTER + ', сохраните и дождитесь «Соединено».')
        pin_identity(i, st.get('own_ieee'))
        if cfg.get('mode') != i + 1:
            raise CheckFailed('Неверная роль или собственный IEEE у ' + host)
        if str(cfg.get('psk', '')).lower() != PSK.lower():
            raise CheckFailed('Сохранённый PSK отличается от переданного у ' + host)
        if not cfg.get('debug_mode'):
            raise CheckFailed('Для приёмки нужна сборка с DEBUG на обоих CZC (pio run -e debug).')
        if st.get('radio_revision') != 20260917 or st.get('radio_protocol') != 2 or not st.get('uart_ok'):
            raise CheckFailed('Несовместимое радио или ошибка UART у ' + host)
        nodes.append(cfg)
    if require_link:
        for i, host in enumerate((MASTER, SATELLITE)):
            st = nodes[i]['status']
            peers = [p for p in st.get('peers', []) if p.get('online')]
            other_host = (MASTER, SATELLITE)[1-i]
            if not st.get('peer') or len(peers) != 1 or normalized_ieee(peers[0].get('ieee')) != IEEES[1-i]:
                raise CheckFailed('Нет ожидаемого взаимного соединения у ' + host)
            peer_ip = normalized_ip(peers[0].get('ip'), without_scope=True)
            allowed = {normalized_ip(other_host, without_scope=True)}
            if peer_ip and peer_ip not in allowed:
                discovered = advertised_addresses(other_host)
                if discovered.get('error'):
                    raise CheckFailed('Не удалось сверить адрес peer %s у %s: /api/network/status %s — %s' %
                                      (peer_ip, host, other_host, discovered['error']))
                allowed.update(normalized_ip(v['host'], without_scope=True)
                               for v in discovered.get('addresses', discovered['candidates']))
                allowed.discard(None)
            if not peer_ip or peer_ip not in allowed:
                raise CheckFailed('Нет ожидаемого взаимного соединения у %s: адрес peer %s не принадлежит другому CZC; '
                                  'адреса %s: %s' % (host, peer_ip or '<invalid>', other_host, ', '.join(sorted(allowed))))
            if PEER_FAMILY and (ipaddress.ip_address(peer_ip).version != PEER_FAMILY or
                                peers[0].get('af', st.get('af')) != PEER_FAMILY):
                raise CheckFailed('Межузловой канал у %s должен использовать IPv%s. '
                                  'Укажите на Satellite адрес Master нужного семейства IP.' % (host, PEER_FAMILY))
    if nodes[0]['peer_port'] != nodes[1]['peer_port'] or nodes[0]['admin_port'] != nodes[1]['admin_port']:
        raise CheckFailed('В этой приёмке нужны одинаковые номера peer/admin-портов на обоих CZC.')
    return nodes


def wait_link(timeout=90):
    end = time.monotonic() + timeout
    last = ''
    while time.monotonic() < end:
        try:
            return pair_snapshot()
        except (OSError, ValueError, CheckFailed) as exc:
            last = str(exc)
            time.sleep(1)
    raise CheckFailed('Соединение не восстановилось за %s с: %s' % (timeout, last))


def boot(host):
    value = web_json(host, '/api/esp-update').get('boot')
    if not isinstance(value, str) or not value:
        raise CheckFailed('API ESP32 не вернул идентификатор запуска (boot): ' + host)
    return value


def reboot(hosts):
    old = {host: boot(host) for host in hosts}
    for host in hosts:
        try:
            web(host, '/api?action=8&cmd=3')
        except (OSError, TimeoutError):
            # A reset may close HTTP before sending its final response.
            pass
    end = time.monotonic() + 90
    while time.monotonic() < end:
        try:
            if all(boot(host) != value for host, value in old.items()):
                wait_link(max(1, int(end - time.monotonic())))
                return
        except (OSError, ValueError):
            pass
        time.sleep(1)
    raise CheckFailed('Новый запуск CZC и восстановление соединения не подтверждены.')


def worker(config, broker):
    """The Docker worker reads source through stdin; no mounted files or Docker socket."""
    name = 'czc-accept-' + secrets.token_hex(5)
    argv = ['docker', 'run', '--rm', '--name', name, '--read-only', '--cap-drop', 'ALL',
            '--security-opt', 'no-new-privileges', '--network', 'container:' + broker,
            '-i', IMAGE, 'python', '-B', '-u', '-']
    code = RADIO_SOURCE + '\nCONFIG = ' + repr(config) + '\nprint("CZC_RESULT=" + json.dumps(radio_accept(CONFIG), ensure_ascii=False))\n'
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report = None
    pending = b''
    try:
        proc.stdin.write(code.encode())
        proc.stdin.close()
        deadline = time.monotonic() + 480
        while time.monotonic() < deadline:
            if not select.select([proc.stdout], [], [], 1)[0]:
                continue
            chunk = os.read(proc.stdout.fileno(), 4096)
            if not chunk:
                break
            pending += chunk
            if len(pending) > 32768:
                raise CheckFailed('Переполнение вывода приёмки.')
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                text = line.decode(errors='replace')
                if text.startswith('CZC_RESULT='):
                    report = json.loads(text[len('CZC_RESULT='):])
                elif text.endswith((': PASS', ': FAIL')) or 'NOT_RUN' in text or text == 'FAIL':
                    print('  ' + text, flush=True)
        if report is None:
            raise CheckFailed('Тестовый контейнер не вернул итог за отведённое время. Проверьте доступ Docker к CZC.')
        if proc.wait(timeout=5) != 0:
            raise CheckFailed('Тестовый контейнер завершился с ошибкой.')
        if report['verdict'] != 'PASS':
            # One bounded diagnostic on failure. No full successful dumps or PSK.
            report['host_http'] = http_observations()
            if report.get('tls'):
                report['admin_tls'] = [dict(host=h, **gateway_observation(h).get('admin_tls', {}))
                                       for h in (MASTER, SATELLITE)]
            for observation in report['host_http']:
                old = config.get('boot_baseline', {}).get(observation['host'])
                if old and observation.get('boot'):
                    observation['boot_changed'] = old != observation['boot']
            print(json.dumps(report, ensure_ascii=False), flush=True)
            raise RadioCheckFailed(report)
        return report
    finally:
        if proc.poll() is None:
            command(['docker', 'stop', '-t', '3', name], timeout=10, required=False)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        proc.stdout.close()


def compose_preflight():
    try:
        services = set(command(COMPOSE + ['config', '--services']).splitlines())
    except CheckFailed:
        raise CheckFailed('Не удалось прочитать Compose. Запустите из каталога moskito с существующим '
                          'Compose-файлом; проверьте docker compose config --services.') from None
    missing = {'mosquitto', 'zigbee2mqtt'} - services
    if missing:
        raise CheckFailed('В Compose отсутствуют службы: ' + ', '.join(sorted(missing)) +
                          '. Найдены: ' + (', '.join(sorted(services)) or 'нет') +
                          '. Запущенные контейнеры для этой проверки не требуются.')


def service_container(service):
    containers = command(COMPOSE + ['ps', '--all', '-q', service]).splitlines()
    if len(containers) > 1:
        raise CheckFailed('Ожидается один контейнер службы ' + service)
    return containers[0] if containers else None


def container_state(container):
    return json.loads(command(['docker', 'inspect', '--format', '{{json .State}}', container]))


def service_start(service):
    if service_container(service):
        command(COMPOSE + ['start', service], timeout=45)
    else:
        # Use this project's configuration and local image; never replace an
        # existing container or silently upgrade a mutable image tag.
        command(COMPOSE + ['up', '-d', '--no-deps', '--no-recreate', '--no-build',
                           '--pull', 'never', service], timeout=60)
    container = service_container(service)
    if not container:
        raise CheckFailed('После запуска не найден контейнер службы ' + service)
    return container


def broker_start():
    container = service_start('mosquitto')
    end = time.monotonic() + 45
    while time.monotonic() < end:
        state = container_state(container)
        if (state.get('Running') and not state.get('Restarting') and
                state.get('Health', {}).get('Status', 'healthy') == 'healthy'):
            return container
        time.sleep(1)
    raise CheckFailed('Mosquitto не готов за 45 с. Проверьте docker compose logs --tail 40 mosquitto.')


def z2m_start():
    container = service_start('zigbee2mqtt')
    end = time.monotonic() + 100
    while time.monotonic() < end:
        state = container_state(container)
        if state.get('Running') and not state.get('Restarting') and state.get('StartedAt'):
            # Only this container's current start counts, including when cleanup
            # finds it already running. Earlier successful starts cannot pass.
            logs = command(COMPOSE + ['logs', '--no-color', '--since', state['StartedAt'],
                                      '--tail', '160', 'zigbee2mqtt'], required=False)
            if 'Zigbee2MQTT started!' in logs:
                return
        time.sleep(2)
    raise CheckFailed('Zigbee2MQTT не подтвердил запуск за 100 с. Посмотрите его журнал.')


def sensor_checks(report):
    print('\nДатчик: ' + SENSOR + '. Z2M подключён только к Master.', flush=True)
    confirmed('Оставьте датчик рядом с Satellite, вне радиозоны Master.\n'
              'В UI Zigbee2MQTT дождитесь свежей температуры/влажности после короткого нажатия кнопки.\n'
              'Если датчик остался дочерним устройством Master: разрешите присоединение в Z2M и переприсоедините\n'
              'только этот датчик рядом с Satellite; дождитесь интервью. Сеть CZC сбрасывать не нужно.\n'
              'Запомните исходные единицы. Через Z2M смените их на противоположные (°C ↔ °F).\n'
              'Подтвердите изменение НА ЭКРАНЕ ДАТЧИКА, затем верните исходные единицы и проверьте экран снова.')
    report['sensor_initial'] = 'MANUAL_CONFIRMED'
    report['restore_satellite_power'] = True
    confirmed('Теперь отключите ПИТАНИЕ только Satellite. Master и Z2M оставьте включёнными.\n'
              'После отключения Satellite в Z2M попробуйте ещё раз сменить единицы.\n'
              'Дождитесь ошибки команды; экран датчика не должен изменить единицы.\n'
              'Satellite пока не включайте. Подтверждайте только ошибку новой команды, а не отсутствие старых MQTT-сообщений.')
    state = web_json(MASTER)['status']
    if state.get('peer') or state.get('peers_online'):
        raise CheckFailed('Master всё ещё видит Satellite; отрицательный контроль не подтверждён.')
    report['sensor_without_satellite'] = 'MANUAL_CONFIRMED'
    confirmed('Верните питание Satellite. Не перезапускайте Z2M и не переприсоединяйте датчик.\n'
              'Дождитесь «Соединено», коротко нажмите кнопку датчика.\n'
              'Через Z2M снова смените единицы и убедитесь, что экран датчика изменился.\n'
              'Верните исходные единицы; дождитесь свежей температуры/влажности без ошибки команды.')
    wait_link()
    report['sensor_recovery'] = 'MANUAL_CONFIRMED'
    report.pop('restore_satellite_power', None)


def main(mode):
    report = dict(schema='czc-two-device-acceptance-1', verdict='INCOMPLETE', topology='1 Master + 1 Satellite',
                  mode=mode, test_revision=TEST_REVISION,
                  rf_isolation='NOT_RUN', reboots=[], sensor_initial='NOT_RUN', sensor_without_satellite='NOT_RUN',
                  sensor_recovery='NOT_RUN', ipv6='NOT_TESTED', eight_satellites='NOT_TESTED',
                  scope='Packet forwarding and sensor dependency; RF bypass between CZCs is not excluded')
    restore_z2m = False
    try:
        print('CZC: единая приёмка двух устройств; режим ' + mode + '; тест ' + report['test_revision'] + '.', flush=True)
        if mode not in ('local', 'full'):
            raise CheckFailed('Неизвестный режим; выберите --mode local или --mode full.')
        command(COMPOSE + ['version'])
        compose_preflight()
        report['http'] = http_preflight()
        initial = pair_snapshot()
        controls = admin_addresses()
        boots = {host: boot(host) for host in (MASTER, SATELLITE)}
        print('Роли, взаимные IEEE/IP, радио, PSK и режим отладки: PASS', flush=True)
        report['admin_hosts'] = controls
        print('Служебный TLS: Master %s; Satellite %s.' % tuple(controls), flush=True)
        print('Mosquitto будет запущен, Z2M остановлен на время тестов; оба CZC будут перезапущены.\n'
              'После автоматических проверок Z2M запустится. Настройки сети и PSK сохраняются.', flush=True)
        if not command(['docker', 'image', 'ls', '--quiet', IMAGE]):
            print('Загружаю контейнер Python 3.13 для TLS PSK…', flush=True)
            command(['docker', 'pull', IMAGE], timeout=180)
        config = dict(master=controls[0], satellite=controls[1], ieees=IEEES, key=PSK,
                      peer_port=initial[0]['peer_port'], admin_port=initial[0]['admin_port'],
                      operation='full', boot_baseline=boots, expect_family=PEER_FAMILY)
        broker = broker_start()
        z2m = service_container('zigbee2mqtt')
        if z2m:
            state = container_state(z2m)
            restore_z2m = bool(state.get('Running') or state.get('Restarting'))
            # Stop even a restarting container so it cannot contend for UART.
            command(COMPOSE + ['stop', '-t', '15', 'zigbee2mqtt'], timeout=40)
            state = container_state(z2m)
            if state.get('Running') or state.get('Restarting'):
                raise CheckFailed('Zigbee2MQTT не остановлен; проверка радио не начата.')
        print('\n[1/3] Радио и зашифрованный канал', flush=True)
        first = worker(config, broker)
        report['radio'] = dict(checks=len(first['checks']), deliveries=first['deliveries'], resources=first['resources'])
        report['radio']['broadcast'] = first.get('broadcast', [])
        report['peer_ip_family'] = first['ip_family']
        families = {first['ip_family']}
        print('\n[2/3] Перезапуски с сохранением сети; Z2M ещё остановлен', flush=True)
        config.update(operation='smoke', baseline=first['network'], post_reboot=True)
        for name, hosts in [('Satellite', [SATELLITE]), ('Master', [MASTER]), ('Оба CZC', [MASTER, SATELLITE])]:
            print('Перезапуск: ' + name, flush=True)
            reboot(hosts)
            config['boot_baseline'] = {host: boot(host) for host in (MASTER, SATELLITE)}
            smoke = worker(config, broker)
            if smoke.get('admin_recovery'):
                report.setdefault('admin_recovery', []).append(dict(reboot=name, nodes=smoke['admin_recovery']))
            families.add(smoke['ip_family'])
            report['reboots'].append(name)
        print('Запускаю Zigbee2MQTT…', flush=True)
        try:
            z2m_start()
        finally:
            # A failed start already has its own bounded wait and diagnostic;
            # do not repeat that same wait from exception cleanup.
            restore_z2m = False
        wait_link()
        report['z2m_recovery'] = 'PASS'
        print('\n[3/3] Датчик через Satellite', flush=True)
        if mode == 'full':
            sensor_checks(report)
            if all(report[k] == 'MANUAL_CONFIRMED' for k in ('sensor_initial', 'sensor_without_satellite', 'sensor_recovery')):
                report['verdict'] = 'PASS_TWO_DEVICES_AND_SENSOR'
        else:
            print('Датчик через Satellite: NOT_RUN (режим local).', flush=True)
        report['final_links'] = 'PASS'
        if families == {6}:
            report['ipv6'] = 'PASS_BACKHAUL'
    except KeyboardInterrupt:
        report['reason'] = 'Прервано пользователем; незавершённые проверки не считаются успешными.'
    except RadioCheckFailed as exc:
        report.update(verdict='FAIL', reason=str(exc))
    except Exception as exc:
        report.update(verdict='FAIL', reason=str(exc) or type(exc).__name__)
    finally:
        if restore_z2m:
            print('Возвращаю Zigbee2MQTT в работу…', flush=True)
            try:
                z2m_start()
            except Exception as exc:
                report.update(verdict='FAIL', cleanup_error=str(exc))
        if report.get('restore_satellite_power'):
            print('Верните питание Satellite и исходные единицы датчика, если меняли их во время проверки.', flush=True)
        print('\nCZC_RESULT ' + json.dumps(report, ensure_ascii=False), flush=True)
        if report['verdict'] == 'PASS_TWO_DEVICES_AND_SENSOR':
            print('Приёмка этой пары и датчика завершена. Для обычной работы установите сборку prod на оба CZC.')
        elif report['verdict'] == 'INCOMPLETE':
            print('Частичный результат: полная работа через удалённый Satellite ещё не доказана.')
    return 0 if report['verdict'] == 'PASS_TWO_DEVICES_AND_SENSOR' else 1


def parse_args(argv=None):
    def psk(value):
        if not re.fullmatch(r'[0-9a-fA-F]{64}', value):
            raise argparse.ArgumentTypeError('PSK должен содержать ровно 64 шестнадцатеричных символа.')
        return value.lower()

    def ieee(value):
        value = normalized_ieee(value)
        if not re.fullmatch(r'[0-9a-f]{16}', value):
            raise argparse.ArgumentTypeError('IEEE должен содержать 16 шестнадцатеричных символов; двоеточия допустимы.')
        return value

    def address(value):
        try:
            return str(ipaddress.ip_address(value))
        except ValueError:
            raise argparse.ArgumentTypeError('Укажите IP-адрес CZC.') from None

    parser = argparse.ArgumentParser(
        description='Приёмка 1 Master + 1 Satellite. Z2M заранее запускать не нужно.',
        epilog='local: устройства рядом, без вопросов; итог всей функции INCOMPLETE. '
               'full: датчик рядом с Satellite, вне радиозоны Master; '
               'прямая радиосвязь между CZC не проверяется; '
               'три ручных подтверждения в терминале. Оба режима перезапускают CZC и временно останавливают Z2M.')
    parser.add_argument('--mode', required=True, choices=('local', 'full', 'diagnose'), help='Приёмка или короткая диагностика HTTP без TLS/Docker/перезагрузок')
    parser.add_argument('--master', type=address, required=True, metavar='IP', help='IP Master')
    parser.add_argument('--satellite', type=address, required=True, metavar='IP', help='IP Satellite')
    parser.add_argument('--admin-family', type=int, choices=(4, 6), default=0,
                        help='IP-семейство служебного TLS; адрес того же интерфейса. По умолчанию как --master/--satellite')
    parser.add_argument('--peer-family', type=int, choices=(4, 6), default=0,
                        help='Требовать IPv4/IPv6 именно у канала Master–Satellite')
    parser.add_argument('--psk', type=psk, metavar='HEX', help='Общий ключ: 64 hex-символа; не нужен для diagnose')
    parser.add_argument('--master-ieee', type=ieee, metavar='IEEE', help='Ожидаемый IEEE Master; иначе определяется при первом запросе')
    parser.add_argument('--satellite-ieee', type=ieee, metavar='IEEE', help='Ожидаемый IEEE Satellite; иначе определяется при первом запросе')
    parser.add_argument('--sensor', type=ieee, metavar='IEEE', help='IEEE датчика; обязателен в режиме full')
    parser.add_argument('--compose-dir', type=Path, default=Path('.'), metavar='DIR', help='Каталог существующего Compose (по умолчанию текущий)')
    parser.add_argument('--compose-file', metavar='FILE', help='Compose-файл, если у него нестандартное имя; путь относительно --compose-dir')
    parser.add_argument('--project-name', metavar='NAME', help='Имя существующего проекта Compose, если задано через -p')
    parser.add_argument('--master-cookie', default='', metavar='COOKIE', help='Cookie входа в web UI Master, если требуется')
    parser.add_argument('--satellite-cookie', default='', metavar='COOKIE', help='Cookie входа в web UI Satellite, если требуется')
    args = parser.parse_args(argv)
    if args.mode != 'diagnose' and not args.psk:
        parser.error('--psk обязателен для local/full.')
    if args.mode == 'full' and not args.sensor:
        parser.error('--sensor обязателен для full.')
    if args.master == args.satellite or (args.master_ieee and args.master_ieee == args.satellite_ieee):
        parser.error('Master и Satellite должны иметь разные IP и IEEE.')
    args.compose_dir = args.compose_dir.expanduser().resolve()
    if args.mode != 'diagnose' and not args.compose_dir.is_dir():
        parser.error('Каталог --compose-dir не найден.')
    return args


def cli(argv=None):
    global MASTER, SATELLITE, IEEES, SENSOR, PSK, COMPOSE, COMPOSE_DIR, WEB_COOKIES, RADIO_SOURCE, ADMIN_FAMILY, PEER_FAMILY
    args = parse_args(argv)
    MASTER, SATELLITE, PSK = args.master, args.satellite, args.psk
    ADMIN_FAMILY, PEER_FAMILY = args.admin_family, args.peer_family
    IEEES = [args.master_ieee, args.satellite_ieee]
    SENSOR = '0x' + args.sensor if args.sensor else None
    COMPOSE = ['docker', 'compose']
    if args.compose_file:
        COMPOSE += ['-f', args.compose_file]
    if args.project_name:
        COMPOSE += ['-p', args.project_name]
    COMPOSE_DIR = str(args.compose_dir)
    WEB_COOKIES = {MASTER: args.master_cookie, SATELLITE: args.satellite_cookie}
    if args.mode == 'diagnose':
        return diagnose()
    if 'RADIO_SOURCE' not in globals():
        directory = Path(__file__).resolve().parent
        RADIO_SOURCE = (directory/'czc_pair.py').read_text().split('\ndef acceptance(args, key):')[0] + '\n' + (directory/'accept_hardware_radio.py').read_text()
    return main(args.mode)


if __name__ == '__main__':
    raise SystemExit(cli())
