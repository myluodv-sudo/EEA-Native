# EEA_Native —— 动态增强敌人属性（原生 SKSE 插件版）

把 [Dynamic Enhance Enemy Attributes](https://www.nexusmods.com/skyrimspecialedition/mods/65861) 的
Papyrus 逻辑整体换成 SKSE 原生插件。配置项与原版 MCM 一一对应，改由游戏内 ImGui 菜单与 INI 管理。

- 目标运行时：**Skyrim SE 1.5.97**（SKSE 2.0.20）
- 依赖：SKSE + Address Library（你已装）
- 依赖：**Microsoft Visual C++ 2015–2022 x64 运行库**（MSVCP140.dll）。
  DLL 按 vcpkg 默认三元组 `x64-windows` 编译，用**动态 CRT（/MD）** —— 因为
  vcpkg 编出来的 CommonLibSSE-NG 就是 /MD，两者必须一致，否则链接期 LNK2038。
  绝大多数 Skyrim 整合包装机时已经带上；若游戏报 DLL 加载失败，装一次运行库即可。
- 可选：SKSE Menu Framework（你整合包里已有，缺失时菜单自动隐藏，功能不受影响）

---

## 一、它修掉了原版的什么

| 原版症状 | 原版根因（精确到行） | 本版做法 |
|---|---|---|
| Boss 体型**永久**变大 / 进地牢掉帧 10–20 秒 | `EnhanceEnemyBoss.psc` 的 `OrinScale` 是脚本局部变量，读档后脚本实例重建 → 原值丢失，`OnEffectFinish` 最后 `SetScale(1.0)` 把原值抹掉；且每次刷新都无条件调 `SetScale` | 原始体型存在**内存 + co-save**里，永远不丢；**只在数值真的变化时**才调 `SetScale`（幂等） |
| 离开区域 / 进门 / **改设置** → 敌人**回满血** | `EnhanceEnemyAV.psc::ResetAV()` 第 37–39 行：**先** `RestoreAV(AVstring, GetAVMax(AVstring))` 把当前值拉满，**再** `modav(GetBaseAV - GetAVMax)` 把上限降回基准 —— 净结果必然是满血。而 `ResetAV` 被 `OnEffectFinish`（L159）和 `OnUpdate`（L99）**无条件**调用，所以只要法术效果一起一落就回一次血。改设置时 `UpdateEnemyAV.psc` 会依次翻转 6 个开关（每个还夹一次 `Utility.Wait(1.0)`），等于整条法术反复起落 6 次 → 满血 6 次。这就是「脱战/进出门/改设置都会满血」的确切机制 | 全部改为**按净差写上限**，且**幂等**（数值没变就一行都不碰引擎）；从不调用 `RestoreAV`，全程不出现「最大值」这个概念。同时对生命/法力/体力**保持当前值百分比** —— 于是残血敌人强化后仍按原比例，改设置时血量占比也不动 |
| Sigdis 传送后满血 | 同上，传送 = 效果重放 | 同上，从机制上不存在 |
| 进地牢瞬间脚本爆发 | 每个被强化单位各复制一份 `ActiveMagicEffect` 实例；`UpdateEnemyAV.psc` 里 6 段 `Utility.Wait(1.0)` 串行占 VM | **零 Papyrus**；队列制 + 每帧最多处理 `iProcessPerTick` 个单位（跨帧摊平） |
| **1 级生效会拖垮游戏**（作者原话：1 级会让各种小动物也生效，脚本数量太多，不建议） | Papyrus 里「生效范围」只能靠 magic effect 条件门控，一旦放开就是**每个单位一份脚本实例**常驻 | 用 `TESCombatEvent` 做**事件驱动**：`iTriggerMode=1` 时只有真正进入战斗的单位才会被处理。于是 `minLevel` 可以放心调到 1 |
| MCM 菜单偶尔不显示 | SkyUI MCM 注册时序竞争 | ImGui 页面由 DLL 注册，无此问题 |
| 10 级才生效改不了 | `GetLevel > 9` 烘焙在 ESP 的 MGEF 条件里 | 变成可调滑块 `minLevel`（1–80） |
| 随从伤害被 NFF 锁死 | 法术发给所有人，Perk 改的是全局性 AV | 由一个 DLL 统一判定与施加，可选 `bExcludeFollowers` / `bExcludeSummons` |

**顺带查实的原版行为（读 ESP + Skyrim.esm 二进制确认，非推测）**：

- **龙其实是被原版主动排除的**：整条法术条件里有 `HasKeyword(ActorTypeDragon) == 0`
  （0x00035D59）和 `GetIsClass(EncClassDragon) == 0`（0x0002F201）。所以 Nexus 上
  「龙不受属性加成影响」并不是被别的龙类 MOD 覆盖，而是原版设计。本版用 `bExcludeDragons`
  控制，默认保持原版行为，改成 0 就能让龙也吃加成。
- **「含友军」这份 ESP 确实被改过，而且改法可以精确定位**。原版（不含友军）每个属性块是
  `SwitchMOD==1 且 SwitchX==1 且 GetDead==0 且 IsHostileToActor(Player)==1 且 GetShouldHelp(Player)==0`；
  「含友军」版把后两条**整组删掉**（5 组共删 10 条），所以随从/召唤物现在也能吃到属性加成。
  Boss 块保留 `IsHostileToActor(Player)==1`，只删掉了 `GetShouldHelp(Player)==0`。
- 原版法术共 23 条条件（不含友军版 34 条），对应 7 个效果块：HP / MP / SP / ATK / MGC / Boss / Update。
  **注意**：这 23 条里 OR 标志位全是 0。若按字面 AND 理解，法术会要求「所有开关同时打开」才生效。
  这点我离线无法定论（属于引擎对 SPEL 条件的语义问题，或保存时标志位被抹平），
  但**对本 DLL 版没有任何影响** —— 属性开关在 C++ 里是独立判断，天生互不影响。

### 为什么「整成插件」能同时解决回血和性能

这两件事其实是**同一个根因**：Papyrus 时代，「生效范围」和「何时生效」都只能塞进 magic effect 的条件里。
想覆盖更多单位，就只能靠更多常驻脚本实例去扛 —— 所以作者被迫用 `GetLevel > 9` 把数量压下来
（他本人的回复：「1 级会让各种小动物也生效，脚本数量太多」）。而「什么时候该回滚数值」这件事，
脚本里也只有 `OnEffectFinish` 这一个粗糙的时机可用 —— 回满血就是这么来的。

插件的代价模型完全不同：

| | Papyrus 原版 | 本插件 |
|---|---|---|
| 每个被强化单位的**常驻**开销 | 一份完整 `ActiveMagicEffect` 脚本实例 | 0（只是一条记录，没有实例） |
| 数量上升时 | 实例数线性增长，VM 队列被占满 | 只影响首次入队时的队列消化速度 |
| 数值没变化时 | 每次刷新仍**无条件** `ModAV` + `SetScale` | **一行都不执行**（幂等） |
| 何时处理 | 只能靠条件门控，条件一满足就全上 | `iTriggerMode=1` → 只在进入战斗那一刻处理 |
| 大批单位同时出现 | 瞬间爆发，掉帧 10–20 秒 | 每帧只处理 `iProcessPerTick` 个，平滑消化 |
| 数值如何回滚 | `RestoreAV(…, GetAVMax)` → 顺带把血拉满 | 净差写回，且保持当前值百分比 |

所以 `minLevel` 在插件版里可以一直调到 **1**，把「省性能」这件事交给触发模式去管。

**保留的机制**：法术强度仍然走原作者那条路 —— Perk `EEA_EnhanceMGCPerk` 读 `LockpickingPowerModifier`(AV #143) 作为法术幅度倍率。同时额外提供一条**纯 DLL 路线**（直接加成五大法术学派 `*PowerModifier`），见下文 `[Magic] sMode`。

---

## 二、与原版文件的关系

**本版需要**（放到 `Data/SKSE/Plugins/`）：

```
EEA_Native.dll
EEA_Native.ini
EEA_Native.log        ← 运行后自动生成，排查问题就看它
```

**需要保留**：`EnhanceEnemyAttributes.esp`，但建议用 `tools/make_minimal_esp.py` 精简成只含 1 条 Perk 的版本
（原文件里 69 条记录，其余 68 条 DLL 已全部接管；不精简的话里面的 MCM Quest 会去调已删除的脚本，日志刷错误）。

> 顺带说明：原 ESP 头部本来就带 **ESL 标记**（`flags = 0x200`），走的是 FE 轻量插件槽位，
> 所以它并不占用你 255 个常规插件槽中的一格 —— 精简后依然如此（1 条记录，本地 ID 0xC59 < 0xFFF）。

**可以删掉**：`scripts/`、`source/`、`MCM/`、`Interface/translations/`、
`SKSE/Plugins/SkyPatcher/npc/EnhanceEnemyAttributes.ini`

> ⚠️ 不要和原版 Papyrus 版同时启用 —— 会双重加成。

---

## 三、目录结构

```
native/
├─ CMakeLists.txt              构建脚本（MSVC 运行时库按 VCPKG_TARGET_TRIPLET 自动对齐）
├─ CMakePresets.json
├─ vcpkg.json                  依赖：commonlibsse-ng-se
├─ vcpkg-configuration.json    指向 Color-Glass 注册表（CommonLibSSE-NG 不在官方 vcpkg 里）
├─ EEA_Native.ini              默认配置（构建时自动拷进 dist/）
├─ src/
│   ├─ PCH.h
│   ├─ Config.h / .cpp              INI 读写，键名与原版 settings.ini 一致
│   ├─ EnhancementManager.h / .cpp  判定 / 计算 / 施加 / 精确回滚 / 跨帧摊平 / co-save
│   ├─ Menu.h / .cpp                SKSE 菜单框架页面（6 个 Tab + 实时预览）
│   └─ main.cpp                     插件入口、事件与序列化注册
├─ third_party/
│   └─ SKSEMenuFramework.h      单头文件接入菜单框架（不需要 vcpkg 里的 imgui）
├─ tools/                       离线分析工具（纯 Python，直接读插件文件，不依赖游戏运行）
│   ├─ make_minimal_esp.py      把 ESP 精简到只剩 1 条 Perk
│   ├─ count_actors.py          统计 NPC_ / CREA / ACHR / LVLN 数量（评估扫描开销）
│   ├─ boss_scan.py             扫全量 NPC_，对比各种 Boss 判据的命中率
│   ├─ acbs_scan.py             统计 ACBS 标志位分布（定义层过滤器的影响面）
│   ├─ npc_dump.py              按 FormID 打印任意 NPC_ 的原始字段（含 ACBS / DNAM / 派系）
│   ├─ player_probe.py          找出全加载顺序里「谁覆盖了玩家那条 NPC_ 0x00000007」
│   └─ find_form.py             按 EDID 反查表单 FormID（支持 --type FACT/NPC_/KYWD…）
└─ .github/workflows/build.yml  云端编译
```

---

## 四、构建

### 路线 A：GitHub Actions 云端编译（推荐，本机零安装）

1. 在 GitHub 新建一个仓库（public 免费无限时长）。
2. **把 `native/` 目录里的内容作为仓库根目录推上去**（注意：是 `native/` 的内容，不是 `native/` 这层文件夹）。
   ```bash
   cd "…/mods/EnhanceEnemyAttributes（含友军）/native"
   git init
   git add .
   git commit -m "EEA_Native: 初始工程"
   git branch -M main
   git remote add origin https://github.com/<你的用户名>/<仓库名>.git
   git push -u origin main
   ```
3. 打开仓库的 **Actions** 页，等 `build` 跑完（首次要把 CommonLibSSE-NG 编一遍，约 10–20 分钟；之后有缓存会快很多）。
4. 进那次运行的页面，在 **Artifacts** 里下载 `EEA_Native`，解压得到 `EEA_Native.dll`。

### 路线 B：本地编译

装 **Visual Studio 2022 Build Tools**（勾选「使用 C++ 的桌面开发」）+ **CMake** + **vcpkg**，然后：

```powershell
setx VCPKG_ROOT "C:\dev\vcpkg"
cd "…\native"
cmake -B build -S . --preset default
cmake --build build --preset release
```

产物会自动被拷到 `native/dist/SKSE/Plugins/`。

---

## 五、安装（MO2）

1. 备份当前 MOD 目录（尤其 `EnhanceEnemyAttributes.esp`）。
2. 把 `dist/SKSE/Plugins/EEA_Native.dll` 放进本 MOD 的 `SKSE/Plugins/`。
3. 生成精简 ESP：
   ```bash
   python native/tools/make_minimal_esp.py
   ```
   它会输出到 `native/dist/EnhanceEnemyAttributes.esp`（365 字节，只含 1 条 Perk），
   用 xEdit 打开确认无误后，**替换** MOD 根目录里那个 13 KB 的原版 ESP。
4. 删掉：`scripts/`、`source/`、`MCM/`、`Interface/translations/`、
   `SKSE/Plugins/SkyPatcher/npc/EnhanceEnemyAttributes.ini`。
5. 进游戏，`Data/SKSE/Plugins/EEA_Native.ini` 会自动生成，`EEA_Native.log` 会有启动日志。

---

## 六、配置

### 游戏内菜单

按 **DELETE** 打开「模组控制面板」→ 左侧 `EEA_Native` → `敌人属性增强`。
共 **7 个 Tab**、**52 个可配置项**，菜单文案全中文。改动**即时生效**（滑条一松就落盘并重算）。

#### Tab ①　总开关（18 项）

| 配置项 | 取值范围 | INI 键 |
|---|---|---|
| 启用本模组 | 开 / 关 | `bEEASwitchMOD` |
| 也强化玩家本人 | 开 / 关 | `bAffectPlayer` |
| 生效的最低等级 | 1 – 80 | `minLevel` |
| Boss 额外属性加成 | 开 / 关 | `bEEASwitchBoss` |
| Boss 体型增量 | −0.10 – 2.00 | `fEEABossScale` |
| 龙类不加体型（避免速度暴走） | 开 / 关 | `bSkipBossScaleOnDragons` |
| Boss 判定 ① 原版地点参照类型（最精确） | 开 / 关 | `bBossUseLocRefType` |
| Boss 判定 ② 记录名含 Boss | 开 / 关 | `bBossUseEditorID` |
| Boss 判定 ③ 战斗 AI 数据 | 开 / 关 | `bBossUseAIData` |
| 排除随从 | 开 / 关 | `bExcludeFollowers` |
| 改用定义层派系判定 | 开 / 关 | `bFollowerCheckByFaction` |
| 排除召唤物 | 开 / 关 | `bExcludeSummons` |
| 定义层过滤：独特 NPC / 重要 NPC / 受保护 | 开 / 关 | `bSkipUnique` / `bSkipEssential` / `bSkipProtected` |
| 定义层过滤：无敌 / 可被召唤 / 模板·捏脸预设 | 开 / 关 | `bSkipInvulnerable` / `bSkipSummonable` / `bSkipTemplated` |

#### Tab ②–⑥　生命 / 法力 / 体力 / 攻击力 / 法术强度（五页结构相同，共 30 项）

| 配置项 | 取值范围 | INI 键 |
|---|---|---|
| 启用该属性强化 | 开 / 关 | `bEEASwitch*` |
| 基础百分比（每 1 点 = 额外 1%） | 0 – 1000 | `iExtrapct*` |
| 叠加玩家等级 | 开 / 关 | `bPCLevelSwitch*` |
| 叠加敌人等级 | 开 / 关 | `bELevelSwitch*` |
| 额外倍率 | 0.0 – 20.0 | `fExtraMult*` |
| Boss 倍率 | 0.0 – 20.0 | `fBossMult*` |
| 预览用玩家等级 / 预览用敌人等级 | 1 – 100 | 仅界面预览，不写盘 |

每页底部有**实时预览**：按上面两个预览等级直接算出「普通敌人额外加成 +x%」与
「Boss 额外加成 +x%」；法术强度页还会显示「法术伤害 / 增益幅度 ×n」。

#### Tab ⑦　关于 / 性能（4 项 + 状态 + 按钮）

| 配置项 | 取值范围 | INI 键 |
|---|---|---|
| 每帧处理单位数 | 1 – 64 | `iProcessPerTick` |
| 详细日志 | 开 / 关 | `bVerboseLog` |
| 仅进入战斗时才强化 | 开 / 关 | `iTriggerMode`（只给 0 / 1 两档） |
| 保持当前血量百分比 | 开 / 关 | `bPreserveCurrentRatio` |

**只读状态**：框架版本、当前已强化单位数、待处理队列长度、法术强度增益载体（Perk）是否接入、
Boss 判定②（记录名 EditorID）是否可用、法术强度实现方式。

**三个按钮**：`立即刷新全部` / `还原所有单位` / `保存到 INI`。

> `还原所有单位` 在卸载插件前必须点一次（默认用 `kPermanent` 桶，数值随存档保存）。

#### 这些只能改 INI，菜单里没有

| INI 键 | 为什么没进菜单 |
|---|---|
| `sMode` | 法术强度实现方式（Perk / PowerMod / Both），牵涉"要不要保留 ESP"，属装前决策 |
| `iTriggerMode=2` | 菜单只给最常用的两档（0 加载 / 1 仅战斗） |
| `bUseTemporaryModifier` | **已失效**：引擎未暴露带桶的 `ModActorValue`，属性上限恒走永久修正。保留键名仅为 INI 兼容 |
| `iBossMinAggression` / `iBossMinConfidence` | Boss 判定③的阈值（该判据默认关闭） |
| `sLocRefTypeBoss` / `sBossEdidPatterns` | Boss 判定的匹配文字 |
| `sBossKeywords` / `sBossRaces` / `sBossNpcs` | Boss 显式名单兜底 |
| `sDragonKeywords` / `sDragonClasses` | 龙类识别的依据 |
| `sPerkPlugin` / `sPerkEditorID` | 法术强度 Perk 的定位方式 |
| `sSectionName` / `bEnabled`（Menu 段） | 菜单自身的标题与总开关 |
| `bTraceValues` / `bLogFirstApply` | 诊断开关 |

### INI 键位表

`[Main]` 以外的键名与原作者 MCM 的 `settings.ini` **完全一致**，可以直接把旧文件改个名沿用。

| 段 | 键 | 含义 | 默认 |
|---|---|---|---|
| Main | `bEEASwitchMOD` | 总开关 | 1 |
| Main | `minLevel` | 生效最低等级 | 10 |
| Main | `bAffectPlayer` | 也强化玩家本人 | 0 |
| Main | `bEEASwitchBoss` / `fEEABossScale` | Boss 额外加成 / 体型增量 | 1 / 0.10 |
| HP MP SP ATK MGC | `bEEASwitch*` | 该属性开关 | 仅 HP 为 1 |
| 同上 | `iExtrapct*` | 基础百分比（每 1 点 = 1%） | 20 |
| 同上 | `bPCLevelSwitch*` / `bELevelSwitch*` | 叠加玩家等级 / 敌人等级 | 1 / 1 |
| 同上 | `fExtraMult*` / `fBossMult*` | 额外倍率 / Boss 倍率 | 1.0 / 2.0 |
| Filter | `bExcludeFollowers` / `bExcludeSummons` | 排除随从 / 召唤物 | 0 / 0（含友军） |
| Filter | `bFollowerCheckByFaction` | 排除随从时是否改用**定义层派系**判定（更彻底） | 1 |
| Filter | `bSkipUnique` / `bSkipEssential` / `bSkipProtected` | 按 ACBS 标志排除：独特 / 重要 / 受保护 | 0 |
| Filter | `bSkipInvulnerable` / `bSkipSummonable` / `bSkipTemplated` | 按 ACBS 标志排除：无敌 / 可召唤 / 用模板 | 0 |
| Filter | `bBossUseLocRefType` / `sLocRefTypeBoss` | Boss 判定①：原版 `HasRefType` 机制（最精确） | 1 / `Boss` |
| Filter | `bBossUseEditorID` / `sBossEdidPatterns` | Boss 判定②：EditorID 含指定片段 | 1 / `Boss` |
| Filter | `bBossUseAIData` / `iBossMinAggression` / `iBossMinConfidence` | Boss 判定③：AI 数据（实测命中 42%，默认关） | 0 / 2 / 4 |
| Filter | `sBossKeywords` / `sBossRaces` / `sBossNpcs` | Boss 判定④：显式名单兜底 | 空 / `DragonPriestRace` / 空 |
| Filter | `bExcludeDragons` | 龙类是否排除在属性强化外（原版行为） | 1 |
| Magic | `sMode` | `Perk` / `PowerMod` / `Both` | Perk |
| Perf | `iProcessPerTick` | 每帧处理单位数 | 6 |
| Perf | `iTriggerMode` | `0`=加载即强化 / `1`=仅战斗触发 / `2`=两者 | 0 |
| Perf | `bPreserveCurrentRatio` | 改上限时保持当前血量百分比（不回血） | 1 |
| Perf | `bUseTemporaryModifier` | **已失效**，恒为 `kPermanent`（见下方「存档残留」说明） | 0 |

> `iTriggerMode=1` 与 `bPreserveCurrentRatio=1` 是最值得动的两个开关：
> 前者让你可以放心把 `minLevel` 设成 1，后者决定「改设置时血量会不会跳」。

### 定义层过滤：为什么「按 NPC 类型排除」比「按当前状态排除」可靠

这一组开关参照的是 **Dynamic NPC Scaling**（Nexus 180306）的 `[SkipFlags]`，
算是这个路数里已经跑通并被 2,500+ 人下载验证过的做法。

判据全部落在 **ActorBase（NPC_ 记录）自己的 ACBS 标志位**上，也就是回答
「这个 NPC 属于哪一类」，而不是「这一份实例现在什么状态」：

| 开关 | ACBS 位 | 含义 | 本机整合包 35,905 条 NPC_ 中的占比 |
|---|---|---|---|
| `bSkipUnique` | `0x00000020` | 独特 NPC | 19.4% |
| `bSkipEssential` | `0x00000002` | 重要 NPC（倒地不死） | 6.5% |
| `bSkipProtected` | `0x00000800` | 受保护（只倒不死） | 2.0% |
| `bSkipInvulnerable` | `0x80000000` | 无敌 | 2.4% |
| `bSkipSummonable` | `0x00004000` | 可被召唤 | 7.7% |
| `bSkipTemplated` | `0x00000100` / `0x00000004` | 用模板 / 捏脸预设 | 2.0% / 0.8% |

> 位值以 CommonLibSSE-NG 的 `RE::ACTOR_BASE_DATA::Flag` 为权威来源。
> ⚠️ UESP / xEdit 的网页文档把 `PCLevelMult` 标成 `0x40` 是**错的**，正确是 `0x80`
> （`0x40` 是 `kDoesntAffectStealthMeter`）。照错的值写代码会静默失效。

好处有三个：**结论稳定**（招募 / 复活 / 读档 / 换地点都不改变判定）、
**开销为零**（内存里几次位测试）、**不依赖运行时 EditorID**（那条路要装 Native EditorID Fix）。

#### 顺带说清「玩家是怎么被排除的」

- **Dynamic NPC Scaling 的做法**：它没有专门的玩家判定，靠的是玩家本体那条
  `NPC_ 0x00000007 "Player"` 带着 ACBS 的 `Unique` 标志（`0x30` = `AutoCalcStats + Unique`，
  见 `tools/player_probe.py`），于是被 `SkipUnique` 顺带挡掉。这在默认配置下有效，
  但**耦合**：用户一旦把 `SkipUnique` 关掉（想让独特 NPC 也吃加成），玩家就会一起被改。
- **本插件的做法**：玩家判定是**独立且无条件**的，不受任何过滤器开关影响
  （`Manager::IsPlayerActor()`，引用级 + 定义级双保险）。
  所以你可以放心把六个 `bSkip*` 全关，玩家不会被漏进来。
- 两个脚本可以直接核对：`tools/player_probe.py`（全加载顺序里玩家记录有没有被别的 mod 改掉）
  和 `tools/npc_dump.py`（按 FormID 打印任意 NPC_ 的原始字段）。

#### 随从判定为什么加了一条

`bExcludeFollowers` 原来只看运行时的 `IsPlayerTeammate()` —— 那只认**此刻已经被招募**的随从。
打开 `bFollowerCheckByFaction` 后改用定义层派系判定，
连「将来可能被招募」的 NPC 也一并排除，不受当前招募状态影响：

```
PotentialFollowerFaction  0x0005C84D   —— Skyrim.esm 里 43 条 NPC_ 带它
CurrentFollowerFaction    0x0005C84E   —— 40 条
```

（这两条走硬编码 FormID 而不走 EditorID 查询 —— 运行时 EditorID 未必可读，
而它们是原版常量，FormID 永远不变。顺带一提：NPC_ 的派系写在 **`SNAM`** 子记录里，
**不是** `FACT` 子记录，这点实解时踩过坑。）

### Boss 判定的三级设计（附实测数据）

原版用的 `HasRefType(LocationRefType "Boss")` 是**引用级**判据，必须在 CK 里逐个手工标记，
所以覆盖面很窄——Nexus 上「Boss 没被识别」的反馈多半来自这里。

另一种常见做法（见 TrueHUD Boss Scanner 那类工具）是**双轨制**：
EditorID 含 "Boss" ＋ AI 数据（`Aggression >= 2` 且 `Confidence = 4`）。
我用 `tools/boss_scan.py` 在你本机整合包上全量测了一遍：

| 判据 | 命中数 | 占 35,904 个 NPC_ |
|---|---|---|
| EditorID 含 "Boss" | 3,508 | 9.8% |
| AI 数据（Aggression≥2 且 Confidence=4） | **15,097** | **42%** |
| 两轨重叠 | 2,366 | — |
| 前缀过滤后保留 | 13,686 | 38% |

**AI 数据这条轨道对属性加成来说太宽泛**——大量 mod 给所有战斗单位都设了
`VeryAggressive + Foolhardy`，连普通尸鬼（`zzzCrbEncDraugrBlades1H02`）都会被算成 Boss。
对 TrueHUD 那种「只决定显不显示血条」的用途无所谓，但 38% 的敌人都吃 Boss 倍率，
Boss 设定就等于不存在。

所以本插件把它拆成三级，**默认只开前两级**：

1. `bBossUseLocRefType=1` —— 原版机制，最准
2. `bBossUseEditorID=1` —— 覆盖 mod 作者命名出来的 Boss
3. `bBossUseAIData=0` —— **默认关**，想要更激进的覆盖再显式打开

**属性映射**（与原版 ESP 里的 FormList `EEA_ActorValueNameList` 一致）：

| 属性 | ActorValue | 枚举 |
|---|---|---|
| 生命 / 法力 / 体力 | `health` / `magicka` / `stamina` | `kHealth` / `kMagicka` / `kStamina` |
| 攻击力 | `AttackDamageMult` | `kAttackDamageMult` (154) |
| 法术强度 | `LockpickingPowerMod` | `kLockpickingPowerModifier` (143) |

计算公式与原版逐字一致：

```
额外值 = ceil( 基础AV × (基础百分比 + 玩家等级 + 敌人等级) × 0.01 × 额外倍率 × Boss倍率 )
```

---

## 七、待实机验证的点（第一次跑起来后请重点看日志）

代码里有几处**无法离线验证**的判断，都留了日志，跑一次就能确认：

1. **`ModActorValue` 的实际接受量** —— 打开 `[Debug] bTraceValues=1`，每次写入都会打印
   `base= / 上限= / 当前= / want= / 实记=`。判定标准：
   - `上限` 一列不随 `want` 变化 → 说明该属性不被所选桶接受（重点看 `AttackDamageMult`，
     它是乘数型 AV，理论基础值 1.0）；
   - `实记` 与 `want` 一致 → 正常；
   - 同一单位连续打印出**递增**的 `实记` → 幂等没生效，请立刻把这行日志发我。
2. **`bPreserveCurrentRatio` 的行为** —— 把敌人打到半血，再在菜单里改动生命百分比。
   期望**血条百分比不变**，而不是回满。若你想让"血量绝对值不动"，把这项关掉。
3. **Boss 判定** —— 三级判据，设计依据与实测数据见第六节。启动日志会逐级打印接入结果，
   重点看第 [2] 级这一行：
   - `[EEA] Boss 判定 [2/3] EditorID 可读，匹配片段：boss` → 正常
   - `[EEA] Boss 判定 [2/3] 运行时取不到 EditorID …` → 需要处理（见下）

   **取不到 EditorID 是正常现象**：Skyrim 默认**不**在内存里保留 EDID
   （这正是 Native EditorID Fix 这类插件存在的理由）。三条路选一条：
   装一个 Native EditorID Fix（推荐，装了这条判据立刻可用）；把 `bBossUseEditorID` 改成 0，
   只靠第 [1] 级；或把 `bBossUseAIData` 改成 1 走 AI 数据（实测会命中 42%，慎用）。
4. **`iTriggerMode=1`（仅战斗触发）** —— 打开后进一个新地牢，日志里**不应**出现批量「首次强化」，
   只有拔刀开打（或被打）的单位才会出现。确认后就可以把 `minLevel` 改成 1。

另外这几处 API 按 CommonLibSSE-NG `ng` 分支的头文件写，首次编译若有报错，把报错贴回来即可快速修：
`RE::TESRace::HasKeyword`、`Actor::IsPlayerTeammate / IsSummoned`、`Actor::AddPerk` 的 rank 参数、
`TESObjectREFR::GetEditorLocation()`、`stl::enumeration::get()`（战斗状态比较）、
`ActorValueOwner::ModActorValue(ActorValue, float)`（2 参数）、
`ActorValueOwner::RestoreActorValue(ACTOR_VALUE_MODIFIER, ActorValue, float)`（3 参数）、
`TESNPC::npcClass`（**没有 `GetClass()`**）、`TESObjectREFR::GetReferenceRuntimeData().refScale`
（**没有 `SetScale()`**）、`TESNPC::GetFormEditorID()`、
`TESAIForm::GetAggressionLevel / GetConfidenceLevel`、
`TESDataHandler::GetFormArray<RE::TESNPC>()`。

还有一类风险**编译查不出来**——是运行时行为：`TESForm::GetFormEditorID()` 到底有没有值、
`TESAIForm` 的 AI 数据是否已反映模板继承的结果。这两条跑一次看日志就能确认。

> 预期第一次 `push` 会有若干编译错误 —— 这是 SKSE 插件的常态，把 Actions 的日志贴出来就行，不需要你自己改。

---

## 八、故障排查

- **日志**：`Data/SKSE/Plugins/EEA_Native.log`（每次启动追加，出问题直接发这个文件）
- **菜单没出现**：先看日志有没有 `菜单已注册`。没有的话说明没检测到 `SKSEMenuFramework.dll`；
  功能不受影响，改 INI 即可。
- **完全没有强化效果**：确认 `bEEASwitchMOD=1`、`minLevel` 不高于目标敌人等级、
  至少有一个 `bEEASwitch*` 是 1，且没有同时开着原版 Papyrus 版。
- **想彻底还原**：菜单里点「还原所有单位」，或把 `bEEASwitchMOD` 改 0 后重新读档。

## 九、卸载

**先在菜单里点一次「还原所有单位」**（重要），然后删掉 `EEA_Native.dll` / `.ini` / `.log`，
把精简 ESP 换回原版 ESP，恢复 `scripts/`、`MCM/`、SkyPatcher 的 ini 即可。

> **关于存档残留**：属性上限写在 `kPermanent` 桶 —— 也就是原版 Papyrus `ModAV` 用的同一个桶
> （CommonLibSSE-NG 3.7.0 没暴露带桶的 `ModActorValue`，所以没有别的选择），
> **代价是数值会随存档保存**。所以卸载前务必先「还原所有单位」；
> 万一忘了，读档后插件已不在，这些增量会留在角色身上（表现为敌人血量偏高）。
> 这是刻意的取舍：换来的是「读档后可以认领、不会把残血敌人扣死」。
> 保持当前血量百分比那一步走的是 `kTemporary` 桶，只影响当前值、不写上限，
> 回滚时会被重新计算的比例自然吸收。
