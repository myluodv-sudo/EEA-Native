#!/usr/bin/env python3
"""按 EDID 在加载顺序里找表单（不依赖游戏运行，直接读插件文件）。

用法：
  python find_form.py <目录> <EDID 精确或片段> [--type FACT]

坑（都是实解踩出来的）：
  · 派系的**记录类型**是 FACT，不是 FACO
  · NPC_ 的**派系成员**写在 SNAM 子记录里，不是 FACT 子记录
  · 记录体可能被压缩（header flags 0x00040000），要先 zlib 解压
"""
import struct, os, sys, zlib

# 4 字符签名 -> 中文类型名（只列常用的）
TYPES = {
    b'FACT': '派系', b'KYWD': '关键字', b'RACE': '种族', b'CLAS': '职业',
    b'LCRT': '地点参照类型', b'NPC_': 'NPC', b'SPEL': '法术', b'PERK': 'Perk',
    b'MGEF': '魔法效果', b'GLOB': '全局变量', b'FLST': '表单列表', b'QUST': '任务',
    b'ARMO': '护甲', b'WEAP': '武器', b'ACTI': '激活器', b'FLOR': '植物',
}


def scan(fn, needle, typ):
    try:
        data = open(fn, 'rb').read()
    except Exception:
        return []
    if len(data) < 24 or data[0:4] != b'TES4':
        return []
    hs = struct.unpack_from('<I', data, 4)[0]
    hits = []
    stack = [(24 + hs, len(data))]
    while stack:
        a, b = stack.pop()
        j = a
        while j + 24 <= b:
            sig = data[j:j+4]
            if sig == b'GRUP':
                gs = struct.unpack_from('<I', data, j+4)[0]
                if gs < 24 or j + gs > b:
                    break
                if struct.unpack_from('<I', data, j+12)[0] == 0:
                    stack.append((j+24, j+gs))
                j += gs
            else:
                rd = struct.unpack_from('<I', data, j+4)[0]
                fl = struct.unpack_from('<I', data, j+8)[0]
                fid = struct.unpack_from('<I', data, j+12)[0]
                if (not typ or sig == typ) and sig not in (b'GRUP', b'TES4'):
                    body = data[j+24:j+24+rd]
                    if fl & 0x00040000:
                        try:
                            body = zlib.decompress(body[4:])
                        except Exception:
                            body = b''
                    if body[:4] == b'EDID':
                        sl = struct.unpack_from('<H', body, 4)[0]
                        edid = body[6:6+sl].rstrip(b'\x00').decode('utf-8', 'replace')
                        if needle.lower() in edid.lower():
                            hits.append((sig, fid, edid))
                j += 24 + rd
    return hits


def main():
    root, needle = sys.argv[1], sys.argv[2]
    typ = None
    if '--type' in sys.argv:
        typ = sys.argv[sys.argv.index('--type') + 1].encode()
    seen = {}
    for dp, _dn, fns in os.walk(root):
        for f in fns:
            if not f.lower().endswith(('.esp', '.esm', '.esl')):
                continue
            fn = os.path.join(dp, f)
            for sig, fid, edid in scan(fn, needle, typ):
                key = (sig, edid)
                seen.setdefault(key, []).append((f, fid))
    for (sig, edid), lst in sorted(seen.items(), key=lambda kv: kv[0][1]):
        t = TYPES.get(sig, sig.decode('ascii', 'replace'))
        refs = '  '.join(f"{n}({i:08X})" for n, i in lst)
        print(f"{t:8} {edid:34} 出现在 {len(lst)} 个插件：{refs}")


if __name__ == '__main__':
    main()
