#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""NHL 19 online server for playing with a friend (run by the player who hosts).

The UFC 3 server (tools/ufc3_server.py: redirector, Nucleus, Blaze login and game sessions) with
NHL 19's answers where the two games differ. NHL 19 runs Blaze SDK 15.1.1.7.1 and DirtySDK
15.1.4.0.1; its preAuth reply carries a QoS config in the newer layout (QosConfigInfo: QCNF the QoS
coordinator, LTPS the ping sites, CQRR/CQFR/SQRR the refresh rates), not UFC 3's BWPS/LNP/LTPS.
The tags come from the game's own TDF reflection (tools/ufc3_tdf_schema.py on a BB_NET_TRAP_DUMP).

    python3 tools/nhl19_server.py [--qos coordinator|none] [--ports ...]

--qos coordinator (default): QCNF points the game at a QoS coordinator on this server
    (qoscoordinator.ea.com, TCP COORDINATOR_PORT). The game speaks gRPC over HTTP/2 there
    (/eadp.qoscoordinator.QOSCoordinator/ClientCall), most likely inside TLS; for now the port only
    logs what arrives, to learn the exchange.
--qos none: no QCNF at all, only ping sites and refresh rates, in case the game then measures QoS
    the older way (UDP ping sites, which the UFC 3 server already answers) instead.

Status: development. Get past the QoS step, then the Blaze login (the same tags as UFC 3's).
"""
import argparse
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ufc3_auth  # noqa: E402
import ufc3_server as base  # noqa: E402
from blaze_tdf import INT, STRING, STRUCT, LIST, MAP, TIME, Struct, encode_struct  # noqa: E402

SERVICE = 'nhl-2019-ps4'
CLIENT_ID, SERVER_ID = 'NHL2019_PS4_CLIENT', 'NHL2019_PS4_SERVER'
COORDINATOR_HOST = 'qoscoordinator.ea.com'  # *.ea.com resolves to this server in the port
COORDINATOR_PORT = 10745
QOS_MODE = 'coordinator'

# The Nucleus client ids the game is told to use (and the identity store accepts).
ufc3_auth.CLIENT_ID, ufc3_auth.SERVER_ID = CLIENT_ID, SERVER_ID
base.CLIENT_CONFIG.update({
    'consoleClientId': CLIENT_ID,
    # The game reads four Blaze config keys for its Nucleus login (eboot getters 0x25e2d10..0x25e2d70):
    # nucleusConnect, nucleusProxy, consoleClientId and identityRedirectUri. Without the last one
    # its /connect/auth carries no redirect_uri. The value is the identity library's own callback
    # address (eboot string next to "Nucleus::IdentitySessionImpl::TaskPool").
    'identityRedirectUri': 'https://nucleus/oauth_callback',
    # PIN telemetry (eboot 0x25cc37d): host, port (strtol base 10) and environment. Without them the
    # game opens a TCP connection to an empty host name right after Blaze connects. Port 443 reaches
    # this server (40443), which accepts the posts below.
    'PIN_SERVER_ADDRESS': 'pin-river.data.ea.com',
    'PIN_SERVER_PORT': '443',
    'PIN_SERVER_ENV': 'prod',
    'easo_sku': 'NHL2019_PS4',
    'gameface_sku': 'NHL2019',
    # fetchClientConfig 'NHL_CTS_CONNECTION' (asked right after preAuth): the keys the eboot reads
    # next to that name. Without them the game opens a TCP connection to an empty host name
    # ("resolve  -> refused") and the online login stops after Nucleus tokeninfo. XMSHD is EA's
    # content service (next to NHL_ROSTER in the eboot); this server answers /xmshd/search.
    'CTS_XMSHD_BASE_URL': 'https://accounts.ea.com/xmshd',
    'CTS_XMSHD_ENVIRONMENT': 'prod',
    'CTS_SKU': 'NHL2019_PS4',
    'CTS_AUTH_SERVER': 'https://accounts.ea.com',
})


def seconds(n):
    """TDF TimeValue: microseconds."""
    return n * 1_000_000


def qos_config(host_ip: str):
    """Blaze::QosConfigInfo as NHL 19's reflection has it (out/nhl19-tdf-schema.txt):
    CQFR clientQosFailureRefreshRate, CQRR clientQosRefreshRate, LTPS pingSiteInfoByAliasMap
    (alias -> QosPingSiteInfo PSA address, PSP port), QCNF qosCoordinatorInfo (QosCoordinatorInfo:
    DPSP defaultPingSitesPort, QCA address, QCP port, QPR profile), SQRR serverQosRefreshRate."""
    site = Struct(PSA=(STRING, host_ip), PSP=(INT, base.QOS_PORT))
    fields = Struct(
        CQFR=(TIME, seconds(60)),
        CQRR=(TIME, seconds(300)),
        LTPS=(MAP, (STRING, STRUCT, [('local', site)])),
        SQRR=(TIME, seconds(300)),
    )
    if QOS_MODE == 'coordinator':
        fields['QCNF'] = (STRUCT, Struct(DPSP=(INT, base.QOS_PORT), QCA=(STRING, COORDINATOR_HOST),
                                         QCP=(INT, COORDINATOR_PORT), QPR=(STRING, SERVICE)))
    return fields


def preauth_reply(request, host_ip: str = '127.0.0.1') -> bytes:
    """Blaze::Util::PreAuthResponse (NHL 19 reflection): ASRC, CIDS, CLID, CONF, ESRC, INST, MAID,
    MINR, NASP, PILD, PLAT, QOSS, RSRC, SVER."""
    return encode_struct([
        ('ASRC', STRING, '303107'),
        ('CIDS', LIST, (INT, base.OFFERED)),
        ('CLID', STRING, SERVER_ID),  # the server's Nucleus client id (2nd /connect/auth)
        ('CONF', STRUCT, Struct(CONF=(MAP, (STRING, STRING, sorted(base.CLIENT_CONFIG.items()))))),
        ('ESRC', STRING, '303107'),
        ('INST', STRING, SERVICE),
        ('MINR', INT, 0),
        ('NASP', STRING, 'cem_ea_id'),
        ('PILD', STRING, ''),
        ('PLAT', STRING, 'ps4'),
        ('QOSS', STRUCT, qos_config(host_ip)),
        ('RSRC', STRING, '303107'),
        ('SVER', STRING, 'Blaze 15.1.1.7.1 (NHL 19 native port server)'),
    ])


base.preauth_reply = preauth_reply  # ufc3_server.blaze looks it up at call time


_ufc3_nucleus_answer = base.nucleus_answer


def nucleus_answer(request: bytes) -> bytes:
    """UFC 3's Nucleus answers, plus what NHL 19's identity library (the eboot's TokenInfoGet)
    reads from /connect/tokeninfo: client_id, scope, expires_in, pid_id, pid_type, user_id,
    device_id, persona_id (xbox_prv on Xbox only), any missing one being "Failed to parse response
    Json for field". The game asks for the device id with an X-Include-Deviceid header."""
    import json
    head, _, _ = request.partition(b'\r\n\r\n')
    line = head.split(b'\r\n', 1)[0].decode('latin-1')
    path = line.split(' ')[1].split('?', 1)[0] if ' ' in line else ''
    asked = [h.decode('latin-1').strip() for h in head.split(b'\r\n')[1:] if h.lower().startswith(b'x-include-')]
    if asked:
        print(f'{base.stamp()} nucleus: {path} asks {", ".join(asked)}', flush=True)
    # Which part of the game made the request: Blaze's console login adds 'display' and its own
    # parameters; the game's identity library sends release_type / guest_mode / ps4_env. Codes,
    # tickets and tokens are not printed.
    import urllib.parse
    target = line.split(' ')[1] if ' ' in line else ''
    body = request.partition(b'\r\n\r\n')[2].decode('utf-8', 'replace')
    params = urllib.parse.parse_qsl(urllib.parse.urlsplit(target).query) + urllib.parse.parse_qsl(body)
    if params and path.startswith('/connect/'):
        secret = ('code', 'access_token', 'refresh_token', 'client_secret', 'ps4_ticket', 'token', 'nonce')
        shown = [f'{k}={"<hidden>" if k in secret else v[:60]}' for k, v in params]
        print(f'{base.stamp()} nucleus: {path} parameters: {"; ".join(shown)}', flush=True)
    if path in ('/pinEvents', '/bugsentry/session/', '/bugsentry/session'):
        # Telemetry and crash-session posts: accepted and dropped.
        return base.json_answer('200 OK', {'status': 'ok'})
    answer = _ufc3_nucleus_answer(request)
    if path == '/connect/token' and answer.startswith(b'HTTP/1.1 200'):
        # The identity library's TokenGetByCode reads access_token, token_type, refresh_token and
        # id_token (eboot strings next to "TokenGetByCode"); UFC 3's answer has no refresh_token.
        import secrets
        body = json.loads(answer.partition(b'\r\n\r\n')[2])
        body.setdefault('refresh_token', 'rt-' + secrets.token_hex(16))
        body.setdefault('refresh_token_expires_in', 2592000)
        print(f'{base.stamp()} nucleus: token fields {sorted(body)}', flush=True)
        return base.json_answer('200 OK', body)
    if path != '/connect/tokeninfo' or not answer.startswith(b'HTTP/1.1 200'):
        return answer
    body = json.loads(answer.partition(b'\r\n\r\n')[2])
    body.setdefault('device_id', f'ps4-{body.get("pid_id", "0")}')
    print(f'{base.stamp()} nucleus: tokeninfo fields {sorted(body)}', flush=True)
    return base.json_answer('200 OK', body)


base.nucleus_answer = nucleus_answer  # ufc3_server.handle looks it up at call time


async def coordinator(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """QoS coordinator port: for now logs what the game sends (a TLS ClientHello, or HTTP/2 in the
    clear: 'PRI * HTTP/2.0'), then closes."""
    peer = writer.get_extra_info('peername')
    print(f'{base.stamp()} qos coordinator: client {peer[0]}:{peer[1]}', flush=True)
    try:
        total = 0
        while total < 65536:
            data = await asyncio.wait_for(reader.read(4096), timeout=10)
            if not data:
                break
            what = base.describe(data) if total == 0 else 'more'
            if data.startswith(b'PRI * HTTP/2.0'):
                what = 'HTTP/2 without TLS (connection preface)'
            print(f'{base.stamp()} qos coordinator: {len(data)} B, {what}: {data[:96].hex(" ")}', flush=True)
            total += len(data)
    except (asyncio.TimeoutError, ConnectionError):
        pass
    finally:
        await base.close_writer(writer)
    print(f'{base.stamp()} qos coordinator: client {peer[0]}:{peer[1]} closed', flush=True)


async def main():
    global QOS_MODE
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--qos', choices=('coordinator', 'none'), default='coordinator',
                   help='QoS config sent in preAuth (see the module docstring)')
    p.add_argument('--ports', default='42230,40443,40080,10041,10744,17502')  # 443/80 arrive at +40000
    p.add_argument('--bind', default='0.0.0.0')
    args = p.parse_args()
    QOS_MODE = args.qos
    print(f'{base.stamp()} NHL 19 server: QoS mode {QOS_MODE}', flush=True)
    sys.argv = [sys.argv[0], '--ports', args.ports, '--bind', args.bind]
    coord = None
    if QOS_MODE == 'coordinator':
        coord = await asyncio.start_server(coordinator, args.bind, COORDINATOR_PORT)
        print(f'{base.stamp()} qos coordinator on {args.bind}:{COORDINATOR_PORT} ({COORDINATOR_HOST})', flush=True)
    try:
        await base.main()
    finally:
        if coord:
            coord.close()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
