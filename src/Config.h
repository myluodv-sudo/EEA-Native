#pragma once

#include "PCH.h"

namespace EEA
{
	// 五个可强化的属性，顺序与原版 FormList EEA_ActorValueNameList 完全一致
	// （原版的 5 个 MESG 记录 FULL 名即下列 AV 名）
	enum class Attr : std::size_t
	{
		kHealth = 0,   // AV "health"                -> ActorValue::kHealth            (24)
		kMagicka,      // AV "magicka"               -> ActorValue::kMagicka           (25)
		kStamina,      // AV "stamina"               -> ActorValue::kStamina           (26)
		kAttack,       // AV "AttackDamageMult"      -> ActorValue::kAttackDamageMult  (154)
		kMagic,        // AV "LockpickingPowerMod"   -> ActorValue::kLockpickingPowerModifier (143)
		kCount
	};

	inline constexpr std::size_t kAttrCount = static_cast<std::size_t>(Attr::kCount);

	inline constexpr const char* kAttrKey[kAttrCount] = { "HP", "MP", "SP", "ATK", "MGC" };
	inline constexpr const char* kAttrNameCN[kAttrCount] = { "生命", "法力", "体力", "攻击力", "法术强度" };

	// 单个属性的 5 个参数 —— 与原版 MCM 每一项一一对应
	struct AttrSettings
	{
		bool  enabled = false;        // bEEASwitch*
		int   basePercent = 20;       // iExtrapct*
		bool  usePlayerLevel = true;  // bPCLevelSwitch*
		bool  useEnemyLevel = true;   // bELevelSwitch*
		float extraMult = 1.0F;       // fExtraMult*
		float bossMult = 2.0F;        // fBossMult*
	};

	// 法术强度的实现方式
	enum class MagicMode : int
	{
		kPerk = 0,      // 使用 ESP 里的 EEA_EnhanceMGCPerk + LockpickingPowerMod（原作者机制）
		kPowerMod = 1,  // 纯 DLL：直接加成五大法术学派 PowerModifier，不需要任何插件
		kBoth = 2
	};

	struct Settings
	{
		// [Main]
		bool  master = true;      // bEEASwitchMOD
		int   minLevel = 10;      // 原版把 "GetLevel > 9" 烘焙在 ESP 里，这里改为可调
		bool  affectPlayer = false;
		bool  bossEnabled = true;  // bEEASwitchBoss
		float bossScale = 0.10F;   // fEEABossScale

		AttrSettings attrs[kAttrCount]{};

		// [Filter]
		bool excludeFollowers = false;
		bool excludeSummons = false;
		bool skipBossScaleOnDragons = true;

		// ================= 定义层过滤器 =================
		// 参照 Dynamic NPC Scaling（Nexus 180306）的 [SkipFlags]，但判据全部落在
		// 「ActorBase（NPC_ 定义）的 ACBS 标志位」上 —— 也叫「定义层判据」：
		//   · 它描述的是「这个 NPC 是哪一类」，而不是「这一份实例现在什么状态」
		//   · 所以结论稳定：不随招募 / 复活 / 读档 / 地点变化而变
		//   · 开销近乎为零（几个位测试），且不依赖运行时 EditorID
		//
		// 位值以 CommonLibSSE-NG 的 RE::ACTOR_BASE_DATA::Flag 为权威来源。
		// 注意：UESP / xEdit 网页把 PCLevelMult 标成 0x40 是错的，正确是 0x80。
		// 全部默认 0 = 保持原 MOD「含友军」版行为 —— 谁都不额外排除。
		bool skipUnique = false;        // 0x00000020 独特 NPC（注意：玩家本体也带这个标志）
		bool skipEssential = false;     // 0x00000002 重要 NPC（倒地不死）
		bool skipProtected = false;     // 0x00000800 受保护（只会倒地，不会被杀）
		bool skipInvulnerable = false;  // 0x80000000 无敌
		bool skipSummonable = false;    // 0x00004000 可被召唤
		bool skipTemplated = false;     // 使用模板 / 捏脸预设的 NPC（改它们多半无效）

		// 排除随从时，是否同时用「定义层派系」判定。
		// 原实现的 IsPlayerTeammate() 只认「此刻已经被招募」的随从；改成派系判定后
		// 连「将来可能被招募」的 NPC 也一并排除，不受当前招募状态影响。
		// 两条派系 FormID 已从 Skyrim.esm 实解确认（见 .cpp 里的常量与注释）。
		bool followerCheckByFaction = true;

		// ---- 龙类是否排除在「属性强化」之外 ----
		// 原版 ESP 在整条法术条件链里放了 HasKeyword(ActorTypeDragon)==0 与
		// GetIsClass(EncClassDragon)==0（两个 ID 均已从 Skyrim.esm 实解：
		// 0x00035D59 = ActorTypeDragon 关键字，0x0002F201 = EncClassDragon 职业）。
		// 也就是说：原版龙是拿不到属性加成的 —— Nexus 上「龙不受影响」的反馈就是这条造成的。
		// excludeDragons = 1 保持原版行为；改成 0 就让龙也吃加成。
		bool                     excludeDragons = true;
		std::vector<std::string> dragonKeywords{ "ActorTypeDragon" };
		std::vector<std::string> dragonClasses{ "EncClassDragon" };

		// ---- Boss 判定：三级，精确度递减，默认只开前两级 ----
		//
		// 背景：三种判据的「精确度」差别极大。用整合包 35,904 个 NPC_ 实测
		// （工具 native/tools/boss_scan.py）：
		//
		//   (1) LocationRefType "Boss"   高   原版机制，需 CK 手工标记，覆盖率低但准
		//   (2) EditorID 含 "Boss"       中   命中 3,508 个（9.8%）
		//   (3) AI 数据                  低   命中 15,097 个（42%）—— 大量 mod 给所有
		//                                     战斗单位都设了 VeryAggressive + Foolhardy
		//
		// 对 TrueHUD 那种「只决定显不显示血条」的用途，判宽一点无妨；
		// 但对属性加成，第 (3) 条会让 38% 的敌人吃上 Boss 倍率，Boss 设定直接失效。
		// 所以默认只开 (1)(2)，(3) 需要显式打开。

		// (1) 原版机制：Skyrim.esm 里 EDID 就叫 "Boss"、FormID 0x000130F7 的
		//     LocationRefType（记录类型 LCRT，已实解确认）。它描述的是
		//     「某个放置参照物在某地点中扮演的角色」——不是 Actor 关键字，
		//     更不是「FormID 里带 BOSS」。
		bool        bossUseLocRefType = true;
		std::string locRefTypeBoss = "Boss";

		// (2) EditorID（CK 里的记录名，如 dunHalldirsBoss）含指定片段。
		//     注意 EDID 与 FormID 是两回事：FormID 是加载时分配的数字，没有语义。
		//     局限：Skyrim 运行时默认不保留 EDID，若日志显示取不到，
		//     需要装 Native EditorID Fix 之类的插件（详见 README）。
		bool                     bossUseEditorID = true;
		std::vector<std::string> bossEdidPatterns{ "Boss" };

		// (3) AI 数据：Aggression >= bossMinAggression 且 Confidence >= bossMinConfidence。
		//     对应枚举 RE::ACTOR_AGGRESSION{0=Unaggressive,1=Aggressive,2=VeryAggressive,3=Frenzied}
		//     与 RE::ACTOR_CONFIDENCE{0=Cowardly..4=Foolhardy}。
		//     实测过于宽泛，默认关闭。
		bool bossUseAIData = false;
		int  bossMinAggression = 2;
		int  bossMinConfidence = 4;

		// (4) 显式名单兜底
		// 注意：vanilla 并没有 ActorTypeBoss 这种关键字（已扫过 Skyrim.esm 全部 KEYM 确认），
		// 所以默认留空，否则只会在日志里刷「找不到关键字」。
		std::vector<std::string> bossKeywords{};
		std::vector<std::string> bossRaces{ "DragonPriestRace" };  // 龙祭司
		std::vector<std::string> bossNpcs{};

		// [Magic]
		MagicMode   magicMode = MagicMode::kPerk;
		std::string magicPerkPlugin = "EnhanceEnemyAttributes.esp";
		std::string magicPerkEditorID = "EEA_EnhanceMGCPerk";

		// [Menu]
		std::string menuSection = "敌人属性增强";
		bool        menuEnabled = true;

		// [Perf]
		int  processPerTick = 6;    // 每帧最多处理几个单位（跨帧摊平）
		bool verboseLog = false;
		bool logFirstApply = true;

		// ---- 触发模式（决定「什么时候」去强化一个单位）----
		// 0 = Load   ：单位一加载就强化（原版行为，配合 minLevel 使用）
		// 1 = Combat ：只在单位进入战斗时强化。这是 Papyrus 时代做不到、但插件可以做到的事：
		//              它让 minLevel 可以一直调到 1，而不会被满地小动物（兔子、狐狸、鹿）
		//              撑爆队列 —— 作者当年拒绝做「1 级生效」的原因正是这个。
		// 2 = Both   ：加载与战斗都触发
		//
		// 注：已经强化过的单位在任何模式下都保持幂等刷新（数值没变就完全不碰引擎）。
		int triggerMode = 0;

		// ---- 改上限时是否保持「当前值百分比」（仅生命 / 法力 / 体力）----
		// 1 = 保持：按当前血量的百分比等比缩放，所以
		//     · 首次强化 → 满血敌人仍是满血（看起来"变强了"）
		//     · 残血敌人 → 仍按原比例（不会借机回血）
		//     · 战斗中改设置 → 血量百分比不动（这就是"回满血"被根治的地方）
		// 0 = 完全交给引擎
		bool preserveCurrentRatio = true;

		// ---- 用哪个 modifier 桶写属性 ----
		// 0 = kPermanent：与原版 Papyrus 的 ModAV 是同一个桶。读档后数值仍在，
		//     插件可以「认领」而不是「撤销」，因此不会把残血敌人扣死。
		//     代价：数值会随存档保存 —— 卸载插件前请先在菜单点一次「还原所有单位」。
		// 1 = kTemporary：不写存档，但引擎是否据此抬高上限尚未实机验证。
		bool useTemporaryModifier = false;

		// [Debug] 仅用于诊断：把实际算出的数值写进日志
		bool traceValues = false;
	};

	class Config
	{
	public:
		static Config& Get();

		// 从 EEA_Native.ini 读取（文件不存在则用默认值并写出模板）
		void LoadFromDisk();

		// 写回 EEA_Native.ini（菜单里改动后调用）
		void SaveToDisk();

		Settings& Data() { return settings_; }
		const Settings& Data() const { return settings_; }

		// ---- 脏标记：菜单/文件改动后置位，主循环消费 ----
		void MarkDirty() { dirtyAll_.store(true); }
		void MarkGlobalDirty() { dirtyGlobal_.store(true); }
		bool ConsumeAllDirty() { return dirtyAll_.exchange(false); }
		bool ConsumeGlobalDirty() { return dirtyGlobal_.exchange(false); }
		bool AnyDirty() const { return dirtyAll_.load() || dirtyGlobal_.load(); }

		std::string GetIniPath() const;

	private:
		Config() = default;
		Config(const Config&) = delete;
		Config& operator=(const Config&) = delete;

		void  ApplyDefaults();
		void  ParseLine(const std::string& a_section, const std::string& a_key, const std::string& a_value);

		Settings settings_{};
		std::atomic<bool> dirtyAll_{ false };
		std::atomic<bool> dirtyGlobal_{ false };
	};
}
