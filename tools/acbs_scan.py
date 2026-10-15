#!/usr/bin/env python3
"""扫描一批插件，统计 NPC_ 的 ACBS 标志分布（用于评估「定义层过滤」能筛掉多少）。"""
import struct, sys, os, zlib, time
from concurrent.futures import ProcessPoolExecutor

# 位值以 CommonLibSSE-NG 的 RE::ACTOR_BASE_DATA::Flag 为权威来源
# （UESP/xEdit 网页上把 PCLevelMult 写成 0x40 是错的，正确是 0x80）
FLAGS = [(0x01, 'Female'), (0x02, 'Essential'), (0x04, 'CharGenPreset'), (0x08, 'Respawn'),
         (0x10, 'AutoCalcStats'), (0x20, 'Unique'), (0x40, 'NoStealthMeter'),
         (0x80, 'PCLevelMult'), (0x100, 'UseTemplate'), (0x200, 'CalcForAllLevels'),
         (0x800, 'Protected'), (0x4000, 'Summonable'), (0x10000, 'DoesntBleed'),
         (0x80000000, 'Invulnerable')]

def scan(fn):
    try:
        data = open(fn,'rb').read()
    except Exception:
        return None
    if len(data) < 24 or data[0:4] not in (b'TES4',):
        return None
    hs = struct.unpack_from('<I', data, 4)[0]
    cnt = {n:0 for _,n in FLAGS}
    total = 0
    stack=[(24+hs,len(data))]
    while stack:
        a,b = stack.pop()
        j=a
        while j+24<=b:
            sig=data[j:j+4]
            if sig==b'GRUP':
                gs=struct.unpack_from('<I',data,j+4)[0]
                if gs<24 or j+gs>b: break
                if struct.unpack_from('<I',data,j+12)[0]==0:
                    stack.append((j+24,j+gs))
                j+=gs
            else:
                rd=struct.unpack_from('<I',data,j+4)[0]
                fl=struct.unpack_from('<I',data,j+8)[0]
                if sig==b'NPC_':
                    body=data[j+24:j+24+rd]
                    if fl & 0x00040000:
                        try: body=zlib.decompress(body[4:])
                        except Exception: body=b''
                    p=0
                    while p+6<=len(body):
                        ss=body[p:p+4]; sl=struct.unpack_from('<H',body,p+4)[0]
                        if ss==b'ACBS' and sl>=4:
                            f=struct.unpack_from('<I',body,p+6)[0]
                            total+=1
                            for bit,n in FLAGS:
                                if f&bit: cnt[n]+=1
                            break
                        p+=6+sl
                j+=24+rd
    return total, cnt

def main():
    root = sys.argv[1] if len(sys.argv)>1 else r'E:/SkyrimAE/Data'
    files=[]
    for dp,_dn,fns in os.walk(root):
        for f in fns:
            if f.lower().endswith(('.esp','.esm','.esl')):
                files.append(os.path.join(dp,f))
    files.sort()
    t0=time.time()
    agg={n:0 for _,n in FLAGS}; tot=0
    with ProcessPoolExecutor() as ex:
        for r in ex.map(scan, files, chunksize=8):
            if r:
                tot+=r[0]
                for k,v in r[1].items(): agg[k]+=v
    print(f"扫描 {len(files)} 个插件，含 ACBS 的 NPC_ 共 {tot:,} 条，耗时 {time.time()-t0:.1f}s\n")
    for _,n in FLAGS:
        print(f"  {n:14} {agg[n]:7,}  {agg[n]/max(tot,1)*100:5.1f}%")

if __name__=='__main__':
    main()
