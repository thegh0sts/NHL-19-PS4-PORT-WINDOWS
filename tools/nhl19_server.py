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
from blaze_tdf import INT, STRING, BLOB, STRUCT, LIST, MAP, TIME, OBJTYPE, VARIABLE, NOTIFICATION, Struct, encode_struct  # noqa: E402

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
    # The game's login state machine (eboot 0x2fe4418, state 6 at 0x2fe469d), once the EA login is
    # done, reads this key from the Blaze config and asks EA for an auth code for that client id,
    # which the Blaze login then carries. Without it the machine goes to its error state (16) with
    # no message: the login stopped after /connect/tokeninfo.
    'blazeServerClientId': SERVER_ID,
    # Dynamic messages (NHL_DYNAMIC_MSG, eboot 0x2fda894..0x2fdacee): the home screen's message
    # tiles come from EA's "aruba" service at UNIFIED_MESSAGING_URL; this server answers /aruba/
    # with no messages. Without the keys no request was ever made.
    'UNIFIED_MESSAGING_URL': 'https://accounts.ea.com/aruba/',
    'UNIFIED_MESSAGING_PROJECT_ID': 'nhl19',
    'UNIFIED_MESSAGING_CLIENT_ID': CLIENT_ID,
    'UNIFIED_MESSAGING_ENV': 'prod',
    # The home screen's featured tiles (job nhl-job:online-config:featured-tiles:list:get, eboot
    # 0x16a7f90) read these through nhl-data:online-config:info:get (0x1167f20), which looks them
    # up in this Blaze config; a missing key fails the job ("Internal error.") and the home screen
    # stays empty. Values are tile ids (strtol base 10); which id is which tile is in the game's UI
    # data, so these are a first guess (override with NHL19_TILES=a,b,c,d).
    **{f'MAIN_HUB_FEATURED_TILE_{n + 1}': v for n, v in
       enumerate((os.environ.get('NHL19_TILES') or '1,2,3,4').split(',')[:4])},
    # Ads (NHL_DYNAMIC_ADS, 0x2fd8b6a..0x2fd91fd): off.
    'UNIFIED_MESSAGING_ADS_ENABLED': 'false',
    'UNIFIED_MESSAGING_ADS_TIMEOUT': '5',
    'UNIFIED_MESSAGING_ADS_TIMER': '60',
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


def _png(size, rgb):
    import struct
    import zlib
    row = b'\0' + bytes(rgb) * size
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(row * size)) + chunk(b'IEND', b''))


AVATAR_PNG = _png(240, (0x1f, 0x3a, 0x5f))  # 240x240, PSN's large avatar size


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
    if path == '/nv2/auth':
        # SportsLoginService::Login: POST {CTS_AUTH_SERVER}/nv2/auth (UFC 3 used /easo/auth/nv2/auth);
        # LoginHandler reads the same EASW-* response headers as UFC 3's.
        from email.parser import BytesHeaderParser
        headers = BytesHeaderParser().parsebytes(head.partition(b'\r\n')[2] + b'\r\n\r\n')
        print(f'{base.stamp()} easo: sports login for {headers.get("X-UFC3-Online-Id", "Player")}', flush=True)
        return base.easo_login_answer(headers)
    if path == '/xmshd/search':
        # Content search (CMS SearchFileHandler, "%ssearch"): during each online check the game
        # looks for its store config (File_Type ps4_dimecfg_na; at boot it has no
        # /temp0/commerce/dimecfg_na.xml). The answer is an EA::Online::CMS::SearchContentResult,
        # whose reflected members (eboot 0x282ca20) are "count" and "files" (a list of
        # ContentSummary); UFC 3's "results" is not read. A 503 here may be why Online Versus
        # gives up and logs out: answer "no files" instead (XMSHD_MODE=offline keeps the 503).
        body = request.partition(b'\r\n\r\n')[2]
        print(f'{base.stamp()} xmshd: search {" ".join(body.decode("utf-8", "replace").split())}', flush=True)
        if os.environ.get('XMSHD_MODE') == 'offline':
            return b'HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
        return base.json_answer('200 OK', {'count': 0, 'files': []})
    if path.startswith('/aruba'):
        # Dynamic messages (UNIFIED_MESSAGING_URL): the game's parser (eboot 0x2fdc340) looks up
        # "triggers", then per entry "triggerid", "actions" and "attributes"; UFC 3's
        # {"messages": []} has no "triggers". No messages: an empty list.
        body = request.partition(b'\r\n\r\n')[2]
        print(f'{base.stamp()} aruba: {path} asks {body[:400].decode("utf-8", "replace")}', flush=True)
        # The home screen asks for trigger "HOME" and builds its tiles from the answer's actions:
        # an empty list (or none) left it with no tiles. Offline the request fails and the game
        # uses its built-in tiles, so fail it the same way (ARUBA_MODE=empty keeps the old answer).
        if os.environ.get('ARUBA_MODE') == 'empty':
            try:
                asked = json.loads(body or b'{}').get('triggerids') or []
            except ValueError:
                asked = []
            return base.json_answer('200 OK', {'triggers': [{'triggerid': str(t), 'actions': []} for t in asked]})
        return b'HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
    if path.startswith('/psn-avatar/'):
        # The PSN profiles the runtime makes up point their avatarUrls here (Online Versus
        # downloads the player's avatar): one plain picture for everybody.
        print(f'{base.stamp()} avatar: {path}', flush=True)
        return (b'HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n'
                b'Connection: close\r\n\r\n' % len(AVATAR_PNG)) + AVATAR_PNG
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


# Stats (component 7). The Online Versus screen asks getStatGroup (4) for "VersusSummary" and then
# getStatsByGroupAsync (0x10): its reply is empty and the values come afterwards as the Stats
# notification GetStatsAsyncNotification (0x32, in the eboot's notification table at 0x348c0d0),
# a KeyScopedStatValues. Without it the screen waited forever. No stats are kept yet: every group
# has the columns below, all 0.
STATS = 0x7
GET_STAT_GROUP, GET_STATS_BY_GROUP_ASYNC, GET_STATS_ASYNC_NOTIFICATION = 0x4, 0x10, 0x32
USER_ENTITY = (0x7802, 1)
_pending_notes = []
# The stat columns the game's summary jobs read (OnlinePlaynowSummary, eboot 0x1148e00: StatsList of
# EntityId, DNF, Wins, Losses, OvertimeLosses, Mmr). With no columns in the group, the Online
# Versus hub ran nhl-data:online:stats:online-playnow-summary:list:get and then cancelled,
# left and logged out. Every player has 0 in each until results are stored.
STAT_COLUMNS = ('DNF', 'Wins', 'Losses', 'OvertimeLosses', 'Mmr')


def _request_value(request, tag, default=None):
    item = request.get(tag) if hasattr(request, 'get') else None
    return item[1] if item else default


def stat_group(request) -> bytes:
    name = _request_value(request, 'NAME', '') or ''
    return encode_struct([
        ('CNAM', STRING, name), ('DESC', STRING, ''), ('ETYP', OBJTYPE, USER_ENTITY),
        ('KSUM', MAP, (STRING, STRING, [])), ('META', STRING, ''), ('NAME', STRING, name),
        ('STAT', LIST, (STRUCT, [
            [('CATG', STRING, name), ('DFLT', STRING, '0'), ('DRVD', INT, 0), ('FRMT', STRING, '%d'),
             ('KIND', STRING, ''), ('LDSC', STRING, column), ('META', STRING, ''), ('NAME', STRING, column),
             ('SDSC', STRING, column), ('TYPE', INT, 0)]
            for column in STAT_COLUMNS])),
    ])


def stats_by_group_async(request) -> bytes:
    name = _request_value(request, 'NAME', '') or ''
    view = _request_value(request, 'VID', 0) or 0
    entities = list((_request_value(request, 'EID', (INT, [])) or (INT, []))[1])
    values = encode_struct([
        ('GRNM', STRING, name), ('KEY', STRING, ''), ('LAST', INT, 1),
        ('STS', STRUCT, [('STAT', LIST, (STRUCT, [
            [('EID', INT, e), ('ETYP', OBJTYPE, USER_ENTITY), ('POFF', INT, 0),
             ('STAT', LIST, (STRING, ['0'] * len(STAT_COLUMNS)))]
            for e in entities]))]),
        ('VID', INT, view),
    ])
    for e in entities:
        writer = base.CONNECTIONS.get(e)
        if writer is not None:
            _pending_notes.append((writer, base.Frame(STATS, GET_STATS_ASYNC_NOTIFICATION, 0, NOTIFICATION, values)))
    print(f'{base.stamp()} stats: {name} for {entities} (view {view}): all {len(STAT_COLUMNS)} stats 0', flush=True)
    return b''


_ufc3_send_game_notes = base.send_game_notes


def _send_notes(notes):
    # The Blaze loop calls this right after each reply: the stats notification goes out after the
    # reply to getStatsByGroupAsync, as the client expects.
    while _pending_notes:
        writer, note = _pending_notes.pop(0)
        try:
            writer.write(note.encode())
        except ConnectionError:
            writer.close()
        else:
            print(f'{base.stamp()} blaze: -> {note}', flush=True)
    _ufc3_send_game_notes(notes)


base.send_game_notes = _send_notes
base.HANDLERS[(STATS, GET_STAT_GROUP)] = stat_group
base.HANDLERS[(STATS, GET_STATS_BY_GROUP_ASYNC)] = stats_by_group_async


# Census data (component 0xa; commands from the eboot's name table at 0x586cb30: 1 subscribe,
# 2 unsubscribe, 3 getRegionCounts, 4 getLatestCensusData, 5 subscribeToCensusDataUpdates;
# notification 1 NotifyServerCensusData). Online Versus asks getLatestCensusData for its
# matchmaking census ("players searching") and fell back to "Press X to start" when it got nothing.
# The answer is a NotifyServerCensusData, as the reply and as the notification: a GameManager and a
# UserManager census (variable TDFs; their TDF ids are in their TypeDescriptions, 0x3428490 and
# 0x33d6760) with the players online and nobody in games or matchmaking.
CENSUS = 0xa
CENSUS_SUBSCRIBE, CENSUS_LATEST, CENSUS_UPDATES = 0x1, 0x4, 0x5
NOTIFY_SERVER_CENSUS_DATA = 0x1
GAME_MANAGER_CENSUS, USER_MANAGER_CENSUS = 0x558ce4ac, 0x220c16c2
CENSUS_PERIOD, CENSUS_TIMEOUT, CENSUS_RESUBSCRIBE = 60_000_000, 180_000_000, 30_000_000  # microseconds


def census_data() -> bytes:
    online = len(base.ONLINE)
    game_manager = [('AGN', INT, 0), ('GGN', INT, 0), ('JPN', INT, 0), ('LSN', INT, online),
                    ('MMCD', STRUCT, []), ('MMSN', INT, 0), ('NUMM', INT, 0), ('PIGG', INT, 0)]
    return encode_struct([
        ('CNP', TIME, CENSUS_PERIOD), ('NTMT', TIME, CENSUS_TIMEOUT), ('RTMT', TIME, CENSUS_RESUBSCRIBE),
        ('TDFL', LIST, (STRUCT, [
            [('TDF', VARIABLE, (GAME_MANAGER_CENSUS, game_manager))],
            [('TDF', VARIABLE, (USER_MANAGER_CENSUS, []))],
        ])),
    ])


def _queue_census(what):
    data = census_data()
    for writer in list(base.CONNECTIONS.values()):
        _pending_notes.append((writer, base.Frame(CENSUS, NOTIFY_SERVER_CENSUS_DATA, 0, NOTIFICATION, data)))
    print(f'{base.stamp()} census: {what}; {len(base.ONLINE)} online, nobody matchmaking', flush=True)
    return data


def census_subscribe(request) -> bytes:
    _queue_census('subscribed')
    return b''


def census_latest(request) -> bytes:
    return _queue_census('latest asked')


def census_updates(request) -> bytes:
    # No notification here: with one per subscribe the game resubscribed every frame.
    print(f'{base.stamp()} census: updates subscribed', flush=True)
    return encode_struct([('CNP', TIME, CENSUS_PERIOD), ('NTMT', TIME, CENSUS_TIMEOUT),
                          ('RTMT', TIME, CENSUS_RESUBSCRIBE)])


base.HANDLERS[(CENSUS, CENSUS_SUBSCRIBE)] = census_subscribe


# UserSessions lookupUsersIdentification (0x7802 cmd 0xf; the eboot's command names at 0x586d1b0
# start at cmd 1). The Online Versus hub asks it for the players it shows (job
# nhl-data:online-user:profiles:list:get) and answers "Failed to retrieve data" (code 9999) when
# nobody comes back. The reply is a LookupUsersResponse: ULST, the UserIdentification of every
# asked user the server knows (by blaze id, name or PSN id).
USER_SESSIONS = 0x7802
LOOKUP_USERS_IDENTIFICATION = 0xf


def _find_account(user):
    accounts = list(base.IDENTITIES.accounts.values())
    blaze_id = _request_value(user, 'ID', 0) or 0
    name = _request_value(user, 'NAME', '') or ''
    external = _request_value(user, 'EXID', 0) or 0
    for account in accounts:
        if (blaze_id and account.id == blaze_id) or (name and account.name == name) \
                or (external and account.psn_id == external):
            return account
    return None


def user_identification(account):
    return [('AID', INT, account.id), ('ALOC', INT, 1701729619), ('CNTY', INT, 21843), ('EXBB', BLOB, b''),
            ('EXID', INT, account.psn_id), ('ID', INT, account.id), ('NAME', STRING, account.name),
            ('NASP', STRING, 'cem_ea_id'), ('ORIG', INT, account.id), ('PIDI', INT, account.id)]


def lookup_users_identification(request) -> bytes:
    asked = (_request_value(request, 'ULST', (STRUCT, [])) or (STRUCT, []))[1]
    found = [a for a in (_find_account(user) for user in asked) if a is not None]
    print(f'{base.stamp()} users: lookup of {len(asked)}, found {[a.name for a in found]}', flush=True)
    return encode_struct([('ULST', LIST, (STRUCT, [user_identification(a) for a in found]))])


base.HANDLERS[(USER_SESSIONS, LOOKUP_USERS_IDENTIFICATION)] = lookup_users_identification
base.HANDLERS[(CENSUS, CENSUS_LATEST)] = census_latest
base.HANDLERS[(CENSUS, CENSUS_UPDATES)] = census_updates


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
