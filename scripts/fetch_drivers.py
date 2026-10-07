#!/usr/bin/env python3
import json, os, pathlib, shutil, subprocess, urllib.parse, urllib.request, zipfile, hashlib

ROOT = pathlib.Path.cwd()
OUT = ROOT / 'out'
DL = OUT / 'downloads'
EX = OUT / 'extracted'
AN = OUT / 'analysis'
for p in (DL, EX, AN): p.mkdir(parents=True, exist_ok=True)

TARGETS = [
('PX-SERIES-Linux-1.0','http://plex-net.co.jp/plex/PX-SERIES_ver.1.0_Linux_Driver.zip'),
('PX-SERIES-Linux-1.0-www','http://www.plex-net.co.jp/plex/PX-SERIES_ver.1.0_Linux_Driver.zip'),
('PX-S3U-1.0.4','https://plex-net.co.jp/plex/px-s3u/PX-S3U_Ver.1.0.4.zip'),
('PX-S3U2-1.0.5','https://plex-net.co.jp/plex/px-s3u2/PX-S3U2_ver.1.0.5.zip'),
('PX-W3U2-1.0.3','https://plex-net.co.jp/plex/px-w3u2/PX-W3U2.Driver_Utility_Package_Ver.1.0.3.zip'),
('PX-W3U3-1.1','https://plex-net.co.jp/plex/px-w3u3/PX-W3U3_driver_Ver-1.1.zip'),
('PX-W3U3-V2-1.0','https://plex-net.co.jp/plex/px-w3u3v2/Driver_PX-W3U3_V2_Ver1.0.zip'),
]

def download(url, dest):
    req = urllib.request.Request(url, headers={'User-Agent':'Mozilla/5.0 asicen-research/1.0'})
    with urllib.request.urlopen(req, timeout=90) as r, open(dest, 'wb') as f:
        shutil.copyfileobj(r, f)

def valid_zip(path):
    try:
        with zipfile.ZipFile(path) as z: return z.testzip() is None
    except Exception: return False

def wayback_candidates(url):
    variants=[url]
    if '://plex-net.co.jp/' in url: variants.append(url.replace('://plex-net.co.jp/','://www.plex-net.co.jp/'))
    if '://www.plex-net.co.jp/' in url: variants.append(url.replace('://www.plex-net.co.jp/','://plex-net.co.jp/'))
    rows=[]
    for v in variants:
        q='https://web.archive.org/cdx/search/cdx?url='+urllib.parse.quote(v,safe='')+'&output=json&fl=timestamp,original,statuscode&filter=statuscode:200&collapse=digest'
        try:
            with urllib.request.urlopen(q, timeout=30) as r: data=json.load(r)
        except Exception: continue
        if not isinstance(data,list) or len(data)<2: continue
        hdr=data[0]
        for it in data[1:]:
            d=dict(zip(hdr,it))
            if d.get('timestamp') and d.get('original'): rows.append((d['timestamp'],d['original']))
    return sorted(set(rows), reverse=True)

status=[]
for name,url in TARGETS:
    dest=DL/(name+'.zip')
    ok=False; source=''
    try:
        download(url,dest)
        ok=valid_zip(dest); source='direct' if ok else ''
    except Exception: pass
    if not ok:
        try: dest.unlink()
        except FileNotFoundError: pass
        for ts,orig in wayback_candidates(url):
            wb='https://web.archive.org/web/'+ts+'id_/'+orig
            try:
                download(wb,dest)
                if valid_zip(dest): ok=True; source=wb; break
            except Exception: pass
    if not ok:
        status.append('FAILED '+name+' '+url); continue
    status.append('OK '+name+' '+source)
    h=hashlib.sha256(dest.read_bytes()).hexdigest()
    with open(OUT/'sha256sums.txt','a') as f: f.write(h+'  '+str(dest.name)+'\n')
    outdir=EX/name; outdir.mkdir(parents=True, exist_ok=True)
    try:
        with zipfile.ZipFile(dest) as z: z.extractall(outdir)
    except Exception: pass

(OUT/'fetch-status.txt').write_text('\n'.join(status)+'\n')

files=[p for p in EX.rglob('*') if p.is_file()]
(AN/'inventory.txt').write_text('\n'.join(str(p.relative_to(EX)) for p in files)+'\n')

def run(cmd):
    try:
        return subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120).stdout
    except Exception as e: return 'ERROR '+repr(e)+'\n'

for p in files:
    rel=str(p.relative_to(EX)); safe=rel.replace('/','__').replace(' ','_')
    meta=run(['file',str(p)])+run(['sha256sum',str(p)])
    (AN/(safe+'.meta.txt')).write_text(meta, errors='ignore')
    low=p.name.lower()
    if low.endswith(('.ko','.o','.so','.elf')):
        txt=run(['modinfo',str(p)])+run(['readelf','-h',str(p)])+run(['readelf','-S',str(p)])+run(['readelf','-sW',str(p)])+run(['readelf','-rW',str(p)])+run(['nm','-an',str(p)])+run(['strings','-a','-n','4',str(p)])
        (AN/(safe+'.elf.txt')).write_text(txt, errors='ignore')
        (AN/(safe+'.objdump.txt')).write_text(run(['objdump','-drwC',str(p)]), errors='ignore')
    elif low.endswith(('.sys','.dll','.exe')):
        txt=run(['objdump','-x',str(p)])+run(['strings','-a','-n','4',str(p)])
        (AN/(safe+'.pe.txt')).write_text(txt, errors='ignore')
        (AN/(safe+'.objdump.txt')).write_text(run(['objdump','-D','-Mintel',str(p)]), errors='ignore')
    elif low.endswith(('.inf','.ini','.txt','.cfg')):
        try: shutil.copyfile(p, AN/(safe+'.text.txt'))
        except Exception: pass

clues=[]
needles=('ASICEN','ASV52','ASIE56','VID_','PID_','USB\\VID','TC905','NM1','TDA20','MULTI2','LNB','TSID','SID')
for p in AN.rglob('*.txt'):
    try:
        for i,line in enumerate(p.read_text(errors='ignore').splitlines(),1):
            if any(n in line for n in needles): clues.append(str(p.name)+':'+str(i)+':'+line)
    except Exception: pass
(AN/'clues.txt').write_text('\n'.join(clues[:20000])+'\n')
print((OUT/'fetch-status.txt').read_text())