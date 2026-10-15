#pragma once

#include "Config.h"

namespace EEA
{
	// 核心管理器
	//
	// 与原版 Papyrus 方案的三个本质区别（也就是三个致命 bug 的根治点）：
	//  1. 幂等 —— 数值没变就完全不碰引擎，杜绝「每次刷新都调 SetScale」
	//  2. 精确回滚 —— 只撤掉「自己实际加进去的量」，绝不调 RestoreAV(..., max)
	//     所以不会再出现进出门/读档敌人回满血
	//  3. 跨帧摊平 —— 每帧只处理 iProcessPerTick 个单位，进地牢不再瞬间爆发
	class Manager
	{
	public:
		static Manager& Get();

		// ---- 生命周期 ----
		void OnDataLoaded();
		void OnPreLoadGame();
		void OnPostLoadGame();
		void OnNewGame();

		// ---- 序列化（co-save）----
		void SerializeSave(SKSE::SerializationInterface* a_intfc);
		void SerializeLoad(SKSE::SerializationInterface* a_intfc);

		// ---- 入队 ----
		// 单位一旦入队就会被处理，没有「按触发模式过滤」这一步 ——
		// 是否真的写数值由 IsEligible()（该不该强化）与幂等判定（数值变没变）决定。
		void Queue(RE::Actor* a_actor);
		void QueueAllLoaded();

		// 对象加载事件的统一入口（顺带检测玩家升级 → 全量刷新）
		void OnObjectLoaded(RE::FormID a_formID);

		// ---- 强制操作（菜单按钮）----
		void RevertAll();
		void RevertOne(RE::Actor* a_actor);

		// 全量重算：清空队列 → 已加载单位全部重新入队 → 逐个按当前设置重算。
		// a_notify = true（默认）会弹「正在更新敌人属性 / 敌人属性已更新」，
		// 对应原版 MCM 的行为；玩家升级触发的刷新传 false，免得每次升级都弹一遍。
		void RefreshAll(bool a_notify = true);

		// 把一次判定诊断（每个过滤器各拒了多少个）写进日志，用来定位「为什么不生效」。
		void RunFilterCensus();

		// ---- 状态（菜单显示）----
		std::size_t TrackedCount() const { return applied_.size() + pendingRevert_.size(); }
		std::size_t QueueSize() const { return queue_.size(); }
		bool        IsPerkAvailable() const { return magicPerk_ != nullptr; }

		// 上一次 RunFilterCensus 的结果（菜单「关于/性能」页显示）
		struct CensusStats
		{
			std::size_t scanned = 0;
			std::size_t eligible = 0;
			std::vector<std::pair<std::string, std::size_t>> reasons;  // 拒绝原因 → 个数
			std::vector<std::string>                         examples; // 可强化单位举例
		};
		const CensusStats& LastCensus() const { return lastCensus_; }

		// 屏幕提示（等价于 Papyrus 的 Debug.Notification），主线程执行
		static void Notify(std::string_view a_text);

		// 启动时探测的结果：运行时能不能读到 EditorID。
		// 决定 Boss 判定第 [2] 级（EditorID 含 "Boss"）是否真的在工作。
		bool        IsEditorIDReadable() const { return edidReadable_; }

		// 与原版脚本逐字一致：
		// ceil(baseAV * (PCLevel + ELevel + Extrapct) * 0.01 * ExtraMult * BossMult)
		static float ComputeExtra(float a_baseAV, const AttrSettings& a_cfg, int a_playerLevel, int a_enemyLevel, bool a_boss);

		// 菜单实时预览：按给定等级算出实际加成百分比
		static float PreviewPercent(const AttrSettings& a_cfg, int a_playerLevel, int a_enemyLevel, bool a_boss);

	private:
		Manager() = default;
		Manager(const Manager&) = delete;
		Manager& operator=(const Manager&) = delete;

		// 记录「我们自己实际加进去的量」，回滚时原样减掉
		struct Applied
		{
			float delta[kAttrCount]{};  // HP / MP / SP / ATK / MGC(LockpickingPowerMod)
			float schoolDelta[5]{};     // 五大法术学派 PowerModifier（kPowerMod 模式用）
			bool  perkAdded = false;
			bool  scaleSaved = false;
			float origScale = 1.0F;
			float curScale = 1.0F;
		};

		void  ScheduleTick();
		void  ProcessSome();
		void  ApplyTo(RE::Actor* a_actor);
		void  RevertWith(RE::Actor* a_actor, const Applied& a_entry);
		void  RevertEntry(RE::Actor* a_actor, Applied& a_entry);

		// 判定「这个单位该不该被强化」。
		// a_reason 非空时回填「被拒原因」（中文短句），供诊断统计使用。
		bool  IsEligible(RE::Actor* a_actor, const char** a_reason = nullptr) const;

		// 玩家判定：引用级 + 定义级双保险。
		// 之所以要「定义级」，是因为还有一条路线（方案 D）直接遍历 NPC_ 数组做定义层改写 ——
		// 玩家的 NPC_（0x00000007）就在那个数组里，必须能认出它。
		bool  IsPlayerActor(RE::Actor* a_actor) const;

		// 定义层过滤：只看 ActorBase（NPC_ / CREA 定义）自身的属性。
		// 与 IsEligible() 里的运行时判据（战斗状态、召唤状态、地点）分开，方便对照调试。
		bool  PassBaseFilters(RE::Actor* a_actor) const;

		bool  IsBoss(RE::Actor* a_actor) const;
		bool  IsBossViaLocRefType(RE::Actor* a_actor) const;
		bool  IsBossViaEditorID(RE::Actor* a_actor) const;
		bool  IsBossViaAIData(RE::Actor* a_actor) const;
		bool  IsDragon(RE::Actor* a_actor) const;
		bool  IsDragonClass(RE::Actor* a_actor) const;

		void  LoadPerkAndKeywords();

		// 把「我们施加在 a_av 上的总量」从 a_old 改成 a_new。
		//
		// 三个关键点，都是原版 Papyrus 方案的失效点：
		//  1. 幂等 —— 净差为 0 时完全不动引擎（原版每次刷新都无条件 ModAV + SetScale）
		//  2. 净差写入 —— 不做「先全撤再加回」，所以改设置时不会出现血量的中间跳变
		//  3. 用 GetPermanentActorValue（上限侧）读回实际接受量，
		//     而不是 GetActorValue —— 对生命而言后者读的是「当前血量」，会算错增量
		//
		// 对生命 / 法力 / 体力还会按 bPreserveCurrentRatio 保持当前值百分比。
		// 返回更新后的实际总量。
		static float SetAVTotal(RE::Actor* a_actor, RE::ActorValue a_av, float a_old, float a_new);

		static RE::ActorValue ToActorValue(Attr a_attr);

		std::unordered_map<RE::FormID, Applied> applied_;
		std::unordered_map<RE::FormID, Applied> pendingRevert_;  // 从 co-save 读回、等待目标加载后回滚

		std::deque<RE::FormID>          queue_;
		std::unordered_set<RE::FormID>  queued_;
		std::atomic<bool>               tickScheduled_{ false };

		// ---- Boss / 龙 判定所需的表单 ----
		// (1) 原版 = LocationRefType "Boss"（Skyrim.esm 0x000130F7）。
		// RE::BGSLocation::specialRefs 是 BSTArray<SpecialRefData>，
		// SpecialRefData = { BGSLocationRefType* type; UnloadedRefData refData /*FormID refID*/; }
		RE::BGSLocationRefType*         bossLocRefType_{ nullptr };

		// (2) EditorID 片段（全部转小写后保存，比较时也转小写）
		std::vector<std::string>        bossEdidPatterns_;

		// (4) 显式名单兜底
		std::vector<RE::BGSKeyword*>    bossKeywords_;
		std::vector<RE::TESRace*>       bossRaces_;
		std::vector<RE::TESNPC*>        bossNpcs_;

		// 诊断：启动时探测一次 EditorID 在运行时是否真的可读
		// （Skyrim 默认不保留 EDID，若为 false 会提示装 Native EditorID Fix）
		bool                            edidReadable_{ false };

		std::vector<RE::BGSKeyword*>    dragonKeywords_;   // ActorTypeDragon（挂在 Race 上）
		std::vector<RE::TESClass*>      dragonClasses_;    // EncClassDragon

		// 定义层「随从」判据用的两条原版派系（FormID 硬编码，不依赖运行时 EditorID）：
		//   PotentialFollowerFaction 0x0005C84D —— 写在 NPC_ 的派系列表里，共 43 条
		//   CurrentFollowerFaction   0x0005C84E —— 共 40 条
		// （NPC_ 的派系存在 SNAM 子记录，不是 FACT —— 这是实解插件文件核对过的）
		std::vector<RE::TESFaction*>    followerFactions_;

		RE::BGSPerk*                    magicPerk_{ nullptr };
		int                             lastPlayerLevel_{ -1 };

		CensusStats                     lastCensus_{};

		// 「这一轮刷新结束后要弹一次『已更新』」的标记。
		// 由 RefreshAll() 置位，ProcessSome() 把队列跑空时消费。
		bool                            notifyOnDrain_{ false };

		// 「正在更新敌人属性」的节流：拖滑块时每帧都会触发刷新，
		// 不节流的话通知会刷屏。500ms 内只弹一次。
		std::chrono::steady_clock::time_point lastNotifyStart_{};
	};
}
