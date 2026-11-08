#pragma once

#include "SKSE/Impl/PCH.h"
#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>

#ifndef NDEBUG
#include <spdlog/sinks/msvc_sink.h>
#endif

#define DLLEXPORT __declspec(dllexport)

using namespace std::literals;
using namespace REL::literals;
