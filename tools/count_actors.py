#!/usr/bin/env python3
"""
统计插件里的 Actor 相关记录量级，用于评估「启动时全量扫描并改写 base AV」方案的开销。

关注记录类型：
  NPC_  ActorBase（人形）——被改写 base AV 的目标
  CREA  ActorBase（生物）——同上
  ACHR  Actor 引用（ACHR/ACRE）——场景里实际实例化的单位
  LVLN / LVLC  等级化 NPC / 生物列表

用法：
  python count_actors.py <插件路径> [更多路径...]
  python count_actors.py --dir <mods目录>      对目录下所有 esp/esm/esl 做汇总
"""
import struct
import sys
import os
import time

WANT = {
    b'NPC_': 'NPC_',
    b'CREA': 'CREA',
    b'ACHR': 'ACHR',
    b'ACRE': 'ACRE',
    b'LVLN': 'LVLN',
    b'LVLC': 'LVLC',
}


def scan(fn):
    """返回 (counts, filesize)。用显式栈迭代，避免深递归爆栈。"""
    size = os.path.getsize(fn)
    with open(fn, 'rb') as fp:
        data = fp.read()
    if len(data) < 24 or data[0:4] not in (b'TES4', b'TES3'):
        return None, size
    hs = struct.unpack('<I', data[4:8])[0]
    counts = {}
    n = len(data)
    # 显式栈：(start, end)
    stack = [(24 + hs, n)]
    unpack_I = struct.unpack_from
    while stack:
        a, b = stack.pop()
        j = a
        while j + 24 <= b:
            sig = data[j:j + 4]
            if sig == b'GRUP':
                gs = unpack_I('<I', data, j + 4)[0]
                if gs < 24 or j + gs > b:
                    break
                stack.append((j + 24, j + gs))
                j += gs
            else:
                rd = unpack_I('<I', data, j + 4)[0]
                nm = WANT.get(sig)
                if nm:
                    counts[nm] = counts.get(nm, 0) + 1
                j += 24 + rd
    return counts, size


def fmt(n):
    return f"{n:,}"


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    if args[0] == '--dir':
        root = args[1]
        exts = ('.esp', '.esm', '.esl')
        total = {}
        files = []
        for dp, _dn, fns in os.walk(root):
            for f in fns:
                if f.lower().endswith(exts):
                    files.append(os.path.join(dp, f))
        files.sort()
        grand_bytes = 0
        t0 = time.time()
        for f in files:
            c, sz = scan(f)
            grand_bytes += sz
            if c:
                for k, v in c.items():
                    total[k] = total.get(k, 0) + v
        dt = time.time() - t0
        print(f"扫描 {len(files)} 个插件，共 {grand_bytes/1024/1024:.1f} MB，耗时 {dt:.1f}s\n")
        for k in ('NPC_', 'CREA', 'ACHR', 'ACRE', 'LVLN', 'LVLC'):
            print(f"  {k:5} {fmt(total.get(k, 0))}")
        npc = total.get('NPC_', 0) + total.get('CREA', 0)
        achr = total.get('ACHR', 0) + total.get('ACRE', 0)
        print(f"\n  ActorBase 合计 = {fmt(npc)}")
        print(f"  Actor 引用合计 = {fmt(achr)}")
        return

    for fn in args:
        if not os.path.exists(fn):
            print(f"跳过（不存在）: {fn}")
            continue
        t0 = time.time()
        c, sz = scan(fn)
        dt = time.time() - t0
        name = os.path.basename(fn)
        if c is None:
            print(f"{name}: 非 TES4 文件")
            continue
        parts = "  ".join(f"{k}={fmt(c.get(k,0))}" for k in ('NPC_', 'CREA', 'ACHR', 'ACRE', 'LVLN', 'LVLC'))
        print(f"{name:44} {sz/1024/1024:7.1f}MB  {dt:5.2f}s  {parts}")


if __name__ == '__main__':
    main()
