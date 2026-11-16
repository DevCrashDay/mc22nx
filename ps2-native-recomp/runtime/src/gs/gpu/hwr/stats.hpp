#pragma once
// PS2_HWR_STATS=1 (stage 0 of the GPU renderer): what the game draws and how often memory traffic would force the GPU
// renderer to copy its pictures back or reload them. Counters only; printed every 300 frames as [hwr-stats] lines.

#include "../../gs.hpp"

#include <cstdint>

namespace ps2::gs::hwr {

bool stats_on();
void stats_shape(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count);
// kind: "download", "local_copy", "upload", "display"; address in 64-word blocks (TBP/DBP units)
void stats_sync(const char *kind, std::uint32_t block, std::uint32_t bytes);
void stats_frame();

}  // namespace ps2::gs::hwr
