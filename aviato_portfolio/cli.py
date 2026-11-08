"""Local setup and official Kite login; callback server exists only during login."""
import argparse
import ctypes
import fcntl
import getpass
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import secrets
import resource
import sys
import time
import warnings
from urllib.parse import urlencode, urlsplit, parse_qs

from .core import (CONFIG, CACHE, CALLBACK, Collector, DataError, KiteClient,
                   credential, next_expiry, read_private, write_private)


def login_url(config, state):
    return 'https://kite.zerodha.com/connect/login?' + urlencode({
        'v':3, 'api_key':credential(config, 'api_key'), 'redirect_params':urlencode({'state':state})})


def complete_login(config, query, state, client_factory=KiteClient, now=None):
    supplied = query.get('state', [])
    tokens = query.get('request_token', [])
    if len(supplied) != 1 or not hmac.compare_digest(supplied[0], state) or len(tokens) != 1:
        raise DataError('Login callback could not be verified. Start a fresh login.')
    client = client_factory(config)
    try:
        session = client.create_session(tokens[0])
    finally:
        close = getattr(getattr(client, 'transport', None), 'close', None)
        if close: close()
    return {'api_key':credential(config,'api_key'), 'api_secret':credential(config,'api_secret'),
            'access_token':credential(session,'access_token'), 'expires_at':next_expiry(time.time() if now is None else now)}


def configure(path):
    try: previous = read_private(path)
    except FileNotFoundError: previous = {}
    print('Enter credentials here locally. Input is hidden; blank keeps an existing value.')
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('error', getpass.GetPassWarning)
            key = getpass.getpass('Kite API key: ').strip() or previous.get('api_key', '')
            secret = getpass.getpass('Kite API secret: ').strip() or previous.get('api_secret', '')
    except getpass.GetPassWarning:
        raise DataError('A terminal with hidden input is required for credentials.') from None
    config = {'api_key':key, 'api_secret':secret}
    credential(config, 'api_key'); credential(config, 'api_secret')
    write_private(path, config)
    print('Private configuration saved. Next run: python3 -m aviato_portfolio login')


def login(path):
    config = read_private(path)
    credential(config, 'api_key'); credential(config, 'api_secret')
    state = secrets.token_urlsafe(32)
    outcome = {'saved':False, 'finished':False}
    class Handler(BaseHTTPRequestHandler):
        timeout = 2
        def log_message(self, *args): pass  # Never log request-token query strings.

        def reply(self, status, text, location=None):
            body = text.encode()
            self.send_response(status)
            self.send_header('Content-Type','text/plain; charset=utf-8')
            self.send_header('Cache-Control','no-store')
            self.send_header('Referrer-Policy','no-referrer')
            self.send_header('Content-Security-Policy',"default-src 'none'; frame-ancestors 'none'")
            self.send_header('Content-Length',str(len(body)))
            if location: self.send_header('Location',location)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.headers.get('Host') not in ('127.0.0.1:8766','localhost:8766') or len(self.path)>4096:
                self.reply(400,'Invalid callback request.'); return
            parsed = urlsplit(self.path)
            if parsed.path == '/done':
                self.reply(200,'Kite connected. You can close this tab.' if outcome['saved'] else 'Login was not completed.')
                outcome['finished']=True
                return
            if parsed.path != '/kite/callback' or outcome['saved']:
                self.reply(404,'Not found.'); return
            try:
                result = complete_login(config, parse_qs(parsed.query), state)
                write_private(path, result)
            except (OSError, DataError, ValueError, TypeError):
                self.reply(400,'Login could not be completed. Check the app credentials and start a fresh login.')
                return
            outcome['saved']=True
            self.reply(303,'Login complete.', '/done')

    with HTTPServer(('127.0.0.1',8766),Handler) as server:
        server.timeout=1
        print('Set this exact Redirect URL in your Kite developer app:', CALLBACK)
        print('Open this official Kite login URL in your browser (expires in 10 minutes):')
        print(login_url(config,state), flush=True)
        deadline=time.monotonic()+600
        while not outcome['finished'] and time.monotonic()<deadline:
            server.handle_request()
    if not outcome['saved']: raise DataError('Login timed out without saving a session.')
    print('Kite connected. Portfolio collection will resume within one minute.')


def collect(config, cache, once=False):
    cache.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    lock_path=cache.parent/'collector.lock'
    descriptor=os.open(lock_path,os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
    with os.fdopen(descriptor,'w') as lock:
        try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError: raise DataError('The portfolio collector is already running.') from None
        collector=Collector(config,cache)
        while True:
            delay=collector.step()
            if once: break
            time.sleep(delay)


def main(argv=None):
    # Credentials and account responses must not be written into crash dumps.
    # RLIMIT_CORE alone does not stop Linux's piped crash collectors (Apport).
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
    libc.prctl.restype = ctypes.c_int
    if libc.prctl(4, 0, 0, 0, 0) != 0:  # Linux PR_SET_DUMPABLE
        print('Unable to disable credential process crash dumps.', file=sys.stderr)
        return 1
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',type=Path,default=CONFIG)
    parser.add_argument('--cache',type=Path,default=CACHE)
    sub=parser.add_subparsers(dest='command',required=True)
    sub.add_parser('configure'); sub.add_parser('login'); sub.add_parser('status')
    sub.add_parser('collect').add_argument('--once',action='store_true')
    args=parser.parse_args(argv)
    try:
        if args.command=='configure': configure(args.config)
        elif args.command=='login': login(args.config)
        elif args.command=='collect': collect(args.config,args.cache,args.once)
        elif args.command=='status':
            try: data=read_private(args.cache,4096)
            except FileNotFoundError: data={}
            print(json.dumps({'status':data.get('status','setup_required'), 'last_update':data.get('updated_at',0)}))
    except (DataError, OSError):
        print('Unable to complete this step. Check local file permissions, credentials, callback URL, and service status.',file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print('Stopped.',file=sys.stderr)
        return 130
    return 0
