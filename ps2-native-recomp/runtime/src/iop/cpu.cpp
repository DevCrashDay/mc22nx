#include "iop.hpp"
#include "kernel.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ps2::iop {

alignas(64) std::uint8_t g_ram[kRamSize + 16];
Cpu g_cpu;

namespace {

inline std::int32_t simm(std::uint32_t w) { return static_cast<std::int16_t>(w & 0xFFFFu); }

std::vector<std::uint8_t> g_probes;

bool load_probes() {
    const char *spec = std::getenv("PS2_IOP_PROBE");
    if (!spec) return false;
    g_probes.assign(kRamSize / 32, 0);
    for (const char *p = spec; *p;) {
        char *end = nullptr;
        const std::uint32_t a = static_cast<std::uint32_t>(std::strtoul(p, &end, 16)) & (kRamSize - 1);
        g_probes[a >> 5] |= static_cast<std::uint8_t>(1u << ((a >> 2) & 7u));
        p = *end ? end + 1 : end;
    }
    return true;
}

// PS2_IOP_IDLE=<pc>:<addr>=<word>,<addr>!<word>,...: a polling loop of a thread that only waits. When the thread is at <pc> (the
// loop head) and every listed word in IOP memory is (=) or is not (!) the given value (its code words included, so another
// module there does not match), nothing it polls can change before the next event (an interrupt or another thread, which preempt
// it): the rest of its time slice is given up instead of running the loop. Midnight Club II's lowest-priority thread polls three
// request counts and the system time:
// 8B048:8B048=8E830000,8B20C=1440FF8E,946B4=0,946CC=0,946D4=0,9C8F4!2
struct IdleWord {
    std::uint32_t addr, value;
    bool equal;
};
std::uint32_t g_idle_pc = 0;
std::vector<IdleWord> g_idle_words;

void load_idle() {
    const char *spec = std::getenv("PS2_IOP_IDLE");
    if (!spec) return;
    char *end = nullptr;
    g_idle_pc = static_cast<std::uint32_t>(std::strtoul(spec, &end, 16));
    for (const char *p = *end == ':' ? end + 1 : end; *p;) {
        IdleWord w{static_cast<std::uint32_t>(std::strtoul(p, &end, 16)) & (kRamSize - 1), 0, *end == '='};
        if (*end != '=' && *end != '!') break;
        w.value = static_cast<std::uint32_t>(std::strtoul(end + 1, &end, 16));
        g_idle_words.push_back(w);
        p = *end ? end + 1 : end;
    }
    std::fprintf(stderr, "[iop] idle loop at 0x%08X, %zu conditions\n", g_idle_pc, g_idle_words.size());
}

bool idle_loop() {
    for (const IdleWord &w : g_idle_words)
        if ((ram32(w.addr & ~3u) == w.value) != w.equal) return false;
    return true;
}

void probe(const Cpu &c, std::uint32_t pc) {
    const kernel::Thread *t = kernel::current();
    std::fprintf(stderr, "[iop-probe] 0x%08X a0 %08X a1 %08X a2 %08X a3 %08X v0 %08X v1 %08X s0 %08X s1 %08X ra %08X thread %d time %llu\n", pc, c.r[4], c.r[5], c.r[6],
                 c.r[7], c.r[2], c.r[3], c.r[16], c.r[17], c.r[31], t ? t->id : 0, static_cast<unsigned long long>(kernel::now()));
    static const char *reg = std::getenv("PS2_IOP_PROBE_MEM");
    if (reg) {
        const std::uint32_t base = phys(c.r[std::atoi(reg) & 31]);
        if (base + 64 <= kRamSize) {
            std::fprintf(stderr, "[iop-probe]   r%s:", reg);
            for (std::uint32_t i = 0; i < 16; ++i) std::fprintf(stderr, " %08X", ram32(base + i * 4));
            std::fprintf(stderr, "\n");
        }
    }
}

void bad(Cpu &c, std::uint32_t pc, std::uint32_t w) {
    std::fprintf(stderr, "[iop] unknown instruction %08X at 0x%08X (ra 0x%08X)\n", w, pc, c.r[31]);
    kernel::fault("unknown instruction");
}

}

std::uint32_t fetch(std::uint32_t pc) {
    const std::uint32_t p = phys(pc);
    if (p < 0x00800000u) return ram32(p);
    return kernel::fetch_special(pc);
}

void execute_body(Cpu &c, std::int64_t &budget);

// PS2_IOP_TIME=1: wall time spent interpreting IOP code, per 120 frames (to tell an IOP-bound scene on a slow host)
void execute(Cpu &c, std::int64_t &budget) {
    static const bool timing = std::getenv("PS2_IOP_TIME") != nullptr;
    if (!timing) return execute_body(c, budget);
    static double ms = 0;
    static std::uint64_t instructions = 0, shown = 0;
    const std::int64_t before = budget;
    const auto t0 = std::chrono::steady_clock::now();
    execute_body(c, budget);
    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    instructions += static_cast<std::uint64_t>(before - std::max<std::int64_t>(budget, 0));
    if (const std::uint64_t f = ps2::frames_run() / 120; f != shown) {
        static auto wall0 = std::chrono::steady_clock::now();
        const auto wall1 = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double, std::milli>(wall1 - wall0).count() / 120.0;
        wall0 = wall1;
        double busy = -1;  // CPU time of the emulation thread (this one), where the system tells it
#ifdef __SWITCH__
        static std::uint64_t ticks0 = 0;
        std::uint64_t ticks = 0;
        if (R_SUCCEEDED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, ~0ull))) {
            busy = static_cast<double>(ticks - ticks0) * 1000.0 / 19200000.0 / 120.0;
            ticks0 = ticks;
        }
#endif
        std::fprintf(stderr, "[iop-time] frames to %llu: %.1f ms per frame interpreting, %.0f k instructions per frame; frame %.1f ms wall, emulation thread busy %.1f ms\n",
                     static_cast<unsigned long long>(f * 120), ms / 120.0, instructions / 120.0 / 1000.0, wall, busy);
        shown = f;
        ms = 0;
        instructions = 0;
    }
}

// PS2_IOP_HIST=<file>: how many instructions ran at each IOP address, written every 600 frames as "address count"
// lines, busiest first (to find the loops worth speeding up). <file>.entries counts how often control went to each address by a jump,
// a branch, a return or an interrupt (where translated regions can start) and <file>.ram the IOP memory, to translate.
std::vector<std::uint32_t> g_hist;
std::vector<std::uint32_t> g_entry;  // times control arrived at the address other than from the instruction before

// translated regions by physical word address (PS2_IOP_INTERPRET=1 leaves them unused)
std::vector<const Region *> g_region_at;

void write_hist() {
    static const char *path = std::getenv("PS2_IOP_HIST");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
    for (std::uint32_t i = 0; i < g_hist.size(); ++i)
        if (g_hist[i]) rows.emplace_back(g_hist[i], i * 4);
    std::sort(rows.rbegin(), rows.rend());
    if (std::FILE *f = std::fopen(path, "wb")) {
        for (const auto &[n, a] : rows) std::fprintf(f, "%08X %u\n", a, n);
        std::fclose(f);
    }
    if (std::FILE *f = std::fopen((std::string(path) + ".entries").c_str(), "wb")) {
        for (std::uint32_t i = 0; i < g_entry.size(); ++i)
            if (g_entry[i]) std::fprintf(f, "%08X %u\n", i * 4, g_entry[i]);
        std::fclose(f);
    }
    if (std::FILE *f = std::fopen((std::string(path) + ".ram").c_str(), "wb")) {  // the IOP memory beside it, to disassemble
        for (std::uint32_t a = 0; a < kRamSize; a += 4) {
            const std::uint32_t w = ram32(a);
            std::fwrite(&w, 4, 1, f);
        }
        std::fclose(f);
    }
}

void execute_body(Cpu &c, std::int64_t &budget) {
    static const bool probing = load_probes();
    static const bool hist = std::getenv("PS2_IOP_HIST") != nullptr;
    if (hist) {
        if (g_hist.empty()) g_hist.assign(kRamSize / 4, 0), g_entry.assign(kRamSize / 4, 0);
        static std::uint64_t written = 0;
        if (ps2::frames_run() / 600 != written) {
            written = ps2::frames_run() / 600;
            write_hist();
        }
    }
    // regions write RAM directly, so PS2_WATCH_IOP (which watches the interpreter's stores) needs them off
    static const bool regions = !hist && !probing && !g_region_at.empty() && !std::getenv("PS2_IOP_INTERPRET") && !std::getenv("PS2_WATCH_IOP");
    static std::uint32_t last_pc = 0;
    static const bool idle_on = (load_idle(), g_idle_pc != 0);
    for (;;) {
        if (idle_on && c.pc == g_idle_pc && c.npc == c.pc + 4 && idle_loop()) {
            budget = 0;  // waits for the next event (kernel.cpp run: the time moves on to it)
            return;
        }
        // a translated region where one starts (never in a delay slot: npc is the next instruction there)
        if (regions && c.npc == c.pc + 4) {
            const std::uint32_t p = phys(c.pc);
            if (p < kRamSize) {
                const Region *region = g_region_at[p >> 2];
                static int traced = std::getenv("PS2_IOP_REGION_TRACE") ? 40 : 0;
                const std::int64_t before = budget;
                const std::uint32_t from = c.pc;
                const bool ran = region && region->pc == c.pc && region->run(c, budget);
                if (region && traced > 0 && traced--)
                    std::fprintf(stderr, "[iop-region] 0x%08X ran %d budget %lld -> %lld, now pc 0x%08X npc 0x%08X switch %d\n", from, ran,
                                 static_cast<long long>(before), static_cast<long long>(budget), c.pc, c.npc, kernel::switch_pending());
                if (ran) {
                    if (kernel::switch_pending()) return;
                    continue;
                }
            }
        }
        if (!(budget-- > 0)) return;
        const std::uint32_t pc = c.pc;
        if (hist) {
            ++g_hist[(pc & (kRamSize - 1)) >> 2];
            if (pc != last_pc + 4) ++g_entry[(pc & (kRamSize - 1)) >> 2];
            last_pc = pc;
        }
        if (probing) {
            const std::uint32_t a = pc & (kRamSize - 1);
            if (g_probes[a >> 5] & (1u << ((a >> 2) & 7u))) probe(c, pc);
        }
        const std::uint32_t w = fetch(pc);
        c.pc = c.npc;
        c.npc += 4;
        const std::uint32_t rs = (w >> 21) & 31u, rt = (w >> 16) & 31u, rd = (w >> 11) & 31u, sa = (w >> 6) & 31u;
        std::uint32_t *r = c.r;
        switch (w >> 26) {
        case 0x00:
            switch (w & 63u) {
            case 0x00: r[rd] = r[rt] << sa; break;
            case 0x02: r[rd] = r[rt] >> sa; break;
            case 0x03: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> sa); break;
            case 0x04: r[rd] = r[rt] << (r[rs] & 31u); break;
            case 0x06: r[rd] = r[rt] >> (r[rs] & 31u); break;
            case 0x07: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> (r[rs] & 31u)); break;
            case 0x08: c.npc = r[rs]; break;
            case 0x09: { const std::uint32_t t = r[rs]; r[rd] = pc + 8; c.npc = t; break; }
            case 0x0C:
                kernel::syscall(c, pc);
                break;
            case 0x0D:
                std::fprintf(stderr, "[iop] break at 0x%08X\n", pc);
                break;
            case 0x0F: break;
            case 0x10: r[rd] = c.hi; break;
            case 0x11: c.hi = r[rs]; break;
            case 0x12: r[rd] = c.lo; break;
            case 0x13: c.lo = r[rs]; break;
            case 0x18: {
                const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) * static_cast<std::int32_t>(r[rt]);
                c.lo = static_cast<std::uint32_t>(p);
                c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
                break;
            }
            case 0x19: {
                const std::uint64_t p = static_cast<std::uint64_t>(r[rs]) * r[rt];
                c.lo = static_cast<std::uint32_t>(p);
                c.hi = static_cast<std::uint32_t>(p >> 32);
                break;
            }
            case 0x1A: {
                const auto n = static_cast<std::int32_t>(r[rs]), d = static_cast<std::int32_t>(r[rt]);
                if (d == 0) { c.lo = n < 0 ? 1u : 0xFFFFFFFFu; c.hi = static_cast<std::uint32_t>(n); }
                else if (n == INT32_MIN && d == -1) { c.lo = static_cast<std::uint32_t>(INT32_MIN); c.hi = 0; }
                else { c.lo = static_cast<std::uint32_t>(n / d); c.hi = static_cast<std::uint32_t>(n % d); }
                break;
            }
            case 0x1B:
                if (r[rt] == 0) { c.lo = 0xFFFFFFFFu; c.hi = r[rs]; }
                else { c.lo = r[rs] / r[rt]; c.hi = r[rs] % r[rt]; }
                break;
            case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; break;
            case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; break;
            case 0x24: r[rd] = r[rs] & r[rt]; break;
            case 0x25: r[rd] = r[rs] | r[rt]; break;
            case 0x26: r[rd] = r[rs] ^ r[rt]; break;
            case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
            case 0x2A: r[rd] = static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(r[rt]); break;
            case 0x2B: r[rd] = r[rs] < r[rt]; break;
            default: bad(c, pc, w); return;
            }
            break;
        case 0x01: {
            const bool link = (rt & 0x1Eu) == 0x10u;
            const bool taken = (rt & 1u) ? static_cast<std::int32_t>(r[rs]) >= 0 : static_cast<std::int32_t>(r[rs]) < 0;
            if (link) r[31] = pc + 8;
            if (taken) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2);
            break;
        }
        case 0x02: c.npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
        case 0x03: r[31] = pc + 8; c.npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
        case 0x04: if (r[rs] == r[rt]) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x05: if (r[rs] != r[rt]) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x06: if (static_cast<std::int32_t>(r[rs]) <= 0) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x07: if (static_cast<std::int32_t>(r[rs]) > 0) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x08: case 0x09: r[rt] = r[rs] + static_cast<std::uint32_t>(simm(w)); break;
        case 0x0A: r[rt] = static_cast<std::int32_t>(r[rs]) < simm(w); break;
        case 0x0B: r[rt] = r[rs] < static_cast<std::uint32_t>(simm(w)); break;
        case 0x0C: r[rt] = r[rs] & (w & 0xFFFFu); break;
        case 0x0D: r[rt] = r[rs] | (w & 0xFFFFu); break;
        case 0x0E: r[rt] = r[rs] ^ (w & 0xFFFFu); break;
        case 0x0F: r[rt] = w << 16; break;
        case 0x10:
            if (rs == 0) r[rt] = rd == 12 ? c.status : rd == 13 ? c.cause : rd == 14 ? c.epc : rd == 15 ? 0x1Fu : 0u;
            else if (rs == 4) {
                if (rd == 12) kernel::set_status(c, r[rt]);
                else if (rd == 13) c.cause = r[rt];
                else if (rd == 14) c.epc = r[rt];
            } else if ((w & 63u) == 0x10) {
                c.status = (c.status & ~0xFu) | ((c.status >> 2) & 0xFu);
            }
            break;
        case 0x20: r[rt] = static_cast<std::uint32_t>(static_cast<std::int8_t>(read8(r[rs] + static_cast<std::uint32_t>(simm(w))))); break;
        case 0x21: r[rt] = static_cast<std::uint32_t>(static_cast<std::int16_t>(read16(r[rs] + static_cast<std::uint32_t>(simm(w))))); break;
        case 0x22: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (3u - (a & 3u)) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            r[rt] = (mem << shift) | (shift ? (r[rt] & ((1u << shift) - 1u)) : 0u);
            break;
        }
        case 0x23: r[rt] = read32(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x24: r[rt] = read8(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x25: r[rt] = read16(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x26: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (a & 3u) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            r[rt] = (mem >> shift) | (shift ? (r[rt] & ~(0xFFFFFFFFu >> shift)) : 0u);
            break;
        }
        case 0x28: write8(r[rs] + static_cast<std::uint32_t>(simm(w)), static_cast<std::uint8_t>(r[rt])); break;
        case 0x29: write16(r[rs] + static_cast<std::uint32_t>(simm(w)), static_cast<std::uint16_t>(r[rt])); break;
        case 0x2A: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (3u - (a & 3u)) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            write32(a & ~3u, (r[rt] >> shift) | (shift ? (mem & ~(0xFFFFFFFFu >> shift)) : 0u));
            break;
        }
        case 0x2B: write32(r[rs] + static_cast<std::uint32_t>(simm(w)), r[rt]); break;
        case 0x2E: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (a & 3u) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            write32(a & ~3u, (r[rt] << shift) | (shift ? (mem & ((1u << shift) - 1u)) : 0u));
            break;
        }
        case 0x2F: break;
        case kHleOpcode:
            c.pc = r[31];
            c.npc = c.pc + 4;
            r[0] = 0;
            call_hle(c, w & 0x03FFFFFFu);
            r[0] = 0;
            if (kernel::switch_pending()) return;
            continue;
        default:
            bad(c, pc, w);
            return;
        }
        r[0] = 0;
        if (kernel::switch_pending()) return;
    }
}

void register_regions(const Region *regions, std::size_t count) {
    if (g_region_at.empty()) g_region_at.assign(kRamSize / 4, nullptr);
    // PS2_IOP_REGIONS=<from>-<to>: only these regions (by their place in the list; to find one that misbehaves)
    std::size_t from = 0, to = count;
    if (const char *only = std::getenv("PS2_IOP_REGIONS")) std::sscanf(only, "%zu-%zu", &from, &to);
    for (std::size_t i = from; i < std::min(to, count); ++i) g_region_at[(phys(regions[i].pc) & (kRamSize - 1)) >> 2] = &regions[i];
    std::fprintf(stderr, "[iop] %zu translated code regions\n", count);
}

}
