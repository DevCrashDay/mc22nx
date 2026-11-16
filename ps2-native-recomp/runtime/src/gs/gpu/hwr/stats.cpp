#include "stats.hpp"

#include "classify.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace ps2::gs::hwr {

namespace {

constexpr int kReasons = 7;
const char *const kReasonNames[kReasons] = {"point/line", "dest-alpha blend", "dest-alpha test", "partial mask", "frame format", "wrap blend", "FBA"};
constexpr int kFeatures = 11;
const char *const kFeatureNames[kFeatures] = {"textured", "blend", "alpha-test", "depth-test", "depth-write", "fog", "mip", "bilinear", "stq", "region-wrap", "palette"};

struct Window {
    std::uint64_t shapes = 0, box = 0, painter_shapes = 0, painter_box = 0;
    std::uint64_t reason_box[kReasons]{}, feature_shapes[kFeatures]{};
    std::uint64_t tex_ee = 0, tex_target = 0;
    std::uint64_t downloads = 0, download_bytes = 0, local_copies = 0, uploads_into_target = 0, displays_non_target = 0;
    struct Target {
        std::uint32_t fbw = 1, page_rows = 1;  // drawn extent in GS pages (64x32 pixels for 32-bit formats, 64x64 for 16-bit)
    };
    std::map<std::uint32_t, Target> targets;  // FBP -> frame buffers drawn to in this window, with the rows actually drawn
    std::uint64_t frame0 = 0;
} g;

std::map<std::uint32_t, std::uint64_t> g_tex_hits, g_upload_hits, g_target_shapes;  // per target FBP, this window

// A target spans FBW pages per page row, 32 blocks of 64 words per page, over the page rows that were drawn.
// Returns the FBP of the target holding `block`, or -1.
long target_of(std::uint32_t block) {
    for (const auto &[fbp, t] : g.targets)
        if (block >= fbp * 32 && block < fbp * 32 + std::max<std::uint32_t>(t.fbw, 1) * t.page_rows * 32) return static_cast<long>(fbp);
    return -1;
}

bool in_target(std::uint32_t block) { return target_of(block) >= 0; }

enum Owner : std::uint8_t { kNone, kDrawn, kUploaded };
Owner g_page[512];  // GS memory: 512 pages of 2048 words
std::uint64_t g_tex_drawn = 0, g_tex_uploaded = 0, g_upload_over_drawn = 0;

void mark_pages(std::uint32_t first, std::uint32_t count, Owner o, std::uint64_t *over_drawn = nullptr) {
    for (std::uint32_t i = 0; i < count && first + i < 512; ++i) {
        if (over_drawn && g_page[first + i] == kDrawn) ++*over_drawn;
        g_page[first + i] = o;
    }
}

// bytes of a texture of TEX0 (tw, th, psm), counted in pages from TBP (the T8H/T4H formats live in 32-bit pages)
std::uint32_t texture_pages(std::uint64_t tex0) {
    const std::uint32_t psm = (tex0 >> 20) & 0x3F, w = 1u << ((tex0 >> 26) & 15), h = 1u << ((tex0 >> 30) & 15);
    const std::uint32_t bpp = (psm == 0x02 || psm == 0x0A) ? 16 : (psm == 0x13) ? 8 : (psm == 0x14) ? 4 : 32;
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(w) * h * bpp / 8 + 8191) / 8192));
}

std::uint32_t box_pixels(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count, const ps2::gs::Context &x, std::int64_t rect[4]) {
    if (count <= 0) return 0;
    const std::int64_t ofx = static_cast<std::int64_t>(x.xyoffset & 0xFFFF), ofy = static_cast<std::int64_t>((x.xyoffset >> 32) & 0xFFFF);
    std::int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
    for (int i = 0; i < count; ++i) {
        const std::int64_t px = (static_cast<std::int64_t>(v[i].x) - ofx) >> 4, py = (static_cast<std::int64_t>(v[i].y) - ofy) >> 4;
        x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
    }
    const std::int64_t sx0 = x.scissor & 0x7FF, sx1 = (x.scissor >> 16) & 0x7FF, sy0 = (x.scissor >> 32) & 0x7FF, sy1 = (x.scissor >> 48) & 0x7FF;
    x0 = std::max(x0, sx0), y0 = std::max(y0, sy0), x1 = std::min(x1, sx1), y1 = std::min(y1, sy1);
    (void)s;
    rect[0] = x0, rect[1] = y0, rect[2] = x1, rect[3] = y1;
    return x1 < x0 || y1 < y0 ? 0 : static_cast<std::uint32_t>((x1 - x0 + 1) * (y1 - y0 + 1));
}

}  // namespace

bool stats_on() {
    static const bool on = std::getenv("PS2_HWR_STATS") != nullptr;
    return on;
}

void stats_shape(const ps2::gs::State &s, const ps2::gs::Vertex *v, int count) {
    const ShapeClass c = classify(s);
    const std::uint64_t attr = (s.prmodecont & 1) ? s.prim : s.prmode;
    std::int64_t rect[4] = {0, 0, -1, -1};
    const std::uint32_t box = box_pixels(s, v, count, s.ctx[(attr >> 9) & 1], rect);
    const std::int64_t bottom = rect[3];
    ++g.shapes;
    g.box += box;
    if (box) {
        Window::Target &t = g.targets[c.fbp];
        t.fbw = c.fbw;
        const std::uint32_t page_h = (c.fpsm == 0x02 || c.fpsm == 0x0A) ? 64 : 32;
        t.page_rows = std::max(t.page_rows, static_cast<std::uint32_t>(bottom / page_h) + 1);
    }
    if (c.painter) {
        ++g.painter_shapes;
        g.painter_box += box;
        for (int i = 0; i < kReasons; ++i)
            if (c.painter & (1u << i)) g.reason_box[i] += box;
    }
    for (int i = 0; i < kFeatures; ++i)
        if (c.features & (1u << i)) ++g.feature_shapes[i];
    if (box) {  // the pages this shape draws into
        const std::uint32_t page_w = 64, page_h = (c.fpsm == 0x02 || c.fpsm == 0x0A) ? 64 : 32;
        for (std::int64_t row = rect[1] / page_h; row <= rect[3] / page_h; ++row)
            mark_pages(c.fbp + static_cast<std::uint32_t>(row) * std::max<std::uint32_t>(c.fbw, 1) + static_cast<std::uint32_t>(rect[0] / page_w),
                       static_cast<std::uint32_t>(rect[2] / page_w - rect[0] / page_w + 1), kDrawn);
    }
    if (c.features & kTextured) {  // drawn by the GS since its last upload: a real render-to-texture read
        const std::uint64_t tex0 = s.ctx[(attr >> 9) & 1].tex0;
        const std::uint32_t first = c.tbp / 32, n = texture_pages(tex0);
        bool drawn = false;
        for (std::uint32_t i = 0; i < n && first + i < 512; ++i) drawn |= g_page[first + i] == kDrawn;
        (drawn ? g_tex_drawn : g_tex_uploaded)++;
    }
    if (c.features & kTextured) {
        const long t = target_of(c.tbp);
        (t >= 0 ? g.tex_target : g.tex_ee)++;
        if (t >= 0) ++g_tex_hits[static_cast<std::uint32_t>(t)];
    }
    if (box) ++g_target_shapes[c.fbp];
}

void stats_sync(const char *kind, std::uint32_t block, std::uint32_t bytes) {
    if (!std::strcmp(kind, "download")) ++g.downloads, g.download_bytes += bytes;
    else if (!std::strcmp(kind, "local_copy")) ++g.local_copies;
    else if (!std::strcmp(kind, "upload")) {
        const long t = target_of(block);
        if (t >= 0) ++g.uploads_into_target, ++g_upload_hits[static_cast<std::uint32_t>(t)];
        mark_pages(block / 32, std::max<std::uint32_t>(1, (bytes + 8191) / 8192), kUploaded, &g_upload_over_drawn);
    }
    else if (!std::strcmp(kind, "display")) g.displays_non_target += !in_target(block);
}

void stats_frame() {
    const std::uint64_t now = ps2::frames_run();
    if (now - g.frame0 < 300) return;
    const double f = static_cast<double>(now - g.frame0);
    std::fprintf(stderr, "[hwr-stats] frames to %llu: per frame %.0f shapes, %.0f box px; painter %.0f shapes, %.0f box px (%.1f %% of box px)",
                 static_cast<unsigned long long>(now), g.shapes / f, g.box / f, g.painter_shapes / f, g.painter_box / f,
                 g.box ? 100.0 * g.painter_box / g.box : 0.0);
    for (int i = 0; i < kReasons; ++i)
        if (g.reason_box[i]) std::fprintf(stderr, " | %s %.1f %%", kReasonNames[i], 100.0 * g.reason_box[i] / std::max<std::uint64_t>(g.box, 1));
    std::fprintf(stderr, "\n[hwr-stats]   features (shapes/frame):");
    for (int i = 0; i < kFeatures; ++i) std::fprintf(stderr, " %s %.0f", kFeatureNames[i], g.feature_shapes[i] / f);
    std::fprintf(stderr, "\n[hwr-stats]   textures/frame: from EE %.0f, from target %.0f | sync/frame: downloads %.2f (%.0f KB), local copies %.2f, uploads into a target %.2f, displays of a non-target %.2f | targets %zu\n",
                 g.tex_ee / f, g.tex_target / f, g.downloads / f, g.download_bytes / f / 1024.0, g.local_copies / f, g.uploads_into_target / f,
                 g.displays_non_target / f, g.targets.size());
    for (const auto &[fbp, t] : g.targets)
        std::fprintf(stderr, "[hwr-stats]   target fbp %u fbw %u page rows %u: shapes/frame %.0f, textures read from it %.0f, uploads into it %.1f\n", fbp, t.fbw, t.page_rows,
                     g_target_shapes[fbp] / f, g_tex_hits[fbp] / f, g_upload_hits[fbp] / f);
    std::fprintf(stderr, "[hwr-stats]   by page owner: textures drawn-by-GS %.0f, uploaded-by-EE %.0f per frame; upload pages over GS-drawn pages %.1f per frame\n",
                 g_tex_drawn / f, g_tex_uploaded / f, g_upload_over_drawn / f);
    g_tex_drawn = g_tex_uploaded = g_upload_over_drawn = 0;
    g_tex_hits.clear(), g_upload_hits.clear(), g_target_shapes.clear();
    g = Window{};
    g.frame0 = now;
}

}  // namespace ps2::gs::hwr
