#include "Config.h"

#include <Windows.h>

namespace EEA
{
	namespace
	{
		std::string Trim(std::string_view a_in)
		{
			const auto begin = a_in.find_first_not_of(" \t\r\n");
			if (begin == std::string_view::npos) {
				return {};
			}
			const auto end = a_in.find_last_not_of(" \t\r\n");
			return std::string{ a_in.substr(begin, end - begin + 1) };
		}

		std::string ToLower(std::string a_in)
		{
			std::transform(a_in.begin(), a_in.end(), a_in.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_in;
		}

		bool ToBool(const std::string& a_in)
		{
			const auto v = ToLower(Trim(a_in));
			return v == "1" || v == "true" || v == "yes" || v == "on";
		}

		int ToInt(const std::string& a_in)
		{
			try {
				return std::stoi(Trim(a_in));
			} catch (...) {
				return 0;
			}
		}

		float ToFloat(const std::string& a_in)
		{
			try {
				return std::stof(Trim(a_in));
			} catch (...) {
				return 0.0F;
			}
		}

		std::vector<std::string> SplitList(const std::string& a_in)
		{
			std::vector<std::string> out;
			std::stringstream ss{ a_in };
			std::string item;
			while (std::getline(ss, item, ',')) {
				auto t = Trim(item);
				if (!t.empty()) {
					out.push_back(std::move(t));
				}
			}
			return out;
		}

		std::string JoinList(const std::vector<std::string>& a_in)
		{
			std::string out;
			for (std::size_t i = 0; i < a_in.size(); ++i) {
				if (i) {
					out += ", ";
				}
				out += a_in[i];
			}
			return out;
		}

		constexpr const char* kBool = "1";
		constexpr const char* kFalse = "0";

		inline const char* B(bool a_v) { return a_v ? kBool : kFalse; }
	}

	std::string Config::GetIniPath() const
	{
		// 用进程 exe 路径推游戏根目录，绕开工作目录不确定的问题
		wchar_t buffer[MAX_PATH]{};
		const auto len = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		std::filesystem::path gameRoot{ std::wstring{ buffer, len } };
		gameRoot = gameRoot.parent_path();

		auto p = gameRoot / "Data" / "SKSE" / "Plugins" / "EEA_Native.ini";
		return p.string();
	}

	Config& Config::Get()
	{
		static Config instance;
		return instance;
	}

	void Config::ApplyDefaults()
	{
		settings_ = Settings{};

		// 与原版 MCM 默认值保持一致
		settings_.attrs[static_cast<std::size_t>(Attr::kHealth)].enabled = true;
		settings_.attrs[static_cast<std::size_t>(Attr::kMagic)].enabled = false;
		for (auto& a : settings_.attrs) {
			a.basePercent = 20;
			a.usePlayerLevel = true;
			a.useEnemyLevel = true;
			a.extraMult = 1.0F;
			a.bossMult = 2.0F;
		}
	}

	void Config::ParseLine(const std::string& a_section, const std::string& a_key, const std::string& a_value)
	{
		auto& s = settings_;

		if (a_section == "Main") {
			if (a_key == "bEEASwitchMOD") { s.master = ToBool(a_value); }
			else if (a_key == "bEEASwitchBoss") { s.bossEnabled = ToBool(a_value); }
			else if (a_key == "fEEABossScale") { s.bossScale = ToFloat(a_value); }
			else if (a_key == "minLevel") { s.minLevel = ToInt(a_value); }
			else if (a_key == "bAffectPlayer") { s.affectPlayer = ToBool(a_value); }
			return;
		}

		// 每个属性页：[HP] [MP] [SP] [ATK] [MGC]
		for (std::size_t i = 0; i < kAttrCount; ++i) {
			if (a_section != kAttrKey[i]) {
				continue;
			}
			auto& a = s.attrs[i];
			const std::string suffix = kAttrKey[i];

			if (a_key == "bEEASwitch" + suffix || a_key == "bEEASwitch" + suffix + ":Main") {
				a.enabled = ToBool(a_value);
			} else if (a_key == "iExtrapct" + suffix) {
				a.basePercent = std::clamp(ToInt(a_value), 0, 10000);
			} else if (a_key == "bPCLevelSwitch" + suffix) {
				a.usePlayerLevel = ToBool(a_value);
			} else if (a_key == "bELevelSwitch" + suffix) {
				a.useEnemyLevel = ToBool(a_value);
			} else if (a_key == "fExtraMult" + suffix) {
				a.extraMult = std::max(0.0F, ToFloat(a_value));
			} else if (a_key == "fBossMult" + suffix) {
				a.bossMult = std::max(0.0F, ToFloat(a_value));
			}
			return;
		}

		if (a_section == "Filter") {
			if (a_key == "bExcludeFollowers") { s.excludeFollowers = ToBool(a_value); }
			else if (a_key == "bExcludeSummons") { s.excludeSummons = ToBool(a_value); }
			else if (a_key == "bFollowerCheckByFaction") { s.followerCheckByFaction = ToBool(a_value); }
			else if (a_key == "bSkipUnique") { s.skipUnique = ToBool(a_value); }
			else if (a_key == "bSkipEssential") { s.skipEssential = ToBool(a_value); }
			else if (a_key == "bSkipProtected") { s.skipProtected = ToBool(a_value); }
			else if (a_key == "bSkipInvulnerable") { s.skipInvulnerable = ToBool(a_value); }
			else if (a_key == "bSkipSummonable") { s.skipSummonable = ToBool(a_value); }
			else if (a_key == "bSkipTemplated") { s.skipTemplated = ToBool(a_value); }
			else if (a_key == "bSkipBossScaleOnDragons") { s.skipBossScaleOnDragons = ToBool(a_value); }
			else if (a_key == "bExcludeDragons") { s.excludeDragons = ToBool(a_value); }
			else if (a_key == "sDragonKeywords") { s.dragonKeywords = SplitList(a_value); }
			else if (a_key == "sDragonClasses") { s.dragonClasses = SplitList(a_value); }
			else if (a_key == "bBossUseLocRefType") { s.bossUseLocRefType = ToBool(a_value); }
			else if (a_key == "sLocRefTypeBoss") { s.locRefTypeBoss = Trim(a_value); }
			else if (a_key == "bBossUseEditorID") { s.bossUseEditorID = ToBool(a_value); }
			else if (a_key == "sBossEdidPatterns") { s.bossEdidPatterns = SplitList(a_value); }
			else if (a_key == "bBossUseAIData") { s.bossUseAIData = ToBool(a_value); }
			else if (a_key == "iBossMinAggression") { s.bossMinAggression = std::clamp(ToInt(a_value), 0, 3); }
			else if (a_key == "iBossMinConfidence") { s.bossMinConfidence = std::clamp(ToInt(a_value), 0, 4); }
			else if (a_key == "sBossKeywords") { s.bossKeywords = SplitList(a_value); }
			else if (a_key == "sBossRaces") { s.bossRaces = SplitList(a_value); }
			else if (a_key == "sBossNpcs") { s.bossNpcs = SplitList(a_value); }
			return;
		}

		if (a_section == "Magic") {
			if (a_key == "sMode") {
				const auto v = ToLower(Trim(a_value));
				if (v == "perk") { s.magicMode = MagicMode::kPerk; }
				else if (v == "powermod") { s.magicMode = MagicMode::kPowerMod; }
				else if (v == "both") { s.magicMode = MagicMode::kBoth; }
			} else if (a_key == "sPerkPlugin") { s.magicPerkPlugin = Trim(a_value); }
			else if (a_key == "sPerkEditorID") { s.magicPerkEditorID = Trim(a_value); }
			return;
		}

		if (a_section == "Menu") {
			if (a_key == "bEnabled") { s.menuEnabled = ToBool(a_value); }
			else if (a_key == "sSectionName") { s.menuSection = Trim(a_value); }
			return;
		}

		if (a_section == "Perf") {
			if (a_key == "iProcessPerTick") { s.processPerTick = std::clamp(ToInt(a_value), 1, 64); }
			else if (a_key == "iTriggerMode") { s.triggerMode = std::clamp(ToInt(a_value), 0, 2); }
			else if (a_key == "bNotifyOnRefresh") { s.notifyOnRefresh = ToBool(a_value); }
			else if (a_key == "bPreserveCurrentRatio") { s.preserveCurrentRatio = ToBool(a_value); }
			else if (a_key == "bUseTemporaryModifier") { s.useTemporaryModifier = ToBool(a_value); }
			else if (a_key == "bVerboseLog") { s.verboseLog = ToBool(a_value); }
			else if (a_key == "bLogFirstApply") { s.logFirstApply = ToBool(a_value); }
			return;
		}

		if (a_section == "Debug") {
			if (a_key == "bTraceValues") { s.traceValues = ToBool(a_value); }
		}
	}

	void Config::LoadFromDisk()
	{
		ApplyDefaults();

		const std::string path = GetIniPath();

		std::ifstream file{ path };
		if (!file.is_open()) {
			logger::info("[Config] 未找到 {} —— 使用默认值并生成模板", path);
			SaveToDisk();
			return;
		}

		std::string line;
		std::string section;
		while (std::getline(file, line)) {
			auto trimmed = Trim(line);
			if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
				continue;
			}
			if (trimmed.front() == '[' && trimmed.back() == ']') {
				section = Trim(std::string_view{ trimmed }.substr(1, trimmed.size() - 2));
				continue;
			}
			const auto eq = trimmed.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			ParseLine(section, Trim(std::string_view{ trimmed }.substr(0, eq)),
				Trim(std::string_view{ trimmed }.substr(eq + 1)));
		}

		logger::info("[Config] 已读取 {}", path);
	}

	void Config::SaveToDisk()
	{
		const auto& s = settings_;
		const std::string path = GetIniPath();

		std::ofstream out{ path, std::ios::trunc };
		if (!out.is_open()) {
			logger::error("[Config] 无法写入 {}", path);
			return;
		}

		out << "; ============================================================\n";
		out << ";  EEA_Native —— 动态增强敌人属性（原生 SKSE 插件版）\n";
		out << ";  键名与原作者 MCM 的 settings.ini 完全一致，可直接把旧文件改名沿用。\n";
		out << ";  游戏内按 DELETE 打开「模组控制面板」即可实时修改，无需手改本文件。\n";
		out << "; ============================================================\n\n";

		out << "[Main]\n";
		out << "; 总开关\n";
		out << "bEEASwitchMOD=" << B(s.master) << "\n";
		out << "; 生效的最低等级（原版把 GetLevel > 9 烘焙在 ESP 里，这里可调）\n";
		out << "minLevel=" << s.minLevel << "\n";
		out << "; 是否也强化玩家本人\n";
		out << "bAffectPlayer=" << B(s.affectPlayer) << "\n";
		out << "; Boss 额外加成开关 / Boss 体型增量\n";
		out << "bEEASwitchBoss=" << B(s.bossEnabled) << "\n";
		out << "fEEABossScale=" << s.bossScale << "\n";

		for (std::size_t i = 0; i < kAttrCount; ++i) {
			const auto& a = s.attrs[i];
			const auto* k = kAttrKey[i];
			out << "\n[" << k << "]\n";
			out << "; " << kAttrNameCN[i] << "强化开关\n";
			out << "bEEASwitch" << k << "=" << B(a.enabled) << "\n";
			out << "; 基础百分比：每 1 点 = 额外 1%\n";
			out << "iExtrapct" << k << "=" << a.basePercent << "\n";
			out << "; 叠加玩家等级 / 叠加敌人等级\n";
			out << "bPCLevelSwitch" << k << "=" << B(a.usePlayerLevel) << "\n";
			out << "bELevelSwitch" << k << "=" << B(a.useEnemyLevel) << "\n";
			out << "; 额外倍率 / Boss 倍率\n";
			out << "fExtraMult" << k << "=" << a.extraMult << "\n";
			out << "fBossMult" << k << "=" << a.bossMult << "\n";
		}

		out << "\n[Filter]\n";
		out << "; 排除随从 / 排除召唤物（默认都包含 —— 与原 MOD「含友军」版一致）\n";
		out << "bExcludeFollowers=" << B(s.excludeFollowers) << "\n";
		out << "; 排除随从时是否同时用「定义层派系」判定（更彻底）\n";
		out << "; 1 = 连「将来可能被招募」的 NPC 也排除，不受当前招募状态影响（推荐）\n";
		out << "; 用到的派系：PotentialFollowerFaction 0x0005C84D / CurrentFollowerFaction 0x0005C84E\n";
		out << "bFollowerCheckByFaction=" << B(s.followerCheckByFaction) << "\n";
		out << "bExcludeSummons=" << B(s.excludeSummons) << "\n";
		out << "\n; ---- 定义层过滤器（参照 Dynamic NPC Scaling 的 [SkipFlags]）----\n";
		out << "; 判据全部落在 ActorBase（NPC_ 记录）的 ACBS 标志位上，描述「这个 NPC 是哪一类」，\n";
		out << "; 因此结论稳定、不随招募/复活/读档变化、开销近乎为零。默认全部 0（谁都不排除）。\n";
		out << "; 位值以 CommonLibSSE-NG 的 RE::ACTOR_BASE_DATA::Flag 为准（UESP 网页把\n";
		out << "; PCLevelMult 标成 0x40 是错的，正确是 0x80）。\n";
		out << "; 实测本机整合包 35,905 条 NPC_ 的分布：Unique 19.4% / Essential 6.5% /\n";
		out << "; Protected 2.0% / Invulnerable 2.4% / Summonable 7.7% / UseTemplate 2.0%\n";
		out << "; 注意：玩家本体那条 NPC_（0x00000007）就带 Unique —— 所以别用这个开关来排除玩家，\n";
		out << "; 插件对玩家的排除是独立且无条件的（除非 bAffectPlayer=1）。\n";
		out << "bSkipUnique=" << B(s.skipUnique) << "\n";
		out << "bSkipEssential=" << B(s.skipEssential) << "\n";
		out << "bSkipProtected=" << B(s.skipProtected) << "\n";
		out << "bSkipInvulnerable=" << B(s.skipInvulnerable) << "\n";
		out << "bSkipSummonable=" << B(s.skipSummonable) << "\n";
		out << "bSkipTemplated=" << B(s.skipTemplated) << "\n";
		out << "; 是否把龙类排除在「属性强化」之外（原版 ESP 就是排除的：\n";
		out << "; HasKeyword(ActorTypeDragon)==0 且 GetIsClass(EncClassDragon)==0）\n";
		out << "bExcludeDragons=" << B(s.excludeDragons) << "\n";
		out << "; Boss 体型增量是否跳过龙类（龙的速度会被体型放大，原作者也建议关掉）\n";
		out << "bSkipBossScaleOnDragons=" << B(s.skipBossScaleOnDragons) << "\n";
		out << "sDragonKeywords=" << JoinList(s.dragonKeywords) << "\n";
		out << "sDragonClasses=" << JoinList(s.dragonClasses) << "\n";
		out << "\n; ---- Boss 判定（三级，精确度递减）----\n";
		out << "; (1) 原版机制：HasRefType(LocationRefType \"Boss\")，即 Skyrim.esm 里 EDID 为 Boss、\n";
		out << ";     FormID 0x000130F7 的 LocationRefType。它标记的是「某地点的某个放置参照物」，\n";
		out << ";     不是 Actor 关键字，也跟「FormID 里带 BOSS」无关。最精确。\n";
		out << "bBossUseLocRefType=" << B(s.bossUseLocRefType) << "\n";
		out << "sLocRefTypeBoss=" << s.locRefTypeBoss << "\n";
		out << "; (2) EditorID 含指定片段（CK 里的记录名，如 dunHalldirsBoss）。\n";
		out << ";     实测整合包 35,904 个 NPC_ 命中 3,508 个（9.8%），精度中等。\n";
		out << ";     若日志显示取不到 EditorID，需安装 Native EditorID Fix 之类的插件。\n";
		out << "bBossUseEditorID=" << B(s.bossUseEditorID) << "\n";
		out << "sBossEdidPatterns=" << JoinList(s.bossEdidPatterns) << "\n";
		out << "; (3) AI 数据：Aggression>=iBossMinAggression 且 Confidence>=iBossMinConfidence\n";
		out << ";     实测命中 15,097 个（42%）—— 大量 mod 给所有战斗单位都设了\n";
		out << ";     VeryAggressive + Foolhardy。会让 Boss 倍率失去意义，所以默认关闭。\n";
		out << ";     取值范围：Aggression 0-3，Confidence 0-4。\n";
		out << "bBossUseAIData=" << B(s.bossUseAIData) << "\n";
		out << "iBossMinAggression=" << s.bossMinAggression << "\n";
		out << "iBossMinConfidence=" << s.bossMinConfidence << "\n";
		out << "; (4) 显式名单兜底。注意 vanilla 没有 ActorTypeBoss 这种关键字，故关键字默认留空。\n";
		out << "sBossKeywords=" << JoinList(s.bossKeywords) << "\n";
		out << "sBossRaces=" << JoinList(s.bossRaces) << "\n";
		out << "sBossNpcs=" << JoinList(s.bossNpcs) << "\n";

		out << "\n[Magic]\n";
		out << "; 法术强度实现方式： Perk = 用 ESP 里的 EEA_EnhanceMGCPerk（原作者机制）\n";
		out << ";                   PowerMod = 纯 DLL，直接加成五大法术学派 PowerModifier\n";
		out << ";                   Both = 两者都开\n";
		out << "sMode=";
		switch (s.magicMode) {
		case MagicMode::kPowerMod: out << "PowerMod"; break;
		case MagicMode::kBoth:     out << "Both"; break;
		default:                   out << "Perk"; break;
		}
		out << "\n";
		out << "sPerkPlugin=" << s.magicPerkPlugin << "\n";
		out << "sPerkEditorID=" << s.magicPerkEditorID << "\n";

		out << "\n[Menu]\n";
		out << "bEnabled=" << B(s.menuEnabled) << "\n";
		out << "sSectionName=" << s.menuSection << "\n";

		out << "\n[Perf]\n";
		out << "; 每帧最多处理几个单位（跨帧摊平，防止进地牢瞬间卡顿）\n";
		out << "iProcessPerTick=" << s.processPerTick << "\n";
		out << "\n; ---- 触发模式 ----\n";
		out << "; 0 = Load   单位一加载就强化（原版行为）\n";
		out << "; 1 = Combat 只在进入战斗时强化。这样 minLevel 可以放到 1 也不会被\n";
		out << ";           满地小动物撑爆队列（作者当年拒绝「1 级生效」的原因就是这个）\n";
		out << "; 2 = Both   两者都触发\n";
		out << "; 注意：本开关只影响「自动触发」；在菜单里改任何设置都会强制重算一遍已加载单位。\n";
		out << "iTriggerMode=" << s.triggerMode << "\n";
		out << "\n; 改设置时是否在屏幕上弹提示（原版行为：正在更新敌人属性 / 敌人属性已更新）\n";
		out << "bNotifyOnRefresh=" << B(s.notifyOnRefresh) << "\n";
		out << "\n; 改上限时是否保持当前值百分比（仅生命/法力/体力）。\n";
		out << "; 1 = 战斗中改设置不会回满血（推荐）；0 = 交给引擎\n";
		out << "bPreserveCurrentRatio=" << B(s.preserveCurrentRatio) << "\n";
		out << "; 0 = kPermanent（与原版 ModAV 同桶，读档后可认领，推荐）\n";
		out << "; 1 = kTemporary（不写存档，但能否抬高上限未验证）\n";
		out << "bUseTemporaryModifier=" << B(s.useTemporaryModifier) << "\n";
		out << "bVerboseLog=" << B(s.verboseLog) << "\n";
		out << "bLogFirstApply=" << B(s.logFirstApply) << "\n";

		out << "\n[Debug]\n";
		out << "bTraceValues=" << B(s.traceValues) << "\n";

		out.flush();
		logger::info("[Config] 已写入 {}", path);
	}
}
