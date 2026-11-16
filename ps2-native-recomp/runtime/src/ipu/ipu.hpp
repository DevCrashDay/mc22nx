#pragma once

#include <cstdint>

namespace ps2::state {
class Stream;
}

namespace ps2::ipu {

void state(ps2::state::Stream &s);

void reset();
std::uint32_t read32(std::uint32_t a);
void write32(std::uint32_t a, std::uint32_t v);
void write_fifo(const std::uint8_t *qword);
void read_fifo(std::uint8_t *qword);
void dma_started(int channel);
void stats();
// The frame of the last picture decode command (IDEC/BDEC): a video, or an animated picture, is playing.
std::uint64_t last_decode_frame();

}
