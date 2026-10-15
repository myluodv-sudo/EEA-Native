#include "Menu.h"

#include "Config.h"
#include "EnhancementManager.h"

// 单头文件接入 SKSE Menu Framework：
// 里面把整套 Dear ImGui API 以 ImGuiMCP:: 命名空间 + GetProcAddress 的形式导出，
// 所以本工程不需要把 imgui 加进 vcpkg，也不需要自己写 D3D11 / 输入 hook。
#include "SKSEMenuFramework.h"

namespace EEA::Menu
{
	namespace
	{
		int g_previewPC = 30;
		int g_previewEnemy = 50;

		// ImGui 的 ID 是按窗口作用域的，多个 Tab 里同名控件会撞 ID。
		// 用 "显示文本##唯一后缀" 处理：## 之后不显示。
		std::string L(const char* a_text, const char* a_key)
		{
			return std::string{ a_text } + "##" + a_key;
		}

		void RenderAttrTab(Settings& a_s, std::size_t a_index)
		{
			auto& a = a_s.attrs[a_index];
			const auto* k = kAttrKey[a_index];
			bool changed = false;

			changed |= ImGuiMCP::Checkbox(L("启用该属性强化", k).c_str(), &a.enabled);

			ImGuiMCP::SeparatorText("计算参数");

			ImGuiMCP::SetNextItemWidth(360.0F);
			changed |= ImGuiMCP::SliderInt(L("基础百分比（每 1 点 = 额外 1%）", k).c_str(),
				&a.basePercent, 0, 1000, "%d");

			changed |= ImGuiMCP::Checkbox(L("叠加玩家等级", k).c_str(), &a.usePlayerLevel);
			changed |= ImGuiMCP::Checkbox(L("叠加敌人等级", k).c_str(), &a.useEnemyLevel);

			ImGuiMCP::SetNextItemWidth(360.0F);
			changed |= ImGuiMCP::SliderFloat(L("额外倍率", k).c_str(), &a.extraMult, 0.0F, 20.0F, "%.1f");

			ImGuiMCP::SetNextItemWidth(360.0F);
			changed |= ImGuiMCP::SliderFloat(L("Boss 倍率", k).c_str(), &a.bossMult, 0.0F, 20.0F, "%.1f");

			ImGuiMCP::SeparatorText("实时预览");

			ImGuiMCP::SetNextItemWidth(140.0F);
			ImGuiMCP::SliderInt(L("预览用玩家等级", k).c_str(), &g_previewPC, 1, 100, "%d");
			ImGuiMCP::SameLine();
			ImGuiMCP::SetNextItemWidth(140.0F);
			ImGuiMCP::SliderInt(L("预览用敌人等级", k).c_str(), &g_previewEnemy, 1, 100, "%d");

			const float normal = Manager::PreviewPercent(a, g_previewPC, g_previewEnemy, false);
			const float bossy = Manager::PreviewPercent(a, g_previewPC, g_previewEnemy, true);

			ImGuiMCP::Text("普通敌人额外加成：+%.1f%%", normal);
			ImGuiMCP::Text("Boss 额外加成：+%.1f%%（Boss 倍率 %.1f）", bossy, a.bossMult);
			if (a_index == static_cast<std::size_t>(Attr::kMagic)) {
				ImGuiMCP::Text("法术强度按倍率生效：法术伤害/增益幅度 ×%.2f", 1.0F + normal * 0.01F);
			}

			if (changed) {
				Config::Get().MarkDirty();
			}
		};

		void RenderMainTab(Settings& a_s)
		{
			bool changed = false;

			ImGuiMCP::TextUnformatted("EEA_Native —— 动态增强敌人属性（原生 SKSE 插件版）");
			ImGuiMCP::TextUnformatted("配置保存在 Data\\SKSE\\Plugins\\EEA_Native.ini，改这里等于改那个文件。");

			ImGuiMCP::SeparatorText("总开关");
			changed |= ImGuiMCP::Checkbox("启用本模组##master", &a_s.master);
			changed |= ImGuiMCP::Checkbox("也强化玩家本人##pc", &a_s.affectPlayer);

			ImGuiMCP::SetNextItemWidth(360.0F);
			changed |= ImGuiMCP::SliderInt("生效的最低等级##minlv", &a_s.minLevel, 1, 80, "%d");
			ImGuiMCP::TextUnformatted("（原版把 GetLevel > 9 烘焙在 ESP 里改不了，这里可以随便调）");

			ImGuiMCP::SeparatorText("Boss");
			changed |= ImGuiMCP::Checkbox("Boss 额外属性加成##boss", &a_s.bossEnabled);
			ImGuiMCP::SetNextItemWidth(360.0F);
			changed |= ImGuiMCP::SliderFloat("Boss 体型增量##scale", &a_s.bossScale, -0.10F, 2.00F, "%.2f");
			changed |= ImGuiMCP::Checkbox("龙类不加体型（避免速度暴走）##dragon", &a_s.skipBossScaleOnDragons);

			ImGuiMCP::TextUnformatted("Boss 判定依据（三级，精确度递减）：");
			changed |= ImGuiMCP::Checkbox("① 原版地点参照类型（最精确）##bloc", &a_s.bossUseLocRefType);
			ImGuiMCP::SameLine();
			changed |= ImGuiMCP::Checkbox("② 记录名含 Boss##bedid", &a_s.bossUseEditorID);
			ImGuiMCP::SameLine();
			changed |= ImGuiMCP::Checkbox("③ 战斗 AI 数据##bai", &a_s.bossUseAIData);
			ImGuiMCP::TextUnformatted("实测：② 命中 9.8%；③ 命中 42%（会让 Boss 倍率失去意义），故 ③ 默认关闭。");

			ImGuiMCP::SeparatorText("生效范围");
			changed |= ImGuiMCP::Checkbox("排除随从##follow", &a_s.excludeFollowers);
			ImGuiMCP::SameLine();
			changed |= ImGuiMCP::Checkbox("改用定义层派系判定##followfac", &a_s.followerCheckByFaction);
			changed |= ImGuiMCP::Checkbox("排除召唤物##summon", &a_s.excludeSummons);
			ImGuiMCP::TextUnformatted("默认两个都不排除 —— 与原 MOD「含友军」版一致。");
			ImGuiMCP::TextUnformatted("勾了「排除随从」后，右边的派系判定会更彻底：它连「将来可能被招募」的 NPC 也算随从。");

			ImGuiMCP::SeparatorText("定义层过滤器（按 NPC 记录类型排除）");
			ImGuiMCP::TextUnformatted("判据落在 ActorBase（NPC_ 记录）的 ACBS 标志上：结论稳定，不随招募/复活/读档变化。");
			changed |= ImGuiMCP::Checkbox("独特 NPC##skipuniq", &a_s.skipUnique);
			ImGuiMCP::SameLine();
			changed |= ImGuiMCP::Checkbox("重要 NPC##skipess", &a_s.skipEssential);
			ImGuiMCP::SameLine();
			changed |= ImGuiMCP::Checkbox("受保护##skippro", &a_s.skipProtected);
			changed |= ImGuiMCP::Checkbox("无敌##skipinv", &a_s.skipInvulnerable);
			ImGuiMCP::SameLine();
			changed |= ImGuiMCP::Checkbox("可被召唤##skipsum", &a_s.skipSummonable);
			ImGuiMCP::SameLine();
			changed |= ImGuiMCP::Checkbox("模板/捏脸预设##skiptpl", &a_s.skipTemplated);
			ImGuiMCP::TextUnformatted("实测整合包 35,905 条 NPC_：独特 19.4% / 重要 6.5% / 受保护 2.0% /");
			ImGuiMCP::TextUnformatted("无敌 2.4% / 可召唤 7.7% / 用模板 2.0%。默认全关（谁都不排除）。");
			ImGuiMCP::TextUnformatted("⚠ 玩家本体那条 NPC_ 也带「独特」标志 —— 但插件排除玩家与此无关，");
			ImGuiMCP::TextUnformatted("   玩家永远被排除，除非上面勾了「也强化玩家本人」。");

			if (changed) {
				Config::Get().MarkDirty();
			}
		}

		void RenderAboutTab(Settings& a_s)
		{
			auto& cfg = Config::Get();
			auto& mgr = Manager::Get();

			ImGuiMCP::SeparatorText("运行状态");
			ImGuiMCP::Text("框架版本：%.2f", SKSEMenuFramework::GetMenuFrameworkVersion());
			ImGuiMCP::Text("当前已强化：%llu 个单位",
				static_cast<unsigned long long>(mgr.TrackedCount()));
			ImGuiMCP::Text("待处理队列：%llu",
				static_cast<unsigned long long>(mgr.QueueSize()));
			ImGuiMCP::Text("法术强度增益载体（Perk 记录）：%s",
				mgr.IsPerkAvailable() ? "已接入" : "未找到 —— 建议改用「纯 DLL 内置」方式");
			ImGuiMCP::Text("Boss 判定②（记录名 EditorID）：%s",
				mgr.IsEditorIDReadable() ? "可用" : "不可用 —— 需装 Native EditorID Fix");
			ImGuiMCP::Text("法术强度实现方式：%s",
				a_s.magicMode == MagicMode::kPerk ? "插件 Perk（原作者机制）" :
				a_s.magicMode == MagicMode::kPowerMod ? "纯 DLL 内置（不需要 ESP）" : "两者同时使用");

			ImGuiMCP::SeparatorText("操作");
			if (ImGuiMCP::Button("立即刷新全部##refresh")) {
				// 强制：无视「仅进入战斗时才强化」，把已加载单位全部重算一遍，
				// 并在屏幕上弹「正在更新敌人属性 / 敌人属性已更新」（可关）。
				mgr.RefreshAll(/*a_force=*/true);
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("还原所有单位##revert")) {
				mgr.RevertAll();
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("保存到 INI##save")) {
				cfg.SaveToDisk();
			}
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("运行判定诊断##census")) {
				mgr.RunFilterCensus();
			}
			ImGuiMCP::TextUnformatted("诊断结果写进 EEA_Native.log：每个过滤器各拒了多少个单位。");

			{
				const auto& c = mgr.LastCensus();
				if (c.scanned > 0) {
					ImGuiMCP::SeparatorText("上次判定诊断");
					ImGuiMCP::Text("采样：%llu 个已加载单位", static_cast<unsigned long long>(c.scanned));
					ImGuiMCP::Text("通过全部判定：%llu 个", static_cast<unsigned long long>(c.eligible));
					for (const auto& [why, n] : c.reasons) {
						ImGuiMCP::Text("　被拒：%s × %llu", why.c_str(),
							static_cast<unsigned long long>(n));
					}
					for (const auto& e : c.examples) {
						ImGuiMCP::Text("　例：%s", e.c_str());
					}
				}
			}

			ImGuiMCP::SeparatorText("屏幕提示");
			if (ImGuiMCP::Checkbox("改设置时弹提示##notify", &a_s.notifyOnRefresh)) {
				Config::Get().MarkDirty();
			}
			ImGuiMCP::TextUnformatted("与原版一致：改任意设置后在屏幕上弹「正在更新敌人属性」，");
			ImGuiMCP::TextUnformatted("全部重算完成后再弹「敌人属性已更新」。");

			ImGuiMCP::SeparatorText("性能");
			ImGuiMCP::SetNextItemWidth(360.0F);
			if (ImGuiMCP::SliderInt("每帧处理单位数##budget", &a_s.processPerTick, 1, 64, "%d")) {
				Config::Get().MarkDirty();
			}
			if (ImGuiMCP::Checkbox("详细日志##verbose", &a_s.verboseLog)) {
				Config::Get().MarkDirty();
			}

			ImGuiMCP::SeparatorText("触发模式");
			{
				// triggerMode == 2（Both）只在 INI 里设置，菜单里给出最常用的两档
				bool combatOnly = (a_s.triggerMode == 1);
				if (ImGuiMCP::Checkbox("仅进入战斗时才强化##combatonly", &combatOnly)) {
					a_s.triggerMode = combatOnly ? 1 : 0;
					Config::Get().MarkDirty();
				}
				ImGuiMCP::Text("勾选后可以把最低等级一路调到 1 —— 只有真打起来的单位才会被处理，");
				ImGuiMCP::Text("不会因为满地小动物而让队列堆积（原版作者当年就是因此不建议 1 级生效）。");
				ImGuiMCP::TextUnformatted("注意：本开关只管「自动触发」。在这里改任何设置都会立刻重算全部单位。");
			}

			ImGuiMCP::SeparatorText("血量处理");
			if (ImGuiMCP::Checkbox("保持当前血量百分比##ratio", &a_s.preserveCurrentRatio)) {
				Config::Get().MarkDirty();
			}
			ImGuiMCP::Text("开启时：残血敌人强化后仍按原比例（不会借机回血）；");
			ImGuiMCP::Text("战斗中修改设置也不会把敌人回满血。");
		}
	}

	void __stdcall Render()
	{
		auto& cfg = Config::Get();
		auto& s = cfg.Data();

		if (ImGuiMCP::BeginTabBar("EEA_Tabs")) {
			if (ImGuiMCP::BeginTabItem("总开关")) {
				RenderMainTab(s);
				ImGuiMCP::EndTabItem();
			}
			for (std::size_t i = 0; i < kAttrCount; ++i) {
				if (ImGuiMCP::BeginTabItem(kAttrNameCN[i])) {
					RenderAttrTab(s, i);
					ImGuiMCP::EndTabItem();
				}
			}
			if (ImGuiMCP::BeginTabItem("关于/性能")) {
				RenderAboutTab(s);
				ImGuiMCP::EndTabItem();
			}
			ImGuiMCP::EndTabBar();
		}

		if (cfg.AnyDirty()) {
			// 有改动 → 落盘 + 让所有单位重算
			cfg.ConsumeAllDirty();
			cfg.ConsumeGlobalDirty();
			cfg.SaveToDisk();
			Manager::Get().RefreshAll();
		}
	}

	void Register()
	{
		auto& cfg = Config::Get();
		if (!cfg.Data().menuEnabled) {
			logger::info("[EEA] 菜单已在配置里关闭。");
			return;
		}

		if (!SKSEMenuFramework::IsInstalled()) {
			logger::warn("[EEA] 未检测到 SKSE Menu Framework —— 游戏内菜单不可用。"
						 "功能完全不受影响，改 EEA_Native.ini 即可，或安装该框架后重进游戏。");
			return;
		}

		// key 决定分区在控制面板里的父级层级
		SKSEMenuFramework::Model::Internal::key = "EEA_Native";
		SKSEMenuFramework::AddSectionItem(cfg.Data().menuSection, Render);

		logger::info("[EEA] 菜单已注册：模组控制面板 → EEA_Native / {}（框架版本 {:.2f}）",
			cfg.Data().menuSection, SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
