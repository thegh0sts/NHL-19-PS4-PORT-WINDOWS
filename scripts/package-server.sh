#!/usr/bin/env bash
# dist/nhl19-server.zip: the NHL 19 online server alone (for a TrueNAS app, a spare PC or a VPS):
# the five Python files, the TrueNAS compose file and its guide, at the top of the zip (unzip into
# the dataset). Standard library only: any
# Python 3.10+ runs it with  python3 nhl19_server.py
set -euo pipefail
cd "$(dirname "$0")/.."
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p dist
cp tools/nhl19_server.py tools/ufc3_server.py tools/ufc3_auth.py tools/ufc3_games.py tools/blaze_tdf.py "$stage/"
cp packaging/truenas/compose.yaml packaging/truenas/README.md "$stage/"
version=$(git describe --tags --always --dirty 2>/dev/null || echo unknown)
echo "$version" > "$stage/VERSION"
rm -f dist/nhl19-server.zip
python3 - "$stage" dist/nhl19-server.zip <<'PY'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for base, _, files in os.walk(stage):
        for f in sorted(files):
            path = os.path.join(base, f)
            z.write(path, os.path.relpath(path, stage))
PY
echo "package-server: dist/nhl19-server.zip ($version)"
