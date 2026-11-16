#include "gif.hpp"

#include "gs.hpp"
#include "queue.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::gif {

namespace {

struct Path {
    bool active = false;
    std::uint32_t nloop = 0, loop = 0, nreg = 0, reg = 0, flg = 0;
    std::uint64_t regs = 0;
    bool eop = false;
    bool address_data = false;
};

Path g_paths[4];
std::uint32_t g_stat = 0;
std::uint64_t g_tags[4][4];

template <typename Sink>
void walk(Path &p, const std::uint8_t *data, std::uint32_t qwc, Sink &sink) {
    for (std::uint32_t i = 0; i < qwc; ++i) {
        Reg128 q;
        std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
        if (!p.active) {
            p.nloop = static_cast<std::uint32_t>(q.d[0]) & 0x7FFFu;
            p.eop = (q.d[0] >> 15) & 1u;
            p.flg = static_cast<std::uint32_t>(q.d[0] >> 58) & 3u;
            p.nreg = static_cast<std::uint32_t>(q.d[0] >> 60) & 0xFu;
            if (!p.nreg) p.nreg = 16;
            p.regs = q.d[1];
            p.loop = 0;
            p.reg = 0;
            sink.tag(p, q);
            p.active = p.nloop != 0;
            continue;
        }
        switch (p.flg) {
        case 0: {
            // PACKED data is nearly all of a draw: take the qwords one after another with the loop counters in registers (the sink is a call
            // the compiler cannot see into, so with `p` in memory it would load and store them for every qword) and write them back at the end.
            std::uint32_t reg = p.reg, loop = p.loop;
            const std::uint32_t nreg = p.nreg, nloop = p.nloop;
            const std::uint64_t regs = p.regs;
            for (;;) {
                sink.packed(static_cast<std::uint32_t>(regs >> (reg * 4)) & 0xFu, q);
                if (++reg >= nreg) {
                    reg = 0;
                    if (++loop >= nloop) {
                        p.active = false;
                        break;
                    }
                }
                if (i + 1 >= qwc) break;
                ++i;
                std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
            }
            p.reg = reg;
            p.loop = loop;
            break;
        }
        case 1:
            for (int half = 0; half < 2 && p.active; ++half) {
                sink.reglist(static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu, q.d[half]);
                if (++p.reg >= p.nreg) {
                    p.reg = 0;
                    if (++p.loop >= p.nloop) p.active = false;
                }
            }
            break;
        default: {
            const std::uint32_t run = std::min(qwc - i, p.nloop - p.loop);
            sink.image(data + static_cast<std::size_t>(i) * 16, run);
            p.loop += run;
            i += run - 1;
            if (p.loop >= p.nloop) p.active = false;
            break;
        }
        }
    }
}

// In the path argument of a GIF message: end that path's packet there (end_runaway_kick).
constexpr int kEndPacket = 0x100;

// Signs that the GIF is reading something that is not a packet (vertex data taken for a tag, which then sprays the rest over random
// registers): writes to GS registers that do not exist, and XGKICK packets without an end. A game does neither on purpose. Counted
// per path on the drawing thread (render()), with the first of each kept, and printed by stats(), so a player's log shows which
// path and from which frame on (public issue #69, Nickelodeon Barnyard: about 15000 writes to every missing register).
struct Odd {
    std::uint64_t count = 0;
    std::uint32_t frame = 0, reg = 0, address = 0;
    char how = 0;  // 'a' an A+D address, 'p' a PACKED register descriptor, 'r' a REGLIST one
    std::uint64_t tag[2] = {0, 0};
};
Odd g_missing[4], g_runaway[4];
std::uint64_t g_last_tag[4][2];

// The GS registers by address (GS manual, register list; 0x7F is the address ps2sdk writes as a NOP).
constexpr std::uint64_t kRegsLow = 0xA8F000041FF037FFull, kRegsHigh = 0x80000007001FFFFFull;
bool exists(std::uint32_t reg) { return reg < 64 ? ((kRegsLow >> reg) & 1u) : reg < 128 && ((kRegsHigh >> (reg - 64)) & 1u); }

// The first of each is also printed at once: a run that freezes later never reaches stats().
void note(Odd &o, int path, std::uint32_t reg, char how, std::uint32_t address) {
    if (o.count++) return;
    o.frame = gs::present_frame();
    o.reg = reg;
    o.how = how;
    o.address = address;
    o.tag[0] = g_last_tag[path][0];
    o.tag[1] = g_last_tag[path][1];
    if (how)
        std::fprintf(stderr, "[gif] path %d writes a GS register that does not exist about frame %u (%s 0x%02X, packet tag %016llX %016llX)\n", path, o.frame,
                     how == 'a' ? "A+D address" : how == 'p' ? "PACKED descriptor" : "REGLIST descriptor", reg,
                     static_cast<unsigned long long>(o.tag[1]), static_cast<unsigned long long>(o.tag[0]));
    else
        std::fprintf(stderr, "[gif] path %d: an XGKICK packet did not end within VU1's data memory about frame %u (kicked at 0x%03X, last tag %016llX %016llX)\n",
                     path, o.frame, address, static_cast<unsigned long long>(o.tag[1]), static_cast<unsigned long long>(o.tag[0]));
}

struct GsSink {
    int path;
    void tag(const Path &p, const Reg128 &q) {
        ++g_tags[path][p.flg];
        g_last_tag[path][0] = q.d[0];
        g_last_tag[path][1] = q.d[1];
        gs::g_state.q = 1.0f;
        if (((q.d[0] >> 46) & 1u) && p.flg == 0) gs::write(0x00, (q.d[0] >> 47) & 0x7FFu);
    }
    void packed(std::uint32_t reg, const Reg128 &q) {
        // 0xB is the one PACKED descriptor the GS manual leaves reserved; 0xE is A+D, whose address byte must name a register.
        if ((reg == 0xEu && !exists(static_cast<std::uint32_t>(q.d[1]) & 0xFFu)) || reg == 0xBu) [[unlikely]]
            note(g_missing[path], path, reg == 0xEu ? static_cast<std::uint32_t>(q.d[1]) & 0xFFu : reg, reg == 0xEu ? 'a' : 'p', 0);
        gs::write_packed(reg, q);
    }
    void reglist(std::uint32_t reg, std::uint64_t value) {
        if (reg == 0xBu || reg == 0xEu) [[unlikely]] note(g_missing[path], path, reg, 'r', 0);
        gs::write(reg, value);
    }
    void image(const std::uint8_t *data, std::uint32_t qwords) { gs::write_image(data, qwords); }
};

void scan(Path &p, const std::uint8_t *data, std::uint32_t qwc) {
    std::uint32_t i = 0;
    while (i < qwc) {
        Reg128 q;
        if (!p.active) {
            std::memcpy(&q, data + static_cast<std::size_t>(i) * 16, 16);
            p.nloop = static_cast<std::uint32_t>(q.d[0]) & 0x7FFFu;
            p.flg = static_cast<std::uint32_t>(q.d[0] >> 58) & 3u;
            p.nreg = static_cast<std::uint32_t>(q.d[0] >> 60) & 0xFu;
            if (!p.nreg) p.nreg = 16;
            p.regs = q.d[1];
            p.loop = 0;
            p.reg = 0;
            p.address_data = false;
            if (p.flg == 0)
                for (std::uint32_t r = 0; r < p.nreg; ++r) p.address_data = p.address_data || ((p.regs >> (r * 4)) & 0xFu) == 0xEu;
            p.active = p.nloop != 0;
            ++i;
            continue;
        }
        if (p.flg == 0 && p.address_data) {
            for (; i < qwc && p.active; ++i) {
                const std::uint8_t *at = data + static_cast<std::size_t>(i) * 16;
                if (((p.regs >> (p.reg * 4)) & 0xFu) == 0xEu && at[8] >= 0x50 && at[8] <= 0x62) {
                    std::uint64_t value;
                    std::memcpy(&value, at, 8);
                    if (at[8] >= 0x60) gs::signal(at[8], value);
                    else if (at[8] <= 0x53) gs::note_transfer(at[8], value);
                }
                if (++p.reg >= p.nreg) {
                    p.reg = 0;
                    if (++p.loop >= p.nloop) p.active = false;
                }
            }
            continue;
        }
        const std::uint64_t done = static_cast<std::uint64_t>(p.loop) * p.nreg + p.reg;
        const std::uint64_t left = static_cast<std::uint64_t>(p.nloop) * p.nreg - done;
        std::uint64_t take, used;
        if (p.flg == 0) {
            take = used = std::min<std::uint64_t>(left, qwc - i);
        } else if (p.flg == 1) {
            take = std::min<std::uint64_t>((left + 1) / 2, qwc - i);
            used = std::min<std::uint64_t>(take * 2, left);
        } else {
            take = std::min<std::uint64_t>(p.nloop - p.loop, qwc - i);
            p.loop += static_cast<std::uint32_t>(take);
            if (p.loop >= p.nloop) p.active = false;
            i += static_cast<std::uint32_t>(take);
            continue;
        }
        p.loop = static_cast<std::uint32_t>((done + used) / p.nreg);
        p.reg = static_cast<std::uint32_t>((done + used) % p.nreg);
        if (used == left) p.active = false;
        i += static_cast<std::uint32_t>(take);
    }
}

Path g_scan[4];

// PATH3 under VIF1's MSKPATH3: while the mask is up a real GIF leaves channel 2's data
// waiting, and a mask that comes up while a packet is under way stops PATH3 at the packet's
// end (the tag with EOP). Games stream textures through a few slots of GS memory this way:
// the VIF1 list drops the mask and raises it again right away before each draw that needs
// the next texture, which lets exactly one packet through, and leaves it down at the end of
// the list, which lets the rest through. Channel 2 finishes at once here, so its data is
// held until then, in the thread that runs VIF1 for real so that it reaches the GS in order
// with VIF1's own.
struct Held {
    bool masked = false, mid_packet = false, flowing = false;
    std::uint64_t left = 0;
    bool eop = false;
    std::vector<std::uint8_t> data;
} g_held;

bool hold_here() { return !gs::queue::vu_active() || gs::queue::role() == gs::queue::Role::Vu; }

void out(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (!qwc) return;
    if (!gs::queue::active()) {
        render(path, data, qwc);
        return;
    }
    if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);
    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, qwc * 16);
}

// How many of these qwords belong to the packet under way; mid_packet is left set when
// the packet goes on past them.
std::uint32_t packet_length(const std::uint8_t *data, std::uint32_t qwc) {
    Held &h = g_held;
    std::uint32_t i = 0;
    while (i < qwc) {
        if (!h.left) {
            std::uint64_t tag;
            std::memcpy(&tag, data + static_cast<std::size_t>(i) * 16, 8);
            const std::uint64_t nloop = tag & 0x7FFFu, flg = (tag >> 58) & 3u;
            std::uint64_t nreg = (tag >> 60) & 0xFu;
            if (!nreg) nreg = 16;
            h.eop = (tag >> 15) & 1u;
            h.left = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            h.mid_packet = true;
            ++i;
        } else {
            const std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(h.left, qwc - i));
            h.left -= take;
            i += take;
        }
        if (!h.left && h.eop) {
            h.mid_packet = false;
            return i;
        }
    }
    return qwc;
}

// Lets held data out while PATH3 may run; with only_one, just the next packet.
void release(bool only_one) {
    Held &h = g_held;
    std::size_t done = 0;
    const std::uint32_t total = static_cast<std::uint32_t>(h.data.size() / 16);
    while (done < total && (!h.masked || h.mid_packet)) {
        const std::uint8_t *at = h.data.data() + done * 16;
        const std::uint32_t n = packet_length(at, static_cast<std::uint32_t>(total - done));
        out(3, at, n);
        done += n;
        if (only_one && !h.mid_packet) break;
    }
    h.data.erase(h.data.begin(), h.data.begin() + static_cast<std::ptrdiff_t>(done * 16));
}

}

void drop_held() {
    const bool masked = g_held.masked;
    g_held = Held{};
    g_held.masked = masked;
}

void reset() {
    g_stat = 0;
    if (hold_here()) drop_held();
    if (gs::queue::active()) {
        for (Path &p : g_scan) p = Path{};
        gs::queue::post(gs::queue::kGifReset, 0, 0, nullptr, 0);
        return;
    }
    render_reset();
}

void render_reset() {
    for (Path &p : g_paths) p = Path{};
}

void render(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (path & kEndPacket) [[unlikely]] {
        path &= 3;
        note(g_runaway[path], path, 0, 0, qwc);  // qwc carries the kick address here
        g_paths[path].active = false;
        return;
    }
    GsSink sink{path};
    walk(g_paths[path], data, qwc, sink);
}

// VU1's GIF reads a kicked packet round the data memory until a tag with EOP has had all its data, and the next XGKICK waits for
// that and then starts with a tag of its own (EE manual, PATH1). vu::xgkick sends at most one pass over the memory; when the
// packet has not ended by then (only a tag that is not one, such as vertex data, does that) this ends it, in order with the data,
// so the next kick is read as a new packet instead of as more of this one. Without it one bad tag with a large NLOOP swallowed the
// packets of many kicks after it and sprayed them over random GS registers.
void end_runaway_kick(std::uint32_t address) {
    if (!gs::queue::active()) {
        render(1 | kEndPacket, nullptr, address);
        return;
    }
    if (gs::queue::role() != gs::queue::Role::Vu) g_scan[1].active = false;
    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(1 | kEndPacket), address, nullptr, 0);
}

// A HUD drawn for a 4:3 picture, kept in 4:3 shapes on a 16:9 one. A game patch (Midnight Club II's mc2-options/hud43.py)
// brackets the drawing of each HUD part with VIF1 MARK commands: bit 15 on, bits 8-12 the part, bits 0-1 the side it belongs
// to (1 left edge, 2 centre, 3 right edge; 0: the third of the frame the part's first drawing is in; PS2_HUD_LOG=1 prints each
// part's box, =2 every transfer's too). While a mark is on, the x of every vertex and
// of the scissor box that goes out on PATH1/PATH2 is moved to A + (x - A) * 3/4, A the side's x: the 16:9 picture widens the
// frame by 4/3 again, so the part keeps its shape and its distance from that edge. MARK 0 ends it. Nothing else uses MARK
// in that game; in a game without the patch no mark is ever on and this costs one test per transfer.
namespace {

struct HudSqueeze {
    std::uint32_t mark = 0;
    int anchor = -1;                     // window pixels, -1: log only
    std::int32_t ofx[2] = {0x7000, 0x7000};  // XYOFFSET_1/2 x (12.4), seen in the stream; 1792 pixels until then
    Path paths[3];
    std::int32_t lo = 1 << 30, hi = -(1 << 30);
    std::uint32_t vertices = 0;
    std::vector<std::uint8_t> copy;
} g_hud;

bool hud_log() {
    static const bool on = std::getenv("PS2_HUD_LOG") != nullptr;
    return on;
}

std::uint32_t hud_x(std::uint32_t x) {  // a vertex x, 12.4 in primitive coordinates
    const std::int32_t window = (static_cast<std::int32_t>(x) - g_hud.ofx[0]) >> 4;
    g_hud.lo = std::min(g_hud.lo, window), g_hud.hi = std::max(g_hud.hi, window);
    ++g_hud.vertices;
    if (g_hud.anchor < 0) return x;
    const std::int32_t a = (g_hud.anchor << 4) + g_hud.ofx[0];
    return static_cast<std::uint32_t>(std::clamp(a + (((static_cast<std::int32_t>(x) - a) * 3) >> 2), 0, 0xFFFF));
}

std::uint64_t hud_scissor(std::uint64_t v) {  // SCISSOR: x0 bits 0-10, x1 (inclusive) bits 16-26, window pixels
    if (g_hud.anchor < 0) return v;
    const std::int32_t a = g_hud.anchor, x0 = static_cast<std::int32_t>(v & 0x7FFu), x1 = static_cast<std::int32_t>((v >> 16) & 0x7FFu);
    // A box over the whole 512-pixel frame clips nothing of the part, and it is how the game puts the scissor back after a part
    // that drew in a box of its own: squeezed (toward the centre, 64..447) it stayed in force for the parts drawn after it and cut
    // the race timer and the position to their first characters. So it is left as it is.
    if (x0 == 0 && x1 >= 511) return v;
    const std::int32_t n0 = std::clamp(a + (((x0 - a) * 3) >> 2), 0, 2047), n1 = std::clamp(a + (((x1 + 1 - a) * 3) >> 2) - 1, 0, 2047);
    return (v & ~0x07FF07FFull) | static_cast<std::uint64_t>(n0) | (static_cast<std::uint64_t>(n1) << 16);
}

bool is_xyz(std::uint32_t reg) { return reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD; }

void hud_squeeze(int path, std::uint8_t *data, std::uint32_t qwc) {
    Path &p = g_hud.paths[path];
    for (std::uint32_t i = 0; i < qwc; ++i) {
        std::uint8_t *at = data + static_cast<std::size_t>(i) * 16;
        std::uint64_t d0, d1;
        std::memcpy(&d0, at, 8);
        std::memcpy(&d1, at + 8, 8);
        if (!p.active) {
            p.nloop = static_cast<std::uint32_t>(d0) & 0x7FFFu;
            p.flg = static_cast<std::uint32_t>(d0 >> 58) & 3u;
            p.nreg = static_cast<std::uint32_t>(d0 >> 60) & 0xFu;
            if (!p.nreg) p.nreg = 16;
            p.regs = d1;
            p.loop = p.reg = 0;
            p.active = p.nloop != 0;
            continue;
        }
        if (p.flg == 0) {
            const std::uint32_t reg = static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu;
            if (is_xyz(reg)) {
                d0 = (d0 & ~0xFFFFull) | hud_x(static_cast<std::uint32_t>(d0 & 0xFFFFu));
            } else if (reg == 0xE) {
                const std::uint32_t address = at[8];
                if (is_xyz(address)) d0 = (d0 & ~0xFFFFull) | hud_x(static_cast<std::uint32_t>(d0 & 0xFFFFu));
                else if (address == 0x18 || address == 0x19) g_hud.ofx[address - 0x18] = static_cast<std::int32_t>(d0 & 0xFFFFu);
                else if (address == 0x40 || address == 0x41) d0 = hud_scissor(d0);
            }
            std::memcpy(at, &d0, 8);
            if (++p.reg >= p.nreg) {
                p.reg = 0;
                if (++p.loop >= p.nloop) p.active = false;
            }
        } else if (p.flg == 1) {
            std::uint64_t half[2] = {d0, d1};
            for (int k = 0; k < 2 && p.active; ++k) {
                const std::uint32_t reg = static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu;
                if (is_xyz(reg)) half[k] = (half[k] & ~0xFFFFull) | hud_x(static_cast<std::uint32_t>(half[k] & 0xFFFFu));
                if (++p.reg >= p.nreg) {
                    p.reg = 0;
                    if (++p.loop >= p.nloop) p.active = false;
                }
            }
            std::memcpy(at, half, 16);
        } else {
            const std::uint32_t run = std::min(qwc - i, p.nloop - p.loop);
            p.loop += run;
            i += run - 1;
            if (p.loop >= p.nloop) p.active = false;
        }
    }
}

}  // namespace

void hud_mark(std::uint32_t value) {
    if ((g_hud.mark & 0x8000u) && hud_log())
        std::fprintf(stderr, "[hud] part %u side %u: x %d..%d, %u vertices\n", (g_hud.mark >> 8) & 0x1Fu, g_hud.mark & 3u, g_hud.lo, g_hud.hi, g_hud.vertices);
    g_hud.mark = value;
    if (!(value & 0x8000u)) return;
    static const int kSides[4] = {-2, 0, 256, 512};  // -2: the part's first transfer picks the side (transfer below)
    g_hud.anchor = kSides[value & 3u];
    for (Path &p : g_hud.paths) p = Path{};
    g_hud.lo = 1 << 30, g_hud.hi = -(1 << 30), g_hud.vertices = 0;
}

void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (path & kEndPacket) [[unlikely]] {  // the message above, should it come through the vector thread's queue
        end_runaway_kick(qwc);
        return;
    }
    if ((g_hud.mark & 0x8000u) && path != 3 && qwc) [[unlikely]] {
        g_hud.copy.assign(data, data + static_cast<std::size_t>(qwc) * 16);
        static const bool each = std::getenv("PS2_HUD_LOG") && std::getenv("PS2_HUD_LOG")[0] == '2';  // PS2_HUD_LOG=2: every transfer too
        const std::int32_t lo = g_hud.lo, hi = g_hud.hi;
        if (g_hud.anchor == -2) {
            // side 0: measure this transfer without changing it, then take the third of the frame its middle is in
            const Path saved = g_hud.paths[path];
            const std::uint32_t vertices = g_hud.vertices;
            g_hud.anchor = -1;
            g_hud.lo = 1 << 30, g_hud.hi = -(1 << 30);
            hud_squeeze(path, g_hud.copy.data(), qwc);
            if (g_hud.lo <= g_hud.hi) {
                const std::int32_t middle = (g_hud.lo + g_hud.hi) / 2;
                g_hud.anchor = middle < 171 ? 0 : middle > 341 ? 512 : 256;
                if (hud_log()) std::fprintf(stderr, "[hud]   part %u: side picked from x %d..%d -> %d\n", (g_hud.mark >> 8) & 0x1Fu, g_hud.lo, g_hud.hi, g_hud.anchor);
            } else {
                g_hud.anchor = -2;  // nothing drawn yet: the next transfer decides
            }
            g_hud.paths[path] = saved;
            g_hud.vertices = vertices;
            g_hud.lo = lo, g_hud.hi = hi;
        }
        if (each) g_hud.lo = 1 << 30, g_hud.hi = -(1 << 30);
        hud_squeeze(path, g_hud.copy.data(), qwc);
        if (each) {
            if (g_hud.lo <= g_hud.hi) std::fprintf(stderr, "[hud]   part %u path %d: x %d..%d\n", (g_hud.mark >> 8) & 0x1Fu, path, g_hud.lo, g_hud.hi);
            g_hud.lo = std::min(lo, g_hud.lo), g_hud.hi = std::max(hi, g_hud.hi);
        }
        data = g_hud.copy.data();
    }
    Held &h = g_held;
    if (!hold_here()) {
        out(path, data, qwc);
        return;
    }
    // Once the mask is down, whatever is still held goes ahead of anything new.
    if (h.flowing && !h.masked) {
        h.flowing = false;
        release(false);
    }
    if (path != 3) {
        out(path, data, qwc);
        return;
    }
    if (h.data.empty() && (!h.masked || h.mid_packet)) {
        std::uint32_t done = 0;
        while (done < qwc && (!h.masked || h.mid_packet)) done += packet_length(data + static_cast<std::size_t>(done) * 16, qwc - done);
        out(3, data, done);
        data += static_cast<std::size_t>(done) * 16;
        qwc -= done;
    }
    h.data.insert(h.data.end(), data, data + static_cast<std::size_t>(qwc) * 16);
}

void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc) { scan(g_scan[path], data, qwc); }

bool busy(int path) { return g_paths[path].active; }

void stats() {
    for (int p = 1; p < 4; ++p)
        std::fprintf(stderr, "[gif] path %d tags: packed %llu reglist %llu image %llu\n", p,
                     static_cast<unsigned long long>(g_tags[p][0]), static_cast<unsigned long long>(g_tags[p][1]),
                     static_cast<unsigned long long>(g_tags[p][2] + g_tags[p][3]));
    for (int p = 1; p < 4; ++p) {
        if (const Odd &o = g_missing[p]; o.count)
            std::fprintf(stderr, "[gif] path %d: %llu writes to GS registers that do not exist (the first about frame %u): the GIF read something that is not a packet\n",
                         p, static_cast<unsigned long long>(o.count), o.frame);
        if (const Odd &o = g_runaway[p]; o.count)
            std::fprintf(stderr, "[gif] path %d: %llu XGKICK packets did not end within VU1's data memory (the first about frame %u); each was cut there\n",
                         p, static_cast<unsigned long long>(o.count), o.frame);
    }
}

std::uint32_t read(std::uint32_t a) {
    // While PATH3 is masked a real GIF holds the stalled channel 2 transfer in its FIFO, and
    // games wait for FQC to read non-zero before they touch the GS; channel 2 finishes at
    // once here, so report the FIFO full for as long as the mask is up.
    if ((a & 0xF0u) == 0x20u) return g_stat | ((g_stat & 2u) ? 16u << 24 : 0u);
    return 0;
}

void write(std::uint32_t a, std::uint32_t v) {
    if ((a & 0xF0u) == 0x00u && (v & 1u)) reset();
    if ((a & 0xF0u) == 0x10u) g_stat = (g_stat & ~1u) | (v & 1u);
}

void vif1_idle() {
    if (!hold_here()) {
        gs::queue::vu().post(gs::queue::kPath3Idle, 0, 0, nullptr, 0);
        return;
    }
    Held &h = g_held;
    h.flowing = false;
    if (!h.masked) release(false);
}

void set_path3_masked(bool masked) {
    if (gs::queue::role() != gs::queue::Role::Vu) g_stat = (g_stat & ~2u) | (masked ? 2u : 0u);
    static const bool trace = std::getenv("PS2_TRACE_PATH3") != nullptr;
    if (trace) std::fprintf(stderr, "[gif] path3 %s\n", masked ? "masked" : "unmasked");
    if (!hold_here()) return;
    Held &h = g_held;
    const bool was = h.masked;
    h.masked = masked;
    // The packet that starts as the mask drops gets through even if the mask goes straight
    // back up; the rest waits for more GIF traffic or for VIF1 to go idle with it still down.
    if (was && !masked && !h.data.empty()) {
        release(true);
        h.flowing = !h.data.empty();
    }
}

}

namespace ps2::gif {

void state(ps2::state::Stream &s) {
    s.tag("gif");
    s.pod(g_paths);
    s.pod(g_stat);
    s.pod(g_tags);
    s.pod(g_scan);
}

}
