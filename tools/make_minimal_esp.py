#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_minimal_esp.py —— 把 EnhanceEnemyAttributes.esp 精简成「只保留一条 Perk」的最小插件。

为什么要这么做
--------------
DLL 版把 Papyrus 脚本、SkyPatcher 分发、41 个 GLOB、MCM Quest 全部接管了，
原 ESP 里 69 条记录只剩下 EEA_EnhanceMGCPerk 还有用 —— 它是「法术强度」的载体
（Perk 入口点读 ActorValue #143 = LockpickingPowerModifier 作为法术幅度倍率）。

保留整份原 ESP 的坏处：里面的 QUST 会去调已被删除的 Papyrus 脚本 → 日志刷错误。

脚本做的事
----------
1. 读出 PERK 记录
2. 删掉它的 13 条顶层 CTDA 条件（否则会被 EHP_SwitchMGC 等已删除的 GLOB 卡住）
3. 把 PRKE 里的条件计数清零
4. 只写回 TES4 头 + 一个 PERK 顶层组

用法
----
    python make_minimal_esp.py                  # 默认：从 MOD 根目录读，写到 native/dist/
    python make_minimal_esp.py 源.esp 目标.esp   # 手动指定
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE = os.path.abspath(os.path.join(HERE, ".."))
MOD_ROOT = os.path.abspath(os.path.join(NATIVE, ".."))

DEFAULT_SRC = os.path.join(MOD_ROOT, "EnhanceEnemyAttributes.esp")
DEFAULT_OUT = os.path.join(NATIVE, "dist", "EnhanceEnemyAttributes.esp")


def split_subrecords(payload):
    out = []
    i = 0
    while i + 6 <= len(payload):
        sig = payload[i:i + 4]
        size = struct.unpack("<H", payload[i + 4:i + 6])[0]
        out.append((sig, payload[i + 6:i + 6 + size]))
        i += 6 + size
    return out


def build_subrecords(subs):
    return b"".join(sig + struct.pack("<H", len(p)) + p for sig, p in subs)


def find_perk(data, start, end):
    """返回 (payload, flags, formID)。"""
    i = start
    while i + 24 <= end and i + 24 <= len(data):
        sig = data[i:i + 4]
        if sig == b"GRUP":
            group_size = struct.unpack("<I", data[i + 4:i + 8])[0]
            found = find_perk(data, i + 24, i + group_size)
            if found:
                return found
            i += group_size
        elif sig == b"\x00\x00\x00\x00":
            return None
        else:
            data_size, flags, form_id = struct.unpack("<III", data[i + 4:i + 16])
            if sig == b"PERK":
                return data[i + 24:i + 24 + data_size], flags, form_id
            i += 24 + data_size
    return None


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT

    if not os.path.isfile(src):
        print("[错误] 找不到源文件：%s" % src)
        return 1

    with open(src, "rb") as f:
        data = f.read()

    if data[0:4] != b"TES4":
        print("[错误] 不是有效的 ESP/ESM 文件：%s" % src)
        return 1

    header_size = struct.unpack("<I", data[4:8])[0]
    tes4 = data[:24 + header_size]

    version, num_records, next_object = struct.unpack("<fII", data[26:38])
    masters = []
    for sig, payload in split_subrecords(data[24:24 + header_size]):
        if sig == b"MAST":
            masters.append(payload.split(b"\x00")[0].decode("utf-8", "replace"))

    print("源文件      : %s (%d 字节)" % (src, len(data)))
    print("头部版本    : %.2f" % version)
    print("前置插件    : %s" % (", ".join(masters) if masters else "(无)"))

    found = find_perk(data, 24 + header_size, len(data))
    if not found:
        print("[错误] 没找到 PERK 记录。")
        return 1

    payload, flags, form_id = found
    subs = split_subrecords(payload)

    kept = []
    removed = 0
    for sig, sub in subs:
        if sig == b"CTDA":
            removed += 1
            continue
        if sig == b"PRKE" and len(sub) >= 3:
            # 第 3 字节是条件计数，清零以免指向已删除的条件块
            sub = sub[:2] + b"\x00" + sub[3:]
        kept.append((sig, sub))

    new_payload = build_subrecords(kept)

    edid = next((p.split(b"\x00")[0].decode("utf-8", "replace")
                 for s, p in subs if s == b"EDID"), "?")
    full = next((p.split(b"\x00")[0].decode("utf-8", "replace")
                 for s, p in subs if s == b"FULL"), "?")

    print("保留记录    : PERK 0x%08X  %s  \"%s\"" % (form_id, edid, full))
    print("删除条件    : %d 条顶层 CTDA" % removed)
    print("保留子记录  : %s" % ", ".join(s.decode("latin1") for s, _ in kept))

    # ---- 组装记录（头 24 字节：sig + dataSize + flags + formID + VC + version + unknown）----
    record = (b"PERK"
              + struct.pack("<II", len(new_payload), flags)
              + struct.pack("<I", form_id)
              + b"\x00" * 8
              + new_payload)

    # ---- 顶层组 ----
    grup = (b"GRUP"
            + struct.pack("<I", 24 + len(record))
            + b"PERK"
            + struct.pack("<i", 0)
            + struct.pack("<HHHH", 0, 0, 0, 0)
            + record)

    # ---- 修 TES4 里的 HEDR.numRecords ----
    new_header_payload = bytearray(tes4[24:])
    idx = new_header_payload.find(b"HEDR")
    if idx >= 0:
        struct.pack_into("<I", new_header_payload, idx + 6 + 4, 1)
    body = tes4[:24] + bytes(new_header_payload) + grup

    os.makedirs(os.path.dirname(out), exist_ok=True)
    backup = out + ".bak"
    if os.path.isfile(out):
        os.replace(out, backup)
        print("已备份旧输出: %s" % backup)

    with open(out, "wb") as f:
        f.write(body)

    print("\n输出        : %s (%d 字节)" % (out, len(body)))
    print("提示：用 xEdit 打开确认只剩 1 条 PERK，游戏里再实测一次法术强度是否生效。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
