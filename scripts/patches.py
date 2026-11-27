"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's image places eboot
vaddr 0 at image offset 0. Only literal writes are supported (bytes, bytes16/32/64, float32/64,
utf8, utf16); pattern ("mask") patches are rejected and the patch is skipped whole.

A line may carry Signature="41 8b ?? ..." (hex bytes, ?? for any byte) and SignatureOffset (where
the write goes, from the signature's start). The write goes to Address when the signature matches
there (the 1.21 build for firmware 5.05); otherwise to the one place in the executable where it
matches (the 6.72+ build, whose code sits at other addresses). No match, or more than one: the
patch is skipped whole, with a log line.

Patch files: patches/*.xml in the checkout (patches/CrashTeamRacing.xml, from
illusionyy/PS-Game-Patch) and any folder named with --patches-dir. A patch is applied when the
in-game menu or the launcher turned its bbport.ini key on (GAME_PATCH_KEYS), when patches.json
lists it under "enabled", or when its file marks it isEnabled and patches.json does not disable it.

Overtime online (--profile overtime, or BB_ONLINE_PROFILE=overtime): the patch set Puck Peak Overtime's
players run (OVERTIME_ONLINE_PATCHES) is applied whole, or nothing starts: a missing or failing patch
is an error, not a skipped patch. Of the launcher's own choices only patches that stay on this PC
(OVERTIME_LOCAL_ONLY, e.g. the picture size) are kept. --overtime-host (BB_OVERTIME_HOST) writes
another server into the host slots (a dev server for tests).
Every run writes out/netplay_fingerprint.txt: Overtime's netplay fingerprint of the applied patches,
which the Overtime online sign-in carries (games with different fingerprints simulate different code).
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
TITLE_ID='CUSA13795'
APP_VERSION='01.21'
# bbport.ini keys (the in-game menu, the launcher) -> "<file>/<patch name>". The patches are
# community work from illusionyy/PS-Game-Patch (https://github.com/illusionyy/PS-Game-Patch):
# 60 FPS Unlock and Disable Motion Blur by illusion, the 1080p Pro resolution patch by TroyWarez.
GAME_PATCH_KEYS={'patch_60fps':'CrashTeamRacing.xml/60 FPS Unlock',
                 'patch_no_motion_blur':'CrashTeamRacing.xml/Disable Motion Blur',
                 'patch_pro_1080p':'CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)'}

# Overtime online: the patches Overtime 1.00.0 turns on (its patches/overtime_lan.xml), from
# NHL19-Overtime.xml and NHL19-Overtime-Online.xml, and the fingerprint they make.
OVERTIME_ONLINE_PATCHES=(
    'NHL19-Overtime.xml/R1: cap check on the Franchise per-team list append',
    'NHL19-Overtime.xml/60 FPS replays (Kravickas)',
    'NHL19-Overtime.xml/E32: 32 teams in every mode',
    'NHL19-Overtime.xml/BAP32: Seattle in Be a Pro',
    'NHL19-Overtime-Online.xml/ProtoHttp: accept EAGAIN while waiting for the status code',
    'NHL19-Overtime-Online.xml/Redirector host: ot.puckpeak.com',
    'NHL19-Overtime-Online.xml/QoS coordinator: skip TLS cert validation',
    'NHL19-Overtime-Online.xml/Nucleus hosts: ot.puckpeak.com, 91.98.195.150',
    'NHL19-Overtime-Online.xml/T1: not-found check on the Threes unlock lookup',
    'NHL19-Overtime-Online.xml/Cross-vendor exact math (rcp/rsqrt)',
    'NHL19-Overtime-Online.xml/WoC: every customization item unlocked',
    'NHL19-Overtime-Online.xml/WoC: every attribute of a created player at 99',
    'NHL19-Overtime-Online.xml/Ones: two players can start a match',
    'NHL19-Overtime-Online.xml/C32: 32 clubs in Classic Franchise and Season')
OVERTIME_ONLINE_FINGERPRINT='bf29376fd641daf7f61b'
# Overtime's netplay fingerprint (overtime_core/xmlpatch.py netplay_fingerprint, MIT): sha256 of the
# sorted "ADDRESS=VALUE" lines (both upper case, as written in the file) of every applied patch, without
# the patches that stay on one PC (OVERTIME_LOCAL_ONLY, by name) and the host slots (any server name
# plays the same game); the first 20 hex digits.
OVERTIME_LOCAL_ONLY=('Resolution 2560x1440 (Kravickas)','Resolution 1280x720 (Kravickas)',
                     'Resolution 3840x2160 (Kravickas)','Resolution 5120x2880 (Kravickas)',
                     'Resolution 7680x4320 (Kravickas)','Compute features off (Kravickas)',
                     '60 FPS replays (Kravickas)','BAP32: Seattle in Be a Pro',
                     'T1: not-found check on the Threes unlock lookup','WoC: every customization item unlocked',
                     'Ones: two players can start a match','C32: 32 clubs in Classic Franchise and Season')
# NHL 19 1.60 host strings (address -> characters it holds before the NUL): the Blaze redirector
# host, then the Nucleus accounts, gateway and sign-in base URLs.
OVERTIME_REDIRECTOR_SLOT=('0x51DCCFF',29)
OVERTIME_NUCLEUS_SLOTS=(('0x526E089',24),('0x526E0A2',23),('0x526E0BA',22))
OVERTIME_HOST_RE=re.compile(r'^[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?$')
OVERTIME_URL_RE=re.compile(r'^https?://([A-Za-z0-9.-]+)(?::\d+)?/$')


def read_settings(path):
    """bbport.ini as a dict (key=value lines; # comments)."""
    settings={}
    if path.exists():
        for line in path.read_text().splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def eboot_segments(elf):
    """(start, end) of eboot.elf's load segments: patches must land inside one. An executable segment
    ends where the loader maps it (prepare.py mapped_size): its last 16 KiB page, where patches put
    code caves."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    loads=[]
    for i in range(phnum):
        kind,flags,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind in (1,0x61000010): loads.append((kind,flags,vaddr,vaddr+memsz))
    size=max((end for _,_,_,end in loads),default=0)
    segments=[]
    for kind,flags,vaddr,end in loads:
        if kind!=1: continue
        if flags&1:
            end=min([(end+0x3fff)&~0x3fff,size]+[start for _,_,start,_ in loads if start>=end and start!=vaddr])
        segments.append((vaddr,end))
    return segments


def load_segments(elf):
    """[(vaddr, file offset, file size)] of eboot.elf's load segments (signature search)."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    found=[]
    for i in range(phnum):
        kind,_,offset,vaddr,_,filesz,_,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: found.append((vaddr,offset,filesz))
    return found


def signature_regex(text):
    tokens=text.split()
    if not tokens: raise ValueError('empty signature')
    return re.compile(b''.join(b'.' if t=='??' else re.escape(bytes([int(t,16)])) for t in tokens),re.S)


def locate(line, offset, elf):
    """The image offset a signed line writes to (see the module docstring)."""
    rx=signature_regex(line.get('Signature'))
    shift=int(line.get('SignatureOffset','0'),0)
    loads=load_segments(elf)
    def at(vaddr):
        for start,pos,size in loads:
            if start<=vaddr and vaddr-start<size: return pos+vaddr-start
    pos=at(offset-shift)
    if pos is not None and rx.match(elf,pos): return offset
    hits=[]
    for start,pos,size in loads:
        hits+=[start+m.start()-pos for m in rx.finditer(elf,pos,pos+size)]
        if len(hits)>1: break
    if len(hits)!=1:
        raise ValueError(f'patch site for {line.get("Address")} not found ({len(hits)} signature matches)')
    return hits[0]+shift


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def external_patches(directory, app_version=APP_VERSION, title_id=TITLE_ID):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json). Files whose
    <TitleID> list does not name title_id are skipped (a file without one applies to any game)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and title_id not in ids: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config, ini=None):
    """Selected patches: patches.json {"enabled": [...], "disabled": [...]} overrides each file's
    isEnabled; bbport.ini patch_* keys (GAME_PATCH_KEYS, the in-game menu) add to it."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text())
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    for key,patch in GAME_PATCH_KEYS.items():
        if (ini or {}).get(key)=='1': enabled.add(patch); disabled.discard(patch)
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def overtime_selection(found, config, ini=None):
    """The Overtime online set: every OVERTIME_ONLINE_PATCHES patch, plus the launcher's own choices
    (external_selection) that stay on this PC. A missing patch is a ValueError."""
    keys={key for key,_,_ in found}
    missing=[key for key in OVERTIME_ONLINE_PATCHES if key not in keys]
    if missing: raise ValueError('Overtime online patches missing: '+', '.join(missing))
    chosen={key for key,_,_ in external_selection(found,config,ini)}
    selected=[]
    for key,path,meta in found:
        if key in OVERTIME_ONLINE_PATCHES: selected.append((key,path,meta))
        elif key in chosen:
            if meta.get('Name') in OVERTIME_LOCAL_ONLY: selected.append((key,path,meta))
            else: print(f'Patches: Overtime online leaves out {key} (Overtime players do not run it)',file=sys.stderr)
    return selected


def netplay_fingerprint(selected):
    """Overtime's netplay fingerprint of the selected patches (see OVERTIME_LOCAL_ONLY)."""
    skip={OVERTIME_REDIRECTOR_SLOT[0].upper()}|{addr.upper() for addr,_ in OVERTIME_NUCLEUS_SLOTS}
    lines=sorted(f'{line.get("Address","").upper()}={(line.get("Value") or "").upper()}'
                 for _,_,meta in selected if meta.get('Name') not in OVERTIME_LOCAL_ONLY
                 for line in meta.iter('Line') if line.get('Address','').upper() not in skip)
    return hashlib.sha256('\n'.join(lines).encode('ascii')).hexdigest()[:20]


def set_overtime_host(selected, host, nucleus_url=None):
    """Writes host into the selected patches' host slots: the redirector slot gets host, each Nucleus
    slot https://<host>/ where it fits and nucleus_url where it does not. ValueError when a name is
    not a host, or a slot is too small."""
    if not OVERTIME_HOST_RE.match(host or '') or '--' in host:
        raise ValueError(f'not a host name or address: {host!r}')
    if nucleus_url and not OVERTIME_URL_RE.match(nucleus_url):
        raise ValueError(f'not a base URL (https://host[:port]/): {nucleus_url!r}')
    texts={OVERTIME_REDIRECTOR_SLOT[0].upper():(host,OVERTIME_REDIRECTOR_SLOT[1])}
    for addr,room in OVERTIME_NUCLEUS_SLOTS:
        url=f'https://{host}/'
        texts[addr.upper()]=(url if len(url)<=room or not nucleus_url else nucleus_url,room)
    for _,_,meta in selected:
        for line in meta.iter('Line'):
            slot=texts.get(line.get('Address','').upper())
            if not slot: continue
            text,room=slot
            if len(text)>room:
                raise ValueError(f'{text!r} is {len(text)} characters; the slot at {line.get("Address")} holds {room}'
                                 ' (give a shorter base URL with --overtime-nucleus-url)')
            line.set('Value',(text.encode('ascii')+b'\0').hex().upper())


def compile_external(selected, segments, elf=None, strict=False):
    """Writes of the selected patches; a patch with unsupported lines is skipped whole (strict: a
    ValueError instead). elf (the eboot.elf bytes) is needed for lines with a Signature."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if line.get('Signature'):
                    if elf is None: raise ValueError('signature without the executable')
                    moved=locate(line,offset,elf)
                    if moved!=offset: print(f'Patches: {key}: {line.get("Address")} found at {moved+EBOOT_BASE:#010x}')
                    offset=moved
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            if strict: raise ValueError(f'{key}: {error}') from None
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def write_blob(out, writes):
    """out/patches.bin (BBPATCH2: the patch base, so the loader can rebase pointers written into
    relocated slots)."""
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (out/'patches.bin').write_bytes(blob)


def main():
    root=Path(__file__).resolve().parent.parent
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,default=root/'patches',help='patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled patches')
    p.add_argument('--app-version',default=APP_VERSION)
    p.add_argument('--title-id',default=TITLE_ID,help='the game (patch files naming other ids are skipped)')
    p.add_argument('--out',type=Path,default=root/'out')
    p.add_argument('--settings',type=Path,default=root/'bbport.ini')
    p.add_argument('--external-only',action='store_true',help='accepted for older callers (the only mode)')
    p.add_argument('--profile',default=os.environ.get('BB_ONLINE_PROFILE') or 'default',
                   help='overtime: the Overtime online patch set (default: BB_ONLINE_PROFILE)')
    p.add_argument('--overtime-host',default=os.environ.get('BB_OVERTIME_HOST') or None,
                   help='Overtime online: server name or address for the host slots (default: BB_OVERTIME_HOST)')
    p.add_argument('--overtime-nucleus-url',default=os.environ.get('BB_OVERTIME_NUCLEUS_URL') or None,
                   help='Overtime online: base URL for Nucleus slots too small for https://<host>/')
    a=p.parse_args()
    if a.profile not in ('default','overtime'): p.error(f'unknown profile {a.profile!r} (default, overtime)')
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    found=external_patches(a.patches_dir,a.app_version,a.title_id)
    ini=read_settings(a.settings)
    (a.out/'patches.bin').unlink(missing_ok=True)
    (a.out/'netplay_fingerprint.txt').unlink(missing_ok=True)
    if a.profile=='overtime':
        try:
            selected=overtime_selection(found,a.patches_config,ini)
            fingerprint=netplay_fingerprint(selected)
            if fingerprint!=OVERTIME_ONLINE_FINGERPRINT:
                raise ValueError(f'netplay fingerprint {fingerprint}, Overtime players have {OVERTIME_ONLINE_FINGERPRINT}')
            if a.overtime_host: set_overtime_host(selected,a.overtime_host,a.overtime_nucleus_url)
            writes=compile_external(selected,segments,elf,strict=True)
        except ValueError as error:
            sys.exit(f'Patches: Overtime online: {error}')
        print(f'Patches: Overtime online, fingerprint {fingerprint}'+(f', server {a.overtime_host}' if a.overtime_host else ''))
    else:
        selected=external_selection(found,a.patches_config,ini)
        writes=compile_external(selected,segments,elf)
        fingerprint=netplay_fingerprint(selected)
    write_blob(a.out,writes)
    (a.out/'netplay_fingerprint.txt').write_text(fingerprint+'\n')
    print(f'Patches: {len(writes)} writes from {len(found)} available patches')


if __name__=='__main__':
    main()
