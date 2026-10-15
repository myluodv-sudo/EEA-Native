#pragma once

// CommonLibSSE-NG
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// spdlog（由 CommonLibSSE 依赖链提供）
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace logger = SKSE::log;

using namespace std::literals;
