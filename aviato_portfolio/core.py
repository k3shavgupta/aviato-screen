"""Bounded Kite account reads and a small private display cache. No trading API."""
from datetime import datetime, timedelta
from contextlib import contextmanager
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import stat
import secrets
import time
from urllib.parse import urlencode
from zoneinfo import ZoneInfo

CONFIG = Path.home() / '.config/aviato-screen/kite.json'
CACHE = Path.home() / '.local/state/aviato-screen/portfolio.json'
CALLBACK = 'http://127.0.0.1:8766/kite/callback'
GET_PATHS = frozenset(('/portfolio/holdings', '/portfolio/positions', '/user/margins/equity'))
MONEY_LIMIT = Decimal('1000000000000000')
SNAPSHOT_FIELDS = ('schema','status','updated_at','currency','holdings_value','holdings_pnl','cash','positions_pnl','unpriced')


class DataError(Exception):
    """A generic error whose message is safe to show without provider payloads."""


class AuthRequired(DataError):
    pass


def json_loads(raw):
    """Reject non-JSON constants and floating-point exponent overflow."""
    def finite_float(text):
        value = float(text)
        if not math.isfinite(value): raise ValueError('Nonfinite JSON number')
        return value
    def constant(text):
        raise ValueError('Nonfinite JSON constant')
    try:
        return json.loads(raw, parse_float=finite_float, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError):
        raise DataError('JSON data is invalid.') from None


@contextmanager
def private_parent(path, create=False):
    """Pin every directory by descriptor; never follow a parent symlink."""
    path = Path(path).absolute()
    if '..' in path.parts or not path.name:
        raise DataError('Private data path is invalid.')
    descriptor = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for part in path.parts[1:-1]:
            if create:
                try: os.mkdir(part, mode=0o700, dir_fd=descriptor)
                except FileExistsError: pass
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                            dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        info = os.fstat(descriptor)
        if info.st_uid != os.getuid():
            raise DataError('Private data directory is not owned by this user.')
        if create:
            os.fchmod(descriptor, 0o700)
        elif info.st_mode & 0o077:
            raise DataError('Private data directory must have mode 700.')
        yield descriptor, path.name
    finally:
        os.close(descriptor)


def read_private(path, limit=16384):
    with private_parent(path) as (parent, name):
        descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
                             dir_fd=parent)
    with os.fdopen(descriptor, 'rb') as file:
        info = os.fstat(file.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                info.st_mode & 0o077 or info.st_nlink != 1):
            raise DataError('Configuration must be a private user-owned file (mode 600).')
        data = file.read(limit + 1)
    if len(data) > limit:
        raise DataError('Local data exceeds its size limit.')
    result = json_loads(data)
    if not isinstance(result, dict):
        raise DataError('Local data must be an object.')
    return result


def write_private(path, value):
    with private_parent(path, create=True) as (parent, name):
        temporary = '.' + name + '-' + secrets.token_hex(16)
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                             0o600, dir_fd=parent)
        try:
            with os.fdopen(descriptor, 'w') as file:
                json.dump(value, file, allow_nan=False, separators=(',', ':'))
                file.write('\n')
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, name, src_dir_fd=parent, dst_dir_fd=parent)
            os.fsync(parent)
        finally:
            try: os.unlink(temporary, dir_fd=parent)
            except FileNotFoundError: pass


def amount(value):
    if isinstance(value, bool) or value is None:
        raise DataError('Account response contains an invalid number.')
    try:
        number = Decimal(str(value))
    except (InvalidOperation, ValueError):
        raise DataError('Account response contains an invalid number.') from None
    if not number.is_finite() or number.copy_abs() > MONEY_LIMIT:
        raise DataError('Account response contains an invalid number.')
    return number


def money(value):
    try:
        return float(amount(value).quantize(Decimal('.01'), rounding=ROUND_HALF_UP))
    except InvalidOperation:
        raise DataError('Account amount cannot be represented safely.') from None


def validated_snapshot(data, now):
    """Retain only display fields after checking the complete cache contract."""
    if (not isinstance(data, dict) or type(data.get('schema')) is not int or data['schema'] != 1 or
            data.get('currency') != 'INR' or
            data.get('status') not in ('ok','stale','setup_required','login_required')):
        raise DataError('Portfolio snapshot is invalid.')
    updated = data.get('updated_at')
    if type(updated) is not int or updated < 0 or updated > now + 60:
        raise DataError('Portfolio timestamp is invalid.')
    balances = ('holdings_value','holdings_pnl','cash','positions_pnl','unpriced')
    if (data['status'] == 'ok' and updated == 0 or
            (updated > 0 or data['status'] == 'ok') and any(key not in data for key in balances)):
        raise DataError('Portfolio snapshot is incomplete.')
    for key in balances[:-1]:
        if key not in data: continue
        value = data[key]
        if key == 'holdings_value' and value is None: continue
        if type(value) not in (int, float):
            raise DataError('Portfolio amount is invalid.')
        number = amount(value)
        if key == 'holdings_value' and number < 0:
            raise DataError('Portfolio holdings value is invalid.')
    if 'unpriced' in data and (type(data['unpriced']) is not int or not 0 <= data['unpriced'] <= 1000000):
        raise DataError('Portfolio unpriced count is invalid.')
    if 'holdings_value' in data and (data['holdings_value'] is None) != (data.get('unpriced', 0) > 0):
        raise DataError('Portfolio holdings availability is inconsistent.')
    return {key:data[key] for key in SNAPSHOT_FIELDS if key in data}


def units(row, key):
    value = amount(row.get(key, 0))
    if value < 0 or value != value.to_integral_value():
        raise DataError('Account response contains an invalid holding quantity.')
    return value


def summarize(holdings, positions, margins, now):
    if not isinstance(holdings, list) or not isinstance(positions, dict) or not isinstance(positions.get('net'), list):
        raise DataError('Account response is incomplete.')
    if not isinstance(margins, dict) or not isinstance(margins.get('available'), dict):
        raise DataError('Account response is incomplete.')
    total = Decimal(0)
    pnl = Decimal(0)
    unpriced = 0
    for row in holdings:
        if not isinstance(row, dict) or not isinstance(row.get('mtf', {}), dict) or not {'quantity','last_price','pnl'} <= row.keys():
            raise DataError('Account response is incomplete.')
        # quantity is already reduced after a CNC sale. used_quantity is NOT
        # another deduction; t1 and funded MTF units are separate holdings.
        quantity = units(row, 'quantity') + units(row, 't1_quantity') + units(row.get('mtf', {}), 'quantity')
        price = row.get('last_price')
        if quantity:
            if price is None or amount(price) <= 0 or row.get('discrepancy'):
                unpriced += 1
            else:
                total += quantity * amount(price)
        pnl += amount(row['pnl'])
    try:
        position_pnl = sum((amount(row['pnl']) for row in positions['net']), Decimal(0))
        cash = amount(margins['available']['cash'])
    except (KeyError, TypeError):
        raise DataError('Account response is incomplete.') from None
    return {'schema': 1, 'status': 'ok', 'updated_at': int(now), 'currency': 'INR',
            'holdings_value': None if unpriced else money(total), 'holdings_pnl': money(pnl),
            'cash': money(cash), 'positions_pnl': money(position_pnl), 'unpriced': unpriced}


def next_expiry(now):
    local = datetime.fromtimestamp(now, ZoneInfo('Asia/Kolkata'))
    cutoff = local.replace(hour=6, minute=0, second=0, microsecond=0)
    if cutoff <= local: cutoff += timedelta(days=1)
    return int(cutoff.timestamp())


def credential(config, name):
    value = config.get(name, '')
    if not isinstance(value, str) or not value or len(value) > 256 or any(ord(c) < 33 or ord(c) > 126 for c in value):
        raise DataError('Kite credential is missing or invalid; run configure locally.')
    return value


class HTTPTransport:
    """One provider host, no redirects; reuse TLS within a collection cycle."""
    def __init__(self):
        self.connection = None
        self.last_request = None

    def __call__(self, method, path, headers, body):
        if not (method == 'GET' and path in GET_PATHS or method == 'POST' and path == '/session/token'):
            raise DataError('Account endpoint is not allowed.')
        if self.last_request is not None:
            time.sleep(max(0, 1 - (time.monotonic() - self.last_request)))
        self.last_request = time.monotonic()
        if self.connection is None:
            self.connection = http.client.HTTPSConnection('api.kite.trade', timeout=8)
        try:
            self.connection.request(method, path, body=body, headers=headers)
            response = self.connection.getresponse()
            raw = response.read(2 * 1024 * 1024 + 1)
            if response.status in (401, 403): raise AuthRequired('Kite login is required.')
            if response.status != 200 or len(raw) > 2 * 1024 * 1024:
                raise DataError('Kite account request failed.')
            value = json_loads(raw)
            if not isinstance(value, dict): raise DataError('Kite account response is invalid.')
            return value
        except DataError:
            self.close()
            raise
        except (OSError, http.client.HTTPException, ValueError):
            self.close()
            raise DataError('Kite connection failed; previous values retained.') from None

    def close(self):
        if self.connection is not None:
            self.connection.close()
            self.connection = None


class KiteClient:
    def __init__(self, config, transport=None):
        self.config = config
        self.transport = transport if transport is not None else HTTPTransport()

    def request(self, method, path, fields=None):
        headers = {'X-Kite-Version':'3', 'User-Agent':'aviato-screen/1', 'Accept':'application/json'}
        if method == 'GET':
            if path not in GET_PATHS: raise DataError('Account endpoint is not allowed.')
            headers['Authorization'] = 'token ' + credential(self.config, 'api_key') + ':' + credential(self.config, 'access_token')
        elif method != 'POST' or path != '/session/token':
            raise DataError('Account endpoint is not allowed.')
        body = urlencode(fields).encode() if fields is not None else None
        if body is not None: headers['Content-Type'] = 'application/x-www-form-urlencoded'
        result = self.transport(method, path, headers, body)
        if not isinstance(result, dict): raise DataError('Kite account response is invalid.')
        if result.get('error_type') == 'TokenException': raise AuthRequired('Kite login is required.')
        if result.get('status') != 'success' or 'data' not in result:
            raise DataError('Kite account request failed.')
        return result['data']

    def read(self, path):
        return self.request('GET', path)

    def create_session(self, token):
        if not isinstance(token, str) or not token or len(token) > 256:
            raise DataError('Kite login response is invalid.')
        key, secret = credential(self.config, 'api_key'), credential(self.config, 'api_secret')
        checksum = hashlib.sha256((key + token + secret).encode()).hexdigest()
        data = self.request('POST', '/session/token', {'api_key':key, 'request_token':token, 'checksum':checksum})
        if not isinstance(data, dict): raise DataError('Kite login response is invalid.')
        credential(data, 'access_token')
        return data

    def snapshot(self, now):
        try:
            return summarize(self.read('/portfolio/holdings'), self.read('/portfolio/positions'),
                             self.read('/user/margins/equity'), now)
        finally:
            close = getattr(self.transport, 'close', None)
            if close: close()


class Collector:
    def __init__(self, config=CONFIG, cache=CACHE, client_factory=KiteClient, clock=time.time):
        self.config, self.cache = Path(config), Path(cache)
        self.client_factory, self.clock = client_factory, clock
        self.delay = 120
        self.blocked = None
        self.cache_dirty = True
        try:
            cached = read_private(self.cache, 4096)
            self.previous = validated_snapshot(cached, self.clock())
            self.cache_dirty = cached != self.previous
        except (OSError, DataError):
            self.previous = {}

    def publish(self, data):
        safe = validated_snapshot(data, self.clock())
        if self.cache_dirty or safe != self.previous or not self.cache.exists():
            write_private(self.cache, safe)
        self.cache_dirty = False
        self.previous = safe

    def state(self, status):
        return dict(self.previous, schema=1, status=status, currency='INR', updated_at=self.previous.get('updated_at', 0))

    def step(self):
        try:
            config = read_private(self.config)
            credential(config, 'api_key'); credential(config, 'api_secret')
        except (OSError, DataError):
            self.publish(self.state('setup_required'))
            return 60
        token = config.get('access_token')
        identity = (config.get('api_key'), token)
        now = self.clock()
        expiry = config.get('expires_at', 0)
        try: expires = float(expiry) if type(expiry) in (int, float) else 0
        except (ValueError, TypeError, OverflowError): expires = 0
        if not token or not math.isfinite(expires) or not now < expires <= now + 86400 or identity == self.blocked:
            self.publish(self.state('login_required'))
            return 60
        try:
            snapshot = validated_snapshot(self.client_factory(config).snapshot(now), now)
        except AuthRequired:
            self.blocked = identity
            self.publish(self.state('login_required'))
            return 60
        except (DataError, OSError, ValueError, KeyError, TypeError, InvalidOperation):
            self.publish(self.state('stale'))
            self.delay = min(self.delay * 2, 3600)
            return self.delay
        self.publish(snapshot)
        self.delay = 120
        self.blocked = None
        return 120
