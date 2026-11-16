#pragma once
// The Switch clock rules (host/switch_clocks.cpp applies them; tests/switch_clocks_logic_test.cpp checks them on the PC).
// Without an overclock kit the system takes rates only up to its stock tables, so a request is read back and lowered to the
// stock maximum when the kit rate did not take.

#include <cstdint>
#include <cstdlib>

namespace ps2::switch_clocks {

constexpr std::uint32_t kKitCpuHz = 2'091'000'000u;    // common Erista kit values, below the kits' upper ends
constexpr std::uint32_t kKitGpuHz = 1'228'800'000u;
constexpr std::uint32_t kStockCpuHz = 1'785'000'000u;  // the highest rates of the stock tables
constexpr std::uint32_t kStockGpuHz = 921'600'000u;

struct ClockRequest {
    bool leave_alone = false;  // PS2_CLOCKS=0
    std::uint32_t cpu_hz = kKitCpuHz;
    std::uint32_t gpu_hz = kKitGpuHz;
    bool custom = false;  // PS2_CLOCKS=<cpu MHz>,<gpu MHz>
};

// PS2_CLOCKS: unset or empty = kit targets; "0" = leave the clocks alone; "<cpu MHz>,<gpu MHz>" = exactly those; anything
// else = kit targets (never a zero or negative rate).
inline ClockRequest parse_clock_env(const char *value) {
    ClockRequest r;
    if (!value || !*value) return r;
    if (value[0] == '0' && value[1] == '\0') {
        r.leave_alone = true;
        return r;
    }
    char *end = nullptr;
    const long cpu = std::strtol(value, &end, 10);
    if (end == value || *end != ',') return r;
    const char *second = end + 1;
    const long gpu = std::strtol(second, &end, 10);
    // within the rates the Erista tables (with a kit) have: a crawling console from a typo helps nobody
    if (end == second || *end != '\0' || cpu < 612 || cpu > 2397 || gpu < 76 || gpu > 1536) return r;
    r.cpu_hz = static_cast<std::uint32_t>(cpu) * 1'000'000u;
    r.gpu_hz = static_cast<std::uint32_t>(gpu) * 1'000'000u;
    r.custom = true;
    return r;
}

// After requesting `requested_hz` and reading back `readback_hz`: 0 = done (it took, within 1 MHz of table rounding, or
// nothing better to try); otherwise the next rate to request (the stock maximum, when a rate above it did not take).
inline std::uint32_t next_request(std::uint32_t requested_hz, std::uint32_t readback_hz, std::uint32_t stock_max_hz) {
    if (readback_hz + 1'000'000u >= requested_hz) return 0;
    return requested_hz > stock_max_hz ? stock_max_hz : 0;
}

}  // namespace ps2::switch_clocks
