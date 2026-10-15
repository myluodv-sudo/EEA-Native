# EEA_Native 机制全书

> 本文介绍 EEA_Native 的**全部工作机制**：它怎么工作、怎么分辨敌我、怎么认 Boss、怎么算数值、
> 怎么落地到角色身上、怎么回滚、怎么存盘。
>
> 与另外两份文档的分工：
> - `README.md` —— 怎么装、怎么配、怎么排错（操作性文档）
> - `DESIGN-options.md` —— 为什么选这条路（决策记录，含「定义层改写」的可行性论证）
> - **`MECHANICS.md`（本文）** —— 它内部到底是怎么运转的（原理文档）
>
> 本文所有结论都对应 `src/` 里的实际代码，不是设计意图。

---

## 0. 一句话概括

把原作者 Mokeine 的三个 Papyrus 脚本（`EnhanceEnemyAV` / `EnhanceEnemyBoss` / `UpdateEnemyAV`）
**整体搬到 SKSE 插件里**，数值公式一个字不改，但把「按效果重放驱动」改成「按事件驱动 + 每帧预算」。

换载体的直接结果：三个已知 BUG（敌人回满血、Boss 体型永久变大、进地牢掉帧）不是被"绕过"了，
而是**产生它们的机制本身不存在了**。

---

## 1. 三个部件，各管什么

| 部件 | 文件 | 职责 | 能不能不要 |
|---|---|---|---|
| **DLL 插件** | `EEA_Native.dll` | 全部逻辑：判定、计算、施加、回滚、持久化、菜单 | 不能 |
| **精简 ESP** | `EnhanceEnemyAttributes.esp`（365 字节） | 只提供法术强度用的那条 Perk | **能** —— `sMode=PowerMod` 时可完全删除 |
| **INI** | `EEA_Native.ini` | 全部可调参数 | 不能（缺失时自动生成模板） |

原 ESP 是 13,211 字节 / 69 条记录（GLOB + MGEF + SPEL + PERK + FLST + MESG 一堆脚手架），
现在只剩 1 条 Perk 记录。**原 MOD 的任何文件都没有被改动**，新工程完全独立在 `native/` 里。

---

## 2. 工作原理：运行时数据流

### 2.1 启动阶段

```
SKSEPluginLoad
  ├─ SetupLog()              日志写到 Data/SKSE/Plugins/EEA_Native.log
  ├─ Config::LoadFromDisk()  读 INI，缺失就生成带注释的模板
  ├─ Menu::Register()        注册 SKSE Menu Framework 页（框架不在就静默跳过）
  ├─ SetUniqueID('EEAN')     注册 co-save：Save / Load / Revert 三个回调
  ├─ msg->RegisterListener   kDataLoaded / kPreLoadGame / kPostLoadGame / kNewGame
  └─ AddEventSink × 2        TESObjectLoadedEvent + TESCombatEvent

kDataLoaded（所有插件加载完，此时才能安全查表单）
  ├─ LoadPerkAndKeywords()   一次性解析所有需要的表单引用
  └─ QueueAllLoaded()        把当前已加载单位全部入队
```

**刻意不 hook 主循环。** 只用官方事件源，不碰 `REL::ID` —— 这意味着不会有
「游戏一升级插件就崩」的版本漂移问题，只依赖 Address Library。

### 2.2 运行阶段：两条触发路径

**路径 A —— 对象加载**（`TESObjectLoadedEvent`，只取 `loaded == true`）

```
OnObjectLoaded(formID)
  ├─ 检查玩家等级是否变化 → 变了就 RefreshAll()（玩家升级后所有单位该重算）
  ├─ if triggerMode == 1（Combat 模式）→ 直接 return，不入队
  └─ LookupByID → As<Actor>() → Queue(actor)
```

**路径 B —— 进入战斗**（`TESCombatEvent`，只取 `newState == kCombat`）

```
Queue(actor, /*a_force=*/true)
```

两条路径最终都汇进同一个队列。

### 2.3 队列：跨帧摊平

```
queue_    std::deque<FormID>        待处理顺序表
queued_   std::unordered_set<FormID> 去重（避免同一个单位被反复入队）
forced_   std::unordered_set<FormID> 「这次是战斗驱动」的标记
```

`ScheduleTick()` 通过 SKSE 的 `TaskInterface` 把工作挂到**下一帧**，一次只处理
`iProcessPerTick`（默认 6）个，没处理完就再挂一帧：

```cpp
SKSE::GetTaskInterface()->AddTask([this] {
    ProcessSome();
    if (!queue_.empty()) ScheduleTick();   // 还有剩 → 下一帧继续
});
```

这就是「进地牢不再瞬间卡顿」的机制：原版是 6 段 `Utility.Wait(1.0)` 串行跑完几十个单位，
新版把同样的工作摊到很多帧上，每帧只花一点点。

### 2.4 存档阶段

| 消息 | 动作 |
|---|---|
| `kPreLoadGame` | 清空 `applied_` / `pendingRevert_` / 队列（旧存档的内存记录作废） |
| `kPostLoadGame` | 重新解析表单 + `LoadPerkAndKeywords()` + `QueueAllLoaded()` |
| `kNewGame` | 清空 + 重新解析表单 |

co-save 走 `SerializeSave` / `SerializeLoad` / `Revert` 三个回调，细节见第 6 节。

---

## 3. 数值模型：加多少

### 3.1 五个属性落在哪个 ActorValue 上

| 属性 | 原版 MCM 名 | ActorValue | 枚举 |
|---|---|---|---|
| HP | `health` | `kHealth` | 24 |
| MP | `magicka` | `kMagicka` | 25 |
| SP | `stamina` | `kStamina` | 26 |
| ATK | `AttackDamageMult` | `kAttackDamageMult` | 154 |
| MGC | `LockpickingPowerMod` | `kLockpickingPowerModifier` | 143 |

顺序与原版 FormList `EEA_ActorValueNameList` 一致 —— 这是为了让 INI 能直接沿用。

### 3.2 公式（与原版脚本逐字一致）

```cpp
float Manager::ComputeExtra(float a_baseAV, const AttrSettings& a_cfg,
                            int a_playerLevel, int a_enemyLevel, bool a_boss)
{
    const int   pc   = a_cfg.usePlayerLevel ? a_playerLevel : 0;
    const int   el   = a_cfg.useEnemyLevel  ? a_enemyLevel  : 0;
    const float pct  = static_cast<float>(a_cfg.basePercent + pc + el) * 0.01F;
    const float mult = a_cfg.extraMult * (a_boss ? a_cfg.bossMult : 1.0F);
    return std::ceil(a_baseAV * pct * mult);
}
```

写成公式：

```
extra = ceil( baseAV × (basePercent + PC + EL) × 0.01 × extraMult × bossMult )
```

- `PC` = `usePlayerLevel ? 玩家等级 : 0`
- `EL` = `useEnemyLevel ? 敌人等级 : 0`
- `bossMult` 只在被判定为 Boss 时参与，否则为 1

**算个例子。** 基础生命 100，`iExtrapctHP=20`，玩家 30 级，敌人 20 级，`fExtraMultHP=1.0`：

```
pct   = (20 + 30 + 20) × 0.01 = 0.70
extra = ceil(100 × 0.70 × 1.0) = 70        → 生命上限 100 → 170
```

若这个单位被判定为 Boss 且 `fBossMultHP=2.0`：

```
extra = ceil(100 × 0.70 × 2.0) = 140       → 上限 100 → 240
```

### 3.3 关键设计：为什么读 `GetBaseActorValue` 而不是 `GetActorValue`

`GetActorValue(kHealth)` 返回的是**当前血量**。如果拿它当计算基准：

- 满血敌人（100/100）加成 70 → 上限 170
- 同一个敌人被打到 30 血后再算一次 → 基准变 30 → 加成 21 → 上限 121

数值会随战斗状态漂移，而且**每次重算都变**。所以基准必须取
`GetBaseActorValue()`（NPC_ 定义上的固定值），结果才稳定可复现。

---

## 4. 落地机制：`SetAVTotal()` —— 全工程最核心的函数

输入是「我们施加在这个 AV 上的总量，要从 `a_old` 变成 `a_new`」。整个函数分四步。

### Step 1 —— 幂等短路

```cpp
const float net = a_new - a_old;
if (std::abs(net) < kEps) return a_old;   // 数值没变 → 完全不动引擎
```

原版每次刷新都**无条件** `ModAV` + `SetScale`。这是掉帧和数值漂移的总根源。
新版只要净差为 0 就直接返回，所以**稳态开销 ≈ 一次 map 查找**。

### Step 2 —— 记下当前值百分比（仅 HP / MP / SP）

```cpp
const float maxBefore = a_actor->GetPermanentActorValue(a_av);   // 上限侧
float ratio = 1.0F;
if (vital && s.preserveCurrentRatio && maxBefore > kEps) {
    ratio = std::clamp(a_actor->GetActorValue(a_av) / maxBefore, 0.0F, 1.0F);
}
```

这里的重点是**两个函数职责必须分清**：

| 调用 | 拿到什么 |
|---|---|
| `GetPermanentActorValue(av)` | **上限**（这个 AV 的永久值） |
| `GetActorValue(av)` | **当前值**（对生命就是当前血量） |

### Step 3 —— 按净差写入，并读回引擎实际接受量

```cpp
a_actor->ModActorValue(a_av, net);

const float maxAfter = a_actor->GetPermanentActorValue(a_av);
const float actual   = maxAfter - maxBefore;     // 引擎实际接受量
```

**为什么是 2 参数的 `ModActorValue`**（这一条是查过引擎接口后才定下来的，不是随手写的）：

CommonLibSSE-NG 3.7.0 的 `ActorValueOwner` 只暴露两个 AV **写**接口 ——

| 接口 | 作用 |
|---|---|
| `ModActorValue(ActorValue, float)` | 改**永久修正**（= Papyrus 的 `ModAV`，"permanently alters the actor value"） |
| `RestoreActorValue(ACTOR_VALUE_MODIFIER, ActorValue, float)` | 改**指定桶**（`kPermanent` / `kTemporary` / `kDamage`） |

**没有**「带桶的 `ModActorValue`」这种三参数形式。所以属性上限只能写在永久修正上，
`bUseTemporaryModifier` 这一项在当前版本上无法生效（保留键仅为 INI 兼容，打开会打一条日志）。
好在 2 参数的 `ModActorValue` 恰好就是原作者 Papyrus 脚本用的那个 `modav`，行为对齐。

两个要点：

1. **不做「先全撤再加回」**，只按净差写一次 —— 所以改设置时不会出现血量的中间跳变。
2. **记录 `actual` 而不是 `net`** —— 引擎可能封顶或部分接受，只有记实际值才能精确回滚。

> ⚠ 这里有个实机踩过的坑：如果改用 `GetActorValue` 前后相减来测增量，
> 对生命而言读到的是当前血量，算出的增量是错的 → **下一帧重复叠加** → 数值无限膨胀。
> 必须用 `GetPermanentActorValue`。

### Step 4 —— 把当前值按原比例放回去

```cpp
if (vital && s.preserveCurrentRatio && maxAfter > kEps) {
    const float desired = maxAfter * ratio;
    const float cur0    = a_actor->GetActorValue(a_av);
    const float diff    = desired - cur0;
    if (std::abs(diff) > kEps) {
        // kTemporary 桶：只影响当前值，不改 GetPermanentActorValue，
        // 也不像 kDamage 桶在 0 处夹取，正反两个方向都能走
        a_actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av, diff);

        // 回读校验：若引擎的参数方向与预期相反（回读离目标更远），补一次反向偏移。
        // 这样不依赖对参数符号的猜测 —— 引擎自己会给出答案。
        const float cur1 = a_actor->GetActorValue(a_av);
        if (std::abs(desired - cur1) > std::abs(diff) + kEps) {
            a_actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av, -2.0F * diff);
        }
    }
}
```

**这一步就是「回血 BUG」被根治的地方**，展开见下一节。

> 残留问题说明：这一步会在 `kTemporary` 桶留下偏移量。回滚时 `RevertWith` 会再走一遍
> `SetAVTotal`（目标值 0），`ratio` 是**当次重新计算**的，所以这份偏移会被自然吸收掉，
> 不需要额外存字段。

---

## 5. 回血 BUG 的根治（机制级对比）

### 原版是怎么回血的

`source/EnhanceEnemyAV.psc` 在 `OnEffectFinish`（L159）和 `OnUpdate`（L99）里**无条件**执行：

```papyrus
RestoreAV(AVstring, GetMaxAV())              ; 把当前值直接拉满
modav(AVstring, GetBaseAV() - GetAVMax())    ; 再调上限
```

而触发它的法术，条件链里**没有任何战斗相关函数**（没有 `IsInCombat`、没有 `GetCombatState`）。
真正的触发链条是：

```
脱战 → 跑开 → 区域卸载 → 区域重新加载 → 效果重放 → OnEffectFinish / OnUpdate 无脑 RestoreAV(max)
```

所以「进门敌人回满血」「Sigdis 传送回血」「改设置敌人满血」是**同一个根因的不同表现**。
它和「脱战」不是因果关系，只是恰好同源。

### 新版的行为

新版**根本没有 `RestoreAV(av, max)` 这种调用**。改上限时按比例还原当前值：

| 情况 | `ratio` | 结果 |
|---|---|---|
| 满血敌人首次强化 | 1.0 | 还是满血（看起来"变强了"） |
| 半血敌人被强化 | 0.5 | 强化后仍是 50%（**绝不回血**） |
| 战斗中改设置 | 不变 | 血量百分比纹丝不动 |
| 回滚（`RevertWith`） | — | 只减去 `delta` 里记录的**自己实际加进去的量** |

`RevertWith` 是这样的 —— 注意它没有一处碰"上限还原"：

```cpp
for (std::size_t i = 0; i < kAttrCount; ++i)
    if (std::abs(a_entry.delta[i]) > kEps)
        SetAVTotal(a_actor, ToActorValue(...), a_entry.delta[i], 0.0F);
```

---

## 6. 回滚与持久化：为什么读档不会把残血敌人扣死

### 6.1 `Applied` —— 我们到底记了什么

```cpp
struct Applied {
    float delta[kAttrCount];   // HP/MP/SP/ATK/MGC 各自「我们实际加进去的量」
    float schoolDelta[5];      // 五大法术学派 PowerModifier 的实加量（PowerMod 模式用）
    bool  perkAdded;           // 是否加过那条 Perk
    bool  scaleSaved;
    float origScale;           // 原始体型（Boss 骨骼 BUG 的修复点）
    float curScale;
};
```

关键点：记录的是**增量**，不是「绝对值」也不是「是否是 Boss」。
所以回滚 = 把增量减掉，天然不会误伤别的 MOD 或引擎自己加上去的数值。

### 6.2 co-save

```
记录类型  'EEAN'
版本      1
内容      u32 count，然后 count 组 (FormID, Applied)
```

`Applied` 是纯 POD，可以直接 `WriteRecordData(entry)` 整块写。

### 6.3 读档时的关键决策：**认领**，不是**撤销**

```cpp
if (auto it = pendingRevert_.find(fid); it != pendingRevert_.end()) {
    applied_[fid] = it->second;      // 认领：数值不动
    pendingRevert_.erase(it);
}
```

为什么必须这样：

- 属性上限写在 `kPermanent` 桶（= 原版 Papyrus 的 `ModAV`），数值**已经写在角色身上、随存档保存了**。
- 如果读档时直接撤销，会把**已经被打到残血**的单位再扣一次 → 直接暴毙。
- 这正是原版「残血敌人读档后被拉满」的**镜像问题** —— 两边都得避开。

> 早期设计里这里还有一个 `bUseTemporaryModifier` 分支（临时桶不存盘 → 只能撤掉重算）。
> 该选项在当前引擎接口下已失效（见 Step 3 的说明），所以这个分支也一并去掉了。

### 6.4 `pendingRevert_` 为什么要延迟

`SerializeLoad` 执行时目标 actor 可能还没加载（`LookupByID` 拿不到）。
所以读回的记录先存进 `pendingRevert_`，等那个单位进队列时（`ApplyTo` 开头）再处理。

### 6.5 modifier 桶：为什么只有一个

引擎里有三个桶（`ACTOR_VALUE_MODIFIERS`：`kPermanent=0` / `kTemporary=1` / `kDamage=2`），
但**上限只能写在 `kPermanent` 上**：

| | `kPermanent` | `kTemporary` | `kDamage` |
|---|---|---|---|
| 用什么接口写 | `ModActorValue(av, x)` | `RestoreActorValue(kTemporary, av, x)` | `RestoreActorValue(kDamage, av, x)` |
| 与原版 `ModAV` | 同一个桶 | 不同桶 | 不同桶 |
| 是否反映在 `GetPermanentActorValue` | **是** | 否 | 否 |
| 本插件用途 | **属性上限增量** | 只用来把当前值拉回目标比例 | 未使用 |

`kTemporary` 桶在本插件里只承担一件事 —— **保持当前血量百分比**（Step 4）。
它不参与上限计算，所以「回读 `GetPermanentActorValue` 测实际接受量」这条自校验链是干净的。

- 随存档保存：`kPermanent` 是，`kTemporary` 否 —— 但 `kTemporary` 只影响当前值，
  回滚时会被重新计算的比例自然吸收（见 Step 4 的残留说明）。
- 卸载插件前：**请先在菜单点「还原所有单位」**（上限增量随存档保存）。

---

## 7. 敌我识别

这是整个 MOD 最容易被误解的部分。**"敌我"其实被拆成了四件独立的事**：
玩家、友军/敌人、随从、召唤物，各用各的判据。

### 7.1 玩家识别 —— `IsPlayerActor()`

```cpp
bool Manager::IsPlayerActor(RE::Actor* a_actor) const
{
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc) return false;
    if (a_actor == pc) return true;                       // ① 引用级
    auto* base = a_actor->GetActorBase();
    return base && base == pc->GetActorBase();            // ② 定义级
}
```

两条判据，**独立于所有过滤器开关**。

**为什么不用「ActorBase 带 Unique 标志」来认玩家**（很多 MOD 是这么做的）：

- 实测 `Unique` 在本机整合包里命中 **19.4%**（6,974 / 35,905 条 NPC_），完全不是玩家专属
- 它由 `bSkipUnique` 控制 —— 用户一关这个开关想让独特 NPC 也吃加成，玩家就被漏进来了
- 现成反例：`Dynamic NPC Scaling`（Nexus 180306）就是靠 `SkipUnique` **顺带**挡掉玩家的，
  它没有一行"排除玩家"的代码。对整个加载顺序做覆盖扫描可以验证：所有覆盖玩家记录
  `NPC_ 0x00000007 "Player"` 的插件（`Skyrim.esm` / `ClassicClassesReimagined.esm` / `ESX1.esp` /
  `LSReborn.esp` / `SOTFull.esp` / `ccbggssse018-shadowrend.esl`）ACBS 恒为 `0x30`，
  所以那条路现在能用 —— 但它是**耦合**的。

判据 ② 是专门为「定义层改写路线」（方案 D）准备的：那条路直接遍历 `NPC_` 数组，
玩家记录 `0x00000007` 就在数组里，必须能认出它。

**判定顺序很讲究**（`IsEligible` 里）：

```
master → 存活/未禁用/是NPC → 等级 >= minLevel → 【玩家判定】 → 定义层过滤器 → 龙 → 随从/召唤
                                                  ↑
                                    排在过滤器之前，但排在 minLevel 之后
```

- 排在**过滤器之前**：过滤器开关不该影响「谁是玩家」这件事
- 排在 **minLevel 之后**：与旧版行为保持一致，不悄悄改变已有表现
- 结果返回 `s.affectPlayer`（默认 `false`）

### 7.2 友军 / 敌人识别

原版的做法（`IsHostileToActor` / `GetShouldHelp`）：

- **「不含友军」版**：每个属性块里都有 `IsHostileToActor(Player) == 1` 且 `GetShouldHelp(Player) == 0`
  —— 意思是「是敌人、而且不会帮玩家」
- **「含友军」版**：把这两条从五个属性块里**删掉了**

本插件用两个开关还原这个行为：

```ini
bExcludeFollowers=0   ; 改成 1 = 「不含友军」版行为
bExcludeSummons=0
```

> **这是定义层方案唯一的能力降级点。** `IsHostileToActor` / `GetShouldHelp` 是**引用级**判据
> （判断的是"这一份实例对玩家什么态度"），无法在 `NPC_` 定义上表达。
> 但「含友军」版已经把这两条删了，所以本项目不受影响 —— 这也是方案 D 与本项目需求吻合的原因。

### 7.3 随从识别 —— 双层判定

```ini
bFollowerCheckByFaction=1   ; 默认开
```

| 层 | 判据 | 覆盖范围 |
|---|---|---|
| 运行时 | `a_actor->IsPlayerTeammate()` | 只认「**此刻已被招募**」的随从 |
| 定义层 | `base->IsInFaction(派系)` | 连「**将来可能被招募**」的也算上 |

定义层用到的两条原版派系（已实解 `Skyrim.esm` 确认）：

```
PotentialFollowerFaction  0x0005C84D   43 条 NPC_ 带它
CurrentFollowerFaction    0x0005C84E   40 条
```

用**硬编码 FormID** 而不是 EDID 查询 —— 因为运行时 EditorID 未必可读（见 8.3）。

> 顺带修正一处一手资料的错误：**`NPC_` 的派系写在 `SNAM` 子记录里，不是 `FACT`**
> （`FACT` 是派系*记录类型*）。按 `FACT` 扫全加载顺序是 0 命中，改成 `SNAM` 立刻出结果。

### 7.4 召唤物识别

`a_actor->IsSummoned()` —— 纯运行时判据，由 `bExcludeSummons` 控制。

### 7.5 龙类识别 —— 双判据

```cpp
bool IsDragon(a_actor)        // HasKeyword(ActorTypeDragon)，Actor 上或 Race 上都查
bool IsDragonClass(a_actor)   // base->GetClass() == EncClassDragon
```

两个 FormID（已实解确认）：

```
0x00035D59  KYWD  ActorTypeDragon   —— 关键字挂在 Race 上
0x0002F201  CLAS  EncClassDragon    —— 职业
```

第二条 `GetIsClass(EncClassDragon)` 是为了排除「**自定义龙形 Boss**」——
作者置顶帖提到的 Vigilant「Julia the Crimson」就是靠这条挡住的。

`bExcludeDragons=1`（默认）保持原版行为：**龙不吃属性加成**。
Nexus 上「龙不受影响」的反馈就是这条造成的，不是被别的龙类 MOD 覆盖了。

### 7.6 定义层过滤器 —— 6 个 ACBS 开关

参照 `Dynamic NPC Scaling`（Nexus 180306）的 `[SkipFlags]`，但判据全部落在
**`ActorBase`（`NPC_` 记录）的 ACBS 标志位**上。

| 开关 | 位值 | 含义 | 实测占比 |
|---|---|---|---|
| `bSkipUnique` | `0x00000020` | 独特 NPC | 19.4% |
| `bSkipEssential` | `0x00000002` | 重要（倒地不死） | 6.5% |
| `bSkipProtected` | `0x00000800` | 受保护 | 2.0% |
| `bSkipInvulnerable` | `0x80000000` | 无敌 | 2.4% |
| `bSkipSummonable` | `0x00004000` | 可被召唤 | 7.7% |
| `bSkipTemplated` | `UsesTemplate()` / `IsPreset()` | 用模板或捏脸预设 | 2.0% |

（占比 = 本机整合包 35,905 条 `NPC_` 的实测分布）

**为什么「定义层」比「运行时状态」可靠：**

- 定义层判据回答的是「**这个 NPC 属于哪一类**」，招募 / 复活 / 读档 / 换地点都不会改变结论
- 运行时判据回答的是「**这一份实例现在什么状态**」，同一句话在不同时刻可能不同
- 开销上，定义层只是内存里的几次位测试，且不依赖运行时 EditorID

全部默认 `0` —— 保持原 MOD「含友军」版行为，谁都不额外排除。

> ⚠ **位值必须用 CommonLibSSE-NG 的 `RE::ACTOR_BASE_DATA::Flag` 作为权威来源。**
> UESP / xEdit 网页把 `PCLevelMult` 标成 `0x40` 是**错的**（正确是 `0x80`），
> 而 `0x40` 其实是 `kDoesntAffectStealthMeter`。照错的值写代码会**静默失效**。

### 7.7 完整判定链（`IsEligible` 短路顺序）

```
① s.master                     总开关
② 非空 && !IsDead() && !IsDisabled() && FormType==NPC && 有 ActorBase
③ GetLevel() >= minLevel
④ IsPlayerActor()  ──→ return affectPlayer     【无条件，独立于所有过滤器】
⑤ PassBaseFilters()            6 个 ACBS 开关 + 随从派系
⑥ bExcludeDragons && (IsDragon || IsDragonClass)
⑦ bExcludeFollowers && (IsPlayerTeammate || 在随从派系里)
⑧ bExcludeSummons && IsSummoned()
⑨ 至少一个属性开着，或者 Boss 体型加成 > 0
```

注意 ⑨：**一个属性都不开、但 Boss 体型加成开着**时，单位仍然会被处理 ——
因为体型是独立于五个属性的另一项加成。

---

## 8. Boss 识别

### 8.1 四级判据，精确度递减

| 级别 | 判据 | 机制 | 实测命中 | 默认 |
|---|---|---|---|---|
| **[1]** | `LocationRefType "Boss"` | 原版机制，需 CK 手工标记 | 准但覆盖面小 | **开** |
| **[2]** | EditorID 含 `Boss` | 覆盖 mod 作者命名 | 3,508 个（**9.8%**） | **开** |
| **[3]** | AI 数据 | `Aggression>=2 && Confidence>=4` | 15,097 个（**42%**） | **关** |
| **[4]** | 显式名单兜底 | 关键字 / 种族 / 具体 NPC | — | 龙祭司 |

（实测基数：本机整合包 35,904 个 `NPC_`，工具 `native/tools/boss_scan.py`）

### 8.2 [1] `LocationRefType` 机制详解

**它不是「FormID 里带 BOSS」，也不是 Actor 关键字。** 这是最容易误解的一点。

- 它是 `Skyrim.esm` 里 EDID 就叫 `Boss`、FormID `0x000130F7` 的一条
  **`LCRT`（LocationRefType，地点参照类型）**记录 —— 已实解确认
- 含义：**「某个放置参照物（ACHR）在某个地点（Location）里扮演的角色」**
- 数据结构：

```cpp
// RE::BGSLocation::specialRefs 是 BSTArray<SpecialRefData>
struct SpecialRefData {
    BGSLocationRefType* type;
    UnloadedRefData     refData;   // 内含 refID
};
```

- 查询逻辑：拿 actor 的 `FormID`，在它所在的地点**及其父级地点**（最多上溯 8 层）的
  `specialRefs` 里找配对：

```cpp
auto* loc = a_actor->GetEditorLocation1();
for (int depth = 0; loc && depth < 8; loc = loc->parentLoc, ++depth) {
    for (const auto& sr : loc->specialRefs) {
        if (sr.type == bossLocRefType_ && sr.refData.refID == refID) return true;
    }
}
```

这正好对应原版 Papyrus 的 `a_actor.HasRefType(Boss)`。
优点是它代表「**原版认为谁是 Boss**」的权威定义 —— CK 里手工标的，最准。

### 8.3 [2] EditorID 机制详解

匹配 `ActorBase`（`NPC_` / `CREA` **定义**）的 EditorID 是否包含配置的片段，**大小写不敏感**。

用 base 的 EDID 而不是引用的，因为「这个 NPC 是不是 Boss」本来就是**定义级**属性，与它被放在哪里无关。

> **前提坑（很重要）：Skyrim 运行时默认不保留 EditorID。**
> 这正是 `Native EditorID Fix` 这类插件存在的原因。所以不能假设 `GetFormEditorID()` 一定有值。

插件的处理方式是**启动时探测一次**：

```cpp
bool ProbeEditorIDAvailability()   // 抽查前 256 个 NPC_，看 GetFormEditorID() 有没有值
```

结果存进 `edidReadable_`，并显示在菜单「关于/性能」页。不可读就**整条判据跳过**
（免得对每个单位白跑一遍字符串查找），同时在日志里给出明确提示。

还要区分两个概念：

| | 是什么 | 有没有语义 |
|---|---|---|
| `EditorID` | CK 里的记录名，如 `dunHalldirsBoss` | 有，人工命名 |
| `FormID` | 加载时分配的数字，如 `0x000130F7` | **没有**语义 |

### 8.4 [3] AI 数据 —— 为什么默认关闭

```cpp
aggro >= s.bossMinAggression && conf >= s.bossMinConfidence
// ACTOR_AGGRESSION{0=Unaggressive,1=Aggressive,2=VeryAggressive,3=Frenzied}
// ACTOR_CONFIDENCE{0=Cowardly,1=Cautious,2=Average,3=Brave,4=Foolhardy}
```

用户提供的参照物 `TrueHUD Boss Scanner` 就是用这一类判据。但对**两种用途**，
同一个判据的合适程度完全不同：

| 用途 | 判宽的后果 |
|---|---|
| 决定**显不显示血条**（TrueHUD） | 多显示几个血条，无害 |
| 决定**吃不吃 Boss 倍率**（本插件） | 42% 的敌人吃上 Boss 倍率 → Boss 设定直接失效 |

所以默认关闭，需要显式打开。这就是「同一个判据不能跨用途照搬」的例子。

### 8.5 [4] 显式名单兜底

```ini
sBossKeywords=              ; vanilla 并没有 ActorTypeBoss 这类关键字
sBossRaces=DragonPriestRace ; 龙祭司
sBossNpcs=
```

> 关键字默认**必须留空**：已扫过 `Skyrim.esm` 全部 `KEYM` 记录确认，
> 原版**不存在** `ActorTypeBoss` / `Boss` / `DLC2Boss` 这类关键字。
> 写成那三个只会让日志刷「找不到关键字」。

### 8.6 判定函数的实际结构

```cpp
IsBoss(actor):
    if (bossUseLocRefType && IsBossViaLocRefType(actor)) return true;   // [1]
    if (bossUseEditorID   && IsBossViaEditorID(actor))   return true;   // [2]
    if (bossUseAIData     && IsBossViaAIData(actor))     return true;   // [3]
    // [4] 无关开关，永久生效
    for (kw   : bossKeywords_) if (actor->HasKeyword(kw))            return true;
    for (npc  : bossNpcs_)     if (npc == actor->GetActorBase())     return true;
    for (race : bossRaces_)    if (actor->GetActorBase()->GetRace() == race) return true;
    return false;
```

**只要命中任意一级就是 Boss** —— 所以 [3] 一开，[1][2] 的精确性就被它稀释了，
这就是"精确度递减"的实际含义。

---

## 9. Boss 体型机制与那个 BUG

### 原版 BUG：Boss 体型永久变大

`source/EnhanceEnemyBoss.psc` 里的 `OrinScale` 是**脚本局部变量**，默认 `1.0`。
读档时脚本实例重建 → `OrinScale` 丢回 `1.0` → 于是在**已经变大的体型**上再加一次
→ 每次读档累积一点 → 永久变大，不可逆。

### 新版怎么修

```cpp
if (wantScale) {
    if (!entry.scaleSaved) {
        entry.origScale  = a_actor->GetScale();   // 只在第一次记录原始体型
        entry.scaleSaved = true;
    }
    const float target = entry.origScale + s.bossScale;
    if (std::abs(entry.curScale - target) > 0.005F) {   // 幂等：与自己上次写进去的值比
        SetActorScale(a_actor, target);
        entry.curScale = target;
    }
} else if (entry.scaleSaved) {
    SetActorScale(a_actor, entry.origScale);      // 还原用原始值，不是 SetScale(1.0)
    entry.scaleSaved = false;
}
```

四个修复点：

1. `origScale` 记在 `Applied` 里，**随 co-save 持久化** —— 读档不会再丢
2. `scaleSaved` 标志保证**只在第一次**记录原始体型
3. **幂等**：与自己上次写进去的 `entry.curScale` 比，而不是与引擎回读比 ——
   写入口径与回读口径未必一致，用回读判断会次次都写（原版掉帧主因之一）
4. 还原用 `SetActorScale(origScale)`，不是原版的 `SetScale(1.0)`
   （原版那种写法会把本来就是 1.2 倍体型的单位强行压回 1.0）

### 写入侧：为什么是 `SetActorScale` 而不是 `SetScale`

**CommonLibSSE-NG 3.7.0 只给 `TESObjectREFR` 暴露了 `GetScale()`，没有 `SetScale`。**
`Actor` 上唯一带 float 参数的 `SetSize()` 语义无法确证（可能是别的属性），所以不走虚函数，
改为直接写体型真正存放的两处：

```cpp
inline void SetActorScale(RE::Actor* a_actor, float a_scale)
{
    const float s = std::clamp(a_scale, 0.05F, 10.0F);   // 与 setscale 的取值范围对齐

    // ① 引用运行时数据：单位是「缩放 × 100」，uint16 存 0.1~10 的小数位刚好够，
    //    这一份参与存档 / 重载
    a_actor->GetReferenceRuntimeData().refScale = static_cast<std::uint16_t>(s * 100.0F);
    a_actor->AddChange(RE::TESObjectREFR::ChangeFlags::kScale);   // 告诉引擎这个引用变了

    // ② 当前 3D 节点：立刻生效的那一份
    if (auto* obj3D = a_actor->GetCurrent3D()) {
        obj3D->local.scale = s;
    }
}
```

`bSkipBossScaleOnDragons=1`：龙跳过体型加成 —— 因为**龙的速度会被体型放大**，
这也会顺带避免「龙速度暴走」这个已知问题。

---

## 10. 法术强度：两条实现路径

| 模式 | 实现 | 依赖 |
|---|---|---|
| `Perk`（默认） | `AddPerk(EEA_EnhanceMGCPerk)` + 写 `LockpickingPowerMod` | 需要 `EnhanceEnemyAttributes.esp` |
| `PowerMod` | 直接加成五大法术学派 `PowerModifier` | **纯 DLL，零插件** |
| `Both` | 两者都开 | — |

**Perk 模式**是作者原机制：那条 Perk 读取 `LockpickingPowerMod` 的值作为法术强度倍率。
`LockpickingPowerMod` 原本是开锁相关的 AV，作者借它当"法术强度倍率"的载体。

**PowerMod 模式**是纯 DLL 路线，直接对五个 AV 各算一次：

```cpp
constexpr RE::ActorValue kSchoolPowerMod[5] = {
    kAlterationPowerModifier, kConjurationPowerModifier, kDestructionPowerModifier,
    kIllusionPowerModifier,   kRestorationPowerModifier
};
```

> 结论：**「纯 DLL」是成立的。** `sMode=PowerMod` 时，那个 365 字节的 ESP 也可以删掉，
> 整个 MOD 就只剩 `EEA_Native.dll` + `EEA_Native.ini` 两个文件。

---

## 11. 菜单系统（SKSE Menu Framework）

`Menu::Register()` —— 框架不在时自动跳过，**核心功能完全不受影响**（只少一个配置界面）。

默认为 **DELETE** 键打开模组控制面板，共 6 个 Tab：

| Tab | 内容 |
|---|---|
| 总开关 | `master`、`minLevel`、`bossEnabled`、`bossScale`、`affectPlayer` |
| 生命 / 法力 / 体力 / 攻击力 / 法术强度 | 每页：开关、基础百分比、叠加玩家等级、叠加敌人等级、额外倍率、Boss 倍率 + **实时预览** |
| 关于/性能 | 运行状态 / 操作 / 性能 / 触发模式 / 血量处理 / 定义层过滤器 |

**实时预览**是每页最有用的东西 —— 直接按当前设置和等级算出实际加成百分比，
不用改完再进游戏试：

```cpp
float PreviewPercent(cfg, playerLevel, enemyLevel, boss)
    → 返回 pct × mult × 100   // 也就是「最终加了多少 %」
```

「关于/性能」页的**运行状态**会显示：

- 追踪单位数（`applied_` + `pendingRevert_`）
- 队列长度
- Perk 是否可用
- **EditorID 是否可读** ← 决定 Boss 判定 [2] 有没有在工作

改动 → `MarkDirty()` → 主循环消费 → `SaveToDisk()` 写回 INI。
INI 在游戏外手改也可以，键名与原版 MCM 的 `settings.ini` 完全一致。

---

## 12. 性能模型

三层消除开销：

| 层 | 机制 | 效果 |
|---|---|---|
| 载体 | 无 Papyrus VM 实例 | 原版每个单位一个脚本实例，1 级生效会有成百上千个 |
| 调度 | 每帧预算 + 跨帧摊平（`iProcessPerTick=6`）| 进地牢不再瞬间爆发 |
| 稳态 | 幂等短路 | 数值没变完全不碰引擎，稳态开销 ≈ 一次 map 查找 |

### `iTriggerMode` 的三个取值

| 值 | 行为 | 适用 |
|---|---|---|
| `0` Load | 单位一加载就强化（原版行为） | 配合 `minLevel=10` 使用 |
| `1` Combat | **只在进入战斗时**强化 | 可以把 `minLevel` 调到 **1** |
| `2` Both | 加载与战斗都触发 | 折中 |

**`iTriggerMode=1` 是 Papyrus 时代做不到、插件才做的事。**

作者在评论区说过：把 `minLevel` 设成 1 会让小动物（兔子、狐狸、鹿）也生效，
**脚本实例太多，不建议**。那是 Papyrus 的代价模型 —— 每个生效单位都要占一个 VM 实例。

插件的代价模型完全不同：事件驱动（只有真正打起来的单位才进队列）+ 每帧预算（不管多少单位，
每帧只花固定的那点时间）。所以现在 `minLevel=1` 是可以放心开的。

---

## 13. 与原版逐项对照

| 项目 | 原版（Papyrus） | 新版（DLL） |
|---|---|---|
| 逻辑载体 | 3 个 `.psc` 脚本 | 1 个 DLL |
| 配置界面 | MCM（需 SkyUI） | INI + SKSE Menu |
| 数值公式 | `ceil(base×(pct+PC+EL)×0.01×extra×boss)` | **完全一致** |
| 属性施加 | `ModAV` + `RestoreAV(max)` | `ModActorValue(bucket, net)` |
| 回血 | 有（效果重放触发） | **根治**（按比例还原，无 `RestoreAV(max)`） |
| Boss 体型 | 脚本局部变量，读档累积 | 持久化 `origScale` + 幂等 |
| 读档 | 效果重放 → 拉满血 | **认领机制**（不撤销） |
| 每帧开销 | 无预算，6 段 `Wait(1.0)` 串行 | 每帧 6 个 + 跨帧摊平 |
| 等级门槛 | ESP 里烘焙 `GetLevel > 9` | INI 可调 `minLevel` |
| 触发时机 | 加载即生效 | Load / Combat / Both |
| 定义层过滤 | 无 | 6 个 ACBS 开关 |
| Boss 判定 | `HasRefType(Boss)` 一级 | 四级 + 兜底 |
| 随从判定 | 无 | 运行时 + 定义层派系 |
| co-save | 无 | 有，记录全部实加量 |
| 卸载 | 脚本随存档残留 | 菜单一键还原 + 删文件 |
| 骨架 ESP | 13,211 字节 / 69 条记录 | 365 字节 / 1 条记录（可删） |

---

## 14. 已知限制与待验证

**代码层面尚未验证的：**

- **本机无 MSVC**，走 GitHub Actions 云端编译（`.github/workflows/build.yml`）。
  首次云端编译已把「构建环境」问题全部清掉（VS 版本写死、`VCPKG_ROOT` 被 `vcvarsall`
  改写），API 签名错误也已按 CommonLibSSE-NG 3.7.0 的真实头文件逐条修正
- `TESForm::GetFormEditorID()` 在本机运行时是否真有值 → 决定 Boss 判定 [2] 能否工作
  （启动日志里会明确写出来）
- **体型写入的口径**：`SetActorScale` 直接写 `REFERENCE_RUNTIME_DATA::refScale`（缩放 × 100）
  与 3D 的 `local.scale`；`GetScale()` 走的是引擎函数而不是这个字段，
  两者口径是否完全一致要在游戏里确认一次
- **`RestoreActorValue` 的参数方向**：Step 4 写了回读校验，
  方向若与预期相反会自动补一次反向偏移，且 `bVerboseLog=1` 时会把
  「调整前 / 调整后 / 目标」打进日志，便于一眼核对

**设计层面的边界：**

- 当前实现是**实例层**（改 Actor 实例），不是**定义层**（改 `NPC_` 记录）。
  定义层方案（方案 D）的可行性论证在 `DESIGN-options.md`，尚未落地
- 实例层的正常性质：**改的不会回写到已实例化的 actor**。
  但影响比"必须开新档"轻得多 —— 非持久化 actor 在单元格卸载后会从 base 重建，
  跨一次区域切换或重读存档后绝大多数单位就是新值了
- 「不含友军」版所需的 `IsHostileToActor` / `GetShouldHelp` 是引用级判据，
  无法在定义层表达（本项目不受影响，因为「含友军」版没有这两条）
- `bUseTemporaryModifier` 在当前引擎接口下无效（引擎未暴露带桶的 `ModActorValue`），
  保留键名仅为 INI 兼容

---

## 附录：关键 FormID 速查

| FormID | 类型 | EDID | 用途 |
|---|---|---|---|
| `0x00000007` | NPC_ | `Player` | 玩家本体（ACBS 恒为 `0x30`） |
| `0x00000014` | ACHR | `PlayerRef` | 玩家引用 |
| `0x000130F7` | LCRT | `Boss` | Boss 判定 [1] |
| `0x00035D59` | KYWD | `ActorTypeDragon` | 龙类判定（挂 Race 上） |
| `0x0002F201` | CLAS | `EncClassDragon` | 龙类判定（职业） |
| `0x0005C84D` | FACT | `PotentialFollowerFaction` | 随从定义层判定 |
| `0x0005C84E` | FACT | `CurrentFollowerFaction` | 随从定义层判定 |
| `0x0001BCC0` | FACT | `BanditFaction` | （参考） |
| `0x0002430D` | FACT | `DraugrFaction` | （参考） |
| `0x00043599` | FACT | `ForswornFaction` | （参考） |
| `0x0002997E` | FACT | `FalmerFaction` | （参考） |
| `0x0002D1DF` | FACT | `SkeletonFaction` | （参考） |
| `0x00043598` | FACT | `DwarvenAutomatonFaction` | （参考） |
| `0x0004359A` | FACT | `GiantFaction` | （参考） |
| `0x000435A1` | FACT | `TrollFaction` | （参考） |
| `0x0002E893` | FACT | `PredatorFaction` | （参考） |
| `0x0004E849` | FACT | `FarmAnimalsFaction` | （参考） |
| `0x00032D9C` | FACT | `DragonFaction` | （参考） |

### ACBS 标志位（`RE::ACTOR_BASE_DATA::Flag`，权威来源）

```
0x00000001 Female            0x00004000 Summonable
0x00000002 Essential         0x00010000 DoesntBleed
0x00000004 CharGenPreset     0x00080000 (未用)
0x00000008 Respawn           0x80000000 Invulnerable
0x00000010 AutoCalcStats
0x00000020 Unique
0x00000040 NoStealthMeter    ← UESP 把 PCLevelMult 错标成这个值
0x00000080 PCLevelMult       ← 正确值
0x00000100 UseTemplate
0x00000200 CalcForAllLevels
0x00000800 Protected
```

### 插件记录体压缩

插件里记录的 `flags & 0x00040000` 表示记录体被 zlib 压缩：
**前 4 字节是解压后长度，后面才是 zlib 流**。离线工具解析时必须处理，
否则所有子记录解析都会失败（本项目写工具时踩过）。
