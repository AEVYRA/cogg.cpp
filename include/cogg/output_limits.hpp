#pragma once
#include <cstddef>
namespace cogg::output_limits {
// Core maxima. Adapters may impose smaller transport/context limits explicitly.
inline constexpr std::size_t memory_writes = 64;
inline constexpr std::size_t notes = 8;
inline constexpr std::size_t sources = 32;
}
