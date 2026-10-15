#include "EnhancementManager.h"

namespace EEA
{
	namespace
	{
		// 原版 ESP 里 Perk EEA_EnhanceMGCPerk 的本地 FormID（从 EnhanceEnemyAttributes.esp 解出）
		constexpr RE::FormID kMagicPerkLocalID = 0x00000C59;

		constexpr std::uint32_t kSerializationRecord = 'EEAN';
		constexpr std::uint32_t kSerializationVersion = 1;

		constexpr float kEps = 0.001F;

		// ---- 原版派系 FormID（Skyrim.esm，已实解插件文件确认，与 EDID 逐一对应）----
		// 注意：NPC_ 的派系写在 SNAM 子记录里，不是 FACT —— 这点实解时核对过，
		// xEdit 之外的资料容易写错。
		// 用硬编码 FormID 而不是 EDID 查询：运行时 EditorID 未必可读（见 ProbeEditorIDAvailability），
		// 而这两条派系是原版常量，FormID 永远是这两个。
		constexpr RE::FormID kFactionCurrentFollower = 0x0005C84E;    // CurrentFollowerFaction
		constexpr RE::FormID kFactionPotentialFollower = 0x0005C84D;  // PotentialFollowerFaction

		// 五大法术学派的 PowerModifier —— 纯 DLL 模式下替代那条 Perk
		constexpr RE::ActorValue kSchoolPowerMod[5] = {
			RE::ActorValue::kAlterationPowerModifier,
			RE::ActorValue::kConjurationPowerModifier,
			RE::ActorValue::kDestructionPowerModifier,
			RE::ActorValue::kIllusionPowerModifier,
			RE::ActorValue::kRestorationPowerModifier
		};

		// 设置 Actor 体型。
		//
		// CommonLibSSE-NG 3.7.0 对 Actor / TESObjectREFR 只暴露了 GetScale()，没有 SetScale，
		// 而对 Actor 唯一带 float 参数的 SetSize 语义无法确证（可能不是体型）。
		// 所以这里不走虚函数，直接写体型真正存放的两处：
		//   ① 引用运行时数据 REFERENCE_RUNTIME_DATA::refScale —— 单位是「缩放 × 100」，
		//      uint16 存 0.1~10 的小数位刚好够，这也是参与存档 / 重载的那一份
		//   ② 当前 3D 节点的 local.scale —— 立刻生效的那一份
		// 顺带置上 ChangeFlags::kScale，让引擎知道这个引用的体型变了。
		//
		// 这样做的另一层意义：原版 Papyrus 的 OrinScale 是个脚本局部变量，读档实例重建就丢值，
		// 于是每读一次档「1.0 + 增量」就再加一遍，体型越滚越大。我们把原始体型存进 co-save，
		// 每次都用「原始体型 + 增量」写绝对值，这才是真正根治那一处 bug 的地方。
		inline void SetActorScale(RE::Actor* a_actor, float a_scale)
		{
			if (!a_actor) {
				return;
			}
			const float s = std::clamp(a_scale, 0.05F, 10.0F);  // 与 setscale 的取值范围对齐

			a_actor->GetReferenceRuntimeData().refScale = static_cast<std::uint16_t>(s * 100.0F);
			a_actor->AddChange(RE::TESObjectREFR::ChangeFlags::kScale);

			if (auto* obj3D = a_actor->GetCurrent3D()) {
				obj3D->local.scale = s;
			}
		}

		// 按 EditorID 解析任意表单类型；解析不到的只记一条日志，不中断
		template <class T>
		std::vector<T*> ResolveForms(const std::vector<std::string>& a_ids, const char* a_kind)
		{
			std::vector<T*> out;
			for (const auto& id : a_ids) {
				if (id.empty()) {
					continue;
				}
				if (auto* f = RE::TESForm::LookupByEditorID<T>(id)) {
					out.push_back(f);
				} else {
					logger::warn("[EEA] 找不到{} \"{}\"（已跳过，可在 EEA_Native.ini 里改）", a_kind, id);
				}
			}
			return out;
		}

		std::vector<RE::BGSKeyword*> ResolveKeywords(const std::vector<std::string>& a_ids)
		{
			return ResolveForms<RE::BGSKeyword>(a_ids, "关键字");
		}

		// ---------------------------------------------------------- EditorID 辅助
		// EditorID（CK 里的记录名，如 dunHalldirsBoss）在编辑器里不区分大小写，
		// 这里统一转小写再做子串匹配。
		// 注意别把它和 FormID 搞混：FormID 是加载时分配的数字，没有任何语义。
		std::string ToLowerAscii(std::string_view a_sv)
		{
			std::string out;
			out.reserve(a_sv.size());
			for (const char ch : a_sv) {
				const auto c = static_cast<unsigned char>(ch);
				out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c));
			}
			return out;
		}

		bool ContainsCI(const std::string& a_lowerHaystack, const std::string& a_lowerNeedle)
		{
			return !a_lowerNeedle.empty() &&
			       a_lowerHaystack.find(a_lowerNeedle) != std::string::npos;
		}

		// 运行时 EditorID 可用性探测。
		// Skyrim 默认**不**在内存里保留 EditorID —— 这正是 Native EditorID Fix 这类
		// 插件存在的原因。所以不能假设 GetFormEditorID() 一定有值，抽查一批看看。
		bool ProbeEditorIDAvailability()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return false;
			}
			const auto& npcs = dh->GetFormArray<RE::TESNPC>();
			int checked = 0;
			int nonEmpty = 0;
			for (auto* npc : npcs) {
				if (!npc) {
					continue;
				}
				++checked;
				if (const char* e = npc->GetFormEditorID(); e && *e) {
					++nonEmpty;
				}
				if (checked >= 256) {
					break;
				}
			}
			return nonEmpty > 0;
		}
	}

	Manager& Manager::Get()
	{
		static Manager instance;
		return instance;
	}

	RE::ActorValue Manager::ToActorValue(Attr a_attr)
	{
		switch (a_attr) {
		case Attr::kHealth:  return RE::ActorValue::kHealth;
		case Attr::kMagicka: return RE::ActorValue::kMagicka;
		case Attr::kStamina: return RE::ActorValue::kStamina;
		case Attr::kAttack:  return RE::ActorValue::kAttackDamageMult;
		case Attr::kMagic:   return RE::ActorValue::kLockpickingPowerModifier;
		default:             return RE::ActorValue::kNone;
		}
	}

	void Manager::LoadPerkAndKeywords()
	{
		auto& s = Config::Get().Data();

		bossKeywords_ = ResolveKeywords(s.bossKeywords);
		bossRaces_ = ResolveForms<RE::TESRace>(s.bossRaces, "种族");
		bossNpcs_ = ResolveForms<RE::TESNPC>(s.bossNpcs, "NPC");
		dragonKeywords_ = ResolveKeywords(s.dragonKeywords);
		dragonClasses_ = ResolveForms<RE::TESClass>(s.dragonClasses, "职业");

		// 定义层「随从」判据的两条原版派系 —— 走 FormID 而不走 EDID。
		// 这里**无条件**解析：菜单里随时可以打开「排除随从」，而本函数只在
		// 数据加载 / 读档时跑一次；是否真的使用在 PassBaseFilters() 里按开关判断。
		followerFactions_.clear();
		if (auto* dh = RE::TESDataHandler::GetSingleton()) {
			for (const auto id : { kFactionCurrentFollower, kFactionPotentialFollower }) {
				if (auto* f = dh->LookupForm<RE::TESFaction>(id, "Skyrim.esm")) {
					followerFactions_.push_back(f);
				} else {
					logger::warn("[EEA] 找不到原版派系 0x{:08X}（随从定义层过滤将退回运行时判定）", id);
				}
			}
			if (followerFactions_.size() != 2) {
				logger::warn("[EEA] 随从派系只解析到 {} / 2 条。", followerFactions_.size());
			}
		}

		// EditorID 片段全部转小写保存（比较时同样转小写）
		bossEdidPatterns_.clear();
		for (const auto& p : s.bossEdidPatterns) {
			auto low = ToLowerAscii(p);
			if (!low.empty()) {
				bossEdidPatterns_.push_back(std::move(low));
			}
		}

		// 原版 HasRefType(Boss) 用的那条 LocationRefType —— Skyrim.esm 里 EDID 就叫 "Boss"
		bossLocRefType_ = nullptr;
		if (s.bossUseLocRefType) {
			bossLocRefType_ = RE::TESForm::LookupByEditorID<RE::BGSLocationRefType>(s.locRefTypeBoss);
			if (bossLocRefType_) {
				logger::info("[EEA] Boss 判定 [1/3] 已接入 LocationRefType \"{}\"（对应原版 HasRefType）",
					s.locRefTypeBoss);
			} else {
				logger::warn("[EEA] Boss 判定 [1/3] 找不到 LocationRefType \"{}\"（已跳过）",
					s.locRefTypeBoss);
			}
		}

		// 运行时 EditorID 可用性 —— 这一条直接决定 [2/3] 能不能用
		edidReadable_ = ProbeEditorIDAvailability();
		if (s.bossUseEditorID) {
			if (edidReadable_) {
				logger::info("[EEA] Boss 判定 [2/3] EditorID 可读，匹配片段：{}",
					[&] {
						std::string j;
						for (const auto& p : bossEdidPatterns_) { j += p; j += ' '; }
						return j;
					}());
			} else {
				logger::warn(
					"[EEA] Boss 判定 [2/3] 运行时取不到 EditorID（Skyrim 默认不保留）。"
					"想用「EditorID 含 Boss」这条判据，需要安装 Native EditorID Fix 之类的插件，"
					"或把 EEA_Native.ini 的 [Filter] bBossUseEditorID 改为 0。");
			}
		}

		if (s.bossUseAIData) {
			logger::warn(
				"[EEA] Boss 判定 [3/3] 已启用 AI 数据判据（Aggression>={} 且 Confidence>={}）。"
				"实测该判据会命中约 42% 的 NPC（大量 mod 给所有战斗单位都设了"
				" VeryAggressive + Foolhardy），慎用。",
				s.bossMinAggression, s.bossMinConfidence);
		}

		magicPerk_ = nullptr;
		if (s.magicMode != MagicMode::kPowerMod) {
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (dh) {
				magicPerk_ = dh->LookupForm<RE::BGSPerk>(kMagicPerkLocalID, s.magicPerkPlugin);
			}
			if (!magicPerk_) {
				magicPerk_ = RE::TESForm::LookupByEditorID<RE::BGSPerk>(s.magicPerkEditorID);
			}
			if (!magicPerk_) {
				logger::warn(
					"[EEA] 未找到法术强度 Perk（{} / 0x{:X}）。"
					"法术强度将只能依赖 PowerMod 模式 —— 可把 [Magic] sMode 改为 PowerMod。",
					s.magicPerkPlugin, kMagicPerkLocalID);
			} else {
				logger::info("[EEA] 已接入法术强度 Perk：{}", magicPerk_->GetFormEditorID());
			}
		}
	}

	void Manager::OnDataLoaded()
	{
		LoadPerkAndKeywords();
		logger::info("[EEA] 数据加载完成，开始工作。");
		QueueAllLoaded();
	}

	void Manager::OnPreLoadGame()
	{
		// 存档即将被替换：当前内存里的记录全部作废
		applied_.clear();
		pendingRevert_.clear();
		queue_.clear();
		queued_.clear();
		forced_.clear();
	}

	void Manager::OnPostLoadGame()
	{
		LoadPerkAndKeywords();
		QueueAllLoaded();
		logger::info("[EEA] 读档完成，待回滚 {} 条、待处理 {} 个。", pendingRevert_.size(), queue_.size());
	}

	void Manager::OnNewGame()
	{
		applied_.clear();
		pendingRevert_.clear();
		queue_.clear();
		queued_.clear();
		forced_.clear();
		LoadPerkAndKeywords();
	}

	// ---------------------------------------------------------------- 入队

	void Manager::Queue(RE::Actor* a_actor, bool a_force)
	{
		if (!a_actor) {
			return;
		}
		const auto fid = a_actor->GetFormID();
		if (fid == 0) {
			return;
		}
		if (a_force) {
			forced_.insert(fid);  // 即便已在队列里，也要把「战斗驱动」这个标记补上
		}
		if (!queued_.emplace(fid).second) {
			return;  // 已在队列里
		}
		queue_.push_back(fid);
		ScheduleTick();
	}

	void Manager::QueueAllLoaded()
	{
		auto* pl = RE::ProcessLists::GetSingleton();
		if (!pl) {
			return;
		}
		std::size_t n = 0;
		for (auto* list : { &pl->highActorHandles, &pl->middleHighActorHandles, &pl->lowActorHandles }) {
			for (auto& handle : *list) {
				auto ref = handle.get();
				if (!ref) {
					continue;
				}
				if (auto* actor = ref->As<RE::Actor>()) {
					Queue(actor);
					++n;
				}
			}
		}
		logger::info("[EEA] 已把 {} 个已加载单位排入队列。", n);
	}

	void Manager::OnObjectLoaded(RE::FormID a_formID)
	{
		// 玩家升级 → 所有已强化单位都该重算
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const int lv = static_cast<int>(player->GetLevel());
			if (lv != lastPlayerLevel_) {
				const bool hadValue = lastPlayerLevel_ > 0;
				lastPlayerLevel_ = lv;
				if (hadValue) {
					logger::info("[EEA] 玩家等级变化 → {}，请求全量刷新。", lv);
					RefreshAll();
				}
			}
		}

		// Combat 模式（iTriggerMode=1）：加载时不入队，等这些单位进入战斗再处理。
		// 这样 minLevel 可以一直降到 1，而不会被满地的小动物（兔子/狐狸/鹿）撑爆队列 ——
		// 作者当年拒绝做「1 级生效」的原因正是脚本实例数量，插件时代没这个约束。
		if (Config::Get().Data().triggerMode == 1) {
			return;
		}

		auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_formID);
		if (ref) {
			if (auto* actor = ref->As<RE::Actor>()) {
				Queue(actor);
			}
		}
	}

	void Manager::ScheduleTick()
	{
		if (tickScheduled_.exchange(true)) {
			return;
		}
		SKSE::GetTaskInterface()->AddTask([this]() {
			tickScheduled_.store(false);
			ProcessSome();
			if (!queue_.empty()) {
				ScheduleTick();  // 还有剩，下一帧继续（跨帧摊平）
			}
		});
	}

	void Manager::ProcessSome()
	{
		const int budget = std::max(1, Config::Get().Data().processPerTick);
		for (int i = 0; i < budget && !queue_.empty(); ++i) {
			const auto fid = queue_.front();
			queue_.pop_front();
			queued_.erase(fid);

			auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
			if (!actor || actor->GetFormID() != fid) {
				forced_.erase(fid);
				continue;  // 已卸载/已删除
			}
			const bool forced = forced_.erase(fid) > 0;
			ApplyTo(actor, forced);
		}
	}

	// ---------------------------------------------------------------- 判定

	bool Manager::IsBoss(RE::Actor* a_actor) const
	{
		if (!a_actor) {
			return false;
		}
		const auto& s = Config::Get().Data();

		// ---- 三级判据，精确度递减（实测数据见 Config.h 的注释）----

		// [1] 原版机制：该引用在其所在地点里是否被标成 Boss 角色。最精确。
		if (s.bossUseLocRefType && IsBossViaLocRefType(a_actor)) {
			return true;
		}

		// [2] EditorID 含 "Boss" —— 覆盖 mod 作者命名出来的 Boss（实测命中 9.8%）。
		if (s.bossUseEditorID && IsBossViaEditorID(a_actor)) {
			return true;
		}

		// [3] AI 数据 —— 实测命中 42%，会让 Boss 倍率失去意义，默认关闭。
		if (s.bossUseAIData && IsBossViaAIData(a_actor)) {
			return true;
		}

		// [4] 显式名单兜底：关键字 / 种族 / 具体 NPC（例如龙祭司）
		for (auto* kw : bossKeywords_) {
			if (kw && a_actor->HasKeyword(kw)) {
				return true;
			}
		}
		if (auto* base = a_actor->GetActorBase()) {
			for (auto* npc : bossNpcs_) {
				if (npc && npc == base) {
					return true;
				}
			}
			for (auto* race : bossRaces_) {
				if (race && base->GetRace() == race) {
					return true;
				}
			}
		}

		return false;
	}

	// [2] EditorID 判据 —— 看 ActorBase（NPC_ / CREA 定义）的 EditorID 里有没有配置的片段。
	//
	// 用 base 的 EditorID 而不是引用的，因为「这个 NPC 是不是 Boss」本来就是定义级属性，
	// 与它被放在哪里无关。
	//
	// 重要前提：Skyrim 运行时默认不保留 EditorID（这正是 Native EditorID Fix 存在的理由）。
	// 所以这里先看启动时探测出来的 edidReadable_，不可读就整条跳过，
	// 免得对每个单位白跑一遍字符串查找。
	bool Manager::IsBossViaEditorID(RE::Actor* a_actor) const
	{
		if (bossEdidPatterns_.empty() || !edidReadable_) {
			return false;
		}
		auto* base = a_actor->GetActorBase();
		if (!base) {
			return false;
		}
		const char* raw = base->GetFormEditorID();
		if (!raw || !*raw) {
			return false;
		}
		const auto lower = ToLowerAscii(raw);
		for (const auto& pat : bossEdidPatterns_) {
			if (ContainsCI(lower, pat)) {
				if (Config::Get().Data().verboseLog) {
					logger::info("[EEA] Boss(EDID) \"{}\" 命中片段 \"{}\"", raw, pat);
				}
				return true;
			}
		}
		return false;
	}

	// [3] AI 数据判据。
	// TESAIForm 是 TESActorBase 的基类之一，所以 TESNPC* 可以直接调用这两个方法。
	// 枚举取值：ACTOR_AGGRESSION{0=Unaggressive,1=Aggressive,2=VeryAggressive,3=Frenzied}
	//           ACTOR_CONFIDENCE{0=Cowardly,1=Cautious,2=Average,3=Brave,4=Foolhardy}
	bool Manager::IsBossViaAIData(RE::Actor* a_actor) const
	{
		auto* base = a_actor->GetActorBase();
		if (!base) {
			return false;
		}
		const auto& s = Config::Get().Data();
		const int aggro = static_cast<int>(base->GetAggressionLevel());
		const int conf = static_cast<int>(base->GetConfidenceLevel());
		return aggro >= s.bossMinAggression && conf >= s.bossMinConfidence;
	}

	bool Manager::IsBossViaLocRefType(RE::Actor* a_actor) const
	{
		// 对应原版 Papyrus 的 akTarget.HasRefType(Boss)。
		// BGSLocation::specialRefs 存的是「(LocationRefType*, 参照物 FormID)」配对，
		// 所以这里查：该引用的 FormID，在其所在（含父级）地点里有没有被标成 Boss。
		if (!bossLocRefType_ || !a_actor) {
			return false;
		}

		const auto refID = a_actor->GetFormID();
		if (refID == 0) {
			return false;
		}

		auto* loc = a_actor->GetEditorLocation1();
		for (int depth = 0; loc && depth < 8; loc = loc->parentLoc, ++depth) {
			for (const auto& sr : loc->specialRefs) {
				if (sr.type == bossLocRefType_ && sr.refData.refID == refID) {
					return true;
				}
			}
		}
		return false;
	}

	bool Manager::IsDragon(RE::Actor* a_actor) const
	{
		if (!a_actor) {
			return false;
		}
		auto* race = a_actor->GetRace();
		for (auto* kw : dragonKeywords_) {
			if (!kw) {
				continue;
			}
			if (a_actor->HasKeyword(kw)) {
				return true;
			}
			// 龙类关键字挂在 Race 上（ActorTypeDragon）
			if (race && race->HasKeyword(kw)) {
				return true;
			}
		}
		return false;
	}

	bool Manager::IsDragonClass(RE::Actor* a_actor) const
	{
		// 原版还有一条 GetIsClass(EncClassDragon)==0，用来排除「自定义龙形 Boss」——
		// 作者置顶帖里提到的 Vigilant「Julia the Crimson」就是靠这条挡住的。
		if (dragonClasses_.empty() || !a_actor) {
			return false;
		}
		if (auto* base = a_actor->GetActorBase()) {
			// TESNPC 没有 GetClass()：职业是成员变量 npcClass（CNAM 子记录）。
			for (auto* cls : dragonClasses_) {
				if (cls && base->npcClass == cls) {
					return true;
				}
			}
		}
		return false;
	}

	// 玩家判定 —— 必须与所有过滤器开关**独立**。
	//
	// 这里刻意不用「ActorBase 带 Unique 标志」来认玩家。理由：
	//   · Unique 在整合包里同时命中 19.4% 的普通独特 NPC（6,974 / 35,905 条）
	//   · 它由 bSkipUnique 控制，用户一关掉，玩家就会被漏进来
	// 所以走两条更硬的判据：
	//   ① 引用级：a_actor 就是 PlayerRef（0x00000014）
	//   ② 定义级：a_actor 的 ActorBase 与玩家的 ActorBase 是同一份 NPC_（0x00000007 "Player"）。
	//      这条是为「定义层改写」路线准备的 —— 那条路直接遍历 NPC_ 数组，
	//      玩家的记录就在数组里，必须能把它认出来。
	bool Manager::IsPlayerActor(RE::Actor* a_actor) const
	{
		if (!a_actor) {
			return false;
		}
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return false;
		}
		if (a_actor == pc) {
			return true;
		}
		auto* base = a_actor->GetActorBase();
		return base && base == pc->GetActorBase();
	}

	// 定义层过滤：只看 ActorBase（NPC_ 记录）自身的 ACBS 标志与派系列表。
	//
	// 这一组判据参照 Dynamic NPC Scaling（Nexus 180306）的 [SkipFlags]。
	// 它们和 IsEligible() 里那些运行时判据的区别在于「稳定性」：
	// 定义层判据描述的是「这个 NPC 属于哪一类」，招募/复活/读档都不会改变结论，
	// 而且只是一次内存里的位测试，没有额外开销。
	bool Manager::PassBaseFilters(RE::Actor* a_actor) const
	{
		const auto& s = Config::Get().Data();
		auto* base = a_actor->GetActorBase();
		if (!base) {
			return false;
		}

		// ACBS 标志位 —— TESActorBaseData 上的谓词，TESNPC 可直接调
		if (s.skipUnique && base->IsUnique()) { return false; }
		if (s.skipEssential && base->IsEssential()) { return false; }
		if (s.skipProtected && base->IsProtected()) { return false; }
		if (s.skipInvulnerable && base->IsInvulnerable()) { return false; }
		if (s.skipSummonable && base->IsSummonable()) { return false; }

		// 「模板 / 预设」的 NPC：它们自身不持有真正的数值，改它们的 base AV 多半白改
		// （数值来自 TPLT 指向的模板，或只是捏脸预设）。
		if (s.skipTemplated && (base->UsesTemplate() || base->IsPreset())) {
			return false;
		}

		// 随从：定义层派系判定（比 IsPlayerTeammate() 更彻底）。
		// 只在「排除随从」打开、且派系判定没被关掉时才生效 ——
		// 这样菜单里实时切换开关立刻起作用，不需要重进游戏。
		if (s.excludeFollowers && s.followerCheckByFaction) {
			for (auto* f : followerFactions_) {
				if (f && base->IsInFaction(f)) {
					if (s.verboseLog) {
						logger::info("[EEA] {} 因定义层派系被排除（随从）。", a_actor->GetDisplayFullName());
					}
					return false;
				}
			}
		}

		return true;
	}

	bool Manager::IsEligible(RE::Actor* a_actor) const
	{
		const auto& s = Config::Get().Data();

		if (!s.master) {
			return false;
		}
		if (!a_actor || a_actor->IsDead()) {
			return false;
		}
		if (a_actor->IsDisabled()) {
			return false;
		}
		if (a_actor->GetFormType() != RE::FormType::NPC || !a_actor->GetActorBase()) {
			return false;
		}

		if (a_actor->GetLevel() < static_cast<std::uint16_t>(std::max(0, s.minLevel))) {
			return false;
		}

		// ---- 玩家：无条件排除（除非显式打开 bAffectPlayer）----
		// 位置很讲究：排在**定义层过滤器之前**。过滤器的开关不应该影响「谁是玩家」这件事，
		// 否则用户关掉 bSkipUnique 之类的时候，玩家就会被漏进来。
		// 但它仍然受 minLevel 约束 —— 保持与旧版一致，不悄悄改变已有行为。
		if (IsPlayerActor(a_actor)) {
			return s.affectPlayer;
		}

		// ---- 定义层过滤（NPC_ 记录自身）----
		if (!PassBaseFilters(a_actor)) {
			return false;
		}

		// 龙类是否排除在属性强化之外 —— 原版 ESP 就是在整条法术条件链里排掉了龙，
		// 所以原版「龙不受影响」并不是被别的龙类 MOD 覆盖，而是它自己的设计。
		if (s.excludeDragons && (IsDragon(a_actor) || IsDragonClass(a_actor))) {
			return false;
		}

		// 随从 / 召唤物排除。
		// 原版「不含友军」版用的是 IsHostileToActor(Player)==1 且 GetShouldHelp(Player)==0，
		// 也就是「是敌人、而且不会帮玩家」；下面用运行时的队友 / 召唤判据来近似，
		// 注意 IsPlayerTeammate() 只认「此刻已被招募」的随从。
		if (s.excludeFollowers && a_actor->IsPlayerTeammate()) {
			return false;
		}
		if (s.excludeSummons && a_actor->IsSummoned()) {
			return false;
		}

		// 至少要有一个属性是开着的，否则不参与
		bool any = false;
		for (const auto& a : s.attrs) {
			if (a.enabled) {
				any = true;
				break;
			}
		}
		return any || (s.bossEnabled && std::abs(s.bossScale) > kEps);
	}

	// ---------------------------------------------------------------- AV 施加 / 回滚

	float Manager::SetAVTotal(RE::Actor* a_actor, RE::ActorValue a_av, float a_old, float a_new)
	{
		if (!a_actor || a_av == RE::ActorValue::kNone) {
			return a_old;
		}

		// (1) 幂等：数值没变就完全不动引擎。
		//     这是原版最大的失效点 —— 它每次刷新都无条件 ModAV + SetScale。
		const float net = a_new - a_old;
		if (std::abs(net) < kEps) {
			return a_old;
		}

		const auto& s = Config::Get().Data();

		// (2) 生命 / 法力 / 体力：先记下「当前值占上限的比例」。
		//     写完之后按同比例放回去，于是：
		//       · 首次强化  → 满血单位仍是满血（外观"变强了"）
		//       · 残血单位  → 仍按原比例（绝不会借机回满血）
		//       · 战斗中改设置 → 百分比不动（这就是"回满血"被根治的机制）
		const bool vital = (a_av == RE::ActorValue::kHealth ||
		                    a_av == RE::ActorValue::kMagicka ||
		                    a_av == RE::ActorValue::kStamina);

		const float maxBefore = a_actor->GetPermanentActorValue(a_av);

		float ratio = 1.0F;
		if (vital && s.preserveCurrentRatio && maxBefore > kEps) {
			ratio = std::clamp(a_actor->GetActorValue(a_av) / maxBefore, 0.0F, 1.0F);
		}

		// (3) 按净差写入「上限」。
		//
		//     CommonLibSSE-NG 3.7.0 里 ActorValueOwner 暴露的 AV 写接口只有两个：
		//       ModActorValue(ActorValue, float)                            —— 改「永久修正」
		//       RestoreActorValue(ACTOR_VALUE_MODIFIER, ActorValue, float)   —— 改指定桶
		//     2 参数的 ModActorValue 就是 Papyrus 的 ModAV（"permanently alters the
		//     actor value"），也正是原作者脚本用的那个；它写的正是
		//     GetPermanentActorValue 读的那一侧 —— 所以下面能立刻回读校验。
		//
		//     注意这里读的是 GetPermanentActorValue（上限侧），而不是 GetActorValue ——
		//     对生命而言后者返回的是「当前血量」，拿它前后相减会算出错误的增量，
		//     进而导致每帧重复叠加。
		a_actor->ModActorValue(a_av, net);
		const float maxAfter = a_actor->GetPermanentActorValue(a_av);
		const float actual = maxAfter - maxBefore;

		// (4) 把当前值拉回「原比例 × 新上限」。
		//
		//     这里**没有** RestoreAV(av, max) 这类调用 —— 那正是原版
		//     「进出门 / 区域重载 / 读档后敌人回满血」的根因。
		//     我们只把当前值挪到目标；挪不动就保持原样，绝不无条件拉满。
		//
		//     实现走 kTemporary 桶：它只影响当前值、不改变 GetPermanentActorValue，
		//     也不像 kDamage 桶在 0 处有夹取，正反两个方向都能走。
		//     写完回读校验一次：若引擎的参数方向与预期相反（回读离目标更远），
		//     立刻补一次反向偏移 —— 这样不依赖对参数符号的猜测。
		if (vital && s.preserveCurrentRatio && maxAfter > kEps) {
			const float desired = maxAfter * ratio;
			const float cur0 = a_actor->GetActorValue(a_av);
			const float diff = desired - cur0;
			if (std::abs(diff) > kEps) {
				a_actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av, diff);
				const float cur1 = a_actor->GetActorValue(a_av);
				if (std::abs(desired - cur1) > std::abs(diff) + kEps) {
					// 方向相反：这一步实际走成了 -diff，补 -2*diff 才回得到目标
					a_actor->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av, -2.0F * diff);
				}
				if (s.verboseLog) {
					logger::info("[EEA] {} 当前值按比例调整：{:.1f} → {:.1f}（目标 {:.1f}，新上限 {:.1f}）",
						a_actor->GetDisplayFullName(), cur0, a_actor->GetActorValue(a_av), desired, maxAfter);
				}
			}
		}

		return a_old + actual;
	}

	void Manager::RevertWith(RE::Actor* a_actor, const Applied& a_entry)
	{
		for (std::size_t i = 0; i < kAttrCount; ++i) {
			if (std::abs(a_entry.delta[i]) > kEps) {
				SetAVTotal(a_actor, ToActorValue(static_cast<Attr>(i)), a_entry.delta[i], 0.0F);
			}
		}
		for (std::size_t i = 0; i < 5; ++i) {
			if (std::abs(a_entry.schoolDelta[i]) > kEps) {
				SetAVTotal(a_actor, kSchoolPowerMod[i], a_entry.schoolDelta[i], 0.0F);
			}
		}
		if (a_entry.perkAdded && magicPerk_) {
			a_actor->RemovePerk(magicPerk_);
		}
		if (a_entry.scaleSaved) {
			// 用「保存下来的原始体型」还原，而不是原版的 SetScale(1.0) ——
			// 那个 1.0 会把本来就不是 1.0 倍的单位（比如某些 Boss）改成标准体型。
			SetActorScale(a_actor, a_entry.origScale);
		}
	}

	void Manager::RevertEntry(RE::Actor* a_actor, Applied& a_entry)
	{
		RevertWith(a_actor, a_entry);
		a_entry = Applied{};
	}

	// ---------------------------------------------------------------- 主逻辑

	void Manager::ApplyTo(RE::Actor* a_actor, bool a_forced)
	{
		const auto& s = Config::Get().Data();
		const auto fid = a_actor->GetFormID();

		// 上一份存档留下的记录。
		// 属性上限写在 kPermanent 桶（也就是原版 Papyrus 的 ModAV），数值仍写在角色身上、
		// 随存档一起保存了，所以这里是「认领」而不是「撤销」——
		// 撤销会把已经被打到残血的单位直接扣死。
		// （这正是原版读档后「残血敌人被拉满」的镜像问题，两边都得避开。）
		//
		// 注：这个版本没有「只写临时桶」的选项 —— 引擎未暴露带桶的 ModActorValue，
		// 上限一律走永久修正（见 SetAVTotal 的说明），所以这里无条件认领。
		if (auto it = pendingRevert_.find(fid); it != pendingRevert_.end()) {
			applied_[fid] = it->second;
			pendingRevert_.erase(it);
		}

		// Combat 模式（iTriggerMode=1）：只处理「战斗驱动的」与「此前已强化过的」单位，
		// 全新单位留到它进入战斗那一刻 —— 这样 minLevel 可以放心设成 1。
		if (s.triggerMode == 1 && !a_forced && applied_.find(fid) == applied_.end()) {
			return;
		}

		if (!IsEligible(a_actor)) {
			// 以前强化过、现在不该强化了（关开关 / 等级不够 / 变成尸鬼）→ 撤销
			if (auto it = applied_.find(fid); it != applied_.end()) {
				RevertEntry(a_actor, it->second);
				applied_.erase(it);
			}
			return;
		}

		auto [it, inserted] = applied_.try_emplace(fid);
		auto& entry = it->second;

		const bool boss = s.bossEnabled && IsBoss(a_actor);
		const int  pcLevel = RE::PlayerCharacter::GetSingleton() ? static_cast<int>(RE::PlayerCharacter::GetSingleton()->GetLevel()) : 0;
		const int  eLevel = static_cast<int>(a_actor->GetLevel());

		const bool firstApply = inserted && s.logFirstApply;

		// ---- 四个「直接映射 AV」的属性 ----
		for (std::size_t i = 0; i < kAttrCount; ++i) {
			const auto attr = static_cast<Attr>(i);
			if (attr == Attr::kMagic) {
				continue;  // 法术强度单独处理（可能牵动 5 个 AV 或一条 Perk）
			}

			const auto& cfg = s.attrs[i];
			const auto  av = ToActorValue(attr);

			float target = 0.0F;
			if (cfg.enabled) {
				const float base = a_actor->GetBaseActorValue(av);
				target = ComputeExtra(base, cfg, pcLevel, eLevel, boss);
			}

			// 幂等：数值没变就完全不碰引擎（SetAVTotal 第一行就会返回）
			if (std::abs(target - entry.delta[i]) > kEps) {
				// 按净差一次写入，不做「先全撤再重加」，因此不会出现血量的中间跳变
				entry.delta[i] = SetAVTotal(a_actor, av, entry.delta[i], target);

				if (s.traceValues) {
					logger::info("[EEA] {} {} : base={:.2f} 上限={:.2f} 当前={:.2f} want={:+.2f} 实记={:+.2f}",
						a_actor->GetDisplayFullName(), kAttrKey[i],
						a_actor->GetBaseActorValue(av),
						a_actor->GetPermanentActorValue(av),
						a_actor->GetActorValue(av),
						target, entry.delta[i]);
				}
			}
		}

		// ---- 法术强度 ----
		{
			const auto& cfg = s.attrs[static_cast<std::size_t>(Attr::kMagic)];
			const bool usePerk = (s.magicMode == MagicMode::kPerk || s.magicMode == MagicMode::kBoth) && magicPerk_;
			const bool usePowerMod = (s.magicMode == MagicMode::kPowerMod || s.magicMode == MagicMode::kBoth);

			const float base = a_actor->GetBaseActorValue(RE::ActorValue::kLockpickingPowerModifier);
			const float target = cfg.enabled ? ComputeExtra(base, cfg, pcLevel, eLevel, boss) : 0.0F;

			if (usePerk) {
				if (cfg.enabled && target > 0.0F && !a_actor->HasPerk(magicPerk_)) {
					a_actor->AddPerk(magicPerk_, 0);
					entry.perkAdded = true;
				} else if ((!cfg.enabled || target <= 0.0F) && entry.perkAdded) {
					a_actor->RemovePerk(magicPerk_);
					entry.perkAdded = false;
				}
			}

			// 原作者的机制：Perk 读取 LockpickingPowerMod 作为法术强度倍率
			if (usePerk) {
				float& slot = entry.delta[static_cast<std::size_t>(Attr::kMagic)];
				if (std::abs(target - slot) > kEps) {
					slot = SetAVTotal(a_actor, RE::ActorValue::kLockpickingPowerModifier, slot, target);
				}
			}

			// 纯 DLL 路线：直接加成五大法术学派 PowerModifier
			for (std::size_t i = 0; i < 5; ++i) {
				float want = 0.0F;
				if (usePowerMod && cfg.enabled) {
					const float schoolBase = a_actor->GetBaseActorValue(kSchoolPowerMod[i]);
					want = ComputeExtra(schoolBase, cfg, pcLevel, eLevel, boss);
				}
				if (std::abs(want - entry.schoolDelta[i]) > kEps) {
					entry.schoolDelta[i] = SetAVTotal(a_actor, kSchoolPowerMod[i], entry.schoolDelta[i], want);
				}
			}
		}

		// ---- Boss 体型 ----
		const bool skipScale = s.skipBossScaleOnDragons && IsDragon(a_actor);
		const bool wantScale = boss && std::abs(s.bossScale) > kEps && !skipScale;

		if (wantScale) {
			if (!entry.scaleSaved) {
				// 只在第一次记录原始体型；之后读档也能精确还原。
				// 原版的 bug 就在这里：OrinScale 是 Papyrus 脚本的局部变量，读档实例重建
				// 会丢值（退回 1.0），于是「1.0 + 增量」每读一次档就再加一遍，体型越读越大。
				entry.origScale = a_actor->GetScale();
				entry.scaleSaved = true;
			}
			const float target = entry.origScale + s.bossScale;
			// 幂等：与自己上次写进去的值比，而不是与引擎回读比 ——
			// 写入口径与回读口径未必一致，用回读判断会导致次次都写（原版的掉帧主因）。
			if (std::abs(entry.curScale - target) > 0.005F) {
				SetActorScale(a_actor, target);
				entry.curScale = target;
			}
		} else if (entry.scaleSaved) {
			SetActorScale(a_actor, entry.origScale);
			entry.scaleSaved = false;
			entry.curScale = entry.origScale;
		}

		if (firstApply) {
			logger::info("[EEA] 首次强化 {}（等级 {}{}）", a_actor->GetDisplayFullName(), eLevel, boss ? "，Boss" : "");
		}
		if (s.verboseLog) {
			logger::info("[EEA] {} 处理完毕，剩余队列 {}", a_actor->GetDisplayFullName(), queue_.size());
		}
	}

	// ---------------------------------------------------------------- 批量操作

	void Manager::RevertAll()
	{
		std::size_t n = 0;
		for (auto& [fid, entry] : applied_) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
			if (actor && actor->GetFormID() == fid) {
				RevertEntry(actor, entry);
				++n;
			}
		}
		applied_.clear();

		// 待回滚的也一并丢掉（否则下次它们加载时又会被撤一次）
		pendingRevert_.clear();

		queue_.clear();
		queued_.clear();
		forced_.clear();
		logger::info("[EEA] 已还原 {} 个单位。", n);
	}

	void Manager::RevertOne(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		const auto fid = a_actor->GetFormID();
		if (auto it = applied_.find(fid); it != applied_.end()) {
			RevertEntry(a_actor, it->second);
			applied_.erase(it);
			logger::info("[EEA] 已还原 {}。", a_actor->GetDisplayFullName());
		}
	}

	void Manager::RefreshAll()
	{
		queue_.clear();
		queued_.clear();
		QueueAllLoaded();
		logger::info("[EEA] 已请求全量刷新。");
	}

	// ---------------------------------------------------------------- 序列化

	void Manager::SerializeSave(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc->OpenRecord(kSerializationRecord, kSerializationVersion)) {
			logger::error("[EEA] 写入存档记录失败。");
			return;
		}
		const auto count = static_cast<std::uint32_t>(applied_.size());
		a_intfc->WriteRecordData(count);
		for (const auto& [fid, entry] : applied_) {
			a_intfc->WriteRecordData(fid);
			a_intfc->WriteRecordData(entry);  // 纯 POD，可直接写
		}
		logger::info("[EEA] 已把 {} 条强化记录写进存档。", count);
	}

	void Manager::SerializeLoad(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;

		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != kSerializationRecord) {
				continue;
			}
			if (version != kSerializationVersion) {
				logger::warn("[EEA] 存档记录版本 {} 不匹配（当前 {}），已跳过。", version, kSerializationVersion);
				continue;
			}

			std::uint32_t count = 0;
			if (!a_intfc->ReadRecordData(count)) {
				break;
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				RE::FormID oldID = 0;
				Applied entry{};
				if (!a_intfc->ReadRecordData(oldID) || !a_intfc->ReadRecordData(entry)) {
					break;
				}
				RE::FormID newID = 0;
				if (a_intfc->ResolveFormID(oldID, newID) && newID != 0) {
					// 不立刻回滚：目标可能还没加载，等它进队列时再撤
					pendingRevert_[newID] = entry;
				}
			}
		}
		logger::info("[EEA] 从存档读回 {} 条待回滚记录。", pendingRevert_.size());
	}

	// ---------------------------------------------------------------- 菜单预览

	float Manager::ComputeExtra(float a_baseAV, const AttrSettings& a_cfg, int a_playerLevel, int a_enemyLevel, bool a_boss)
	{
		const int pc = a_cfg.usePlayerLevel ? a_playerLevel : 0;
		const int el = a_cfg.useEnemyLevel ? a_enemyLevel : 0;
		const float pct = static_cast<float>(a_cfg.basePercent + pc + el) * 0.01F;
		const float mult = a_cfg.extraMult * (a_boss ? a_cfg.bossMult : 1.0F);
		return std::ceil(a_baseAV * pct * mult);
	}

	float Manager::PreviewPercent(const AttrSettings& a_cfg, int a_playerLevel, int a_enemyLevel, bool a_boss)
	{
		const int pc = a_cfg.usePlayerLevel ? a_playerLevel : 0;
		const int el = a_cfg.useEnemyLevel ? a_enemyLevel : 0;
		const float pct = static_cast<float>(a_cfg.basePercent + pc + el) * 0.01F;
		const float mult = a_cfg.extraMult * (a_boss ? a_cfg.bossMult : 1.0F);
		return pct * mult * 100.0F;
	}
}
