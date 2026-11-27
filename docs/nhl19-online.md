# NHL 19 online: where it stands (2026-11-21)

Private online play (one player hosts a server, a friend joins) on the UFC 3 model
(`docs/ufc3/HOW-TO-PLAY-ONLINE.md`). Not working yet: the game stops during its EA account
sign-in, before the Blaze login. This note is the state of the work and the next step.

## How to test

```bash
bash scripts/build-msys2.sh                       # after any change in src/ or gpu/
python3 tools/nhl19_server.py                     # terminal 1, leave it running
rm -rf user-online/temp0                          # terminal 2
BB_USER_DIR=user-online bash scripts/run-msys2.sh # add BB_SKIP_PREPARE=1 when only the server changed
```

- With the server running the game signs in by itself while it boots; no menu action is needed.
  Wait ~30 s, close the game, read the server terminal and `out/run.txt`.
- Use a separate user folder (`user-online`) for online tests. The online attempts left data in
  `user/temp0` that blanked the main menu (no tiles, no pinned modes) until `temp0` was deleted.
  The home screen is also empty while the server runs: the game waits for the login to finish.

## What works

| Step | Result |
|---|---|
| Redirector (`spring18.gosredirector.ea.com:42230`, `getServerInstance`) | answered, points the game at Blaze on 10041 without TLS |
| Blaze connect, `Util::preAuth` (9:7), ping (9:2), `fetchClientConfig 'NHL_CTS_CONNECTION'` (9:1) | answered |
| EA account sign-in: `/connect/auth`, `/connect/token`, `/connect/tokeninfo` | answered (HTTP; our sceHttp sends https:// to the server in the clear) |
| EAWebKit | linked (`nhl/data/kettle/eawebkit.prx`); the online login's `sceKernelLoadStartModule` now gets the linked copy (`-> linked module 3`) |

The game identifies itself in preAuth as `CLNT 'NHL2019'`, service `nhl-2019-ps4`, Blaze SDK
15.1.1.7.1 (built 2019-03-14), DirtySDK 15.1.4.0.1.

## Where it stops

The `/connect/auth` above is the game's own EA identity library, not Blaze's login: its
parameters are `release_type`, `guest_mode`, `ps4_env=prod`, `swap_xo` (Blaze's console login
builds `%s/connect/auth?response_type=code` with `display`). The library's Login runs
AuthForCode -> TokenGetByCode -> TokenInfoGet -> **EnsureEnvironment** (messages "timeout in
getting environment", "Failed to get environment information"). After tokeninfo nothing more
happens: no request, no Blaze login (Authentication 1:0xa), no error in the log, until the game
shows the network error. The game is waiting on something, not failing a call: an unsupported
PS4 import would stop the runtime with "STOP: first unsupported PS4 import".

## Ruled out tonight (each changed nothing)

- QoS: NHL 19's `QosConfigInfo` is `CQFR CQRR LTPS QCNF SQRR`, `QCNF` = QosCoordinatorInfo
  (`DPSP QCA QCP QPR`); the coordinator is gRPC over HTTP/2
  (`/eadp.qoscoordinator.QOSCoordinator/ClientCall`). Sending a coordinator, or none at all:
  the game never connected to it.
- `NHL_CTS_CONNECTION` config keys `CTS_XMSHD_BASE_URL`, `CTS_XMSHD_ENVIRONMENT`, `CTS_SKU`,
  `CTS_AUTH_SERVER`: sent, no change.
- tokeninfo: the parser requires client_id, scope, expires_in, pid_id, pid_type, user_id (number
  or string each); device_id, persona_id, xbox_prv are optional. All present, device_id added.
- EAWebKit "not found": now loads, no change.
- Still unexplained: right after the Blaze connect the game opens a second TCP socket (TCP_NODELAY)
  to an empty host name (`resolve  -> refused`), whatever the server sends.

## Second session: the game's own log (BB_GAME_LOG)

`BB_GAME_LOG=1` (src/probe.c) hooks three NHL 19 functions and prints "Game log:" lines; the game
runs unchanged (each entry's first instructions move into a thunk):
- 0x45c1750: the message formatter of the EA identity library (Nucleus SDK 16.2.3). Its messages
  are two-stage (`[%s:%04d] %%s SetDone()`), so the second `%s` stays as written.
- 0x45fbb30: the sports login service's logger (`SportsLoginService::*`, `LoginHandler::*`).
- 0x45c0f60: the identity session's Login result handler (error code at result+0x1c, message at
  +0x20; success goes on to a `service.%s` scope check).

What it shows: AuthForCode, TokenGetByCode, TokenInfoGet, then `[Login:0001] SetDone()`, then the
task pool only ticks. **The Login result handler 0x45c0f60 is never called** and the sports login
service never starts: the Login finishes but its result is not delivered to anyone. The Login
task's done callback is an EASTL function at task+0x88 (fallback +0x98, result at +0xa8; see
0x45dadb0). Next: find who created this Login (callers of 0x45db2c0: 0x45b3290, 0x45b3c30,
0x45b53e0) and which callback it stored, then hook that.

Also ruled out in this session (each changed nothing):
- Patch `patches/NHL19-Online.xml` (off by default; `tools/nhl19-online-patches.json` turns it on):
  the setting `login_with_nucleussdk` ("Nucleus Login", byte 0x61e50d8, default 1) set to 0.
- `/connect/token` with `refresh_token` (TokenGetByCode reads access_token, token_type,
  refresh_token, id_token): now sent.
- Blaze config `identityRedirectUri` (the game reads nucleusConnect, nucleusProxy,
  consoleClientId, identityRedirectUri through getters 0x25e2d10..0x25e2d70): now sent
  (`https://nucleus/oauth_callback`); `/connect/auth` now carries `redirect_uri`.
- PIN telemetry keys `PIN_SERVER_ADDRESS`, `PIN_SERVER_PORT`, `PIN_SERVER_ENV` (read at
  0x25cc37d): now sent, `/pinEvents` and `/bugsentry/session/` answered 200. The empty-host socket
  still happens, so it is not PIN (or PIN reads another config).

Blaze traffic per run: preAuth (9:7), ping (9:2), fetchClientConfig `NHL_CTS_CONNECTION` (9:1),
then only pings. No Authentication login is ever sent.

## Breakthrough (2026-11-22): the login works

The game's login state machine (object vtable 0x5824ae8, state at +0x98, switch at 0x2fe4418,
index = state - 2, table 0x4b3d200) runs: state 5 waits for the EA identity Login (job callback
0x2fe5a90: success -> 6); **state 6 (0x2fe469d) reads the Blaze config key `blazeServerClientId`**
and asks EA for an auth code for it (second `/connect/auth`, client_id NHL2019_PS4_SERVER), then
the Blaze login. Without the key it went to its error state 16 with no message: that was the stop
after tokeninfo. The chain was found with the BB_GAME_LOG hooks 0x45dadb0 (Login task done),
0x45c2040 (session result), 0x2feba20 (listener: request id -> job table), 0x2feba3b (job complete).

Now, with tools/nhl19_server.py: Blaze Authentication login (1:0xa) -> authenticated, postAuth,
UserSessions updateNetworkInfo, fetchClientConfig NHL_DYNAMIC_MSG / NHL_DYNAMIC_ADS / NHL_ROSTER,
SportsLoginService POST `/nv2/auth` (answered with UFC 3's EASW-* headers: no error), xmshd
`/xmshd/search` for the roster (answered with no results), then the "Connect with the NHL" screen
(component 0x86c = ThirdPartyOptIn: cmd 1 getLegalText, 3 status, 2 setStatus; legal text empty).

**Where it stops now:** after Continue/Accept the main menu stays empty. Candidates: the roster
check (NHL_ROSTER config has none of its keys; the search sends `CRC` ""), NHL_DYNAMIC_MSG/ADS
(answered with the generic config map), or a Blaze request answered with an empty reply (Stats 7:0xf
and 7:0x3, Util 9:0x19, ThirdPartyOptIn 0x86c:1/3). Without the server the menu works.

## Earlier step: the call tracer

`BB_TRACE_CALLS=1` (src/probe.c) logs every PS4 system function the runtime implements, with a
time stamp and a count: the first `BB_TRACE_FIRST` (default 20) calls of each, then each power of
two. With it the game was seen to poll sceNpCheckCallback, sceNetCtlCheckCallback and
sceUserServiceGetEvent while it waits; delivering the NP and NetCtl callbacks changed nothing.

```bash
BB_USER_DIR=user-online BB_SKIP_PREPARE=1 BB_TRACE_CALLS=1 bash scripts/run-msys2.sh
grep -n "tokeninfo -> 200" out/run.txt          # the line to read from
```

## Tools added

- `tools/nhl19_server.py`: the UFC 3 server with NHL 19's preAuth (QoS layout above), names
  (`nhl-2019-ps4`, `NHL2019_PS4_CLIENT/SERVER`), the CTS keys, tokeninfo with device_id, a
  logging QoS coordinator port (10745, `--qos coordinator|none`), and logs of the login request
  parameters (codes, tickets and tokens hidden).
- `BB_NET_TRAP=9:7 BB_NET_TRAP_DUMP=out/guest-live.bin` (src/runtime_net.c): now on Windows too,
  sized for NHL 19. `python3 tools/ufc3_tdf_schema.py out/guest-live.bin` lists every Blaze
  message with its wire tags (1066 classes for NHL 19). The dump and the list come from the
  game: keep them in `out/`, never commit them.
- `BB_TRACE_CALLS` (above).
- EAWebKit linking: `scripts/game_check.py` module list, `scripts/link_modules.py` (modules in the
  game's data folders, case ignored), `runtime_module_linked` + `sceKernelLoadStartModule` in
  src/runtime_kernel.c, module_start result 1 accepted in src/probe.c, the launcher's link list.

## Later

- The login after the account sign-in should be close to UFC 3's: NHL 19's
  `Authentication::LoginRequest` (`AUTH CTYP EXTB EXTI PRDT`) and `LoginResponse`
  (`ANON SESS SPAM UNDR`, UserLoginInfo `1CON BUID FRST GEO KEY LLOG MAIL PDTL UID`) have the
  tags tools/ufc3_auth.py already answers.
- Then matchmaking (Online Versus first), peer-to-peer (`src/runtime_net_p2p.h`) and determinism
  between two PCs, and the launcher's Host / Join.

## Overtime online

The launcher's "Overtime online" mode plays the game's own Online Versus on Overtime's community
servers (github.com/MatusRiedl/nhl19-pc-online-overtime), against their shadPS4 players and other
copies of this port. `BB_ONLINE_PROFILE=overtime` from a .bat or nhl19.conf does the same.

- Account: the player's own ShadNet account (shadPS4's PSN service, srv.shadps4.net), the one
  they use in shadPS4. The launcher's ShadNet ID and password go to the game as `BB_SHADNET_ID`
  and `BB_SHADNET_PASSWORD`; src/runtime_shadnet.c signs in once at start-up and asks ShadNet for
  the authorization codes, as shadPS4 does (account id = ShadNet's, online id = the ShadNet ID).
  Without an account, or when ShadNet refuses it, the game is signed out of PSN and the log says
  why. sacredfire.ini keeps the password encrypted for the Windows user (DPAPI); on Linux it is
  in the settings file, readable by the user only. The password is never written to a log.
  `BB_SHADNET_SERVER` (host[:port]) names another ShadNet server, for tests.
- Server: `ot.puckpeak.com` (`BB_OVERTIME_HOST` names a test server); names through this PC's
  DNS, https:// over TLS (src/runtime_tls.c).
- Patches: `patches/NHL19-Overtime.xml` + `NHL19-Overtime-Online.xml`, the set Overtime's
  players have; patches.py writes its netplay fingerprint to the log.
- Floating point: shadPS4's rcp/rsqrt rule (native on AMD, AMD's tables elsewhere), so a match
  against an AMD PC on shadPS4 stays in step.
- Tests: `ninja overtime-net-test` then `python3 -m pytest tests/test_overtime_net.py` (a fake
  ShadNet and HTTPS server on 127.0.0.1, test values only).
