#!/usr/bin/env python3
"""在整个加载顺序里找出「谁覆盖了玩家本体 NPC_ 0x00000007」，并打印其 ACBS。
用来确认：无论装多少 mod，玩家记录是否始终带 Unique 标志。"""
import struct, os, zlib, sys

TARGET = 0x00000007

def walk(data, a, b, root_plugin, out):
    j = a
    while j + 24 <= b:
        sig = data[j:j+4]
        if sig == b'GRUP':
            gs = struct.unpack_from('<I', data, j+4)[0]
            if gs < 24 or j+gs > b: return
            if struct.unpack_from('<I', data, j+12)[0] == 0:
                walk(data, j+24, j+gs, root_plugin, out)
            j += gs
        else:
            rd = struct.unpack_from('<I', data, j+4)[0]
            fl = struct.unpack_from('<I', data, j+8)[0]
            fid = struct.unpack_from('<I', data, j+12)[0]
            if sig == b'NPC_' and (fid & 0x00FFFFFF) == TARGET:
                body = data[j+24:j+24+rd]
                if fl & 0x00040000:
                    try: body = zlib.decompress(body[4:])
                    except Exception: body = b''
                p = 0; acbs = None; edid = None
                while p + 6 <= len(body):
                    ss = body[p:p+4]; sl = struct.unpack_from('<H', body, p+4)[0]
                    pl = body[p+6:p+6+sl]
                    if ss == b'ACBS' and sl >= 4: acbs = struct.unpack_from('<I', pl, 0)[0]
                    if ss == b'EDID': edid = pl.rstrip(b'\x00').decode('utf-8', 'replace')
                    p += 6 + sl
                out.append((fid, acbs, edid))
            j += 24 + rd

def main():
    root = sys.argv[1]
    files = []
    for dp, _dn, fns in os.walk(root):
        for f in fns:
            if f.lower().endswith(('.esp', '.esm', '.esl')):
                files.append(os.path.join(dp, f))
    for fn in sorted(files):
        try:
            data = open(fn, 'rb').read()
        except Exception:
            continue
        if len(data) < 24 or data[0:4] != b'TES4':
            continue
        hs = struct.unpack_from('<I', data, 4)[0]
        out = []
        walk(data, 24+hs, len(data), fn, out)
        for fid, acbs, edid in out:
            marks = []
            for bit, nm in ((0x01,'Female'),(0x02,'Essential'),(0x10,'AutoCalcStats'),
                            (0x20,'Unique'),(0x80,'PCLevelMult'),(0x100,'UseTemplate'),
                            (0x80000000,'Invulnerable')):
                if acbs is not None and acbs & bit: marks.append(nm)
            print(f"{os.path.basename(fn):48} NPC_ 0x{fid:08X}  ACBS=0x{acbs:08X}  [{','.join(marks)}]  EDID={edid}")

if __name__ == '__main__':
    main()
