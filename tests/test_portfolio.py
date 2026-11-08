import hashlib
import http.client
import importlib.util
import json
import os
import socket
import subprocess
import sys
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


class PortfolioTests(unittest.TestCase):
    def setUp(self):
        path = ROOT / 'aviato_portfolio/core.py'
        self.assertTrue(path.exists(), 'Kite collector is not implemented')
        spec = importlib.util.spec_from_file_location('portfolio_core', path)
        self.core = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.core)

    def test_settled_t1_and_mtf_units_not_sold_units_or_positions(self):
        data = self.core.summarize(
            [{'quantity': 60, 't1_quantity': 2, 'used_quantity': 40,
              'mtf': {'quantity': 3}, 'last_price': 10, 'pnl': 50}],
            {'net': [{'quantity': -40, 'pnl': -12}], 'day': [{'pnl': 999}]},
            {'available': {'cash': 100}, 'net': 80}, 1000)
        self.assertEqual(data['holdings_value'], 650)
        self.assertEqual(data['cash'], 100)
        self.assertEqual(data['positions_pnl'], -12)
        self.assertEqual(data['holdings_pnl'], 50)
        self.assertNotIn('total', data)
        self.assertEqual(data['currency'], 'INR')

    def test_unpriced_holding_cannot_understate_total(self):
        data = self.core.summarize([{'quantity': 1, 'last_price': 0, 'pnl': 0}],
                                   {'net': []}, {'available': {'cash': 0}}, 1000)
        self.assertIsNone(data['holdings_value'])
        self.assertEqual(data['unpriced'], 1)
        self.assertEqual(data['cash'], 0)

    def test_empty_accounts_are_real_zero(self):
        data = self.core.summarize([], {'net': []}, {'available': {'cash': 0}}, 1000)
        self.assertEqual(data['holdings_value'], 0)
        self.assertEqual(data['positions_pnl'], 0)

    def test_invalid_data_rejected(self):
        for row in ({}, {'quantity':1}, {'quantity':1,'last_price':10}):
            with self.assertRaises(self.core.DataError):
                self.core.summarize([row], {'net':[]}, {'available':{'cash':100}}, 1000)
        for price in (float('nan'), float('inf'), 'nonsense'):
            with self.assertRaises(self.core.DataError):
                self.core.summarize([{'quantity': 1, 'last_price': price, 'pnl': 0}],
                                    {'net': []}, {'available': {'cash': 0}}, 1000)
        with self.assertRaises(self.core.DataError):
            self.core.summarize([], {}, {'available': {'cash': 0}}, 1000)

    def test_private_atomic_file_and_unsafe_mode_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'config/kite.json'
            self.core.write_private(path, {'api_key': 'test-key'})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(self.core.read_private(path)['api_key'], 'test-key')
            path.chmod(0o644)
            with self.assertRaises(self.core.DataError): self.core.read_private(path)

    def test_private_json_rejects_nonfinite_numbers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'cache.json'
            for value in ('NaN', 'Infinity', '-Infinity', '1e9999'):
                with self.subTest(value=value):
                    path.write_text('{"nested":{"cash":' + value + '}}')
                    path.chmod(0o600)
                    with self.assertRaises(self.core.DataError): self.core.read_private(path)

    def test_provider_json_rejects_nonfinite_numbers(self):
        class Response:
            status = 200
            def read(self, limit): return b'{"status":"success","data":{"cash":1e9999}}'
        class Connection:
            def request(self, *args, **kwargs): pass
            def getresponse(self): return Response()
            def close(self): pass
        transport = self.core.HTTPTransport()
        transport.connection = Connection()
        with self.assertRaises(self.core.DataError):
            transport('GET', '/portfolio/holdings', {}, None)

    def test_corrupt_cached_snapshots_are_discarded_without_crashing(self):
        valid = self.core.summarize([], {'net':[]}, {'available':{'cash':100}}, 1000)
        invalid = [dict(valid, **{key:value}) for key, value in (
            ('schema', True), ('schema', 2), ('currency', 'USD'), ('status', 'unknown'),
            ('updated_at', True), ('updated_at', -1), ('updated_at', 1061),
            ('updated_at', 0), ('cash', '100'), ('cash', {'secret':'never-retain'}),
            ('cash', True), ('cash', 1e15 + 1), ('cash', float('nan')),
            ('holdings_value', -1), ('holdings_value', None),
            ('positions_pnl', -1e15 - 1), ('unpriced', -1), ('unpriced', True),
            ('unpriced', 1000001))]
        invalid.append({key:value for key, value in valid.items() if key != 'cash'})
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'cache.json'
            for value in invalid:
                with self.subTest(value=value):
                    cache.write_text(json.dumps(value))
                    cache.chmod(0o600)
                    collector = self.core.Collector(Path(directory)/'missing', cache, clock=lambda:1000)
                    self.assertEqual(collector.previous, {})
                    self.assertEqual(collector.step(), 60)
                    self.assertEqual(self.core.read_private(cache), {
                        'schema':1, 'status':'setup_required', 'currency':'INR', 'updated_at':0})

    def test_valid_cached_snapshots_are_sanitized_and_retained(self):
        valid = self.core.summarize([], {'net':[]}, {'available':{'cash':100}}, 1000)
        states = [dict(valid, status=status) for status in ('ok','stale','login_required','setup_required')]
        states.append(dict(valid, holdings_value=None, unpriced=1))
        states.extend({'schema':1, 'currency':'INR', 'status':status, 'updated_at':0}
                      for status in ('stale','login_required','setup_required'))
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)/'cache.json'
            for state in states:
                with self.subTest(state=state):
                    self.core.write_private(cache, dict(state, api_secret='must-disappear'))
                    collector = self.core.Collector(Path(directory)/'missing', cache, clock=lambda:1000)
                    self.assertEqual(collector.previous, state)
                    collector.step()
                    self.assertNotIn('api_secret', self.core.read_private(cache))

    def test_aggregate_money_is_bounded_and_decimal_errors_are_safe(self):
        from decimal import Decimal, localcontext
        cases = [
            ([{'quantity':2,'last_price':1e15,'pnl':0}], {'net':[]}),
            ([{'quantity':0,'last_price':0,'pnl':1e15}] * 2, {'net':[]}),
            ([], {'net':[{'pnl':1e15},{'pnl':1}]}),
        ]
        for holdings, positions in cases:
            with self.subTest(holdings=holdings, positions=positions):
                with self.assertRaises(self.core.DataError):
                    self.core.summarize(holdings, positions, {'available':{'cash':0}}, 1000)
        self.assertEqual(self.core.money(Decimal('1000000000000000')), 1e15)
        with self.assertRaises(self.core.DataError): self.core.money(Decimal('1e9999999'))
        with localcontext() as context:
            context.prec = 2
            with self.assertRaises(self.core.DataError): self.core.money(Decimal('123.45'))

    def test_invalid_token_expiry_never_calls_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            config, cache = Path(directory)/'config.json', Path(directory)/'cache.json'
            for expiry in ('NaN', 'Infinity', '-Infinity', '1e9999', 'null', 'true', '"nan"',
                           '"inf"', '-1', '1000', '87401'):
                with self.subTest(expiry=expiry):
                    config.write_text('{"api_key":"test-key","api_secret":"test-secret",'
                                      '"access_token":"test-access","expires_at":' + expiry + '}')
                    config.chmod(0o600)
                    def forbidden(config): raise AssertionError('provider called with invalid expiry')
                    collector = self.core.Collector(config, cache, forbidden, lambda:1000)
                    self.assertEqual(collector.step(), 60)
                    self.assertIn(self.core.read_private(cache)['status'], ('setup_required','login_required'))

    def test_expiry_boundary_and_invalid_fresh_snapshot_preserve_last_good(self):
        with tempfile.TemporaryDirectory() as directory:
            config, cache = Path(directory)/'config.json', Path(directory)/'cache.json'
            self.core.write_private(config, {'api_key':'test-key','api_secret':'test-secret',
                                            'access_token':'test-access','expires_at':87400})
            valid = self.core.summarize([], {'net':[]}, {'available':{'cash':100}}, 1000)
            responses = [valid, dict(valid, cash=float('nan'))]
            class Client:
                def snapshot(self, now): return responses.pop(0)
            collector = self.core.Collector(config, cache, lambda config:Client(), lambda:1000)
            self.assertEqual(collector.step(), 120)
            self.assertEqual(collector.step(), 240)
            self.assertEqual(self.core.read_private(cache), dict(valid, status='stale'))

    def test_checksum_and_api_allowlist(self):
        requests = []
        def transport(method, path, headers, body):
            requests.append((method, path, headers, body))
            return {'status': 'success', 'data': {'access_token': 'test-access'}}
        client = self.core.KiteClient({'api_key': 'test-key', 'api_secret': 'test-secret'}, transport)
        result = client.create_session('test-request')
        self.assertEqual(result['access_token'], 'test-access')
        from urllib.parse import parse_qs
        fields = parse_qs(requests[0][3].decode())
        self.assertEqual(fields['checksum'][0], hashlib.sha256(b'test-keytest-requesttest-secret').hexdigest())
        self.assertEqual(requests[0][1], '/session/token')
        with self.assertRaises(self.core.DataError): client.read('/orders')

    def test_private_storage_rejects_symlinked_ancestors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            outside = root / 'outside'
            outside.mkdir()
            (root / 'redirect').symlink_to(outside, target_is_directory=True)
            target = root / 'redirect' / 'private' / 'kite.json'
            with self.assertRaises((OSError, self.core.DataError)):
                self.core.write_private(target, {'api_secret': 'test-secret'})
            self.assertFalse((outside / 'private' / 'kite.json').exists())
            self.core.write_private(outside / 'private' / 'kite.json', {'api_secret': 'test-secret'})
            with self.assertRaises((OSError, self.core.DataError)):
                self.core.read_private(target)

    def test_private_read_rejects_hardlinked_credentials(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'kite.json'
            self.core.write_private(path, {'api_secret': 'test-secret'})
            os.link(path, Path(directory) / 'alias.json')
            with self.assertRaises(self.core.DataError): self.core.read_private(path)

    def test_collector_preserves_snapshot_and_backs_off_without_logging_error(self):
        with tempfile.TemporaryDirectory() as directory:
            config, cache = Path(directory)/'config.json', Path(directory)/'cache.json'
            self.core.write_private(config, {'api_key':'test-key', 'api_secret':'test-secret',
                                            'access_token':'test-access', 'expires_at':2000})
            responses = [self.core.summarize([], {'net': []}, {'available': {'cash': 100}}, 1000),
                         OSError('SECRET must never appear in cache')]
            class Client:
                def snapshot(self, now):
                    result = responses.pop(0)
                    if isinstance(result, Exception): raise result
                    return result
            collector = self.core.Collector(config, cache, lambda c: Client(), lambda:1000)
            self.assertEqual(collector.step(), 120)
            self.assertEqual(collector.step(), 240)
            data = self.core.read_private(cache)
            self.assertEqual(data['status'], 'stale')
            self.assertEqual(data['cash'], 100)
            self.assertEqual(data['updated_at'], 1000)
            self.assertNotIn('SECRET', cache.read_text())

    def test_auth_error_waits_for_new_token(self):
        with tempfile.TemporaryDirectory() as directory:
            config, cache = Path(directory)/'config.json', Path(directory)/'cache.json'
            credentials = {'api_key':'test-key', 'api_secret':'test-secret', 'access_token':'bad', 'expires_at':2000}
            self.core.write_private(config, credentials)
            calls=[]
            class Client:
                def snapshot(self, now):
                    calls.append(1)
                    raise self_core.AuthRequired()
            self_core = self.core
            collector = self.core.Collector(config, cache, lambda c: Client(), lambda:1000)
            collector.step(); collector.step()
            self.assertEqual(len(calls), 1)
            self.assertEqual(self.core.read_private(cache)['status'], 'login_required')
            credentials['access_token']='new'
            self.core.write_private(config, credentials)
            collector.step()
            self.assertEqual(len(calls), 2)

    def test_missing_credentials_never_call_network(self):
        with tempfile.TemporaryDirectory() as directory:
            def forbidden(config): raise AssertionError('network with missing config')
            collector=self.core.Collector(Path(directory)/'missing', Path(directory)/'cache', forbidden, lambda:1000)
            self.assertEqual(collector.step(), 60)
            self.assertEqual(self.core.read_private(Path(directory)/'cache')['status'], 'setup_required')


class LoginTests(unittest.TestCase):
    def module(self):
        self.assertTrue((ROOT/'aviato_portfolio/cli.py').exists(), 'Kite login is not implemented')
        from aviato_portfolio import cli
        return cli

    def test_callback_requires_matching_state_before_token_exchange(self):
        cli = self.module()
        def forbidden(config): raise AssertionError('exchange with unverified state')
        with self.assertRaises(cli.DataError):
            cli.complete_login({}, {'state':['wrong'], 'request_token':['test-token']}, 'expected', forbidden, 1000)

    def test_idle_connection_times_out_before_valid_callback(self):
        cli = self.module()
        from http.server import HTTPServer
        ready, accepted = threading.Event(), threading.Event()
        servers, errors, exchanges = [], [], []
        complete_login = cli.complete_login

        class CallbackServer(HTTPServer):
            def get_request(self):
                connection = super().get_request()
                accepted.set()
                return connection

        def server_factory(address, handler):
            server = CallbackServer(('127.0.0.1', 0), handler)
            servers.append(server)
            ready.set()
            return server

        class Client:
            def create_session(self, token):
                exchanges.append(token)
                return {'access_token': 'test-access'}

        def finish(config, query, state):
            return complete_login(config, query, state, lambda config: Client(), 1000)

        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'kite.json'
            cli.write_private(config, {'api_key':'test-key', 'api_secret':'test-secret'})

            def login():
                try:
                    cli.login(config)
                except Exception as error:
                    errors.append(error)

            def request(path):
                connection = http.client.HTTPConnection(*servers[0].server_address, timeout=4)
                try:
                    connection.request('GET', path, headers={'Host':'127.0.0.1:8766'})
                    response = connection.getresponse()
                    result = response.status, response.read()
                    return result
                finally:
                    connection.close()

            with patch.object(cli, 'HTTPServer', server_factory), \
                    patch.object(cli.secrets, 'token_urlsafe', return_value='test-state'), \
                    patch.object(cli, 'complete_login', finish), \
                    patch.object(cli, 'print', create=True):
                worker = threading.Thread(target=login, daemon=True)
                idle = None
                worker.start()
                try:
                    self.assertTrue(ready.wait(2), errors)
                    idle = socket.create_connection(servers[0].server_address, timeout=1)
                    self.assertTrue(accepted.wait(2), 'Idle connection was not accepted')
                    status, _ = request('/kite/callback?state=test-state&request_token=test-request')
                    self.assertEqual(status, 303)
                    # Keep the first socket open until the callback succeeds:
                    # only the server's connection timeout can unblock it.
                    self.assertEqual(idle.recv(1), b'')
                    self.assertEqual(request('/done')[0], 200)
                    worker.join(3)
                    self.assertFalse(worker.is_alive())
                    self.assertEqual(errors, [])
                    self.assertEqual(exchanges, ['test-request'])
                    self.assertEqual(cli.read_private(config)['access_token'], 'test-access')
                finally:
                    if idle is not None:
                        idle.close()
                    if worker.is_alive() and servers:
                        try:
                            request('/done')
                        except OSError:
                            pass
                        worker.join(3)

    def test_configure_refuses_terminal_fallback_that_can_echo_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'kite.json'
            result = subprocess.run([sys.executable, '-m', 'aviato_portfolio', '--config', str(config), 'configure'],
                                    input='test-key\ntest-secret\n', text=True, capture_output=True,
                                    start_new_session=True, cwd=ROOT, timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(config.exists())
            self.assertNotIn('test-secret', result.stdout + result.stderr)

    def test_cli_disables_core_dumps(self):
        with tempfile.TemporaryDirectory() as directory:
            code = ('import ctypes, resource; from aviato_portfolio.cli import main; '
                    'main(["--cache", __import__("sys").argv[1], "status"]); '
                    'assert resource.getrlimit(resource.RLIMIT_CORE) == (0, 0); '
                    'assert ctypes.CDLL(None).prctl(3, 0, 0, 0, 0) == 0')
            result = subprocess.run([sys.executable, '-c', code, str(Path(directory)/'missing')],
                                    capture_output=True, text=True, cwd=ROOT, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_login_url_and_session_keep_only_credentials(self):
        cli = self.module()
        from urllib.parse import urlparse, parse_qs
        url = cli.login_url({'api_key':'test-key'}, 'random-state')
        self.assertEqual(urlparse(url).hostname, 'kite.zerodha.com')
        self.assertEqual(parse_qs(parse_qs(urlparse(url).query)['redirect_params'][0])['state'], ['random-state'])
        class Client:
            def create_session(self, token): return {'access_token':'test-access', 'user_name':'do-not-store'}
        result = cli.complete_login({'api_key':'test-key','api_secret':'test-secret'},
                                    {'state':['expected'],'request_token':['test-request']},
                                    'expected', lambda config:Client(), 1000)
        self.assertEqual(result['access_token'], 'test-access')
        self.assertGreater(result['expires_at'], 1000)
        self.assertNotIn('user_name', result)
        self.assertNotIn('request_token', result)


if __name__ == '__main__': unittest.main()
