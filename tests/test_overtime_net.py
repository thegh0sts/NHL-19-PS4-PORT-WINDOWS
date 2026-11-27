# SPDX-License-Identifier: GPL-2.0-or-later
"""NHL 19 "Overtime online" in runtime_net.c (tests/test_overtime_net.c) against local servers.

BB_OVERTIME_NET_TEST names the test program (default: the MSYS2 build's overtime-net-test.exe).
The PSN account comes from a fake ShadNet server on 127.0.0.1 (shadPS4's protocol 1) with test
values only; the program never reaches the real ShadNet. Its packets are checked against messages
encoded by protoc from shadPS4's shadnet.proto.
The TLS server needs the openssl command for a throwaway certificate; without it that test skips.
"""
import os
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIG = 3 * 1024 * 1024 + 17

# Test values (never a real account).
SHADNET_ID = 'tester-01'
PASSWORD = 'pw-test-not-real'
ACCOUNT = 98765432101
AUTH_CODE = 'AbCdEf'
# protoc --encode, package shadnet:
LOGIN_REQUEST = bytes.fromhex(  # {npid tester-01, password, title_id CUSA11126, title_name "NHL™ 19"}
    '0a097465737465722d3031121070772d746573742d6e6f742d7265616c22094355534131313132362a094e484ce284a2203139')
AUTH_REQUEST = bytes.fromhex(  # {client_id "0a1b2c3d-test-client", scope "psn:s2s"}
    '0a1430613162326333642d746573742d636c69656e74120770736e3a733273')
CLIENT_VERSION = bytes.fromhex(  # {version "SacredFire NHL 19 port unknown (Overtime online)"}
    '0a3053616372656446697265204e484c20313920706f727420756e6b6e6f776e20284f76657274696d65206f6e6c696e6529')
LOGIN_REPLY = bytes.fromhex(  # {avatar_url, user_id 98765432101, friends {npid "pal", online}}
    '0a0e687474703a2f2f612f622e706e6710a5ca83f7ef021a070a0370616c1001')
AUTH_REPLY = bytes.fromhex('0a0641624364456610ac02')  # {code "AbCdEf", expires_in 300}
CLIENT_REJECTED = bytes.fromhex(  # {reason 1, message "Update please", version "x"}
    '0801120d55706461746520706c656173651a0178')

HEADER = 15
REQUEST, REPLY, NOTIFICATION, SERVER_INFO = 0, 1, 2, 3
LOGIN, SET_CLIENT_VERSION, GET_AUTHORIZATION_CODE = 0, 13, 42
NOTIFY_CLIENT_REJECTED = 19
INVALID_PASSWORD, INVALID_INPUT, ALREADY_LOGGED_IN = 8, 3, 6


def program():
    named = os.environ.get('BB_OVERTIME_NET_TEST')
    if named:
        return Path(named)
    return ROOT / 'out/windows/build-msys2' / ('overtime-net-test.exe' if sys.platform == 'win32' else 'overtime-net-test')


def shadnet_env(server, npid=SHADNET_ID, password=PASSWORD):
    env = {k: v for k, v in os.environ.items() if not k.startswith('BB_SHADNET_')}
    env['BB_SHADNET_SERVER'] = server  # always set: a test never falls back to srv.shadps4.net
    if npid:
        env['BB_SHADNET_ID'] = npid
    if password:
        env['BB_SHADNET_PASSWORD'] = password
    return env


def run(env, *args, timeout=60):
    return subprocess.run([str(program()), *map(str, args)], capture_output=True, text=True, timeout=timeout, env=env)


def packet(kind, command, pid, payload=b''):
    return struct.pack('<BHIQ', kind, command, HEADER + len(payload), pid) + payload


def recv_exact(conn, size):
    data = b''
    while len(data) < size:
        chunk = conn.recv(size - len(data))
        if not chunk:
            return None
        data += chunk
    return data


LONG_REJECTION = ('This client version is not accepted on this server: please update to the current release, '
                  'which you can find on the project page; older builds cannot sign in until they are updated, '
                  'and this message is long on purpose')  # over 160 characters


def varint(n):
    out = bytearray()
    while True:
        out.append((n & 0x7f) | (0x80 if n > 0x7f else 0))
        n >>= 7
        if not n:
            return bytes(out)


def field(number, value):
    """One protobuf field: an int as a varint, bytes as length-delimited."""
    if isinstance(value, int):
        return varint(number << 3) + varint(value)
    return varint(number << 3 | 2) + varint(len(value)) + value


def string_field(message, number):
    """The bytes of string field `number` in a protobuf message (b'' when absent)."""
    i = 0
    while i < len(message):
        key, shift = 0, 0
        while True:
            b = message[i]; i += 1
            key |= (b & 0x7f) << shift; shift += 7
            if b < 0x80:
                break
        size, shift = 0, 0
        while True:
            b = message[i]; i += 1
            size |= (b & 0x7f) << shift; shift += 7
            if b < 0x80:
                break
        if key == (number << 3 | 2):
            return message[i:i + size]
        i += size
    return b''


def closed_port():
    with socket.create_server(('127.0.0.1', 0)) as s:
        return s.getsockname()[1]


class FakeShadNet:
    """ShadNet on 127.0.0.1: the server's version first, then replies to Login, SetClientVersion and
    GetAuthorizationCode. Login succeeds only for the test account. Records (connection, command, message)."""

    def __init__(self, protocol=1, drop_first_code=False, reject_code=False, reject_version=None, silent=False,
                 login_error=None):
        """reject_version: 'close' or 'open' sends ClientRejected instead of the SetClientVersion reply,
        then closes or keeps the connection; 'server' does what shadNet's CheckClientVersion does: ClientRejected
        echoing the version it was sent with the rule's message (here LONG_REJECTION), then the reply (no error),
        then a close 500 ms later. silent: accepts and never speaks (no server version). login_error: Login is
        answered with this error byte even for the test account."""
        self.protocol, self.drop_first_code, self.reject_code = protocol, drop_first_code, reject_code
        self.reject_version, self.silent, self.login_error = reject_version, silent, login_error
        self.held = []
        self.lock = threading.Lock()
        self.packets = []
        self.connections = 0
        self.codes_asked = 0
        self.listener = socket.create_server(('127.0.0.1', 0))
        self.address = '127.0.0.1:%d' % self.listener.getsockname()[1]
        threading.Thread(target=self.accept, daemon=True).start()

    def close(self):
        self.listener.close()
        for conn in self.held:
            conn.close()

    def accept(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            with self.lock:
                self.connections += 1
                n = self.connections
            threading.Thread(target=self.serve, args=(conn, n), daemon=True).start()

    def serve(self, conn, n):
        def reply(command, pid, error, message=None):
            body = bytes([error]) + (struct.pack('<I', len(message)) + message if message is not None else b'')
            conn.sendall(packet(REPLY, command, pid, body))

        if self.silent:
            self.held.append(conn)  # open, never answered
            return
        with conn:
            try:
                conn.sendall(packet(SERVER_INFO, 0, 0, struct.pack('<I', self.protocol)))
                while True:
                    head = recv_exact(conn, HEADER)
                    if head is None:
                        return
                    kind, command, size, pid = struct.unpack('<BHIQ', head)
                    payload = recv_exact(conn, size - HEADER)
                    if payload is None or kind != REQUEST:
                        return
                    (length,) = struct.unpack_from('<I', payload)
                    message = payload[4:4 + length]
                    with self.lock:
                        self.packets.append((n, command, message))
                    if command == LOGIN:
                        if self.login_error is not None:
                            reply(command, pid, self.login_error)
                        elif message == LOGIN_REQUEST:
                            reply(command, pid, 0, LOGIN_REPLY)
                        else:
                            reply(command, pid, INVALID_PASSWORD)
                    elif command == SET_CLIENT_VERSION:
                        if self.reject_version:
                            notice = CLIENT_REJECTED
                            if self.reject_version == 'server':
                                notice = (field(1, 1) + field(2, LONG_REJECTION.encode()) +
                                          field(3, string_field(message, 1)))
                            conn.sendall(packet(NOTIFICATION, NOTIFY_CLIENT_REJECTED, 0,
                                                struct.pack('<I', len(notice)) + notice))
                            if self.reject_version == 'close':
                                return
                            if self.reject_version == 'server':
                                reply(command, pid, 0)
                                time.sleep(0.5)
                                return
                            continue  # no reply; the connection stays open
                        reply(command, pid, 0)
                    elif command == GET_AUTHORIZATION_CODE:
                        with self.lock:
                            self.codes_asked += 1
                            first = self.codes_asked == 1
                        if self.reject_code:
                            conn.sendall(packet(NOTIFICATION, NOTIFY_CLIENT_REJECTED, 0,
                                                struct.pack('<I', len(CLIENT_REJECTED)) + CLIENT_REJECTED))
                            return
                        if self.drop_first_code and first:
                            return  # closes without an answer
                        reply(command, pid, 0, AUTH_REPLY)
                    else:
                        reply(command, pid, INVALID_INPUT)
            except OSError:
                pass

    def commands(self, connection=None):
        with self.lock:
            return [(c, m) for n, c, m in self.packets if connection is None or n == connection]


class Recorder:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.sni = []

    def add(self, request):
        with self.lock:
            self.requests.append(request)


def read_request(conn):
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        data += chunk
    head, _, rest = data.partition(b'\r\n\r\n')
    lines = head.decode('latin-1').split('\r\n')
    method, path, version = lines[0].split(' ', 2)
    headers = [tuple(part.strip() for part in line.split(':', 1)) for line in lines[1:]]
    lengths = [v for k, v in headers if k.lower() == 'content-length']
    want = int(lengths[0]) if lengths else 0
    while len(rest) < want:
        chunk = conn.recv(4096)
        if not chunk:
            break
        rest += chunk
    return {'method': method, 'path': path, 'version': version, 'headers': headers, 'body': rest[:want]}


def answer(conn, request):
    path = request['path']
    if path.startswith('/connect/auth'):
        time.sleep(0.3)  # the client sees EAGAIN from the status first
        conn.sendall(b'HTTP/1.1 302 Found\r\nLocation: nucleus:rest?code=OTC-test\r\nContent-Length: 0\r\n\r\n')
    elif path == '/connect/token':
        body = b'{"access_token":"AT"}'
        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s' % (len(body), body))
    elif path == '/big':
        body = bytes((i * 7) & 0xff for i in range(BIG))
        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n' % BIG + body)
        time.sleep(8)  # keeps the connection open: the client must stop at Content-Length
    elif path.startswith('/nv2/auth'):
        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok')
    else:
        conn.sendall(b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n')


def server(recorder, context=None):
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            conn = self.request
            try:
                if context:
                    conn = context.wrap_socket(conn, server_side=True)
                else:
                    first = conn.recv(1, socket.MSG_PEEK)
                    if first[:1] == b'\x16':  # a TLS ClientHello on the plain port
                        conn.sendall(b'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n')
                        return
                request = read_request(conn)
                if request:
                    request['tls'] = bool(context)
                    recorder.add(request)
                    answer(conn, request)
            except (ssl.SSLError, OSError):
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    class Server(socketserver.ThreadingTCPServer):
        daemon_threads = True
        allow_reuse_address = True

    s = Server(('127.0.0.1', 0), Handler)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s


@unittest.skipUnless(program().exists(), 'overtime-net-test is not built (ninja overtime-net-test)')
class OvertimeNetTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.user = self.tmp / 'user'
        self.user.mkdir()

    def fake(self, **options):
        fake = FakeShadNet(**options)
        self.addCleanup(fake.close)
        return fake

    def check(self, result, *secrets):
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn('Overtime online checks OK', result.stdout)
        for secret in (PASSWORD, *secrets):
            self.assertNotIn(secret, output)  # a password never reaches the log
        return result.stdout

    def test_sign_in_and_code(self):
        fake = self.fake()
        out = self.check(run(shadnet_env(fake.address), self.user, ACCOUNT, AUTH_CODE))
        self.assertIn(f'account {ACCOUNT}\ncode {AUTH_CODE}', out)
        self.assertIn(f'signed in to ShadNet as {SHADNET_ID} (account {ACCOUNT})', out)
        self.assertEqual(fake.connections, 1)
        self.assertEqual(fake.commands(), [(LOGIN, LOGIN_REQUEST), (SET_CLIENT_VERSION, CLIENT_VERSION),
                                           (GET_AUTHORIZATION_CODE, AUTH_REQUEST)])

    def test_wrong_password(self):
        fake = self.fake()
        out = self.check(run(shadnet_env(fake.address, password='wrong-pw-test'), self.user, 'signed-out', 'none'),
                         'wrong-pw-test')
        self.assertIn('signed out OK', out)
        self.assertIn('not signed in to ShadNet', out)
        self.assertEqual([c for c, _ in fake.commands()], [LOGIN])

    def test_already_logged_in(self):
        fake = self.fake(login_error=ALREADY_LOGGED_IN)
        out = self.check(run(shadnet_env(fake.address), self.user, 'signed-out', 'none'))
        self.assertIn('signed out OK', out)
        self.assertIn('signed in somewhere else', out)
        self.assertEqual([c for c, _ in fake.commands()], [LOGIN])

    def test_no_account_given(self):
        fake = self.fake()
        out = self.check(run(shadnet_env(fake.address, npid=None, password=None), self.user, 'signed-out', 'none'))
        self.assertIn('no ShadNet account given', out)
        self.assertEqual(fake.connections, 0)

    def test_id_too_long(self):
        fake = self.fake()
        out = self.check(run(shadnet_env(fake.address, npid='x' * 17), self.user, 'signed-out', 'none'))
        self.assertIn('at most 16 characters', out)
        self.assertEqual(fake.connections, 0)

    def test_server_unreachable(self):
        out = self.check(run(shadnet_env(f'127.0.0.1:{closed_port()}'), self.user, 'signed-out', 'none'))
        self.assertIn('not signed in to ShadNet', out)

    def test_newer_protocol(self):
        fake = self.fake(protocol=2)
        out = self.check(run(shadnet_env(fake.address), self.user, 'signed-out', 'none'))
        self.assertIn('speaks protocol 2', out)
        self.assertEqual(fake.commands(), [])

    def test_connection_lost_during_code(self):
        fake = self.fake(drop_first_code=True)
        out = self.check(run(shadnet_env(fake.address), self.user, ACCOUNT, AUTH_CODE))
        self.assertIn('signing in again', out)
        self.assertEqual(fake.connections, 2)
        self.assertEqual(fake.commands(2), [(LOGIN, LOGIN_REQUEST), (SET_CLIENT_VERSION, CLIENT_VERSION),
                                            (GET_AUTHORIZATION_CODE, AUTH_REQUEST)])

    def test_client_rejected(self):
        fake = self.fake(reject_code=True)
        out = self.check(run(shadnet_env(fake.address), self.user, ACCOUNT, 'none'))
        self.assertIn('Update please', out)
        self.assertIn('no code (0x80550006)', out)  # signed out, and no new sign-in
        self.assertIn('signed out of PSN until it restarts', out)
        self.assertEqual(fake.connections, 1)

    def rejected_after_version(self, how, words='Update please'):
        fake = self.last_fake = self.fake(reject_version=how)
        out = self.check(run(shadnet_env(fake.address), self.user, 'signed-out', 'none'))
        self.assertIn(words, out)  # the server's own words in the log
        self.assertIn('not signed in to ShadNet', out)
        self.assertEqual(fake.connections, 1)  # no new sign-in after a refusal
        self.assertEqual([c for c, _ in fake.commands()], [LOGIN, SET_CLIENT_VERSION])
        return out

    def test_rejected_after_version_and_closed(self):
        self.rejected_after_version('close')

    def test_rejected_after_version_left_open(self):
        self.rejected_after_version('open')

    def test_rejected_after_version_as_shadnet_does(self):
        out = self.rejected_after_version('server', LONG_REJECTION[:200])
        sent = string_field(dict(self.last_fake.commands())[SET_CLIENT_VERSION], 1).decode()
        self.assertTrue(sent.startswith('SacredFire NHL 19 port '), sent)
        self.assertIn(f'version "{sent}"', out)  # the echoed version whole

    def test_silent_server(self):
        fake = self.fake(silent=True)
        start = time.monotonic()
        out = self.check(run(shadnet_env(fake.address), self.user, 'signed-out', 'none'))
        self.assertLess(time.monotonic() - start, 20)  # sign-in is capped at 12 s
        self.assertIn('did not answer as a ShadNet server', out)

    @unittest.skipUnless(shutil.which('openssl'), 'needs the openssl command for a test certificate')
    def test_http_over_tls(self):
        key, cert = self.tmp / 'key.pem', self.tmp / 'cert.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key), '-out', str(cert),
                        '-days', '1', '-subj', '/CN=localhost'], check=True, capture_output=True)
        recorder = Recorder()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(cert), str(key))
        context.sni_callback = lambda sock, name, ctx: recorder.sni.append(name)
        tls, plain = server(recorder, context), server(recorder)
        for s in (tls, plain):
            self.addCleanup(s.server_close)
            self.addCleanup(s.shutdown)
        fake = self.fake()
        result = run(shadnet_env(fake.address), self.user, ACCOUNT, AUTH_CODE,
                     '--https', tls.server_address[1], '--http', plain.server_address[1])
        self.check(result)
        if os.environ.get('BB_TEST_VERBOSE'):
            print(*[l for l in result.stdout.splitlines() if ' OK' in l], sep='\n', file=sys.stderr)
        self.assertEqual(recorder.sni, ['localhost', None, 'localhost'])
        by_path = {r['path'].split('?')[0]: r for r in recorder.requests}
        self.assertEqual(set(by_path), {'/connect/auth', '/connect/token', '/big', '/nv2/auth'})
        for r in recorder.requests:
            names = [k.lower() for k, _ in r['headers']]
            self.assertEqual(r['version'], 'HTTP/1.0')
            self.assertEqual(names.count('host'), 1, r)
            self.assertEqual(names.count('content-length'), 1, r)
            self.assertEqual([v for k, v in r['headers'] if k.lower() == 'connection'], ['close'])
            self.assertFalse([k for k in names if k.startswith('x-ufc3')], r)
        auth = by_path['/connect/auth']
        self.assertTrue(auth['tls'])
        self.assertIn(('ps4_ticket', AUTH_CODE), auth['headers'])  # ShadNet's code, as from shadPS4
        self.assertIn(('Host', 'localhost'), auth['headers'])
        token = by_path['/connect/token']
        self.assertIn(('Host', '127.0.0.1'), token['headers'])
        self.assertIn(('Content-Type', 'application/x-www-form-urlencoded'), token['headers'])
        self.assertEqual(token['body'], b'grant_type=authorization_code&code=OTC-test')
        self.assertFalse(by_path['/nv2/auth']['tls'])


if __name__ == '__main__':
    unittest.main()
