#pragma once

#include "PCH.h"

namespace EEA::Menu
{
	// 把页面注册进 SKSE Menu Framework 的「模组控制面板」
	void Register();

	// 渲染函数（框架要求 __stdcall + 无参）
	void __stdcall Render();
}
