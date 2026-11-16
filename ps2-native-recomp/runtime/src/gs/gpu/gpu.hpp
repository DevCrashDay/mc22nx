#pragma once

#include "../gs.hpp"

#include <cstdint>
#include <vector>

namespace ps2::gs::gpu {

enum class Mode { off, mirror, primary };

struct DisplaySource {
    std::uint32_t fbp, fbw, psm, dbx, dby, width, height;
    bool halved;
    DisplayOverlay overlay;
};

struct SurfaceTarget {
    enum Kind { X11, Wayland, Canvas, Win32, Metal } kind;
    void *display;
    std::uint64_t window;
    void *surface;
    const char *selector;
    std::uint32_t width, height;
};

extern Mode g_current_mode;  // set by init(); read through mode() on every shape, so it is not a call
inline Mode mode() { return g_current_mode; }
bool init(Mode m);
void draw(const State &s, const Vertex *v, int count);
// Set by gs.cpp (PS2_TEX_REMAP with :g) around one draw: its texture's alpha is taken from the texel's green (make_setup, flag 1 << 17).
extern bool g_alpha_from_green;
void image_begin(const State &s);
void image_data(const std::uint8_t *data, std::size_t bytes);
void local_copy(const State &s);
void load_clut(std::uint64_t tex0, std::uint64_t texclut);
void flush();
void texflush();
void wait_idle();
void download(std::uint8_t *memory, std::uint32_t offset = 0, std::uint32_t bytes = kMemorySize);
bool download_async(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes, std::uint32_t serial);
bool finish_download(std::uint32_t serial);
void read_state(std::vector<std::uint32_t> &vram, std::uint32_t &scale, std::vector<std::uint32_t> &clut);
void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut);
bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool display_async(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool attach_surface(const SurfaceTarget &target);
void resize_surface(std::uint32_t width, std::uint32_t height);
bool has_surface();
bool present(const DisplaySource &src);
// True colour (PS2_TRUE_COLOR=1): a game that draws its frame in 32 bits and copies it 1:1 into a 16-bit buffer to show it
// (Midnight Club II) loses three bits per channel there, and the GS's dither is not drawn here. true_color_copy, at the first
// sprite of such a copy, keeps the 32-bit source as it is then; the screen shows that picture instead of the 16-bit buffer
// while the buffer shown is the one the copy went to. true_color_forget: something else wrote into that buffer (blocks).
void true_color_copy(std::uint32_t dst_fbp, std::uint32_t src_tbp, std::uint32_t src_tbw, std::uint32_t src_psm);
void true_color_forget(std::uint32_t block);
bool true_color_on();
double busy_ms();
void stats();

}
