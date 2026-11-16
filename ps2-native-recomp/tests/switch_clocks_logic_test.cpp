// PC test of host/switch_clocks_logic.hpp (the Switch clock request rules). Build and run:
//   clang++ -std=c++20 -I host tests/switch_clocks_logic_test.cpp -o clk_test && ./clk_test
#undef NDEBUG
#include "switch_clocks_logic.hpp"

#include <cassert>
#include <initializer_list>

int main() {
    using namespace ps2::switch_clocks;

    const ClockRequest kit = parse_clock_env(nullptr);
    assert(!kit.leave_alone && kit.cpu_hz == kKitCpuHz && kit.gpu_hz == kKitGpuHz && !kit.custom);
    assert(parse_clock_env("").cpu_hz == kKitCpuHz);

    assert(parse_clock_env("0").leave_alone);

    const ClockRequest custom = parse_clock_env("1785,921");
    assert(!custom.leave_alone && custom.custom && custom.cpu_hz == 1'785'000'000u && custom.gpu_hz == 921'000'000u);

    const ClockRequest lowest = parse_clock_env("612,76");  // the lowest rates the tables have
    assert(lowest.custom && lowest.cpu_hz == 612'000'000u && lowest.gpu_hz == 76'000'000u);

    for (const char *bad : {"abc", "2091", "2091,", "-5,10", "10,-5", "2091,1228x", "0,0", "1,1", "611,921", "1785,75", "2398,921", "1785,1537"}) {
        const ClockRequest r = parse_clock_env(bad);
        assert(!r.leave_alone && !r.custom && r.cpu_hz == kKitCpuHz && r.gpu_hz == kKitGpuHz);
    }

    assert(next_request(kKitCpuHz, kKitCpuHz, kStockCpuHz) == 0);
    assert(next_request(kKitCpuHz, 1'785'000'000u, kStockCpuHz) == kStockCpuHz);
    assert(next_request(kStockCpuHz, 1'020'000'000u, kStockCpuHz) == 0);
    assert(next_request(kKitGpuHz, 1'228'000'000u, kStockGpuHz) == 0);
    assert(next_request(kKitGpuHz, 0, kStockGpuHz) == kStockGpuHz);
    return 0;
}
