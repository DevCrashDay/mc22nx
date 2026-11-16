#include "renderer.hpp"

#include "classify.hpp"
#include "pages.hpp"
#include "state.hpp"
#include "vertices.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace ps2::gs::hwr {

namespace be = ps2::gs::gpu::be;

namespace {

constexpr std::uint32_t kPages = 512;                  // GS memory: 512 pages of 2048 words
constexpr std::uint64_t kTextureCapBytes = 256ull << 20;  // design: texture cache cap (least recently used dropped first)
constexpr std::size_t kMaxBatchVertices = 1u << 18;

enum Owner : std::uint8_t { kNone, kDrawn, kUploaded };
Owner g_page_owner[kPages]{};
std::uint64_t g_frame = 0;

// What a page holds, as a signature: its content at some point (`base`: 0 = fresh memory, else a number never used before, for
// content nobody can name: drawn by the GPU, copied inside the GS, an upload cut short) and the EE uploads written over it since,
// in order (an upload that writes all memory blocks an earlier one wrote replaces it). The same data uploaded into the same places
// every frame so gives the same signature; texture_for names textures by the upload that wrote them (upload_key) where it can.
struct Upload {
    std::uint32_t dbp, dbw, dpsm, x, y, w, h;
    std::uint64_t hash;
    std::shared_ptr<const std::vector<std::uint16_t>> blocks;  // the memory blocks it writes, sorted (none: format not known)
    bool whole = false;  // writes them whole (its rectangle is in whole blocks)
    // writes everything `o` wrote
    bool covers(const Upload &o) const {
        if (blocks && o.blocks) return whole && std::includes(blocks->begin(), blocks->end(), o.blocks->begin(), o.blocks->end());
        return dbp == o.dbp && dbw == o.dbw && dpsm == o.dpsm && x <= o.x && y <= o.y && x + w >= o.x + o.w && y + h >= o.y + o.h;
    }
};
struct Page {
    std::uint64_t base = 0, sig = 0;
    std::uint32_t pending = 0;  // an upload into it started and did not finish yet (its id)
    bool read = false;          // `sig` went into a texture's name since it was set (rect_sig)
    std::vector<Upload> uploads;
};
constexpr std::size_t kMaxPageUploads = 32;  // more: the page becomes nameless content (a new base)
Page g_pages[kPages];
std::uint64_t g_unique = 0;
std::uint32_t g_upload_id = 0;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return ContentHash::round(h ^ (v * 0x9E3779B97F4A7C15ull), v); }

void update_sig(Page &pg) {
    pg.read = false;
    if (pg.pending) {
        pg.sig = ~++g_unique;  // contents in flux: matches nothing
        return;
    }
    std::uint64_t h = mix(0x51ED270B27u, pg.base);
    for (const Upload &u : pg.uploads)
        h = mix(mix(mix(mix(h, u.hash), u.dbp | static_cast<std::uint64_t>(u.dbw) << 16 | static_cast<std::uint64_t>(u.dpsm) << 32),
                    u.x | static_cast<std::uint64_t>(u.y) << 32), u.w | static_cast<std::uint64_t>(u.h) << 32);
    pg.sig = h;
}

void forget(Page &pg) {  // content nobody can name
    // already nameless and no texture was named after its signature since: the same signature can stand for the new content
    // (each shape drawn into a page comes here; a draw does not need a new name until someone reads the page)
    if (!pg.read && !pg.pending && pg.base && pg.uploads.empty()) return;
    pg.base = ++g_unique;
    pg.pending = 0;
    pg.uploads.clear();
    update_sig(pg);
}

// a rectangle of a buffer was written by a GPU draw or a GS-to-GS copy
void touch_rect(std::uint32_t block, std::uint32_t bw, std::uint32_t psm, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, Owner o) {
    for_pages(block, bw, psm, x0, y0, x1, y1, [&](std::uint32_t p) {
        if (p >= kPages) return;
        g_page_owner[p] = o;
        forget(g_pages[p]);
    });
}

// GS memory blocks (64 words) by pixel format: block size in pixels, blocks per page row and column, block order in a page (GS
// manual). Formats it does not list (Z buffers) get no content key.
struct BlockLayout {
    std::uint32_t bw, bh, cols, rows;
    const std::uint8_t *table;  // [row * cols + col]
};
constexpr std::uint8_t kBlocks32[32] = {0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31};
constexpr std::uint8_t kBlocks16[32] = {0, 2, 8, 10, 1, 3, 9, 11, 4, 6, 12, 14, 5, 7, 13, 15, 16, 18, 24, 26, 17, 19, 25, 27, 20, 22, 28, 30, 21, 23, 29, 31};
constexpr std::uint8_t kBlocks16S[32] = {0, 2, 16, 18, 1, 3, 17, 19, 8, 10, 24, 26, 9, 11, 25, 27, 4, 6, 20, 22, 5, 7, 21, 23, 12, 14, 28, 30, 13, 15, 29, 31};

bool block_layout(std::uint32_t psm, BlockLayout &l) {
    switch (psm) {
    case 0x00: case 0x01: case 0x1B: case 0x24: case 0x2C: l = {8, 8, 8, 4, kBlocks32}; return true;  // CT32, CT24, T8H, T4HL, T4HH
    case 0x02: l = {16, 8, 4, 8, kBlocks16}; return true;   // CT16
    case 0x0A: l = {16, 8, 4, 8, kBlocks16S}; return true;  // CT16S
    case 0x13: l = {16, 16, 8, 4, kBlocks32}; return true;  // T8
    case 0x14: l = {32, 16, 4, 8, kBlocks16}; return true;  // T4
    default: return false;
    }
}

// the blocks the rectangle (x0, y0)-(x1, y1) inclusive of a buffer touches, sorted
std::vector<std::uint16_t> rect_blocks(const BlockLayout &l, std::uint32_t bp, std::uint32_t bw, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    const std::uint32_t per_row = std::max<std::uint32_t>(1, std::max<std::uint32_t>(bw, 1) * 64 / (l.bw * l.cols));
    // marked in a bitmap of all 16384 blocks and read back in order (no sort)
    static std::uint64_t bits[16384 / 64];
    std::uint32_t lo = 16384, hi = 0;
    for (std::uint32_t by = y0 / l.bh; by <= y1 / l.bh; ++by)
        for (std::uint32_t bx = x0 / l.bw; bx <= x1 / l.bw; ++bx) {
            const std::uint32_t page = (by / l.rows) * per_row + bx / l.cols;
            const std::uint32_t b = (bp + page * 32 + l.table[(by % l.rows) * l.cols + bx % l.cols]) & 0x3FFF;
            bits[b >> 6] |= 1ull << (b & 63);
            lo = std::min(lo, b >> 6), hi = std::max(hi, b >> 6);
        }
    std::vector<std::uint16_t> out;
    for (std::uint32_t i = lo; i <= hi && lo <= hi; ++i)
        for (std::uint64_t m = bits[i]; m; m &= m - 1) out.push_back(static_cast<std::uint16_t>(i * 64 + __builtin_ctzll(m)));
    for (std::uint32_t i = lo; i <= hi && lo <= hi; ++i) bits[i] = 0;
    return out;
}

// What the rectangle (0, 0)-(w-1, h-1) of the buffer at `bp` holds, by content: when the latest finished upload into its memory
// blocks wrote all of them (in whole blocks, in any pixel format: games upload 8- and 4-bit textures as 32-bit pixels), the hash of
// that upload's data, its layout and where the rectangle lies in it (not the address); else 0.
std::uint64_t upload_key(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t w, std::uint32_t h) {
    BlockLayout lt;
    if (!block_layout(psm, lt)) return 0;
    const std::vector<std::uint16_t> want = rect_blocks(lt, bp, bw, 0, 0, w - 1, h - 1);
    const Upload *found = nullptr;
    bool ok = true;
    // the pages that hold those blocks (not for_pages: a neighbour page it may name holds none of them)
    std::vector<std::uint32_t> pages;
    for (std::uint16_t blk : want)
        if (pages.empty() || pages.back() != blk / 32u) pages.push_back(blk / 32u);
    const auto visit = [&](std::uint32_t p) {
        if (!ok) return;
        if (p >= kPages || g_pages[p].pending) {
            ok = false;
            return;
        }
        const std::vector<Upload> &ups = g_pages[p].uploads;
        for (auto it = ups.rbegin(); it != ups.rend(); ++it) {  // the latest upload into this page that writes any of it
            bool covers;
            if (it->dbp == bp && it->dbw == bw && it->dpsm == psm) {  // the same layout: by pixels (a palette is part of a block)
                if (it->x >= w || it->y >= h) continue;
                covers = it->x == 0 && it->y == 0 && it->w >= w && it->h >= h;
            } else {  // another layout: by whole blocks
                if (!it->blocks) {  // cannot tell which blocks: may write them
                    ok = false;
                    return;
                }
                const std::vector<std::uint16_t> &got = *it->blocks;
                bool common = false;  // any block in both (sorted lists)
                for (auto a = want.begin(), b = got.begin(); a != want.end() && b != got.end() && !common;)
                    if (*a < *b) ++a;
                    else if (*b < *a) ++b;
                    else common = true;
                if (!common) continue;
                covers = it->whole && std::includes(got.begin(), got.end(), want.begin(), want.end());
            }
            if (!covers || (found && (found->hash != it->hash || found->dbp != it->dbp || found->dbw != it->dbw || found->dpsm != it->dpsm ||
                                      found->x != it->x || found->y != it->y || found->w != it->w || found->h != it->h))) {
                ok = false;
                return;
            }
            found = &*it;
            return;
        }
        ok = false;  // no upload into these blocks since the page's content became nameless
    };
    for (std::uint32_t p : pages) visit(p);
    if (!ok || !found) return 0;
    const Upload &u = *found;
    std::uint64_t k = mix(u.hash, u.dbw | u.dpsm << 8 | ((bp - u.dbp) & 0x3FFF) << 16);
    k = mix(k, u.x | static_cast<std::uint64_t>(u.y) << 32);
    return mix(k, u.w | static_cast<std::uint64_t>(u.h) << 32);
}

// The palette buffer of the texture fetch (glsl/gs_load_clut.comp: 256 entries), as signatures of its 16-entry parts. What it
// holds depends on the loads (TEX0 CLD) before, not on the memory at the texture's CBP now. A 4-bit palette is part CSA & 15, an
// 8-bit one all parts.
std::uint64_t g_clut_part[16]{};

template <class F>
void clut_parts(std::uint32_t psm, std::uint32_t csa, F &&f) {
    if (psm == 0x13 || psm == 0x1B)
        for (std::uint32_t i = 0; i < 16; ++i) f(i, i);
    else
        f(csa & 15, 0);
}

std::uint64_t clut_sig(std::uint32_t psm, std::uint32_t csa) {
    std::uint64_t sig = 0;
    clut_parts(psm, csa, [&](std::uint32_t part, std::uint32_t) { sig = mix(sig, g_clut_part[part] ^ part); });
    return sig;
}

std::uint64_t upload_key(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t w, std::uint32_t h);

// the signature of what a rectangle of a buffer holds (the pages it covers)
std::uint64_t rect_sig(std::uint32_t block, std::uint32_t bw, std::uint32_t psm, std::uint32_t w, std::uint32_t h) {
    std::uint64_t sig = 0;
    for_pages(block, bw, psm, 0, 0, w - 1, h - 1, [&](std::uint32_t p) {
        if (p >= kPages) {
            sig = mix(sig, p);
            return;
        }
        g_pages[p].read = true;
        sig = mix(sig, g_pages[p].sig ^ p);
    });
    return sig;
}

struct Target {
    be::Image img;
    std::uint32_t fbp = 0, fbw = 0, w = 0, h = 0;
};
std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Target> g_targets;  // (FBP, FBW, PSM)

struct Texture {
    be::Image img;
    std::uint32_t w = 0, h = 0;
    bool valid = false;
    std::uint64_t last_used = 0;
};
constexpr std::size_t kMaxTextures = 700;  // descriptor slots are 1024, shared with targets and depth images
// (TEX0 without TFX/TCC/CLD, TEXA, signature of its texel and palette pages): an image is never fetched over, so draws of the
// open batch that use it stay right when the same address gets other content
std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, Texture> g_textures;
std::uint64_t g_texture_bytes = 0;
// Images of evicted textures, by size, for new textures: creating and destroying GPU images costs much on the Switch (memory
// mapping; a destroy waits for the GPU), and the content-keyed cache makes a new texture whenever new content shows up.
// A reused image is written only by a later fetch, ordered after the draws that used it.
std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<be::Image>> g_free_images;
std::size_t g_free_count = 0;
constexpr std::size_t kMaxFreeImages = 200;  // more: destroyed (image slots are 1024 on deko3d)
struct SigCache {
    std::uint64_t stamp = 0, sig = 0;
    bool valid = false;
};
std::map<std::pair<std::uint64_t, std::uint64_t>, SigCache> g_sig_cache;  // (TEX0 without TFX/TCC/CLD, TEXA)
std::uint64_t g_clut_gen = 0;  // bumped by every palette load
std::uint64_t g_tex_gen = 0;   // bumped whenever g_textures drops an entry (Memo points into it)
struct Memo {
    std::uint64_t tex0 = 0, texa = 0, stamp = 0, clut_gen = 0, tex_gen = 0;
    Texture *t = nullptr;
};
Memo g_memo[64];  // texture_for's last answers, by TEX0

std::map<std::uint32_t, be::Image> g_feedback;  // target image id -> its copy for shapes that read their own target
std::vector<be::HwrVertex> g_verts;
std::vector<be::HwrDraw> g_draws;

struct Counters {
    std::uint64_t shapes = 0, dropped = 0, fetches = 0, from_target = 0, unmapped_blends = 0;
    std::uint64_t flushes[7]{};  // why batches were sent: other, target read, texture refetch, feedback, full, eviction, present
    double ms_draw = 0, ms_fetch = 0, ms_submit_draws = 0;  // CPU time: all of draw(), queued fetches run, hwr_draw
    double ms_create = 0, ms_evict = 0;  // creating texture images, evicting textures
    double ms_upload = 0, ms_hash = 0;   // page tracking of finished uploads, hashing their data (gpu.cpp)
    std::uint64_t created = 0, reused = 0;  // texture images made new / taken from the evicted ones
} g_count;
std::map<std::uint32_t, std::uint64_t> g_unmapped_formulas;  // ALPHA register low byte -> shapes (PS2_HWR_LOG)

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void flush_batch(int why = 0) {
    ++g_count.flushes[why];
    const auto t0 = std::chrono::steady_clock::now();
    ps2::gs::gpu::hwr_flush_fetches();  // the textures of these draws
    const auto t1 = std::chrono::steady_clock::now();
    g_count.ms_fetch += std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!g_draws.empty()) be::hwr_draw(g_verts.data(), static_cast<std::uint32_t>(g_verts.size()), g_draws.data(), static_cast<std::uint32_t>(g_draws.size()));
    g_verts.clear();
    g_draws.clear();
    g_count.ms_submit_draws += ms_since(t1);
}

// Only draws into the image: reading a target that earlier draws of the batch merely sampled as well needs no flush.
bool batch_writes(be::Image img) {
    for (const be::HwrDraw &d : g_draws)
        if (d.target.id == img.id) return true;
    return false;
}

// GS depth buffers: one depth image per (ZBP, target), cleared to 0 (the GS's farthest Z) when created
std::map<std::tuple<std::uint32_t, std::uint32_t>, be::Image> g_depths;

struct Target;
be::Image depth_for(std::uint32_t zbp, const Target &t);

Target &target_for(const ShapeClass &c) {
    Target &t = g_targets[{c.fbp, c.fbw, c.fpsm}];
    if (!t.img) {
        t.fbp = c.fbp, t.fbw = std::max<std::uint32_t>(c.fbw, 1);
        t.w = std::min<std::uint32_t>(t.fbw * 64, 2048), t.h = 512;
        t.img = be::create_image(t.w, t.h, true);
        be::clear_image(t.img, 0.0f, 0.0f, 0.0f, 0.0f);  // like fresh GS memory: alpha 0 too (a target read as a texture uses it)
    }
    return t;
}

be::Image depth_for(std::uint32_t zbp, const Target &t) {
    be::Image &d = g_depths[{zbp, t.img.id}];
    if (!d) {
        d = be::create_depth_image(t.w, t.h);
        be::clear_depth(t.img, d, 0.0f);
    }
    return d;
}

// the least recently used texture of this size that no draw used this frame, or end()
auto lru_same_size(std::uint32_t w, std::uint32_t h) {
    auto best = g_textures.end();
    for (auto it = g_textures.begin(); it != g_textures.end(); ++it) {
        const Texture &x = it->second;
        if (x.img && x.w == w && x.h == h && x.last_used < g_frame && (best == g_textures.end() || x.last_used < best->second.last_used))
            best = it;
    }
    return best;
}

void evict_textures() {
    if (g_texture_bytes <= kTextureCapBytes && g_textures.size() <= kMaxTextures) return;
    struct Timed {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { g_count.ms_evict += ms_since(t0); }
    } timed;
    flush_batch(5);
    std::vector<std::pair<std::uint64_t, decltype(g_textures)::iterator>> order;
    for (auto it = g_textures.begin(); it != g_textures.end(); ++it) order.push_back({it->second.last_used, it});
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (auto &[used, it] : order) {
        if (g_texture_bytes <= kTextureCapBytes * 3 / 4 && g_textures.size() <= kMaxTextures * 3 / 4) break;
        g_texture_bytes -= static_cast<std::uint64_t>(it->second.w) * it->second.h * 4;
        if (g_free_count < kMaxFreeImages) {
            g_free_images[{it->second.w, it->second.h}].push_back(it->second.img);
            ++g_free_count;
        } else {
            be::destroy_image(it->second.img);
        }
        g_textures.erase(it);
    }
    ++g_tex_gen;
}

// The texture of this shape as a GPU image: a target the GPU drew (render-to-texture), or the cached fetch of GS memory.
be::Image texture_for(const ps2::gs::State &s, const ps2::gs::Context &x, const std::uint32_t *setup, std::uint32_t &w, std::uint32_t &h, bool &is_target) {
    is_target = false;
    const std::uint64_t tex0 = x.tex0;
    w = std::min(1u << ((tex0 >> 26) & 15), 1024u), h = std::min(1u << ((tex0 >> 30) & 15), 1024u);
    const std::uint32_t tbp = static_cast<std::uint32_t>(tex0 & 0x3FFF), first = tbp / 32;
    const std::uint32_t tbw = static_cast<std::uint32_t>((tex0 >> 14) & 0x3F), tpsm = static_cast<std::uint32_t>((tex0 >> 20) & 0x3F);
    if (g_page_owner[first] == kDrawn && tbp % 32 == 0)
        for (auto &[key, t] : g_targets)
            if (t.fbp == first) {
                ++g_count.from_target;
                is_target = true;
                // draws in this batch into that target must land before it is read (one barrier per batch in hwr_draw)
                if (batch_writes(t.img)) flush_batch(1);
                return t.img;
            }
    // what the texels and the palette are: by content where an upload names it (then the same texture found at another address
    // is the same image), else by the pages' signatures and the address; the palette by what the palette buffer holds
    const bool palette = tpsm == 0x13 || tpsm == 0x14 || tpsm == 0x1B || tpsm == 0x24 || tpsm == 0x2C;  // T8, T4, T8H, T4HL, T4HH
    // the texels' part, worked out again only when one of their pages changed (their signatures)
    const std::uint64_t stamp = rect_sig(tbp, tbw, tpsm, w, h);
    // runs of shapes share TEX0: the same pages, palette loads and cache as last time give the same image (no map lookups)
    Memo &memo = g_memo[(tex0 ^ (tex0 >> 29) ^ s.texa) & 63];
    if (memo.t && memo.tex0 == tex0 && memo.texa == s.texa && memo.stamp == stamp && memo.tex_gen == g_tex_gen &&
        (!palette || memo.clut_gen == g_clut_gen)) {
        memo.t->last_used = g_frame;
        return memo.t->img;
    }
    SigCache &sc = g_sig_cache[{tex0 & ~((7ull << 34) | (7ull << 61)), s.texa}];
    if (!sc.valid || sc.stamp != stamp) {
        std::uint64_t sig = upload_key(tbp, tbw, tpsm, w, h);
        if (!sig) sig = mix(stamp, tbp | 1ull << 40);
        sc = SigCache{stamp, sig, true};
    }
    const std::uint64_t sig = palette ? mix(sc.sig, clut_sig(tpsm, static_cast<std::uint32_t>(tex0 >> 56) & 31)) : sc.sig;
    // TCC, TFX and CLD do not change the texels; TBP0 and CBP are in `sig`
    const std::uint64_t key0 = tex0 & ~((7ull << 34) | (7ull << 61) | 0x3FFFull | (0x3FFFull << 37));
    if (g_textures.size() > kMaxTextures && !g_textures.count({key0, s.texa, sig})) evict_textures();
    Texture &t = g_textures[{key0, s.texa, sig}];
    t.last_used = g_frame;
    const auto remember = [&] { memo = Memo{tex0, s.texa, stamp, g_clut_gen, g_tex_gen, &t}; };
    if (t.valid) {
        remember();
        return t.img;
    }
    if (!t.img) {
        if (auto f = g_free_images.find({w, h}); f != g_free_images.end() && !f->second.empty()) {
            t.img = f->second.back();
            f->second.pop_back();
            --g_free_count;
            ++g_count.reused;
        } else if (auto old = g_textures.size() >= kMaxTextures / 2 ? lru_same_size(w, h) : g_textures.end(); old != g_textures.end()) {
            // none free of this size: the least recently used texture of it gives its image (not used this frame, so
            // no draw of the open batch samples it)
            t.img = old->second.img;
            g_texture_bytes -= static_cast<std::uint64_t>(w) * h * 4;
            g_textures.erase(old);
            ++g_tex_gen;
            ++g_count.reused;
        } else {
            const auto c0 = std::chrono::steady_clock::now();
            t.img = be::create_image(w, h, false);
            g_count.ms_create += ms_since(c0);
            ++g_count.created;
        }
        if (!t.img) {  // no image slot or memory: drawn untextured this time
            g_textures.erase({key0, s.texa, sig});
            ++g_tex_gen;
            return be::Image{};
        }
        t.w = w, t.h = h;
        g_texture_bytes += static_cast<std::uint64_t>(w) * h * 4;
    }
    ++g_count.fetches;
    t.valid = ps2::gs::gpu::hwr_fetch_texture(setup, w, h, t.img);  // not valid: fetched again next time
    if (!t.valid) return be::Image{};
    remember();
    return t.img;
}

}  // namespace

bool active() {
    static const bool on = [] {
        const char *e = std::getenv("PS2_HWR");
        return e && e[0] == '1';
    }();
    return on;
}

void uploaded(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    if (w && h) touch_rect(dbp, dbw, dpsm, x, y, x + w - 1, y + h - 1, kUploaded);
}

void clut_loaded(std::uint64_t tex0, std::uint64_t texclut) {
    (void)texclut;
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3F, cpsm = static_cast<std::uint32_t>(tex0 >> 51) & 0xF;
    const std::uint32_t cbp = static_cast<std::uint32_t>(tex0 >> 37) & 0x3FFF, csa = static_cast<std::uint32_t>(tex0 >> 56) & 31;
    const bool t8 = psm == 0x13 || psm == 0x1B;
    if ((tex0 >> 55) & 1) {  // CSM2 (entries anywhere in the buffer): everything it may hold is new
        for (std::uint64_t &part : g_clut_part) part = mix(++g_unique, 0xC2u);
        ++g_clut_gen;
        return;
    }
    ++g_clut_gen;
    std::uint64_t src = upload_key(cbp, 1, cpsm, t8 ? 16 : 8, t8 ? 16 : 2);
    if (!src) src = mix(++g_unique, 0xC1u);  // what it loaded has no name
    src = mix(src, cpsm | (t8 ? 0x100u : 0u));
    clut_parts(psm, csa, [&](std::uint32_t part, std::uint32_t at) { g_clut_part[part] = mix(src, at); });
}

void upload_begin(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    if (!w || !h) return;
    ++g_upload_id;
    for_pages(dbp, dbw, dpsm, x, y, x + w - 1, y + h - 1, [&](std::uint32_t p) {
        if (p >= kPages) return;
        Page &pg = g_pages[p];
        if (pg.pending) forget(pg);  // the upload before was cut short: what it left is nameless
        g_page_owner[p] = kUploaded;
        pg.pending = g_upload_id;
        update_sig(pg);
    });
}

void hashed(double ms) { g_count.ms_hash += ms; }

void upload_done(std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dpsm, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                 std::uint64_t hash) {
    if (!w || !h) return;
    struct Timed {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { g_count.ms_upload += ms_since(t0); }
    } timed;
    Upload u{dbp, dbw, dpsm, x, y, w, h, hash};
    if (BlockLayout l; block_layout(dpsm, l)) {
        u.blocks = std::make_shared<const std::vector<std::uint16_t>>(rect_blocks(l, dbp, dbw, x, y, x + w - 1, y + h - 1));
        u.whole = x % l.bw == 0 && y % l.bh == 0 && w % l.bw == 0 && h % l.bh == 0;
    }
    for_pages(dbp, dbw, dpsm, x, y, x + w - 1, y + h - 1, [&](std::uint32_t p) {
        if (p >= kPages) return;
        Page &pg = g_pages[p];
        if (pg.pending != g_upload_id) return;  // drawn over or another upload started since: stays nameless
        pg.pending = 0;
        std::erase_if(pg.uploads, [&](const Upload &o) { return u.covers(o); });
        if (pg.uploads.size() >= kMaxPageUploads) {
            pg.base = ++g_unique;
            pg.uploads.clear();
        }
        pg.uploads.push_back(u);
        update_sig(pg);
    });
}

void draw_shape(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count, const std::uint32_t *setup);

void draw(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count, const std::uint32_t *setup) {
    const auto t0 = std::chrono::steady_clock::now();
    draw_shape(s, v, count, setup);
    g_count.ms_draw += ms_since(t0);
}

void draw_shape(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count, const std::uint32_t *setup) {
    ++g_count.shapes;
    const ShapeClass c = classify(s);
    if (c.painter) {  // stage 1: left out (stage 3: the painter draws these)
        ++g_count.dropped;
        return;
    }
    be::HwrVertex quad[6];
    const std::uint32_t n = convert_shape(s, c, v, count, quad);
    if (!n) return;
    const std::uint64_t attr = (s.prmodecont & 1) ? s.prim : s.prmode;
    const ps2::gs::Context &x = s.ctx[(attr >> 9) & 1];
    Target &t = target_for(c);

    be::HwrDraw d{};
    d.target = t.img;
    d.scissor[0] = static_cast<std::uint32_t>(x.scissor & 0x7FF);
    d.scissor[2] = static_cast<std::uint32_t>((x.scissor >> 16) & 0x7FF);
    d.scissor[1] = static_cast<std::uint32_t>((x.scissor >> 32) & 0x7FF);
    d.scissor[3] = static_cast<std::uint32_t>((x.scissor >> 48) & 0x7FF);
    const GpuState g = map_state(s, c);
    std::copy(std::begin(g.shader), std::end(g.shader), d.state);
    d.blend = g.blend, d.ztest = g.ztest, d.zwrite = g.zwrite, d.write_mask = g.write_mask;
    d.state[5] = g.blend;  // state[1].y in the shader: the blend row (ZB_ONLY handling)
    if (g.blend == kBlendUnmapped) {
        ++g_count.unmapped_blends;
        ++g_unmapped_formulas[static_cast<std::uint32_t>(x.alpha & 0xFF)];  // A | B << 2 | C << 4 | D << 6
    }
    if (c.features & kTextured) {
        std::uint32_t tw = 0, th = 0;
        bool is_target = false;
        d.texture = texture_for(s, x, setup, tw, th, is_target);
        if (is_target) d.state[0] |= kShFlipV;
        if (is_target && d.texture.id == t.img.id) {  // feedback: sample a copy, not the image being drawn into
            flush_batch(3);
            be::Image &copy = g_feedback[t.img.id];
            if (!copy) copy = be::create_image(t.w, t.h, true);
            be::copy_image(t.img, copy);
            d.texture = copy;
        }
        if (!d.texture) d.state[0] &= ~kShTextured;
    }
    if (g.ztest != kZOff || g.zwrite) d.depth = depth_for(g.zbp, t);
    // PS2_HWR_DEBUG=<list>: nodepth (no depth test/write), noatest (no alpha test), noblend (blending off) — bisecting pictures
    static const std::string dbg = std::getenv("PS2_HWR_DEBUG") ? std::getenv("PS2_HWR_DEBUG") : "";
    if (!dbg.empty()) {
        if (dbg.find("nodepth") != std::string::npos) d.depth = be::Image{}, d.ztest = kZOff, d.zwrite = false;
        if (dbg.find("noatest") != std::string::npos) d.state[0] &= ~kShAlphaTest;
        if (dbg.find("noblend") != std::string::npos) d.state[0] |= kShNoBlend, d.blend = kBlendLerp;
        if (const std::size_t at = dbg.find("skipblend"); at != std::string::npos && dbg.size() > at + 9 && d.blend == dbg[at + 9] - '0')
            return;  // skipblend<n>: leave out shapes with hwr::Blend n
    }
    // the pages this shape writes (for render-to-texture and texture invalidation)
    {
        float y0 = quad[0].y, y1 = quad[0].y, x0 = quad[0].x, x1 = quad[0].x;
        for (std::uint32_t i = 1; i < n; ++i) y0 = std::min(y0, quad[i].y), y1 = std::max(y1, quad[i].y), x0 = std::min(x0, quad[i].x), x1 = std::max(x1, quad[i].x);
        const auto px = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 2047.0f)); };
        touch_rect(c.fbp * 32, t.fbw, c.fpsm, px(x0), px(y0), px(x1), px(y1), kDrawn);
    }

    if (g_verts.size() + n > kMaxBatchVertices) flush_batch(4);
    be::HwrDraw *last = g_draws.empty() ? nullptr : &g_draws.back();
    const bool same = last && last->target.id == d.target.id && last->texture.id == d.texture.id && last->depth.id == d.depth.id &&
                      last->blend == d.blend && last->ztest == d.ztest && last->zwrite == d.zwrite && last->write_mask == d.write_mask &&
                      std::equal(std::begin(d.scissor), std::end(d.scissor), last->scissor) && std::equal(std::begin(d.state), std::end(d.state), last->state) &&
                      last->first + last->count == g_verts.size();
    g_verts.insert(g_verts.end(), quad, quad + n);
    if (same) {
        last->count += n;
    } else {
        d.first = static_cast<std::uint32_t>(g_verts.size() - n);
        d.count = n;
        g_draws.push_back(d);
    }
}

bool present(const ps2::gs::gpu::DisplaySource &src, std::uint32_t surface_w, std::uint32_t surface_h, bool widescreen) {
    flush_batch(6);
    ++g_frame;
    evict_textures();
    if (g_sig_cache.size() > 8192) g_sig_cache.clear();
    static const bool log = std::getenv("PS2_HWR_LOG") != nullptr;
    if (log && g_frame % 300 == 0) {
        std::fprintf(stderr, "[hwr] frame %llu: %llu shapes, %llu left out (painter), %llu unmapped blends, %llu texture fetches, %llu render-to-texture reads; %zu targets, %zu textures (%.1f MB)\n",
                     static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(g_count.shapes), static_cast<unsigned long long>(g_count.dropped),
                     static_cast<unsigned long long>(g_count.unmapped_blends),
                     static_cast<unsigned long long>(g_count.fetches), static_cast<unsigned long long>(g_count.from_target), g_targets.size(), g_textures.size(),
                     g_texture_bytes / 1048576.0);
        for (const auto &[f, n] : g_unmapped_formulas)
            std::fprintf(stderr, "[hwr]   unmapped blend A=%u B=%u C=%u D=%u: %llu shapes\n", f & 3, (f >> 2) & 3, (f >> 4) & 3, (f >> 6) & 3,
                         static_cast<unsigned long long>(n));
        g_unmapped_formulas.clear();
        std::fprintf(stderr, "[hwr]   CPU per frame: draw() %.2f ms (of which queued fetches %.2f, draw batches %.2f); batches per frame: "
                             "other %.1f, target read %.1f, texture refetch %.1f, feedback %.1f, full %.1f, eviction %.1f, present %.1f; "
                             "texture images %.1f new (%.2f ms), %.1f reused; evicting %.2f ms; uploads: hashing %.2f ms, pages %.2f ms\n",
                     g_count.ms_draw / 300.0, g_count.ms_fetch / 300.0, g_count.ms_submit_draws / 300.0, g_count.flushes[0] / 300.0,
                     g_count.flushes[1] / 300.0, g_count.flushes[2] / 300.0, g_count.flushes[3] / 300.0, g_count.flushes[4] / 300.0,
                     g_count.flushes[5] / 300.0, g_count.flushes[6] / 300.0, g_count.created / 300.0, g_count.ms_create / 300.0,
                     g_count.reused / 300.0, g_count.ms_evict / 300.0, g_count.ms_hash / 300.0, g_count.ms_upload / 300.0);
        g_count = Counters{};
    }
    for (auto &[key, t] : g_targets) {
        if (t.fbp * 32 != src.fbp) continue;
        const float sw = static_cast<float>(surface_w), sh = static_cast<float>(surface_h);
        const float aspect = widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float vw = std::min(sw, sh * aspect), vh = vw / aspect;
        be::PresentInfo info{};
        info.surface_w = surface_w, info.surface_h = surface_h;
        info.vp_x = (sw - vw) / 2.0f, info.vp_y = (sh - vh) / 2.0f, info.vp_w = vw, info.vp_h = vh;
        return be::present_image(t.img, info, src.dbx, src.dby, std::min(src.width, t.w), std::min(src.height, t.h));
    }
    return false;
}

}  // namespace ps2::gs::hwr
