#!/usr/bin/env python3
"""纯 Python 的 PE 体检工具（零依赖，不需要 dumpbin / VS）。

用途：编译产物拿到手后，先离线确认这颗 DLL 到底「是给谁用的」，
避免装进游戏才发现架构不对 / 缺依赖 / 导出名写错。

检查项：
  · 机器类型（x64?）、是否 DLL、子系统
  · 导出表：SKSE 插件必须有 SKSEPlugin_Version + SKSEPlugin_Load
  · 导入表：完整列出依赖的 DLL，标出 CRT 口径（/MD 还是 /MT）
  · 误链调试版 CRT（MSVCP140D 之类）—— Release 包出现即为事故
  · 意外依赖：除系统 DLL / CRT / API Set 外还依赖了别的 DLL
    （典型坑：vcpkg 用动态三元组 x64-windows 时会把 fmt.dll / spdlog.dll
      带进来，而 MOD 包里根本没有这两个文件 → 进游戏直接加载失败）

用法：
    python tools/pe_info.py dist/SKSE/Plugins/EEA_Native.dll
    python tools/pe_info.py --strict <dll>     # 有问题就 exit 1，适合塞进 CI
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

# Windows 上重定向输出时 Python 默认走 cp1252/locale 编码，本脚本要打中文，
# 不显式改成 UTF-8 会直接 UnicodeEncodeError（CI 上踩过一次）。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

MACHINES = {
    0x014C: "x86 (32 位)",
    0x8664: "x64 (64 位)",
    0xAA64: "ARM64",
}
SUBSYSTEMS = {1: "NATIVE", 2: "WINDOWS_GUI", 3: "WINDOWS_CUI"}

# 期望的 SKSE 导出（缺少就直接不能加载）
REQUIRED_EXPORTS = ("SKSEPlugin_Version", "SKSEPlugin_Load")
# 可选导出
OPTIONAL_EXPORTS = ("SKSEPlugin_Query",)

# /MD（动态 CRT）相关
RUNTIME_DLLS = {
    "msvcp140.dll": "C++ 标准库（动态 CRT）",
    "vcruntime140.dll": "C++ 运行时（动态 CRT）",
    "vcruntime140_1.dll": "C++ 异常处理（动态 CRT，VS2019+ 必需）",
    "ucrtbase.dll": "Universal CRT（Win10 自带）",
}
DEBUG_CRT_MARKERS = ("msvcp140d.dll", "vcruntime140d.dll", "ucrtbased.dll")

# 允许出现的系统 DLL：其余一律视为「需要额外随包分发」的依赖
SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-win-")
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll",
    "shell32.dll", "shlwapi.dll", "gdi32.dll", "gdi32full.dll", "user32.dll",
    "version.dll", "ws2_32.dll", "crypt32.dll", "bcrypt.dll", "ncrypt.dll",
    "ntdll.dll", "psapi.dll", "dbghelp.dll", "powrprof.dll", "setupapi.dll",
    "winmm.dll", "imm32.dll", "dwmapi.dll", "uxtheme.dll", "comdlg32.dll",
    "rpcrt4.dll", "secur32.dll", "mswsock.dll", "d3d11.dll", "dxgi.dll",
    "dinput8.dll", "xinput1_3.dll", "d3dcompiler_47.dll", "msvcrt.dll",
    "ucrtbase.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll",
    "concrt140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
}


def _rva_to_off(rva: int, sections) -> int:
    for _name, va, vsize, psize, poff in sections:
        if va <= rva < va + max(vsize, psize):
            return poff + (rva - va)
    return rva


def _cstr(blob: bytes, off: int) -> str:
    end = blob.find(b"\x00", off)
    if end < 0:
        return ""
    return blob[off:end].decode("ascii", "replace")


def parse(path: Path, strict: bool) -> int:
    problems: list[str] = []
    blob = path.read_bytes()
    if blob[:2] != b"MZ":
        print("!! 不是 PE 文件（缺 MZ 头）")
        return 1

    pe_off = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe_off:pe_off + 4] != b"PE\0\0":
        print("!! 缺 PE 签名")
        return 1

    coff = pe_off + 4
    machine, nsec, _ts, _sym, _nsym, opt_size, chars = struct.unpack_from(
        "<HHIIIHH", blob, coff)
    opt = coff + 20
    plus = struct.unpack_from("<H", blob, opt)[0] == 0x20B
    subsystem = struct.unpack_from("<H", blob, opt + 68)[0]
    num_dirs = struct.unpack_from("<I", blob, opt + (108 if plus else 92))[0]
    dirs_off = opt + (112 if plus else 96)

    sec_off = opt + opt_size
    sections = []
    for i in range(nsec):
        b = sec_off + i * 40
        name = blob[b:b + 8].rstrip(b"\x00").decode("ascii", "replace")
        vsize, va, psize, poff = struct.unpack_from("<IIII", blob, b + 8)
        sections.append((name, va, vsize, psize, poff))

    def dir_entry(idx: int):
        if idx >= num_dirs:
            return 0, 0
        return struct.unpack_from("<II", blob, dirs_off + idx * 8)

    print(f"文件      : {path}")
    print(f"大小      : {len(blob):,} 字节")
    print(f"架构      : {MACHINES.get(machine, hex(machine))}")
    kind = "DLL" if chars & 0x2000 else "EXE"
    print(f"类型      : {kind}   （字符特征 0x{chars:04X}）")
    print(f"子系统    : {SUBSYSTEMS.get(subsystem, subsystem)}")

    if machine != 0x8664:
        problems.append(f"架构不是 x64（{MACHINES.get(machine, hex(machine))}），Skyrim SE 不认")
    if not chars & 0x2000:
        problems.append("不是 DLL")

    # ---- 导出表 ----
    # IMAGE_EXPORT_DIRECTORY 布局（11 个字段，共 40 字节）：
    #  0 Characteristics     4 TimeDateStamp     8 MajorVersion(2)  10 MinorVersion(2)
    # 12 Name               16 Base             20 NumberOfFunctions
    # 24 NumberOfNames      28 AddressOfFunctions
    # 32 AddressOfNames     36 AddressOfNameOrdinals
    exp_rva, _ = dir_entry(0)
    exports: list[str] = []
    if exp_rva:
        eo = _rva_to_off(exp_rva, sections)
        (_chars, _ts, _maj, _mino, _name_rva, _base,
         nfunc, nnames, _addr_func, addr_names, _addr_ord) = struct.unpack_from(
            "<IIHHIIIIIII", blob, eo)
        no = _rva_to_off(addr_names, sections)
        for i in range(nnames):
            nrva = struct.unpack_from("<I", blob, no + i * 4)[0]
            exports.append(_cstr(blob, _rva_to_off(nrva, sections)))

    print("\n--- 导出符号 ---")
    if not exports:
        print("  (导出表为空 —— SKSE 插件必须导出 SKSEPlugin_Version / SKSEPlugin_Load)")
        problems.append("没有导出表")
    else:
        print(f"  共 {len(exports)} 个；NumberOfFunctions={nfunc}")
        for name in exports:
            mark = "   <== SKSE 入口" if name in REQUIRED_EXPORTS + OPTIONAL_EXPORTS else ""
            print(f"  {name}{mark}")

    lower_exports = {e.lower() for e in exports}
    missing = [r for r in REQUIRED_EXPORTS if r.lower() not in lower_exports]
    if missing:
        problems.append("缺少必要导出: " + ", ".join(missing))
    else:
        print("  ✓ SKSEPlugin_Version + SKSEPlugin_Load 齐全")

    # ---- 导入表 ----
    imp_rva, _ = dir_entry(1)
    dlls: list[str] = []
    if imp_rva:
        io = _rva_to_off(imp_rva, sections)
        i = 0
        while True:
            desc = struct.unpack_from("<IIIII", blob, io + i * 20)
            if desc == (0, 0, 0, 0, 0):
                break
            dlls.append(_cstr(blob, _rva_to_off(desc[3], sections)))
            i += 1

    print(f"\n--- 依赖 DLL（共 {len(dlls)} 个）---")
    for d in sorted(dlls, key=str.lower):
        note = RUNTIME_DLLS.get(d.lower(), "")
        print(f"  {d}" + (f"    <- {note}" if note else ""))

    low = {d.lower() for d in dlls}

    print("\n--- 运行库口径 ---")
    bad = [d for d in dlls if any(m in d.lower() for m in DEBUG_CRT_MARKERS)]
    if bad:
        print(f"  ✗ 链了调试版 CRT: {', '.join(bad)} —— Release 包不能出现这个")
        problems.append("链了调试版 CRT: " + ", ".join(bad))
    elif low & {"msvcp140.dll", "vcruntime140.dll"}:
        print("  动态 CRT（/MD）：依赖 msvcp140 / vcruntime140")
        print("  → 装机需要 Microsoft Visual C++ 2015–2022 x64 运行库")
    else:
        print("  ✓ 静态 CRT（/MT）：单文件自包含，装机不需要额外运行库")

    # ---- 意外依赖：除系统/CRT/API Set 之外的 DLL ----
    unexpected = [
        d for d in dlls
        if d.lower() not in SYSTEM_DLLS
        and not d.lower().startswith(SYSTEM_PREFIXES)
    ]
    print("\n--- 需要随包分发的依赖 ---")
    if unexpected:
        for d in sorted(unexpected, key=str.lower):
            print(f"  ✗ {d}")
        print("  → DLL 目录里必须带上这些文件，否则游戏加载插件会直接失败")
        problems.append("存在需要随包分发的依赖: " + ", ".join(sorted(unexpected)))
    else:
        print("  ✓ 无 —— 只依赖系统 DLL 与 CRT，单文件可直接放进 SKSE/Plugins/")

    print("\n--- 节区 ---")
    for name, va, vsize, psize, poff in sections:
        print(f"  {name:<8} VA 0x{va:08X}  vsize {vsize:>9,}  raw {psize:>9,}")

    print("\n--- 结论 ---")
    if problems:
        for p in problems:
            print(f"  ✗ {p}")
        return 1 if strict else 0
    print("  ✓ 全部检查通过")
    return 0


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--strict"]
    strict = "--strict" in sys.argv
    if not args:
        print(__doc__)
        return 1
    rc = 0
    for arg in args:
        p = Path(arg)
        if not p.is_file():
            print(f"!! 找不到文件: {p}")
            rc = 1
            continue
        rc |= parse(p, strict)
        print()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
