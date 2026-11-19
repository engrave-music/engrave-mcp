// SPDX-License-Identifier: GPL-3.0-only
// Stable third-party and standard library precompiled header for Ring 2 (engrave_mcp).
// Project headers are intentionally excluded so host header edits never invalidate the PCH.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <glaze/json/generic.hpp>
#include <proxy/proxy.h>
