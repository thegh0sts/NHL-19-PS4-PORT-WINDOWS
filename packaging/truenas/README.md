# NHL 19 online server on TrueNAS (Community Edition 25.10)

The server (`nhl19_server.py`) is the online lobby: EA/Blaze sign-in, the online menus,
matchmaking and stats. The match itself goes **directly between the two players' PCs** (UDP), so
the players' PCs must still reach each other (same network, port forwarding or a VPN).

It uses only Python's standard library: the app runs the official `python:3-slim` image with the
server files from a dataset. Nothing to build or publish.

## 1. The files

Build `dist/nhl19-server.zip` on the PC with the repository:

```bash
bash scripts/package-server.sh
```

It holds the five server files, this guide and `compose.yaml`.

## 2. A dataset for them

1. **Datasets → Add Dataset**, for example `nhl19-server` under your apps dataset
   (`tank/apps/nhl19-server`). Choose the **Apps** dataset preset so the TrueNAS `apps` user
   (568) can read it.
2. Copy the contents of `nhl19-server.zip` into it, so that `nhl19_server.py` is directly inside
   the dataset (`/mnt/tank/apps/nhl19-server/nhl19_server.py`): through an SMB share of the
   dataset, or with `scp` and `unzip` in **System → Shell**.

## 3. The app

1. **Apps → Discover Apps → Custom App → Install via YAML** (the three-dot menu beside
   **Custom App** on some versions).
2. Name: `nhl19-server`. Paste `compose.yaml`.
3. In `volumes:`, change `/mnt/tank/apps/nhl19-server` to your dataset's path. Optionally set
   `TZ` and `NHL19_TILES`.
4. **Save**. Under **Apps → nhl19-server → Workloads → Logs** the server prints
   `listening on 0.0.0.0:10041` and the other ports.

The app uses **host networking**: these ports must be free on the NAS (the TrueNAS web UI's 80/443
are not among them):

| Protocol | Ports |
| --- | --- |
| TCP | 42230, 10041, 40443, 40080, 10744, 10745 |
| UDP | 17502 |

## 4. Playing

In the NHL 19 launcher on each player's PC: **Online play = Join server**, **Join address = the
NAS's address**: its LAN address at home, or its Tailscale address (the TrueNAS Tailscale app)
for players elsewhere. Each player needs a different **player name**: the server tells accounts
apart by it.

Over the internet without a VPN, forward the TCP and UDP ports above to the NAS, and the match
port (UDP 3659) to each player's PC. Some internet providers (CGNAT) make port forwarding
impossible: then use a VPN.

## 5. Updating

Copy the new server files over the old ones in the dataset and **Restart** the app. The server
keeps no files yet (accounts are made again from the player names), so nothing else changes.

## A ready-made image (optional)

`packaging/truenas/Dockerfile` bakes the files into an image (`docker build -f
packaging/truenas/Dockerfile -t nhl19-server .` from the repository root). Push it to a registry
(for example GitHub Container Registry), then in `compose.yaml` use that image and remove the
`volumes:` and `working_dir:` lines.
