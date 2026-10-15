#!/usr/bin/env python3
"""
双轨制 Boss 检测扫描器 —— 复刻 TrueHUD Boss Scanner 的判据，用于评估覆盖率。

判据（都在 NPC_ 记录上，即「定义级」）：
  Track A  EDID 含 "boss"（不区分大小写）—— 命名 Boss，永不参与通用过滤
  Track B  AIDT 里 Aggression >= 2（VeryAggressive/Frenzied）且 Confidence == 4（Foolhardy）

通用 NPC 前缀过滤（仅作用于 Track B）：
  见 GENERIC_PREFIXES

用法：
  python boss_scan.py <插件路径> [...]            扫描指定插件
  python boss_scan.py --dir <mods目录>            扫描目录下全部 esp/esm/esl
  python boss_scan.py --dump <插件路径>           调试：打印前 20 条命中的原始子记录
"""
import os
import struct
import sys
import zlib
from collections import Counter

GENERIC_PREFIXES = (
    "enc", "lvl", "lchar",
    "dun", "mg", "da", "ms", "mq", "tg", "db",
    "guard", "soldier", "bandit", "draugr", "falmer", "warlock", "necromancer",
    "vampire", "hagraven", "dremora", "atronach", "giant", "skeleton",
    "myth", "creature", "test",
    "horse", "summon", "dog", "fox", "chicken", "rabbit", "cow",
    "c0",
)

AG_NAME = {-1: "Calmed", 0: "Unaggressive", 1: "Aggressive", 2: "VeryAggressive", 3: "Frenzied"}
CF_NAME = {0: "Cowardly", 1: "Cautious", 2: "Average", 3: "Brave", 4: "Foolhardy"}

REC_DELETED = 1 << 5
REC_COMPRESSED = 1 << 18


def subrecords(payload):
    """遍历子记录，处理 XXXX 扩展长度。"""
    i = 0
    n = len(payload)
    while i + 6 <= n:
        sig = payload[i:i + 4]
        sz = struct.unpack_from("<H", payload, i + 4)[0]
        if sig == b"XXXX":
            if i + 10 > n:
                return
            big = struct.unpack_from("<I", payload, i + 6)[0]
            i += 10
            if i + 4 > n:
                return
            sig = payload[i:i + 4]
            yield sig, payload[i + 4:i + 4 + big]
            i += 4 + big
            continue
        yield sig, payload[i + 6:i + 6 + sz]
        i += 6 + sz


def scan(path, dump=0):
    """返回 (stats, hits)。hits 每项: (mod, edid, full, aggression, confidence, track)"""
    with open(path, "rb") as fp:
        data = fp.read()
    if len(data) < 24 or data[0:4] not in (b"TES4", b"TES3"):
        return None, []
    hs = struct.unpack_from("<I", data, 4)[0]
    mod = os.path.basename(path)
    st = Counter()
    hits = []
    stack = [(24 + hs, len(data))]
    while stack:
        a, b = stack.pop()
        j = a
        while j + 24 <= b:
            sig = data[j:j + 4]
            if sig == b"GRUP":
                gs = struct.unpack_from("<I", data, j + 4)[0]
                if gs < 24 or j + gs > b:
                    break
                stack.append((j + 24, j + gs))
                j += gs
                continue
            rd = struct.unpack_from("<I", data, j + 4)[0]
            fl = struct.unpack_from("<I", data, j + 8)[0]
            if sig == b"NPC_" and not (fl & REC_DELETED):
                st["npc"] += 1
                payload = data[j + 24:j + 24 + rd]
                if fl & REC_COMPRESSED:
                    try:
                        payload = zlib.decompress(payload[4:])
                    except Exception:
                        st["bad_zlib"] += 1
                        j += 24 + rd
                        continue
                edid = full = None
                ag = cf = None
                lvl = None
                for s, p in subrecords(payload):
                    if s == b"EDID":
                        edid = p.split(b"\x00")[0].decode("utf-8", "replace")
                    elif s == b"FULL":
                        full = p.split(b"\x00")[0].decode("utf-8", "replace")
                    elif s == b"AIDT" and len(p) >= 2:
                        ag = struct.unpack_from("<b", p, 0)[0]
                        cf = struct.unpack_from("<b", p, 1)[0]
                    elif s == b"ACBS" and len(p) >= 10:
                        lvl = struct.unpack_from("<h", p, 8)[0]
                if not edid:
                    st["no_edid"] += 1
                    j += 24 + rd
                    continue
                low = edid.lower()
                track_a = "boss" in low
                track_b = (ag is not None and ag >= 2 and cf == 4)
                if track_a:
                    st["trackA"] += 1
                if track_b:
                    st["trackB"] += 1
                if track_a and track_b:
                    st["both"] += 1
                if track_a or track_b:
                    generic = low.startswith(GENERIC_PREFIXES)
                    if track_a or not generic:
                        st["kept"] += 1
                        if dump and len(hits) < dump:
                            hits.append((mod, edid, full, ag, cf, lvl))
                        else:
                            hits.append((mod, edid, full, ag, cf, lvl))
                    else:
                        st["filtered_generic"] += 1
            j += 24 + rd
    return st, hits


def show(hits, limit=40, title=""):
    if title:
        print(f"\n{title}")
    for h in hits[:limit]:
        mod, edid, full, ag, cf, lvl = h
        ags = AG_NAME.get(ag, str(ag)) if ag is not None else "-"
        cfs = CF_NAME.get(cf, str(cf)) if cf is not None else "-"
        print(f"  {edid:38} | {str(full)[:26]:26} | ag={ags:15} cf={cfs:9} | lv={lvl} | {mod}")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return

    if args[0] == "--dump":
        st, hits = scan(args[1])
        print(f"{os.path.basename(args[1])}: {dict(st)}")
        show(hits, 20, "前 20 条命中：")
        return

    paths = []
    if args[0] == "--dir":
        for dp, _dn, fns in os.walk(args[1]):
            for f in fns:
                if f.lower().endswith((".esp", ".esm", ".esl")):
                    paths.append(os.path.join(dp, f))
        paths.sort()
    else:
        paths = args

    grand = Counter()
    allhits = []
    per_mod = []
    for p in paths:
        try:
            st, hits = scan(p)
        except Exception as e:
            print(f"  跳过 {os.path.basename(p)}: {e}")
            continue
        if st is None:
            continue
        grand.update(st)
        allhits.extend(hits)
        if st["kept"]:
            per_mod.append((st["kept"], os.path.basename(p)))

    print(f"扫描 {len(paths)} 个插件")
    print(f"  NPC_ 记录总数        {grand['npc']:>8,}")
    print(f"  Track A（EDID 含 Boss）{grand['trackA']:>8,}")
    print(f"  Track B（AI 数据）     {grand['trackB']:>8,}")
    print(f"  两轨同时命中          {grand['both']:>8,}")
    print(f"  被通用前缀过滤掉      {grand['filtered_generic']:>8,}")
    print(f"  ── 最终保留 Boss       {grand['kept']:>8,}")
    show(allhits, 30, "命中样例（前 30）：")
    print("\n命中最多的插件（前 15）：")
    for n, m in sorted(per_mod, reverse=True)[:15]:
        print(f"  {n:>5}  {m}")


if __name__ == "__main__":
    main()
