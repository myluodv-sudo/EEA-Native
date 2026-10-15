#include "PCH.h"

#include <Windows.h>

#include "Config.h"
#include "EnhancementManager.h"
#include "Menu.h"

namespace
{
	void SetupLog()
	{
		// 日志写到 Data/SKSE/Plugins/EEA_Native.log —— 出问题时把这个文件发出来即可
		wchar_t buffer[MAX_PATH]{};
		const auto len = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		std::filesystem::path p{ std::wstring{ buffer, len } };
		p = p.parent_path() / "Data" / "SKSE" / "Plugins" / "EEA_Native.log";

		try {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(p.string(), true);
			auto log = std::make_shared<spdlog::logger>("EEA", std::move(sink));
			log->set_level(spdlog::level::trace);
			log->flush_on(spdlog::level::info);
			spdlog::set_default_logger(std::move(log));
			spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		} catch (...) {
			// 日志失败不能拖垮插件
		}
	}

	// 只依赖 TESObjectLoadedEvent：单位进场景时入队。
	// 刻意不 hook 主循环 —— 不需要 REL::ID，跨版本更稳。
	class ObjectLoadedSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESObjectLoadedEvent* a_event,
			RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
		{
			if (a_event && a_event->loaded) {
				// 字段名是 formID（CommonLibSSE-NG 的 TESObjectLoadedEvent 定义），
				// 不是 objectFormID。
				EEA::Manager::Get().OnObjectLoaded(a_event->formID);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// 战斗事件：单位一旦进入战斗就入队。
	// 配合 [Perf] iTriggerMode=1，就能做到「只有真正打起来的单位才被强化」——
	// 于是 minLevel 可以放心设成 1，而不会被满地小动物撑爆队列。
	// 这正是原作者想做但受 Papyrus 限制做不到的事（他自己的回复：1 级会让小动物也生效，
	// 脚本太多不建议）。插件用事件驱动 + 每帧预算，代价模型完全不同。
	class CombatSink : public RE::BSTEventSink<RE::TESCombatEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::TESCombatEvent* a_event,
			RE::BSTEventSource<RE::TESCombatEvent>*) override
		{
			if (!a_event || !a_event->actor) {
				return RE::BSEventNotifyControl::kContinue;
			}
			// newState 是 stl::enumeration<ACTOR_COMBAT_STATE, uint32_t>，
			// .get() 取出的就是枚举本身，所以直接和枚举常量比 —— 不要转成 uint32_t 比，
			// 那个方向 scoped enum 不隐式转换，编译不过。
			if (a_event->newState.get() != RE::ACTOR_COMBAT_STATE::kCombat) {
				return RE::BSEventNotifyControl::kContinue;
			}
			if (auto* actor = a_event->actor->As<RE::Actor>()) {
				EEA::Manager::Get().Queue(actor, /*a_force=*/true);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			EEA::Manager::Get().OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			EEA::Manager::Get().OnPreLoadGame();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			EEA::Manager::Get().OnPostLoadGame();
			break;
		case SKSE::MessagingInterface::kNewGame:
			EEA::Manager::Get().OnNewGame();
			break;
		default:
			break;
		}
	}

	void OnSave(SKSE::SerializationInterface* a_intfc) { EEA::Manager::Get().SerializeSave(a_intfc); }
	void OnLoad(SKSE::SerializationInterface* a_intfc) { EEA::Manager::Get().SerializeLoad(a_intfc); }
	void OnRevert(SKSE::SerializationInterface*) { EEA::Manager::Get().OnPreLoadGame(); }
}

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 0, 0 },
	.Name = "EEA_Native",
	.Author = "Native port of Mokeine's EnhanceEnemyAttributes",
	.SupportEmail = "",
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SetupLog();

	logger::info("[EEA] ============================================");
	logger::info("[EEA] EEA_Native 启动，游戏版本 {}", a_skse->RuntimeVersion().string());

	SKSE::Init(a_skse);

	// 配置优先：缺失时自动生成一份带注释的模板
	EEA::Config::Get().LoadFromDisk();

	// 引擎接口提示：CommonLibSSE-NG 3.7.0 只暴露了两个 AV 写接口 ——
	//   ModActorValue(ActorValue, float)                          改「永久修正」
	//   RestoreActorValue(ACTOR_VALUE_MODIFIER, ActorValue, float) 改指定桶
	// 没有「带桶的 ModActorValue」，所以上限增量只能走永久修正。
	// 这一项留着是为了 INI 兼容，打开也只是打条日志。
	if (EEA::Config::Get().Data().useTemporaryModifier) {
		logger::warn("[EEA] bUseTemporaryModifier=1 本次不生效：本版本引擎接口未暴露"
					 "「带桶的 ModActorValue」，属性上限一律走永久修正（kPermanent）。");
	}

	// 菜单（框架不在时自动跳过，功能不受影响）
	EEA::Menu::Register();

	if (auto* ser = SKSE::GetSerializationInterface()) {
		ser->SetUniqueID('EEAN');
		ser->SetSaveCallback(OnSave);
		ser->SetLoadCallback(OnLoad);
		ser->SetRevertCallback(OnRevert);
	} else {
		logger::error("[EEA] 取不到序列化接口：强化记录无法持久化（功能仍可用，但读档会重新计算）。");
	}

	if (auto* msg = SKSE::GetMessagingInterface()) {
		msg->RegisterListener(OnMessage);
	} else {
		logger::error("[EEA] 取不到消息接口。");
	}

	if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
		static ObjectLoadedSink sink;
		holder->AddEventSink<RE::TESObjectLoadedEvent>(&sink);
		static CombatSink combatSink;
		holder->AddEventSink<RE::TESCombatEvent>(&combatSink);
		logger::info("[EEA] 已监听对象加载 / 战斗事件。");
	} else {
		logger::error("[EEA] 取不到 ScriptEventSourceHolder。");
	}

	{
		const auto mode = EEA::Config::Get().Data().triggerMode;
		const char* desc = (mode == 1) ? "仅战斗触发（Combat）"
		                 : (mode == 2) ? "加载 + 战斗触发（Both）"
		                               : "加载即强化（Load，原版行为）";
		logger::info("[EEA] 触发模式 = {}，最低生效等级 = {}，每帧处理上限 = {}。",
			desc, EEA::Config::Get().Data().minLevel, EEA::Config::Get().Data().processPerTick);
	}

	logger::info("[EEA] 初始化完成。");
	return true;
}
