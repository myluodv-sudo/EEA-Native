#!/usr/bin/env python3
"""
按 FormID 精确导出某个 NPC_ 记录的原始数据（含 ACBS 各字段），
用于查实「玩家本体」这条记录到底长什么样 —— 例如它是否带 Unique / Essential /
Invulnerable / Template 等标记，从而判断别的插件是靠哪一条把玩家排除掉的。

用法：
  python npc_dump.py Skyrim.esm 0x00000007 [更多 FormID...]
  python npc_dump.py --acbs Skyrim.esm            # 列出所有 NPC_ 的 ACBS 摘要（前 N 条）
"""
import struct
import sys
import os

# ACBS 标志位（xEdit / UESP NPC_ 定义）
ACBS_FLAGS = [
    (0x00000001, 'Female'),
    (0x00000002, 'Essential'),
    (0x00000004, 'IsCharGenFacePreset'),
    (0x00000008, 'Respawn'),
    (0x00000010, 'AutoCalcStats'),
    (0x00000020, 'Unique'),
    (0x00000040, 'PCLevelMult'),
    (0x00000080, 'UseTemplate'),
    (0x00000100, 'CalcForAllLevels'),
]

# 只关心这些子记录
WANT_SUBS = [b'EDID', b'ACBS', b'DATA', b'DNAM', b'TPLT', b'TPTA', b'FACT', b'RNAM',
             b'SPLO', b'SPED', b'PRKR', b'KWDA', b'CNAM', b'ZNAM', b'DOFT', b'SOFT',
             b'VTCK', b'INAM', b'PNAM', b'OBTE', b'LVLD', b'LVLF', b'Template']

TYPE_NAME = {
    b'EDID': '编辑器 ID',
    b'ACBS': '配置标志',
    b'DATA': '等级',
    b'DNAM': '技能(25×u8)',
    b'TPLT': '模板继承',
    b'TPTA': '模板标记',
    b'FACT': '派系',
    b'RNAM': '种族',
    b'SPLO': '法术',
    b'PRKR': 'Perk',
    b'KWDA': '关键字',
    b'CNAM': '职业',
    b'ZNAM': '声音',
    b'DOFT': '默认装备',
    b'SOFT': '睡眠装备',
    b'VTCK': 'voice',
    b'INAM': '死亡音效',
    b'PNAM': 'AI 数据',
    b'LVLD': '等级化-lvl',
    b'LVLF': '等级化-flag',
    b'OBTE': '对象模板',
}


def walk_group(data, a, b, cb, label=None):
    """遍历 [a,b) 区域内的记录；cb(sig, fid, payload_start, payload_len, label)"""
    j = a
    while j + 24 <= b:
        sig = data[j:j + 4]
        if sig == b'GRUP':
            gs = struct.unpack_from('<I', data, j + 4)[0]
            if gs < 24 or j + gs > b:
                return
            gtype = struct.unpack_from('<I', data, j + 12)[0]
            glabel = data[j + 16:j + 20]
            sub_label = glabel if gtype == 0 else label
            if gtype == 0:
                walk_group(data, j + 24, j + gs, cb, sub_label)
            else:
                # 内部 GRUP（如 GRUP 下的 cell block）——NPC_ 顶层不会用到，跳过
                pass
            j += gs
        else:
            rd = struct.unpack_from('<I', data, j + 4)[0]
            rflags = struct.unpack_from('<I', data, j + 8)[0]
            fid = struct.unpack_from('<I', data, j + 12)[0]
            cb(sig, fid, j + 24, rd, rflags, label)
            j += 24 + rd


def dump_record(data, rstart, rlen, flags=0):
    """打印一条记录的顶层子记录（自动处理压缩记录）"""
    body = data[rstart:rstart + rlen]
    if flags & 0x00040000:
        import zlib
        usize = struct.unpack_from('<I', body, 0)[0]
        body = zlib.decompress(body[4:])
        print(f"  记录体 压缩后 {rlen} 字节 → 解压 {len(body)} 字节（头部声明 {usize}）")
    else:
        print(f"  记录体 {rlen} 字节")
    p = 0
    end = len(body)
    while p + 6 <= end:
        ssig = body[p:p + 4]
        slen = struct.unpack_from('<H', body, p + 4)[0]
        payload = body[p + 6:p + 6 + slen]
        show(ssig, payload)
        p += 6 + slen


def show(ssig, payload):
    nm = TYPE_NAME.get(ssig, '')
    if ssig == b'EDID':
        print(f"  {ssig.decode()}  {nm:12} {payload.rstrip(b'\\x00').decode('utf-8', 'replace')}")
    elif ssig == b'ACBS':
        if len(payload) >= 4:
            fl = struct.unpack_from('<I', payload, 0)[0]
            on = [n for bit, n in ACBS_FLAGS if fl & bit]
            lv = struct.unpack_from('<h', payload, 4)[0] if len(payload) >= 6 else None
            mn = struct.unpack_from('<h', payload, 6)[0] if len(payload) >= 8 else None
            print(f"  ACBS        {nm:12} flags=0x{fl:08X} [{', '.join(on) or '-'}]"
                  + (f" 等级={lv} min={mn}" if lv is not None else ""))
        else:
            print(f"  ACBS        长度异常 {len(payload)}")
    elif ssig == b'DATA':
        if len(payload) == 2:
            print(f"  DATA        {nm:12} u16={struct.unpack_from('<H', payload, 0)[0]}")
        elif len(payload) == 4:
            print(f"  DATA        {nm:12} 基础HP(u16)={struct.unpack_from('<H', payload, 0)[0]}"
                  f" 重量(float)={struct.unpack_from('<f', payload, 2)[0]:.1f}")
    elif ssig == b'DNAM':
        vals = list(payload[:25]) + [0] * (25 - len(payload))
        names = ['OneH','TwoH','Archery','Block','Smithing','HeavyArm','LightArm','Pickpocket',
                 'Lockpick','Sneak','Alchemy','Speech','Alteration','Conjuration','Destruction',
                 'Illusion','Restoration','Enchanting','Marksman','Speechcraft','','','','','']
        nz = [(nm2, v) for nm2, v in zip(names, vals) if v and nm2]
        print(f"  DNAM        {nm:12} 非零技能: {nz}")
    elif ssig in (b'TPLT', b'RNAM', b'CNAM', b'ZNAM', b'INAM'):
        print(f"  {ssig.decode()}        {nm:12} 引用 0x{struct.unpack_from('<I', payload, 0)[0]:08X}")
    elif ssig in (b'LVLD', b'LVLF'):
        v = list(payload)
        print(f"  {ssig.decode()}        {nm:12} {v}")
    else:
        print(f"  {ssig.decode('ascii', 'replace')}        {nm:12} {len(payload)} 字节 "
              f"{payload[:24].hex()}{'…' if len(payload) > 24 else ''}")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    only = None
    if args[0] == '--acbs':
        args = args[1:]
    path = args[0]
    wanted = set()
    for a in args[1:]:
        wanted.add(int(a, 16) & 0x00FFFFFF)
    print(f"文件 {os.path.basename(path)} ({os.path.getsize(path)/1024/1024:.1f} MB)")
    data = open(path, 'rb').read()
    hs = struct.unpack_from('<I', data, 4)[0]

    found = []

    def cb(sig, fid, start, ln, rflags, label):
        if sig != b'NPC_':
            return
        local = fid & 0x00FFFFFF
        if wanted and local not in wanted:
            return
        found.append((fid, start, ln, rflags))

    walk_group(data, 24 + hs, len(data), cb)
    print(f"命中 {len(found)} 条")
    for fid, start, ln, rflags in found:
        print(f"\n=== NPC_ 0x{fid:08X} ===")
        dump_record(data, start, ln, rflags)


if __name__ == '__main__':
    main()
