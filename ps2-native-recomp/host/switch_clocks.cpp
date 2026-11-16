#include "switch_clocks.hpp"

#include "switch_clocks_logic.hpp"

#include <switch.h>

#include <cstdio>
#include <cstdlib>

namespace {

using namespace ps2::switch_clocks;

enum Module { kCpu, kGpu, kEmc, kModules };
constexpr PcvModuleId kIds[kModules] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
constexpr PcvModule kPcv[kModules] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};
constexpr u32 kHandheldGpuHz = 384'000'000u;  // the system's usual handheld GPU rate for games (restore after a mode change)
constexpr u32 kDockedGpuHz = 768'000'000u;    // and the docked one

bool g_on = false;
bool g_clkrst = false;
ClkrstSession g_session[kModules]{};
u32 g_saved[kModules]{};
bool g_saved_docked = false;
ClockRequest g_request;
u32 g_cpu_set = 0;  // the rates that took (0 = not set)
u32 g_gpu_set = 0;
bool g_was_docked = false;
u64 g_next_tick = 0;

bool get_rate(int m, u32 *hz) {
    return R_SUCCEEDED(g_clkrst ? clkrstGetClockRate(&g_session[m], hz) : pcvGetClockRate(kPcv[m], hz));
}

void set_rate(int m, u32 hz) {
    if (g_clkrst) clkrstSetClockRate(&g_session[m], hz);
    else pcvSetClockRate(kPcv[m], hz);
}

bool docked() { return appletGetOperationMode() == AppletOperationMode_Console; }

// Requests `want`, reads it back, and steps down to the stock maximum when a kit rate did not take: returns what took.
u32 apply(int m, u32 want, u32 stock_max) {
    set_rate(m, want);
    u32 got = 0;
    get_rate(m, &got);
    if (const u32 next = next_request(want, got, stock_max)) {
        set_rate(m, next);
        get_rate(m, &got);
    }
    return got;
}

const char *kind(u32 got, u32 stock_max) {
    if (g_request.custom) return "custom";
    if (got > stock_max + 1'000'000u) return "kit";
    return got + 1'000'000u >= stock_max ? "stock" : "system";
}

}  // namespace

void switch_clocks_start() {
    g_request = parse_clock_env(std::getenv("PS2_CLOCKS"));
    if (g_request.leave_alone) {
        std::fprintf(stderr, "[clocks] PS2_CLOCKS=0: left as the system set them\n");
        return;
    }
    Result rc = 0;
    if (hosversionAtLeast(8, 0, 0)) {
        g_clkrst = true;
        rc = clkrstInitialize();
        for (int m = 0; m < kModules && R_SUCCEEDED(rc); ++m) rc = clkrstOpenSession(&g_session[m], kIds[m], 3);
    } else {
        rc = pcvInitialize();
    }
    bool readable = R_SUCCEEDED(rc);
    for (int m = 0; m < kModules && readable; ++m) readable = get_rate(m, &g_saved[m]) && g_saved[m] != 0;
    if (!readable) {
        std::fprintf(stderr, "[clocks] unavailable (0x%X): clocks left alone\n", static_cast<unsigned>(rc));
        return;
    }
    g_saved_docked = g_was_docked = docked();
    g_cpu_set = apply(kCpu, g_request.cpu_hz, kStockCpuHz);
    g_gpu_set = apply(kGpu, g_request.gpu_hz, kStockGpuHz);  // docked and handheld alike (user's choice; more battery and heat in handheld)
    u32 gpu = 0, emc = 0;
    get_rate(kGpu, &gpu);
    get_rate(kEmc, &emc);
    std::fprintf(stderr, "[clocks] cpu %u MHz (%s), gpu %u MHz (%s), emc %u MHz (was cpu %u, gpu %u)\n", g_cpu_set / 1'000'000u,
                 kind(g_cpu_set, kStockCpuHz), gpu / 1'000'000u, kind(gpu, kStockGpuHz),
                 emc / 1'000'000u, g_saved[kCpu] / 1'000'000u, g_saved[kGpu] / 1'000'000u);
    g_on = true;
    g_next_tick = armGetSystemTick() + armGetSystemTickFreq();
}

void switch_clocks_tick() {
    if (!g_on || armGetSystemTick() < g_next_tick) return;
    g_next_tick = armGetSystemTick() + armGetSystemTickFreq();
    u32 cur = 0;
    // dock/undock resets the system's rates; a rate that never read back (0) is never set again
    if (g_cpu_set && get_rate(kCpu, &cur) && cur != g_cpu_set) set_rate(kCpu, g_cpu_set);
    if (g_gpu_set && get_rate(kGpu, &cur) && cur != g_gpu_set) set_rate(kGpu, g_gpu_set);
    const bool now_docked = docked();
    if (now_docked != g_was_docked) std::fprintf(stderr, "[clocks] %s: rates set again\n", now_docked ? "docked" : "handheld");
    g_was_docked = now_docked;
}

void switch_clocks_restore() {
    if (!g_on) return;
    g_on = false;
    set_rate(kCpu, g_saved[kCpu]);
    // the GPU rate saved at start belongs to the mode the game started in
    const bool now_docked = docked();
    set_rate(kGpu, now_docked == g_saved_docked ? g_saved[kGpu] : (now_docked ? kDockedGpuHz : kHandheldGpuHz));
}
