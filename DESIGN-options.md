# 改造路线对比：实例层修正 vs 定义层修正

> 决策记录 · 2026-10-15
> 相关文件：`README.md`（构建与安装）、`EEA_Native.ini`（配置）

## 一、实测数据：扫描开销的真实量级

对本机整合包做了全量统计（工具：`tools/count_actors.py`，纯 Python 逐字节解析磁盘文件）：

| 范围 | 插件数 | 体积 | `NPC_` 记录 | `ACHR` 引用 | 解析耗时 |
|---|---|---|---|---|---|
| 原版 5 个主文件 | 5 | 346 MB | 6,525 | 10,978 | 0.57 s |
| **完整整合包** | **822** | **1,030 MB** | **35,905** | **39,765** | **6.8 s** |

**两点必须澄清**：

1. 上面的耗时是**逐字节解析磁盘文件**。插件在游戏里运行时**不读文件**——所有 ESP 已经在内存里，只需要遍历
   `TESDataHandler::GetSingleton()->GetFormArray(FormType::NPC)` 这个指针数组。C++ 里 3.6 万条记录做过滤 + 3 次浮点写入，
   量级是**毫秒**，不是秒。
2. **改 base 不需要重启游戏**，只需要**重读存档**。base 修改是运行时内存操作，重读存档会让所有 actor 重新实例化，100% 生效。
   用户原先设想的"改数值要重启游戏"是过度保守了。

**结论：全量扫描的成本可以忽略，两个担心都不成立。**

## 二、两种方案的本质区别

| 维度 | 实例层修正（当前 B） | 定义层修正（你的思路） |
|---|---|---|
| 施加位置 | `Actor` 的当前值 | `TESNPC` 的 base 值 |
| 单位粒度 | 每个 actor 实例 | 每个 `NPC_` 定义（可被数百引用共享） |
| 状态追踪 | 每个单位记录增量 + 写 co-save | **完全不需要** |
| 生效时机 | 每次进入场景逐个处理 | 读档时一次全量 |
| 稳态开销 | 近零，但事件回调常驻 | **零——处理完即停止** |
| 残血 / 回血风险 | 机制上已根治 | **物理上不可能**（根本不碰当前值） |
| 存档污染 | 有（走 `kPermanent` 桶时） | 无（只要不调 `AddChange`） |
| 引用级过滤 | ✅ 支持 | ❌ 不适用 |

**核心洞察**：你的方案真正省掉的不是"脚本"，而是**一整套状态管理**。
因为 base 是共享的定义、且不落盘，"撤销"和"回滚"这两个概念自然消失了。这是它最漂亮的地方。

## 三、定义层方案的决定性障碍：过滤能力降级

原 MOD（含友军版）的过滤条件里，这几条是**引用级**的：

```
IsHostileToActor(PlayerRef) == 1      ; 这个 NPC 是否敌视玩家
GetShouldHelp(PlayerRef)    == 0      ; 这个 NPC 是否会帮助玩家
```

`NPC_` 是**定义**，一个定义可能被几百个引用共享——"敌视玩家"无法定义在 base 上。
所以纯定义层方案会**失去"友军 / 敌人"的分辨能力**，只能退回到种族 / 关键字 / 职业 / 等级这类
**定义级特征**。

而这恰好是原 MOD 花力气改 ESP（把 34 条条件删到 23 条）、也是你当前正在使用的能力。

> **更正（2026-10-15 追加）**：这堵墙其实**只覆盖「友军 / 敌人」这一格**。Boss 判定是个例外——
> 它本来就有定义级做法（EDID ／ AI 数据），而且比原版的引用级做法覆盖更广。详见**第八节**。
> 也就是说：方案 D 真正丢掉的只有"友军/敌人"这一类判断，而原版「含友军」版本恰恰已经把这类
> 判断删掉了 —— 所以方案 D 与当前实际行为是吻合的。

> 附带确认：`TESNPC` 的 `GetActorValue` / `SetActorValue` 是 `ActorValueOwner` 的虚函数重写，
> base 层**可读可写**，这条路技术上完全通。

## 四、推荐路线：D+ 混合（定义层为主，实例层兜底）

**主体 —— 覆盖约 99% 的单位**

```
读档完成（此时已知玩家等级）
  → 遍历 GetFormArray(FormType::NPC) 的 35,905 条记录
  → 过滤（等级 / 种族 / 关键字 / 职业 / Boss）
  → 写 base AV
之后进入任何场景，新加载的 actor 天然带着强化。
```

**兜底 —— 覆盖剩余的单位**

```
读档瞬间已经在场景里的 actor（通常几十个）
  → 只对这一小批做一次性实例层调整
```

**这个组合的关键收益**：不需要 per-actor 追踪、不需要队列摊平、不需要 co-save。
状态管理几乎完全消失——因为"可回滚"这件事不再需要了。

**保留 B 模式的两个理由**：

1. 想要"玩家每升一级，敌人立刻跟着变强"的**实时跟随** → 需要 B
2. 担心改写共享 base 波及其他 mod（同一个 base 被任务 NPC 复用）→ 需要 B

建议做成可切换：`iEnhanceLayer = Base / Instance / Both`。

## 五、已确认可用的 API

| 用途 | API | 出处 |
|---|---|---|
| 遍历所有 NPC_ | `TESDataHandler::GetSingleton()->GetFormArray(FormType::NPC)` → `BSTArray<TESForm*>&` | `RE/T/TESDataHandler.h` |
| 写 base AV | `TESActorBase::SetBaseActorValue(ActorValue, float)` | `RE/T/TESActorBase.h` |
| 读 base AV | `TESActorBase::GetBaseActorValue(ActorValue)` | 同上 |
| 写当前值 | `ActorValueOwner::ModActorValue(ActorValue, float)`（**2 参数**，改永久修正；3 参数的带桶版本此版本不存在） | `RE/A/ActorValueOwner.h` |
| 控制落盘 | `TESActorBase::AddChange(uint32_t)` / `RemoveChange(uint32_t)` | `RE/T/TESActorBase.h` |
| 判断等级化 | `TESActorBase::IsLeveled()` | 同上 |
| 判断模板 | `TESActorBaseData::UsesTemplate()` | `RE/T/TESActorBaseData.h` |

`TESNPC` 自身的属性存放位置：`playerSkills`（DNAM 子记录，`health` / `magicka` / `stamina` 各为 `uint16`），
偏移 `0x190`；另有 `TESActorBaseData::actorData` 里的 `healthOffset` / `magickaOffset` / `staminaOffset`（`int16` 增量）。

## 六、待实机验证（写代码前无法离线确定）

1. **`SetBaseActorValue()` 是否内部自动 `AddChange`（导致落盘）**。
   签名里 `AddChange` / `RemoveChange` 是显式接口，理论上不会；若会，改完调一次 `RemoveChange` 即可。
2. **改 base 后，已经实例化的 actor 如何触发重算**。这是 D+ 兜底环节的核心未知数。
3. **模板继承（`kUsesTemplate`）的 NPC**，改自己的 base 是否被 TPLT 模板覆盖。
4. **等级化 NPC（`kPCLevelMult`）**，DNAM 中的数值是否被等级计算覆盖。

## 七、同类实现参考

- **`Real Time NPC Stat Scaler`**（Nexus 184693）——"static stats are applied once to NPCs, never again"，
  依赖正是 CommonLibSSE-NG + SKSE Menu Framework。思路与本方案 D 高度重合，可作为实机行为对照。
- **`Actor Impact Framework`**（CommonLibSSE-NG，JSON 驱动）——纯 DLL 改 actor value 且无需 ESP 的现成范例。

## 八、补充：Boss 判定可以在定义层做（2026-10-15 追加）

第三节指出"纯定义层会失去引用级过滤"，但那堵墙**只覆盖「友军 / 敌人」这一格**。
**Boss 判定不受影响** —— 它本来就有定义级做法，而且比原版的引用级做法覆盖更广。

原版用的是 `HasRefType(LocationRefType "Boss")`：**引用级**，必须在 CK 里逐个手工标记，
所以覆盖面很窄（Nexus 上「Boss 没被识别」的反馈多半来自这里）。
常见的另一种做法（TrueHUD Boss Scanner 那类工具）是**双轨制**：

| 轨道 | 判据 | 层级 |
|---|---|---|
| A | EditorID 含 "Boss" | 定义级（`NPC_` 的 EDID） |
| B | `Aggression >= 2` 且 `Confidence == 4` | 定义级（`NPC_` 的 AIDT） |

**注意：EditorID 不是 FormID。** FormID 是加载时分配的数字，没有任何语义；
EDID 是 CK 里的记录名（如 `dunHalldirsBoss`）。把两者的区别搞清楚很重要 ——
FormID 里是不可能"带 BOSS"的。

### 实测（`tools/boss_scan.py`，本机整合包 35,904 个 `NPC_`）

| 判据 | 命中数 | 占比 |
|---|---|---|
| EditorID 含 "Boss" | 3,508 | 9.8% |
| AI 数据（Aggression≥2 且 Confidence=4） | 15,097 | **42%** |
| 两轨重叠 | 2,366 | — |
| 前缀过滤后保留 | 13,686 | 38% |

**AI 数据这条轨对属性加成太宽泛**：大量 mod 给所有战斗单位都设了
`VeryAggressive + Foolhardy`，连普通尸鬼（`zzzCrbEncDraugrBlades1H02`）都会被算成 Boss。
对 TrueHUD 那种「只决定显不显示血条」的用途无所谓，但 38% 的敌人都吃 Boss 倍率，
Boss 设定就等于不存在。

**所以插件把它拆成三级**（`bBossUseLocRefType` / `bBossUseEditorID` / `bBossUseAIData`），
默认只开前两级。这三条判据都是（或可以是）**定义级**的，因此方案 D 不受影响。

### 唯一的新增限制

**Skyrim 运行时默认不保留 EditorID**（这正是 `Native EditorID Fix` 这类插件存在的理由）。
所以 EDID 那一级需要三选一：

1. 装一个 Native EditorID Fix（推荐），或
2. 关掉 EDID 级，只用 LocationRefType，或
3. 改用 AI 数据级（但见上面那个 42% 的问题）

插件启动时会自动探测并在日志里报告可用性，菜单「关于」页也会显示。

## 九、实证参照：Dynamic NPC Scaling 拆解（2026-10-15 追加）

用户提供了本机已安装的 **Dynamic NPC Scaling**（Nexus 180306，MichelKI，v1.1）作为示范
—— 它就是一个**用定义层改写跑通的现成实现**，正好可以拿来验证方案 D 的可行性边界。

位置：`mods/Dynamic NPC Scaling/SKSE/Plugins/DynamicNPCScaling.dll`（907 KB）+ 同名 `.ini`。
反解方式：`.ini` 全量读取 + DLL 字符串表（函数签名与日志文案）+ 直接解析 `Skyrim.esm` 核对。

### 9.1 它的架构（从 DLL 函数签名反推）

```
MessageHandler(SKSE::MessagingInterface::Message*)
  └─ PatchNPCs()                        ← 全量改写入口
       ├─ ShouldSkip(TESNPC*, bool, const ScalingRule*)
       ├─ IsExcluded(TESNPC*, bool)
       ├─ ApplyScaling(TESNPC*, const ScalingRule*, bool)
       └─ ApplySmartScaling()           ← 按「进阶档位」在组内插值
```

关键点：**所有函数的参数都是 `RE::TESNPC*`，没有任何 `Actor` / `TESObjectREFR`。**
它完全不碰实例层 —— 没有事件钩子、没有队列、没有 co-save、没有 per-actor 状态。
这就是方案 D 的形态，作者自己也是这么描述的：
*"works at runtime through SKSE and does not permanently modify NPC records"*。

它的日志文案也印证了工作模式：`Processing {} NPCs...` → `Finished patching NPCs. Total patched: {}`。

### 9.2 它证明了：定义层过滤够用

`[SkipFlags]` 一共 10 条（非等级化 / 等级化各一套），全部是**定义级**判据：

| 它的开关 | 判据落在哪里 | 层级 |
|---|---|---|
| `SkipUnique` / `SkipEssential` / `SkipProtected` / `SkipInvulnerable` | `ACBS` 标志位 | 定义级 |
| `SkipSummonable` / `SkipTemplatesAndPresets` | `ACBS` 标志位 | 定义级 |
| `SkipFollowers` | `CurrentFollowerFaction` / `PotentialFollowerFaction` | **定义级（派系）** |
| `SkipCivilians` / `SkipUnaggressiveAnimals` | `ACBS` + Race | 定义级 |

再加上规则匹配用的 `EditorID` / `EditorIDContains` / `Race` / `Faction` / `Keyword` —— **全是定义级**。

> **这就回答了第三节留下的疑问**：纯定义层并不会"退化到没法用"。
> 「随从」这类身份原本以为必须靠运行时的 `IsPlayerTeammate()`，实际上原版给了
> `PotentialFollowerFaction` 这条**写在 NPC_ 记录里**的派系，定义层完全够用，而且
> 比运行时判定更彻底（连"将来可能被招募"的也算上）。
>
> 真正只有引用级才能表达的，只剩 `IsHostileToActor` / `GetShouldHelp` 这一对
> —— 而原版「含友军」版本已经把这两条删掉了。**所以方案 D 与本项目的实际需求是吻合的。**

### 9.3 「玩家是怎么被排除的」——它靠的是巧合，我们靠的是显式判定

这是本次拆解最有价值的一条发现。

`Dynamic NPC Scaling` 里**没有任何**针对玩家的代码：DLL 字符串表里搜不到 `Player`，
函数签名里也没有玩家参数。它能做到"只对 NPC 生效"，是因为：

```
玩家本体那条记录： NPC_ 0x00000007 "Player"
  ACBS = 0x00000030  =  AutoCalcStats(0x10) | Unique(0x20)
                          ↑ 就是它撞上了 SkipUnique
```

用 `tools/player_probe.py` 扫了本机**整个加载顺序**（MO2 mods + Data 共 900+ 插件），
凡是覆盖 `0x00000007` 的插件（`Skyrim.esm` / `ClassicClassesReimagined.esm` / `ESX1.esp` /
`LSReborn.esp` / `SOTFull.esp` / `ccbgssse018-shadowrend.esl`）**无一例外都是 `ACBS=0x30`**。
也就是说：玩家记录永远带 `Unique`，这条排除路径是稳定的。

**但它是耦合的。** 一旦用户把 `SkipUnique` 改成 `false`（想让独特 NPC 也吃加成），
玩家那条记录就会被一起改写。对一个改 `PCLevelMult` / `SetAutoCalcStats` 的插件来说，
这是实打实的隐患（玩家每级生命成长会受影响）。

> **所以本插件不走这条路。** `Manager::IsPlayerActor()` 是**独立且无条件**的判定：
>
> ```
> ① 引用级：a_actor == PlayerCharacter::GetSingleton()
> ② 定义级：a_actor->GetActorBase() == 玩家 ActorBase（NPC_ 0x00000007）
> ```
>
> 判② 是专门为方案 D 准备的 —— 那条路直接遍历 `GetFormArray(FormType::NPC)`，
> 玩家的记录就在数组里，必须能把它认出来。两条判据都不依赖任何过滤器开关。

### 9.4 它自己承认的两个限制（对方案 D 很重要）

作者的 Install/Uninstall 一节写得很直白：

1. *"Its recommended to start a new game as **already spawned actors will not be scaled**."*
2. *"If uninstalled, **already spawned actors will keep their scaled stats**."*

第 1 条**印证了第六节的待验证项 2**：改 base 不会自动回写到已经实例化的 actor。
作者干脆放弃了这块，直接建议开新档。

但实际影响没有"开新档"这么重 —— 关键在于 **actor 的生命周期**：

- 游戏**当前已加载**的单位（玩家周围几十个）确实保持旧值，要等它们卸载后才更新
- **非持久化** actor 在单元格卸载时被销毁，**再次进入该单元格时会从 base 重新实例化** → 自动拿到新值
- 所以实际上**跨一次区域切换（或重读存档）之后，绝大多数单位就已经是新值了**

第 2 条则说明：`NPC_` 记录本身不落盘（那是插件文件的事），落盘的是**已生成 actor 的数值**。
对我们来说这条反而有用 —— 它正是第四节「D+ 兜底」需要处理的那一小批单位。

### 9.5 落到本项目的结论

| 判断 | 结论 |
|---|---|
| 方案 D 能不能跑通 | **能**，且有 2,500+ 下载量的现成实现 |
| 定义层过滤够不够 | **够**，连「随从」都有定义级判据；只丢 `IsHostileToActor` / `GetShouldHelp` |
| 玩家排除 | 方案 D **必须显式处理**，不能像它那样靠 `Unique` 顺带挡掉 |
| 「已生成 actor 不回写」 | 真实存在，但只需兜底「读档瞬间已加载的那几十个」 |
| 已落地的改动 | 见下 |

**本次已把它的两个可直接移植的优点落进代码**（不改变默认行为）：

1. **定义层过滤器**（`bSkipUnique` / `bSkipEssential` / `bSkipProtected` /
   `bSkipInvulnerable` / `bSkipSummonable` / `bSkipTemplated`）
   —— 见 `Manager::PassBaseFilters()`，全部默认关闭
2. **随从的定义层派系判定**（`bFollowerCheckByFaction`，默认开）
   —— 用 `PotentialFollowerFaction`(0x0005C84D) / `CurrentFollowerFaction`(0x0005C84E)
   替代/补强运行时的 `IsPlayerTeammate()`

**另外顺手修正的两处一手资料错误**（都是实解时踩出来的）：

- `NPC_` 的**派系成员**写在 `SNAM` 子记录里，**不是** `FACT`（`FACT` 是派系**记录类型**）
- `ACBS` 的 `PCLevelMult` 位是 **`0x80`**，不是网上流传的 `0x40`（`0x40` 是 `kDoesntAffectStealthMeter`）
