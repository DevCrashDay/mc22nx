#include "backend.hpp"

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <cstring>
#include <string>
#include <vector>

extern const char kGlslUpload[];
extern const char kGlslCopyRead[];
extern const char kGlslCopyWrite[];
extern const char kGlslLoadClut[];
extern const char kGlslDisplay[];
extern const char kGlslRaster[];
extern const char kGlslRasterMip[];
extern const char kGlslResolve[];
extern const char kGlslSnapshot[];
extern const char kGlslDecode[];
extern const char kGlslBlitVert[];
extern const char kGlslBlitFrag[];
extern const char kGlslBlitEasuFrag[];
extern const char kGlslTexFetch[];
extern const char kGlslHwrVert[];
extern const char kGlslHwrFrag[];
extern const char kGlslTexBlitFrag[];
extern const char kGlslOverlayVert[];

namespace ps2::gs::gpu::be {

namespace {

using PFNGLGETSTRINGPROC_PS2 = const GLubyte *(APIENTRY *)(GLenum);
using PFNGLGETINTEGERVPROC_PS2 = void (APIENTRY *)(GLenum, GLint *);
using PFNGLGETERRORPROC_PS2 = GLenum (APIENTRY *)(void);
using PFNGLFINISHPROC_PS2 = void (APIENTRY *)(void);
using PFNGLVIEWPORTPROC_PS2 = void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei);
using PFNGLCLEARCOLORPROC_PS2 = void (APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat);
using PFNGLCLEARPROC_PS2 = void (APIENTRY *)(GLbitfield);
using PFNGLDRAWARRAYSPROC_PS2 = void (APIENTRY *)(GLenum, GLint, GLsizei);
using PFNGLENABLEPROC_PS2 = void (APIENTRY *)(GLenum);
using PFNGLDISABLEPROC_PS2 = void (APIENTRY *)(GLenum);
using PFNGLGENTEXTURESPROC_PS2 = void (APIENTRY *)(GLsizei, GLuint *);
using PFNGLDELETETEXTURESPROC_PS2 = void (APIENTRY *)(GLsizei, const GLuint *);
using PFNGLBINDTEXTUREPROC_PS2 = void (APIENTRY *)(GLenum, GLuint);
using PFNGLREADPIXELSPROC_PS2 = void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
using PFNGLTEXPARAMETERIPROC_PS2 = void (APIENTRY *)(GLenum, GLenum, GLint);
using PFNGLSCISSORPROC_PS2 = void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei);
using PFNGLDEPTHFUNCPROC_PS2 = void (APIENTRY *)(GLenum);
using PFNGLDEPTHMASKPROC_PS2 = void (APIENTRY *)(GLboolean);
using PFNGLCOLORMASKPROC_PS2 = void (APIENTRY *)(GLboolean, GLboolean, GLboolean, GLboolean);
using PFNGLGETTEXIMAGEPROC_PS2 = void (APIENTRY *)(GLenum, GLint, GLenum, GLenum, void *);
using PFNGLPIXELSTOREIPROC_PS2 = void (APIENTRY *)(GLenum, GLint);
using PFNGLTEXSUBIMAGE2DPROC_PS2 = void (APIENTRY *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);

#define PS2_GL_PROCS(X) \
    X(PFNGLGETSTRINGPROC_PS2, glGetString) \
    X(PFNGLGETINTEGERVPROC_PS2, glGetIntegerv) \
    X(PFNGLGETERRORPROC_PS2, glGetError) \
    X(PFNGLFINISHPROC_PS2, glFinish) \
    X(PFNGLVIEWPORTPROC_PS2, glViewport) \
    X(PFNGLCLEARCOLORPROC_PS2, glClearColor) \
    X(PFNGLCLEARPROC_PS2, glClear) \
    X(PFNGLDRAWARRAYSPROC_PS2, glDrawArrays) \
    X(PFNGLENABLEPROC_PS2, glEnable) \
    X(PFNGLDISABLEPROC_PS2, glDisable) \
    X(PFNGLGENTEXTURESPROC_PS2, glGenTextures) \
    X(PFNGLDELETETEXTURESPROC_PS2, glDeleteTextures) \
    X(PFNGLBINDTEXTUREPROC_PS2, glBindTexture) \
    X(PFNGLREADPIXELSPROC_PS2, glReadPixels) \
    X(PFNGLTEXBUFFERPROC, glTexBuffer) \
    X(PFNGLBINDIMAGETEXTUREPROC, glBindImageTexture) \
    X(PFNGLUNIFORM3UIPROC, glUniform3ui) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLFENCESYNCPROC, glFenceSync) \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) \
    X(PFNGLDELETESYNCPROC, glDeleteSync) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData) \
    X(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) \
    X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) \
    X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLGENQUERIESPROC, glGenQueries) \
    X(PFNGLDELETEQUERIESPROC, glDeleteQueries) \
    X(PFNGLQUERYCOUNTERPROC, glQueryCounter) \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v) \
    X(PFNGLGETINTEGERI_VPROC, glGetIntegeri_v) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)     X(PFNGLTEXPARAMETERIPROC_PS2, glTexParameteri)     X(PFNGLSCISSORPROC_PS2, glScissor)     X(PFNGLDEPTHFUNCPROC_PS2, glDepthFunc)     X(PFNGLDEPTHMASKPROC_PS2, glDepthMask)     X(PFNGLCOLORMASKPROC_PS2, glColorMask)     X(PFNGLGETTEXIMAGEPROC_PS2, glGetTexImage)     X(PFNGLPIXELSTOREIPROC_PS2, glPixelStorei)     X(PFNGLTEXSUBIMAGE2DPROC_PS2, glTexSubImage2D)     X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)     X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)     X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)     X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D)     X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData)     X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv)     X(PFNGLGENSAMPLERSPROC, glGenSamplers)     X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)     X(PFNGLBINDSAMPLERPROC, glBindSampler)     X(PFNGLACTIVETEXTUREPROC, glActiveTexture)     X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)     X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)     X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)

#define PS2_GL_DECL(type, name) type p_##name = nullptr;
PS2_GL_PROCS(PS2_GL_DECL)
#undef PS2_GL_DECL

bool load_gl_procs() {
#define PS2_GL_LOAD(type, name) \
    p_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name)); \
    if (!p_##name) { std::fprintf(stderr, "[gl] missing proc: %s\n", #name); return false; }
    PS2_GL_PROCS(PS2_GL_LOAD)
#undef PS2_GL_LOAD
    return true;
}

SDL_Window *g_window = nullptr;
bool g_owns_window = false;
SDL_GLContext g_ctx = nullptr;
GLuint g_vao = 0;
GLuint g_blit_prog = 0;
GLuint g_overlay_prog = 0;
GLuint g_overlay_buf = 0, g_overlay_rect = 0;
std::size_t g_overlay_cap = 0;
bool g_has_surface = false;
std::uint32_t g_surface_w = 0, g_surface_h = 0;

struct BindingSet {
    Buffer buffers[8]{};
    int count = 0;
    int dynamic_slot = 2;
    std::uint32_t dynamic_size = 256;
} g_bindings;

Buffer g_blit_scratch{};
Buffer g_blit_ubo{};

GLuint g_queries[2] = {0, 0};
bool g_has_ts = false;

// The shaders see every storage buffer as an r32ui buffer image (scripts/glsl_compat.py: nouveau compiles storage
// buffers to LD/ST, which Ryujinx cannot run). Each buffer gets a buffer texture over all of it, found by buffer name.
std::vector<GLuint> g_buffer_texture;
std::vector<std::pair<std::size_t, GLenum>> g_buffer_size;  // by buffer name: bytes and usage hint, for write_buffer_fresh

GLuint texture_of(GLuint buffer) { return buffer < g_buffer_texture.size() ? g_buffer_texture[buffer] : 0; }

void make_texture_view(GLuint buffer) {
    if (buffer >= g_buffer_texture.size()) g_buffer_texture.resize(buffer + 1, 0);
    GLuint &t = g_buffer_texture[buffer];
    if (!t) p_glGenTextures(1, &t);
    p_glBindTexture(GL_TEXTURE_BUFFER, t);
    p_glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, buffer);
    p_glBindTexture(GL_TEXTURE_BUFFER, 0);
}

void drop_texture_view(GLuint buffer) {
    if (buffer < g_buffer_texture.size() && g_buffer_texture[buffer]) {
        p_glDeleteTextures(1, &g_buffer_texture[buffer]);
        g_buffer_texture[buffer] = 0;
    }
}

void bind_image(GLuint unit, GLuint buffer, GLenum access) {
    p_glBindImageTexture(unit, texture_of(buffer), 0, GL_FALSE, 0, access, GL_R32UI);
}

GLuint compile_shader(GLenum stage, const char *src) {
    GLuint s = p_glCreateShader(stage);
    p_glShaderSource(s, 1, &src, nullptr);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[gl] shader compile error: %s\n", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link_program(GLuint a, GLuint b = 0) {
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, a);
    if (b) p_glAttachShader(p, b);
    p_glLinkProgram(p);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "[gl] program link error: %s\n", log);
        p_glDeleteProgram(p);
        return 0;
    }
    return p;
}

// Waits until the GPU's work is done and in memory. On the Switch one glFinish (or a fence wait) returns a step early: every
// read right after one was wrong, none after two (PS2_GL_SELFTEST, "wait before a read").
void gpu_idle() {
    p_glFinish();
#ifdef __SWITCH__
    p_glFinish();
#endif
}

// One throwaway dispatch of a new program, on dummy buffers with all parameters zero (the GS shaders then stop at once), and a
// wait for it: on the Switch the first work groups of a program's first dispatch ran before Mesa had finished uploading its
// code, and their results were lost (PS2_GL_SELFTEST: always the first few hundred words, only on a new program).
void warm_up(GLuint program) {
    static GLuint dummy = 0, dummy_ubo = 0;
    if (!dummy) {
        p_glGenBuffers(1, &dummy);
        p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, dummy);
        p_glBufferData(GL_SHADER_STORAGE_BUFFER, 1 << 20, nullptr, GL_DYNAMIC_DRAW);
        p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        make_texture_view(dummy);
        p_glGenBuffers(1, &dummy_ubo);
        p_glBindBuffer(GL_UNIFORM_BUFFER, dummy_ubo);
        const std::vector<std::uint8_t> zero(64 << 10, 0);
        p_glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(zero.size()), zero.data(), GL_STATIC_DRAW);
        p_glBindBuffer(GL_UNIFORM_BUFFER, 0);
        p_glFinish();
    }
    p_glUseProgram(program);
    for (GLuint unit = 0; unit < 8; ++unit) bind_image(unit, dummy, GL_READ_WRITE);
    for (GLuint slot = 0; slot < 4; ++slot) p_glBindBufferBase(GL_UNIFORM_BUFFER, slot, dummy_ubo);
    p_glDispatchCompute(1, 1, 1);
    p_glMemoryBarrier(GL_ALL_BARRIER_BITS);
    p_glUseProgram(0);
    gpu_idle();
}

GLuint build_compute(const char *src) {
    GLuint cs = compile_shader(GL_COMPUTE_SHADER, src);
    if (!cs) return 0;
    GLuint p = link_program(cs);
    p_glDeleteShader(cs);
    if (p) warm_up(p);
    return p;
}

const char *const kOverlayVert = R"(#version 430 core
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(p.x, 1.0 - p.y);
}
)";

const char *const kOverlayFrag = R"(#version 430 core
layout(r32ui, binding = 0) uniform readonly uimageBuffer pix;
layout(std140, binding = 1) uniform Rect { uvec4 size; } rect;
in vec2 v_uv;
out vec4 frag_color;
void main() {
    uint w = max(rect.size.x, 1u), h = max(rect.size.y, 1u);
    uint x = min(uint(v_uv.x * float(w)), w - 1u);
    uint y = min(uint(v_uv.y * float(h)), h - 1u);
    uint c = imageLoad(pix, int(y * w + x)).x;
    frag_color = vec4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float((c >> 24) & 0xFFu)) / 255.0;
}
)";

}  // namespace

void self_test();

bool init_device(Limits &limits, std::string &compiler_tag) {
    compiler_tag.clear();
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "[gl] SDL_InitSubSystem(VIDEO): %s\n", SDL_GetError());
            return false;
        }
    }
    g_ctx = SDL_GL_GetCurrentContext();
    g_window = SDL_GL_GetCurrentWindow();
    if (!g_ctx) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        g_window = SDL_CreateWindow("ps2gl", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 16, 16,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!g_window) {
            std::fprintf(stderr, "[gl] SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
        g_owns_window = true;
        g_ctx = SDL_GL_CreateContext(g_window);
        if (!g_ctx) {
            std::fprintf(stderr, "[gl] SDL_GL_CreateContext: %s\n", SDL_GetError());
            SDL_DestroyWindow(g_window);
            g_window = nullptr;
            g_owns_window = false;
            return false;
        }
        SDL_GL_MakeCurrent(g_window, g_ctx);
    }
    if (!load_gl_procs()) return false;

    const char *renderer = reinterpret_cast<const char *>(p_glGetString(GL_RENDERER));
    const char *version = reinterpret_cast<const char *>(p_glGetString(GL_VERSION));
    std::fprintf(stderr, "[gpu] %s on OpenGL %s\n", renderer ? renderer : "unknown", version ? version : "?");

    GLint max_wg_x = 256, max_wg_y = 256, max_wg_inv = 256;
    GLint max_cnt_x = 0, max_cnt_y = 0, max_cnt_z = 0, max_img = 0, max_cs_img = 0;
    p_glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &max_wg_x);
    p_glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &max_wg_y);
    p_glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &max_cnt_x);
    p_glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &max_cnt_y);
    p_glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &max_cnt_z);
    p_glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &max_wg_inv);
    p_glGetIntegerv(GL_MAX_IMAGE_UNITS, &max_img);
    p_glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &max_cs_img);
    GLint max_texels = 0;
    p_glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texels);  // buffers are seen as r32ui buffer images
    limits.max_storage_binding = max_texels > 0 ? static_cast<std::uint64_t>(max_texels) * 4 : (128ull << 20);
    std::fprintf(stderr, "[gl] buffer images up to %d texels; wg_cnt=(%d,%d,%d) imgs=%d cs_imgs=%d\n",
                 max_texels, max_cnt_x, max_cnt_y, max_cnt_z, max_img, max_cs_img);
    limits.max_buffer = limits.max_storage_binding;
    limits.max_wg_x = static_cast<std::uint32_t>(max_wg_x);
    limits.max_wg_y = static_cast<std::uint32_t>(max_wg_y);
    limits.max_wg_invocations = static_cast<std::uint32_t>(max_wg_inv);
    limits.timestamps = false;  // optional via GL_TIMESTAMP; keep simple/portable for bring-up

    p_glGenVertexArrays(1, &g_vao);
    p_glBindVertexArray(g_vao);
    if (std::getenv("PS2_GL_SELFTEST")) self_test();
    return true;
}

void shutdown_device() {
    if (g_blit_prog) p_glDeleteProgram(g_blit_prog), g_blit_prog = 0;
    if (g_overlay_prog) p_glDeleteProgram(g_overlay_prog), g_overlay_prog = 0;
    if (g_overlay_buf) p_glDeleteBuffers(1, &g_overlay_buf), g_overlay_buf = 0;
    if (g_overlay_rect) p_glDeleteBuffers(1, &g_overlay_rect), g_overlay_rect = 0;
    if (g_vao) p_glDeleteVertexArrays(1, &g_vao), g_vao = 0;
    if (g_has_ts) p_glDeleteQueries(2, g_queries), g_has_ts = false;
    if (g_owns_window) {
        if (g_ctx) SDL_GL_DeleteContext(g_ctx), g_ctx = nullptr;
        if (g_window) SDL_DestroyWindow(g_window), g_window = nullptr;
        g_owns_window = false;
    }
}

void pump() {}

void wait_idle() { gpu_idle(); }

Buffer create_buffer(std::size_t bytes, std::uint32_t usage) {
    GLuint id = 0;
    p_glGenBuffers(1, &id);
    GLenum target = (usage & kUniform) ? GL_UNIFORM_BUFFER : GL_SHADER_STORAGE_BUFFER;
    GLenum hint = (usage & kMapRead) ? GL_STREAM_READ : GL_DYNAMIC_DRAW;
    p_glBindBuffer(target, id);
    p_glBufferData(target, static_cast<GLsizeiptr>(std::max<std::size_t>(bytes, 16)), nullptr, hint);
    p_glBindBuffer(target, 0);
    if (!(usage & kUniform)) make_texture_view(id);
    if (std::getenv("PS2_GL_SELFTEST") && bytes >= (1u << 20)) std::fprintf(stderr, "[gl] buffer %u: %.1f MB\n", id, bytes / 1048576.0);
    if (id >= g_buffer_size.size()) g_buffer_size.resize(id + 1);
    g_buffer_size[id] = {std::max<std::size_t>(bytes, 16), hint};
    return Buffer{nullptr, id};
}

void destroy_buffer(Buffer &b) {
    drop_texture_view(b.id);
    if (b.id) p_glDeleteBuffers(1, &b.id);
    b = Buffer{};
}

#ifdef __SWITCH__
extern "C" void armDCacheFlush(void *addr, size_t size);  // libnx: clean and invalidate the CPU's data cache lines
#endif

// The Switch's Mesa maps buffers with the CPU's cache on, and the GPU does not see the CPU's cache (nor the CPU the GPU's
// writes) unless the cache lines are cleaned and invalidated; glBufferSubData and glGetBufferSubData do not do it
// (PS2_GL_SELFTEST: reads came back with the old contents, the GPU worked on the old contents). So the CPU's side of every
// transfer goes through a mapping with armDCacheFlush around it there. Elsewhere this is the driver's job.
void cpu_write(GLenum target, std::size_t offset, const void *data, std::size_t bytes) {
#ifdef __SWITCH__
    // unsynchronized: the caller makes sure the GPU no longer uses the range (glFinish, or new storage), and no staging copy
    if (void *p = p_glMapBufferRange(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT)) {
        armDCacheFlush(p, bytes);  // no stale dirty lines that a later clean would write over the GPU's data next to ours
        std::memcpy(p, data, bytes);
        armDCacheFlush(p, bytes);  // to memory, for the GPU
        p_glUnmapBuffer(target);
        return;
    }
#endif
    p_glBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), data);
}

void cpu_read(GLenum target, std::size_t offset, void *out, std::size_t bytes, bool flush = true) {
#ifdef __SWITCH__
    if (const void *p = p_glMapBufferRange(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT)) {
        if (flush) armDCacheFlush(const_cast<void *>(p), bytes);  // drop lines from before the GPU wrote
        std::memcpy(out, p, bytes);
        p_glUnmapBuffer(target);
        return;
    }
#endif
    (void)flush;
    p_glGetBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), out);
}

void write_buffer_fresh(Buffer b, const void *data, std::size_t bytes) {
    if (!b.id || !bytes) return;
#ifdef __SWITCH__
    static const int fresh_mode = std::getenv("PS2_GL_FRESH_MODE") ? std::atoi(std::getenv("PS2_GL_FRESH_MODE")) : 2;
#else
    static const int fresh_mode = std::getenv("PS2_GL_FRESH_MODE") ? std::atoi(std::getenv("PS2_GL_FRESH_MODE")) : 0;
#endif
    const auto [cap, hint] = b.id < g_buffer_size.size() ? g_buffer_size[b.id] : std::pair<std::size_t, GLenum>{0, GL_DYNAMIC_DRAW};
    if (fresh_mode >= 2) gpu_idle();
    p_glBindBuffer(GL_COPY_WRITE_BUFFER, b.id);
    if (fresh_mode < 2 && cap >= bytes) {
        p_glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(cap), nullptr, hint);  // new storage, no wait
        if (fresh_mode == 1 && texture_of(b.id)) make_texture_view(b.id);
    }
    cpu_write(GL_COPY_WRITE_BUFFER, 0, data, bytes);
    p_glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

// PS2_GL_TIMING=1: CPU time spent in each backend call, printed every 2000 calls of a kind
struct Spent {
    std::string name;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::size_t bytes = 0;
    explicit Spent(std::string n, std::size_t b = 0) : name(std::move(n)), bytes(b) {}
    ~Spent() {
        static const bool on = std::getenv("PS2_GL_TIMING") != nullptr;
        if (!on) return;
        struct Sum { double ms = 0; std::uint64_t n = 0, bytes = 0; };
        static std::map<std::string, Sum> sums;
        static std::mutex lock;
        std::lock_guard guard(lock);
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        Sum &s = sums[name];
        s.ms += std::chrono::duration<double, std::milli>(now - t0).count();
        s.bytes += bytes;
        ++s.n;
        if (now - last > std::chrono::seconds(5)) {  // every 5 s: what each kind of call cost in that time
            last = now;
            for (auto &[k, v] : sums) {
                std::fprintf(stderr, "[gl-time] %s: %llu calls, %.1f ms, %.1f MB\n", k.c_str(), static_cast<unsigned long long>(v.n), v.ms, v.bytes / 1048576.0);
                v = Sum{};
            }
        }
    }
};

void write_buffer(Buffer b, std::size_t offset, const void *data, std::size_t bytes) {
    if (!b.id || !bytes) return;
    Spent spent("write_buffer", bytes);
#ifdef __SWITCH__
    // Into a buffer the GPU may still use, the Switch's Mesa writes through a staging buffer and a GPU copy that lands after
    // later work (PS2_GL_SELFTEST): wait until the GPU is idle, then Mesa writes the buffer directly.
    gpu_idle();
#endif
    p_glBindBuffer(GL_COPY_WRITE_BUFFER, b.id);
    cpu_write(GL_COPY_WRITE_BUFFER, offset, data, bytes);
    p_glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

// A read waits for all GPU work and reads the buffer itself. (A GPU copy into a staging buffer first is wrong on the Switch:
// its Mesa does not finish buffer copies before it says the GPU is idle; PS2_GL_SELFTEST shows it.)
void read_buffer(Buffer b, std::size_t offset, void *out, std::size_t bytes) {
    if (!b.id || !bytes) return;
    Spent spent("read_buffer", bytes);
    gpu_idle();  // Mesa does not wait for shader image writes before a buffer read on its own
    p_glBindBuffer(GL_COPY_READ_BUFFER, b.id);
    cpu_read(GL_COPY_READ_BUFFER, offset, out, bytes);
    p_glBindBuffer(GL_COPY_READ_BUFFER, 0);
}

// Buffer copies are done by a compute shader: glCopyBufferSubData is not finished in time on the Switch (PS2_GL_SELFTEST).
GLuint g_copy_prog = 0;
GLint g_copy_args = -1;

void copy_program() {
    if (!g_copy_prog) {
        g_copy_prog = build_compute(R"(#version 430 core
layout(local_size_x = 64) in;
layout(r32ui, binding = 0) uniform readonly uimageBuffer src;
layout(r32ui, binding = 1) uniform writeonly uimageBuffer dst;
uniform uvec3 args;  // first source word, first destination word, words
void main() {
    uint i = gl_GlobalInvocationID.y * 65535u * 64u + gl_GlobalInvocationID.x;
    if (i < args.z) imageStore(dst, int(args.y + i), imageLoad(src, int(args.x + i)));
}
)");
        if (g_copy_prog) g_copy_args = p_glGetUniformLocation(g_copy_prog, "args");
    }
}

void copy_buffer(Buffer src, std::size_t src_off, Buffer dst, std::size_t dst_off, std::size_t bytes) {
    if (!src.id || !dst.id || !bytes) return;
    Spent spent("copy_buffer", bytes);
    copy_program();
    if (!g_copy_prog || (src_off | dst_off | bytes) & 3u) {  // not whole words (never so far): the driver's copy
        p_glBindBuffer(GL_COPY_READ_BUFFER, src.id);
        p_glBindBuffer(GL_COPY_WRITE_BUFFER, dst.id);
        p_glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(src_off), static_cast<GLintptr>(dst_off),
                              static_cast<GLsizeiptr>(bytes));
        p_glBindBuffer(GL_COPY_READ_BUFFER, 0);
        p_glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        return;
    }
    const GLuint words = static_cast<GLuint>(bytes / 4), groups = (words + 63) / 64;
    p_glUseProgram(g_copy_prog);
    p_glUniform3ui(g_copy_args, static_cast<GLuint>(src_off / 4), static_cast<GLuint>(dst_off / 4), words);
    bind_image(0, src.id, GL_READ_ONLY);
    bind_image(1, dst.id, GL_WRITE_ONLY);
    p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    p_glDispatchCompute(std::min<GLuint>(groups, 65535u), (groups + 65534u) / 65535u, 1);
    p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    p_glUseProgram(0);
}

bool map_read_async(Buffer, std::size_t, bool *busy, bool *ready) {
    *busy = true;
    *ready = true;
    return true;
}

bool collect_mapped(Buffer b, void *dst, std::size_t bytes) {
    read_buffer(b, 0, dst, bytes);
    return true;
}

void set_compute_bindings(const Buffer *buffers, int count, int dynamic_slot, std::uint32_t dynamic_size) {
    g_bindings.count = std::min(count, 8);
    for (int i = 0; i < g_bindings.count; ++i) g_bindings.buffers[i] = buffers[i];
    g_bindings.dynamic_slot = dynamic_slot;
    g_bindings.dynamic_size = dynamic_size;
}

bool init_compute_shaders(int, int, bool, bool, bool, const std::string &, Pipeline *out_plain, Pipeline *out_mip) {
    const char *sources[9] = {
        kGlslUpload, kGlslCopyRead, kGlslCopyWrite, kGlslLoadClut, kGlslDisplay,
        kGlslRaster, kGlslResolve, kGlslSnapshot, kGlslDecode
    };
    for (int i = 0; i < 9; ++i) {
        GLuint p = build_compute(sources[i]);
        if (!p) return false;
        out_plain[i] = Pipeline{nullptr, p};
        std::fprintf(stderr, "[gl] program %u: %s\n", p, (const char *[]){"upload", "copy_read", "copy_write", "load_clut", "display", "raster", "resolve", "snapshot", "decode"}[i]);
    }
    copy_program();  // now, not on the first copy (warm_up)
    GLuint rm = build_compute(kGlslRasterMip);
    if (rm) *out_mip = Pipeline{nullptr, rm};
    return true;
}

void destroy_pipeline(Pipeline &p) {
    if (p.id) p_glDeleteProgram(p.id);
    p = Pipeline{};
}

bool attach_surface(const SurfaceTarget &target, bool) {
    if (target.surface) {
        SDL_Window *win = static_cast<SDL_Window *>(target.surface);
        if (g_owns_window && g_window != win) {
            if (g_ctx) SDL_GL_DeleteContext(g_ctx), g_ctx = nullptr;
            SDL_DestroyWindow(g_window);
            g_window = nullptr;
            g_owns_window = false;
        }
        g_window = win;
        if (!g_ctx) g_ctx = SDL_GL_GetCurrentContext();
        if (!g_ctx) g_ctx = SDL_GL_CreateContext(g_window);
        if (!g_ctx) {
            std::fprintf(stderr, "[gl] SDL_GL_CreateContext on surface: %s\n", SDL_GetError());
            return false;
        }
        SDL_GL_MakeCurrent(g_window, g_ctx);
        SDL_GL_SetSwapInterval(1);
    }
    g_surface_w = target.width;
    g_surface_h = target.height;

    if (!g_blit_prog) {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, kGlslBlitVert);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kGlslBlitFrag);
        if (vs && fs) g_blit_prog = link_program(vs, fs);
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
    }
    if (!g_overlay_prog) {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, kOverlayVert);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kOverlayFrag);
        if (vs && fs) g_overlay_prog = link_program(vs, fs);
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        p_glGenBuffers(1, &g_overlay_rect);
        p_glBindBuffer(GL_UNIFORM_BUFFER, g_overlay_rect);
        p_glBufferData(GL_UNIFORM_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
        p_glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
    g_has_surface = g_blit_prog != 0;
    return g_has_surface;
}

void resize_surface(std::uint32_t w, std::uint32_t h) {
    g_surface_w = w;
    g_surface_h = h;
}

bool has_surface() { return g_has_surface; }

void set_blit_buffers(Buffer scratch, Buffer blit_params) {
    g_blit_scratch = scratch;
    g_blit_ubo = blit_params;
}

void update_overlay(const void *rgba, std::uint32_t w, std::uint32_t h, std::uint32_t, std::uint32_t) {
    if (!rgba || !w || !h) return;
    const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
    if (bytes > g_overlay_cap) {
        if (!g_overlay_buf) p_glGenBuffers(1, &g_overlay_buf);
        p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_overlay_buf);
        p_glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_DRAW);
        make_texture_view(g_overlay_buf);  // the new storage needs a new view
        g_overlay_cap = bytes;
    }
    p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_overlay_buf);
    cpu_write(GL_SHADER_STORAGE_BUFFER, 0, rgba, bytes);
    p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    const std::uint32_t rect[4] = {w, h, 0, 0};
    p_glBindBuffer(GL_UNIFORM_BUFFER, g_overlay_rect);
    cpu_write(GL_UNIFORM_BUFFER, 0, rect, sizeof rect);
    p_glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

static void run_compute_cmds(const ComputeCmd *cmds, std::size_t count) {
    if (!count) return;
    Spent all("run_compute_cmds", count);
    for (int i = 0; i < g_bindings.count; ++i) {
        if (i == g_bindings.dynamic_slot) continue;
        // slots 1, 3 and 6 (the tables, the per-submit data, the texture cache) are readonly in every shader (glsl/*.comp)
        const bool read_only = i == 1 || i == 3 || i == 6;
        if (g_bindings.buffers[i].id) bind_image(static_cast<GLuint>(i), g_bindings.buffers[i].id, read_only ? GL_READ_ONLY : GL_READ_WRITE);
    }
    GLuint cur = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const ComputeCmd &c = cmds[i];
        if (c.pipeline.id != cur) {
            p_glUseProgram(c.pipeline.id);
            cur = c.pipeline.id;
        }
        if (g_bindings.buffers[g_bindings.dynamic_slot].id) {
            p_glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(g_bindings.dynamic_slot),
                                g_bindings.buffers[g_bindings.dynamic_slot].id,
                                static_cast<GLintptr>(c.param_offset), static_cast<GLsizeiptr>(g_bindings.dynamic_size));
        }
        // PS2_GL_TIMING=1: wait for each dispatch and add up the time per program (slows everything; for finding the costly one)
        // PS2_GL_TIMING=2: only the call times, without waiting for each dispatch
        static const bool timing = std::getenv("PS2_GL_TIMING") != nullptr && std::string(std::getenv("PS2_GL_TIMING")) != "2";
        if (timing) {
            Spent before("wait for earlier GPU work before a dispatch");
            p_glFinish();
        }
        // PS2_GL_SKIP=<program>,...: leave out the dispatches of these programs (the picture is wrong; to measure their cost)
        static const std::string skip = std::getenv("PS2_GL_SKIP") ? "," + std::string(std::getenv("PS2_GL_SKIP")) + "," : "";
        if (!skip.empty() && skip.find("," + std::to_string(c.pipeline.id) + ",") != std::string::npos) continue;
        const auto t0 = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // PS2_GL_TRACE=<n>: the first n dispatches are logged before they go out and waited for, to name one that hangs the GPU
        static long trace = std::getenv("PS2_GL_TRACE") ? std::atol(std::getenv("PS2_GL_TRACE")) : 0;
        const bool traced = trace > 0 && trace--;
        if (traced) {
            std::fprintf(stderr, "[gl-trace] program %u, %u x %u x %u groups, parameters at %u ...", c.pipeline.id, c.gx, c.gy, c.gz, c.param_offset);
            std::fflush(stderr);
        }
        p_glDispatchCompute(c.gx, c.gy, c.gz);
        p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        if (traced) {
            const auto s0 = std::chrono::steady_clock::now();
            gpu_idle();
            const GLenum err = p_glGetError();
            std::fprintf(stderr, " done in %.2f ms (err=0x%X)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count(), static_cast<unsigned>(err));
            std::fflush(stderr);
        }
        if (timing) {
            p_glFinish();
            static std::map<GLuint, std::pair<double, std::uint64_t>> spent;
            static std::uint64_t total = 0;
            auto &[ms, n] = spent[c.pipeline.id];
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            n += static_cast<std::uint64_t>(c.gx) * c.gy * c.gz;
            if (++total % 2000 == 0)
                for (const auto &[id, v] : spent)
                    std::fprintf(stderr, "[gl-time] program %u: %.1f ms over %llu groups\n", id, v.first, static_cast<unsigned long long>(v.second));
        }
    }
}

void submit_compute(const ComputeCmd *cmds, std::size_t count, bool, std::uint64_t &, std::uint64_t &) {
    Spent spent("submit_compute", count);
    run_compute_cmds(cmds, count);
}

// After the picture is drawn into the window: the quick menu over it, PS2_GL_SCREEN_DUMP, the swap.
void finish_present(const PresentInfo &info) {
    if (info.overlay_on && g_overlay_prog && g_overlay_buf) {
        p_glEnable(GL_BLEND);
        p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        p_glViewport(static_cast<GLint>(info.ov_x), static_cast<GLint>(info.ov_y),
                   static_cast<GLsizei>(info.ov_w), static_cast<GLsizei>(info.ov_h));
        p_glUseProgram(g_overlay_prog);
        bind_image(0, g_overlay_buf, GL_READ_ONLY);
        p_glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_overlay_rect);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glDisable(GL_BLEND);
    }
    {
        Spent blit("present: blit (finish)");
        static const bool timing = std::getenv("PS2_GL_TIMING") != nullptr;
        if (timing) p_glFinish();
    }
    // PS2_GL_SCREEN_DUMP=<file>@<picture>: the window as it is shown (top row first), to check what reaches the screen
    static const std::string screen_dump = std::getenv("PS2_GL_SCREEN_DUMP") ? std::getenv("PS2_GL_SCREEN_DUMP") : "";
    static std::uint64_t pictures = 0;
    // <file>@<n>[,<n>...]: a %u in the file name becomes the picture number
    const std::size_t at = screen_dump.rfind('@');
    bool hit = false;
    if (at != std::string::npos) {
        ++pictures;
        for (const char *q = screen_dump.c_str() + at + 1; *q;) {
            char *end = nullptr;
            hit |= std::strtoull(q, &end, 10) == pictures;
            if (end == q || *end != ',') break;
            q = end + 1;
        }
    }
    if (hit) {
        char name[512];
        std::snprintf(name, sizeof name, screen_dump.substr(0, at).c_str(), static_cast<unsigned>(pictures));
        const int w = static_cast<int>(info.surface_w), h = static_cast<int>(info.surface_h);
        std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
        p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        if (std::FILE *f = std::fopen(name, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; --y)  // OpenGL's rows start at the bottom
                for (int x = 0; x < w; ++x) std::fwrite(&px[(static_cast<std::size_t>(y) * w + x) * 4], 1, 3, f);
            std::fclose(f);
            std::fprintf(stderr, "[gl] screen of picture %llu written (%dx%d)\n", static_cast<unsigned long long>(pictures), w, h);
        }
    }
    if (g_window) {
        Spent swap("present: SDL_GL_SwapWindow");
        SDL_GL_SwapWindow(g_window);
    }
}

bool submit_present(const ComputeCmd *cmds, std::size_t count, const PresentInfo &info,
                    bool, std::uint64_t &, std::uint64_t &) {
    Spent spent("submit_present", count);
    run_compute_cmds(cmds, count);
    if (!g_has_surface || !g_blit_prog) return false;
    static long trace_p = std::getenv("PS2_GL_TRACE") ? std::atol(std::getenv("PS2_GL_TRACE")) : 0;
    const bool tp = trace_p > 0 && trace_p--;
    if (tp) {
        std::fprintf(stderr, "[gl-trace] present (%ux%u, cmds=%zu) ...", info.surface_w, info.surface_h, count);
        std::fflush(stderr);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, static_cast<GLsizei>(info.surface_w), static_cast<GLsizei>(info.surface_h));
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glViewport(static_cast<GLint>(info.vp_x), static_cast<GLint>(info.vp_y),
               static_cast<GLsizei>(info.vp_w), static_cast<GLsizei>(info.vp_h));
    p_glUseProgram(g_blit_prog);
    bind_image(0, g_blit_scratch.id, GL_READ_ONLY);
    p_glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_blit_ubo.id);
    p_glBindVertexArray(g_vao);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    finish_present(info);
    if (tp) {
        std::fprintf(stderr, " swapped (err=0x%X)\n", static_cast<unsigned>(p_glGetError()));
        std::fflush(stderr);
    }
    return true;
}


#ifdef PS2_GPU_HWR
// ---------------------------------------------------------------- GPU triangle renderer API (backend.hpp), as backend_deko3d.cpp:
// images are GL textures, rows stored bottom first as there (GL convention), so renderer.cpp and the shaders are the same.
namespace {

struct GlImg {
    GLuint tex = 0;
    std::uint32_t w = 0, h = 0;
    bool rt = false, depth = false;
};
std::vector<GlImg> g_imgs(1);
std::map<std::pair<std::uint32_t, std::uint32_t>, GLuint> g_fbos;  // (target, depth) -> framebuffer
GLuint g_samplers[4]{};  // bit 0 linear, bit 1 clamp to edge
GLuint g_hwr_prog = 0, g_tex_blit_prog = 0, g_hwr_vao = 0, g_hwr_vbo = 0, g_hwr_ubo = 0;
GLint g_ubo_align = 256;

const GlImg *img(Image i) { return i.id && i.id < g_imgs.size() && g_imgs[i.id].tex ? &g_imgs[i.id] : nullptr; }

GLuint graphics_program(const char *vs_src, const char *fs_src) {
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src), fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    const GLuint p = vs && fs ? link_program(vs, fs) : 0;
    if (vs) p_glDeleteShader(vs);
    if (fs) p_glDeleteShader(fs);
    return p;
}

bool hwr_ready() {
    if (g_hwr_prog) return true;
    g_hwr_prog = graphics_program(kGlslHwrVert, kGlslHwrFrag);
    g_tex_blit_prog = graphics_program(kGlslOverlayVert, kGlslTexBlitFrag);
    if (!g_hwr_prog || !g_tex_blit_prog) return false;
    p_glGenSamplers(4, g_samplers);
    for (int i = 0; i < 4; ++i) {
        const GLint filter = (i & 1) ? GL_LINEAR : GL_NEAREST, wrap = (i & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        p_glSamplerParameteri(g_samplers[i], GL_TEXTURE_MIN_FILTER, filter);
        p_glSamplerParameteri(g_samplers[i], GL_TEXTURE_MAG_FILTER, filter);
        p_glSamplerParameteri(g_samplers[i], GL_TEXTURE_WRAP_S, wrap);
        p_glSamplerParameteri(g_samplers[i], GL_TEXTURE_WRAP_T, wrap);
    }
    p_glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g_ubo_align);
    p_glGenBuffers(1, &g_hwr_vbo);
    p_glGenBuffers(1, &g_hwr_ubo);
    p_glGenVertexArrays(1, &g_hwr_vao);
    p_glBindVertexArray(g_hwr_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_hwr_vbo);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(HwrVertex), reinterpret_cast<const void *>(offsetof(HwrVertex, x)));
    p_glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(HwrVertex), reinterpret_cast<const void *>(offsetof(HwrVertex, rgba)));
    p_glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(HwrVertex), reinterpret_cast<const void *>(offsetof(HwrVertex, s)));
    for (GLuint a = 0; a < 3; ++a) p_glEnableVertexAttribArray(a);
    p_glBindVertexArray(g_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void bind_fbo(const GlImg &t, std::uint32_t target, std::uint32_t depth) {
    GLuint &f = g_fbos[{target, depth}];
    if (!f) {
        p_glGenFramebuffers(1, &f);
        p_glBindFramebuffer(GL_FRAMEBUFFER, f);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        if (depth) p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_imgs[depth].tex, 0);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, f);
    p_glViewport(0, 0, static_cast<GLsizei>(t.w), static_cast<GLsizei>(t.h));
}

// what hwr_draw leaves behind, and what the clears, the compute path and the window blit expect
void plain_state() {
    p_glDisable(GL_BLEND);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_DEPTH_TEST);
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glDepthMask(GL_TRUE);
}

Image make_image(std::uint32_t w, std::uint32_t h, bool render_target, bool depth) {
    if (!w || !h) return Image{};
    std::uint32_t id = 1;
    while (id < g_imgs.size() && g_imgs[id].tex) ++id;
    if (id == g_imgs.size()) g_imgs.emplace_back();
    GlImg &m = g_imgs[id];
    p_glGenTextures(1, &m.tex);
    p_glBindTexture(GL_TEXTURE_2D, m.tex);
    p_glTexStorage2D(GL_TEXTURE_2D, 1, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    p_glBindTexture(GL_TEXTURE_2D, 0);
    m.w = w, m.h = h, m.rt = render_target, m.depth = depth;
    return Image{id};
}

}  // namespace

Image create_image(std::uint32_t w, std::uint32_t h, bool render_target) { return make_image(w, h, render_target, false); }

Image create_depth_image(std::uint32_t w, std::uint32_t h) { return make_image(w, h, false, true); }

void copy_image(Image src, Image dst) {
    const GlImg *s = img(src), *d = img(dst);
    if (!s || !d || s->depth || d->depth) return;
    p_glCopyImageSubData(s->tex, GL_TEXTURE_2D, 0, 0, 0, 0, d->tex, GL_TEXTURE_2D, 0, 0, 0, 0, static_cast<GLsizei>(std::min(s->w, d->w)),
                         static_cast<GLsizei>(std::min(s->h, d->h)), 1);
}

void clear_depth(Image target, Image depth, float value) {
    const GlImg *t = img(target), *d = img(depth);
    if (!t || !t->rt || !d || !d->depth) return;
    bind_fbo(*t, target.id, depth.id);
    plain_state();
    p_glClearBufferfv(GL_DEPTH, 0, &value);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void destroy_image(Image &i) {
    if (const GlImg *m = img(i)) {
        for (auto it = g_fbos.begin(); it != g_fbos.end();) {
            if (it->first.first == i.id || it->first.second == i.id) {
                p_glDeleteFramebuffers(1, &it->second);
                it = g_fbos.erase(it);
            } else {
                ++it;
            }
        }
        p_glDeleteTextures(1, &m->tex);
        g_imgs[i.id] = GlImg{};
    }
    i = Image{};
}

void clear_image(Image i, float r, float g, float b, float a) {
    const GlImg *t = img(i);
    if (!t || !t->rt) return;
    bind_fbo(*t, i.id, 0);
    plain_state();
    const float c[4] = {r, g, b, a};
    p_glClearBufferfv(GL_COLOR, 0, c);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void upload_image_from_buffer(Buffer src, std::size_t offset, Image dst, std::uint32_t w, std::uint32_t h) {
    const GlImg *t = img(dst);
    if (!src.id || !t) return;
    p_glMemoryBarrier(GL_PIXEL_BUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);  // written by compute shaders through images
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, src.id);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(w));
    p_glBindTexture(GL_TEXTURE_2D, t->tex);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(std::min(w, t->w)), static_cast<GLsizei>(std::min(h, t->h)), GL_RGBA,
                      GL_UNSIGNED_BYTE, reinterpret_cast<const void *>(offset));
    p_glBindTexture(GL_TEXTURE_2D, 0);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

bool hwr_draw(const HwrVertex *verts, std::uint32_t nverts, const HwrDraw *draws, std::uint32_t ndraws) {
    if (!nverts || !ndraws || !hwr_ready()) return false;
    Spent spent("hwr_draw", ndraws);
    // every draw's uniforms in one buffer, each at an offset the driver accepts
    struct Ubo {
        std::uint32_t state[16];
        float size[4];
    };
    const std::size_t align = static_cast<std::size_t>(std::max(g_ubo_align, 16));
    const std::size_t stride = (sizeof(Ubo) + align - 1) / align * align;
    static std::vector<std::uint8_t> ubos;
    ubos.assign(stride * ndraws, 0);
    for (std::uint32_t i = 0; i < ndraws; ++i) {
        Ubo u{};
        std::memcpy(u.state, draws[i].state, sizeof u.state);
        if (const GlImg *t = img(draws[i].target)) u.size[0] = static_cast<float>(t->w), u.size[1] = static_cast<float>(t->h);
        std::memcpy(ubos.data() + stride * i, &u, sizeof u);
    }
    p_glBindBuffer(GL_UNIFORM_BUFFER, g_hwr_ubo);
    p_glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(ubos.size()), ubos.data(), GL_STREAM_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_hwr_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(nverts * sizeof(HwrVertex)), verts, GL_STREAM_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    p_glBindVertexArray(g_hwr_vao);
    p_glUseProgram(g_hwr_prog);
    p_glActiveTexture(GL_TEXTURE0);
    p_glEnable(GL_BLEND);
    p_glEnable(GL_SCISSOR_TEST);
    std::uint32_t bound_target = 0, bound_depth = ~0u;
    int bound_blend = -1, bound_z = -1, bound_mask = -1;
    for (std::uint32_t i = 0; i < ndraws; ++i) {
        const HwrDraw &d = draws[i];
        const GlImg *t = img(d.target);
        if (!t || !t->rt || d.first + d.count > nverts) continue;
        const GlImg *z = img(d.depth);
        if (z && (!z->depth || z->w != t->w || z->h != t->h)) z = nullptr;
        if (d.target.id != bound_target || (z ? d.depth.id : 0u) != bound_depth) {
            bind_fbo(*t, d.target.id, z ? d.depth.id : 0u);
            bound_target = d.target.id, bound_depth = z ? d.depth.id : 0u;
        }
        if (d.blend != bound_blend) {  // the same rows as backend_deko3d.cpp
            const bool add = d.blend == 1, rev = d.blend == 2 || d.blend == 6, src_only = d.blend == 3, dst_scale = d.blend == 5;
            const bool brighten = d.blend == 7;
            const GLenum src = dst_scale ? GL_ONE : brighten ? GL_DST_COLOR : GL_SRC1_COLOR;
            const GLenum dst = add || d.blend == 2 || brighten ? GL_ONE : src_only ? GL_ZERO
                             : dst_scale || d.blend == 6 ? GL_SRC1_COLOR : GL_ONE_MINUS_SRC1_COLOR;
            p_glBlendEquationSeparate(rev ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
            p_glBlendFuncSeparate(src, dst, GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA);
            bound_blend = d.blend;
        }
        if (d.write_mask != bound_mask) {
            p_glColorMask(d.write_mask & 1, (d.write_mask >> 1) & 1, (d.write_mask >> 2) & 1, (d.write_mask >> 3) & 1);
            bound_mask = d.write_mask;
        }
        const int zkey = z ? (d.ztest | (d.zwrite ? 8 : 0)) : 0;
        if (zkey != bound_z) {
            if (z && d.ztest != 0) p_glEnable(GL_DEPTH_TEST);
            else p_glDisable(GL_DEPTH_TEST);
            p_glDepthMask(z && d.zwrite ? GL_TRUE : GL_FALSE);
            p_glDepthFunc(d.ztest == 1 ? GL_NEVER : d.ztest == 3 ? GL_GEQUAL : d.ztest == 4 ? GL_GREATER : GL_ALWAYS);
            bound_z = zkey;
        }
        const std::uint32_t x0 = std::min(d.scissor[0], t->w), x1 = std::min(d.scissor[2] + 1, t->w);
        const std::uint32_t y0 = std::min(d.scissor[1], t->h), y1 = std::min(d.scissor[3] + 1, t->h);
        if (x1 <= x0 || y1 <= y0) continue;
        p_glScissor(static_cast<GLint>(x0), static_cast<GLint>(t->h - y1), static_cast<GLsizei>(x1 - x0), static_cast<GLsizei>(y1 - y0));
        p_glBindBufferRange(GL_UNIFORM_BUFFER, 0, g_hwr_ubo, static_cast<GLintptr>(stride * i), sizeof(Ubo));
        if (const GlImg *tx = img(d.texture)) {
            p_glBindTexture(GL_TEXTURE_2D, tx->tex);
            p_glBindSampler(0, g_samplers[((d.state[0] >> 2) & 1u) | (((d.state[0] >> 3) & 1u) << 1)]);
        }
        p_glDrawArrays(GL_TRIANGLES, static_cast<GLint>(d.first), static_cast<GLsizei>(d.count));
    }
    plain_state();
    p_glBindSampler(0, 0);
    p_glBindTexture(GL_TEXTURE_2D, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glBindVertexArray(g_vao);
    p_glUseProgram(0);
    return true;
}

bool present_image(Image src, const PresentInfo &info, std::uint32_t region_x, std::uint32_t region_y, std::uint32_t region_w, std::uint32_t region_h) {
    const GlImg *s = img(src);
    if (!s || !g_has_surface || !hwr_ready()) return false;
    plain_state();
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, static_cast<GLsizei>(info.surface_w), static_cast<GLsizei>(info.surface_h));
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glViewport(static_cast<GLint>(info.vp_x), static_cast<GLint>(info.vp_y), static_cast<GLsizei>(info.vp_w), static_cast<GLsizei>(info.vp_h));
    // as backend_deko3d.cpp: GS row r is at row h-1-r
    const float region[4] = {static_cast<float>(region_w) / static_cast<float>(s->w), static_cast<float>(region_h) / static_cast<float>(s->h),
                             static_cast<float>(region_x) / static_cast<float>(s->w),
                             static_cast<float>(s->h - std::min(s->h, region_y + region_h)) / static_cast<float>(s->h)};
    p_glBindBuffer(GL_UNIFORM_BUFFER, g_hwr_ubo);
    p_glBufferData(GL_UNIFORM_BUFFER, sizeof region, region, GL_STREAM_DRAW);
    p_glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_hwr_ubo);
    p_glUseProgram(g_tex_blit_prog);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, s->tex);
    p_glBindSampler(0, g_samplers[3]);
    p_glBindVertexArray(g_vao);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindSampler(0, 0);
    p_glBindTexture(GL_TEXTURE_2D, 0);
    finish_present(info);
    p_glUseProgram(0);
    return true;
}

bool read_image(Image src, std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    const GlImg *s = img(src);
    if (!s || !rgba || s->depth) return false;
    w = std::min(w, s->w), h = std::min(h, s->h);
    std::vector<std::uint32_t> all(static_cast<std::size_t>(s->w) * s->h);
    p_glBindTexture(GL_TEXTURE_2D, s->tex);
    p_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
    p_glBindTexture(GL_TEXTURE_2D, 0);
    for (std::uint32_t y = 0; y < h; ++y) std::memcpy(rgba + static_cast<std::size_t>(y) * w, all.data() + static_cast<std::size_t>(y) * s->w, w * 4);
    return true;
}

Pipeline texfetch_pipeline() {
    static const GLuint p = build_compute(kGlslTexFetch);
    return Pipeline{nullptr, p};
}
#endif

}  // namespace ps2::gs::gpu::be

namespace ps2::gs::gpu::be {

// PS2_GL_SELFTEST=1: three round trips through the GPU, to tell a drawing problem from a readback problem on a new
// driver or emulator: what the CPU wrote, what a compute shader wrote through an image, and what it wrote after reading.
void self_test() {
    constexpr int n = 4096;
    std::vector<std::uint32_t> in(n), out(n);
    for (int i = 0; i < n; ++i) in[i] = 0x9E3779B9u * static_cast<std::uint32_t>(i + 1);
    Buffer a = create_buffer(n * 4, kStorage | kCopySrc | kCopyDst), b = create_buffer(n * 4, kStorage | kCopySrc | kCopyDst);
    const auto check = [&](const char *what, const std::vector<std::uint32_t> &want) {
        int bad = 0;
        std::string where;  // the wrong word ranges, to see a cache-line or page pattern
        for (int i = 0; i < n; ++i) {
            if (out[i] == want[i]) continue;
            if (!bad++ || out[i - 1] == want[i - 1]) {
                int j = i;
                while (j + 1 < n && out[j + 1] != want[j + 1]) ++j;
                if (where.size() < 200) where += " " + std::to_string(i) + "-" + std::to_string(j);
            }
        }
        std::fprintf(stderr, "[gl] self-test %s: %s (%d of %d words wrong, first %08X want %08X)%s%s\n", what, bad ? "FAILED" : "ok", bad, n,
                     out[0], want[0], bad ? " at" : "", where.c_str());
    };
    const auto read_direct = [&](Buffer x) { read_buffer(x, 0, out.data(), n * 4); };
    Buffer c = create_buffer(n * 4, kStorage | kCopySrc | kCopyDst);
    write_buffer(a, 0, in.data(), n * 4);
    read_buffer(a, 0, out.data(), n * 4);
    check("cpu write -> read", in);
    copy_buffer(a, 0, c, 0, n * 4);
    read_direct(c);
    check("cpu write -> gpu copy -> direct read", in);
    const GLuint prog = build_compute(R"(#version 430 core
layout(local_size_x = 64) in;
layout(r32ui, binding = 0) uniform readonly uimageBuffer src;
layout(r32ui, binding = 1) uniform writeonly uimageBuffer dst;
void main() {
    int i = int(gl_GlobalInvocationID.x);
    imageStore(dst, i, uvec4(imageLoad(src, i).x ^ 0xFFFFFFFFu, 0u, 0u, 0u));
}
)");
    if (!prog) {
        std::fprintf(stderr, "[gl] self-test: the shader did not build\n");
        return;
    }
    p_glUseProgram(prog);
    bind_image(0, a.id, GL_READ_ONLY);
    bind_image(1, b.id, GL_WRITE_ONLY);
    p_glDispatchCompute(n / 64, 1, 1);
    p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    p_glUseProgram(0);
    std::vector<std::uint32_t> want(n);
    for (int i = 0; i < n; ++i) want[i] = ~in[i];
    p_glFinish();
    p_glBindBuffer(GL_COPY_READ_BUFFER, b.id);
    cpu_read(GL_COPY_READ_BUFFER, 0, out.data(), n * 4, false);
    p_glBindBuffer(GL_COPY_READ_BUFFER, 0);
    check("image load+store -> read without the cache flush (expected to fail on the Switch)", want);
    read_direct(b);
    check("image load+store -> direct read", want);
    {  // how to wait for a shader's writes before the CPU reads them: 20 tries of each way
        const char *const ways[4] = {"glFinish", "glFinish twice", "fence + glClientWaitSync", "glFinish + 1 ms"};
        std::vector<std::uint32_t> zero(n, 0);
        for (int way = 0; way < 4; ++way) {
            int failed = 0, words = 0;
            for (int t = 0; t < 20; ++t) {
                write_buffer(b, 0, zero.data(), n * 4);
                p_glUseProgram(prog);
                bind_image(0, a.id, GL_READ_ONLY);
                bind_image(1, b.id, GL_WRITE_ONLY);
                p_glDispatchCompute(n / 64, 1, 1);
                p_glMemoryBarrier(GL_ALL_BARRIER_BITS);
                p_glUseProgram(0);
                if (way == 0) p_glFinish();
                if (way == 1) p_glFinish(), p_glFinish();
                if (way == 2) {
                    GLsync f = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    p_glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
                    p_glDeleteSync(f);
                }
                if (way == 3) p_glFinish(), SDL_Delay(1);
                p_glBindBuffer(GL_COPY_READ_BUFFER, b.id);
                cpu_read(GL_COPY_READ_BUFFER, 0, out.data(), n * 4);
                p_glBindBuffer(GL_COPY_READ_BUFFER, 0);
                int bad = 0;
                for (int i = 0; i < n; ++i) bad += out[i] != want[i];
                failed += bad != 0;
                words += bad;
            }
            std::fprintf(stderr, "[gl] self-test wait before a read, %s: %d of 20 reads wrong (%d words)\n", ways[way], failed, words);
        }
    }
    read_buffer(b, 0, out.data(), n * 4);
    check("image load+store -> staging read", want);
    std::vector<std::uint32_t> zero(n, 0);
    write_buffer(c, 0, zero.data(), n * 4);
    copy_buffer(b, 0, c, 0, n * 4);
    read_direct(c);
    check("image load+store -> gpu copy -> direct read", want);
    // what the GS shaders use besides plain loads and stores: a coherent image with atomics, parameters from a uniform
    // buffer bound at an offset, the division helper of glsl_compat.py, 16x16 work groups
    const GLuint prog2 = build_compute(R"(#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;
layout(r32ui, binding = 0) uniform coherent uimageBuffer v;
layout(std140, binding = 2) uniform P { uvec4 p; };
uint udiv_(uint a, uint b) {
    float fb = float(b);
    uint q = uint(float(a) / fb);
    q = uint(int(q) + int(floor(float(int(a - q * b)) / fb)));
    int r = int(a - q * b);
    if (r < 0) q -= 1u;
    else if (uint(r) >= b) q += 1u;
    return q;
}
void main_body_() {
    uint i = gl_GlobalInvocationID.y * 64u + gl_GlobalInvocationID.x;
    if (i >= p.y) return;
    uint old = imageLoad(v, int(i)).x;
    imageAtomicXor(v, int(i), p.x);
    imageStore(v, int(i + p.y), uvec4(udiv_(old, i + 3u) + old, 0u, 0u, 0u));
}
void main() { main_body_(); }
)");
    if (prog2) {
        std::vector<std::uint32_t> big(2 * n, 0), want2(2 * n);
        for (int i = 0; i < n; ++i) big[i] = in[i];
        for (int i = 0; i < n; ++i) want2[i] = in[i] ^ 0x5A5A5A5Au, want2[i + n] = in[i] / (i + 3u) + in[i];
        Buffer v = create_buffer(2 * n * 4, kStorage | kCopySrc | kCopyDst), u = create_buffer(512, kUniform | kCopyDst);
        write_buffer(v, 0, big.data(), 2 * n * 4);
        const std::uint32_t params[4] = {0x5A5A5A5Au, n, 0, 0};
        p_glBindBuffer(GL_UNIFORM_BUFFER, u.id);
        cpu_write(GL_UNIFORM_BUFFER, 256, params, 16);
        p_glBindBuffer(GL_UNIFORM_BUFFER, 0);
        p_glUseProgram(prog2);
        bind_image(0, v.id, GL_READ_WRITE);
        p_glBindBufferRange(GL_UNIFORM_BUFFER, 2, u.id, 256, 256);
        p_glDispatchCompute(64 / 16, n / 64 / 16, 1);
        p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        p_glUseProgram(0);
        std::vector<std::uint32_t> got(2 * n);
        p_glFinish();
        p_glBindBuffer(GL_COPY_READ_BUFFER, v.id);
        p_glGetBufferSubData(GL_COPY_READ_BUFFER, 0, 2 * n * 4, got.data());
        p_glBindBuffer(GL_COPY_READ_BUFFER, 0);
        int bad_xor = 0, bad_div = 0;
        for (int i = 0; i < n; ++i) bad_xor += got[i] != want2[i], bad_div += got[i + n] != want2[i + n];
        std::fprintf(stderr, "[gl] self-test atomics + offset uniforms + 16x16 groups: %s (%d xor wrong, %d div wrong; first %08X/%08X want %08X/%08X)\n",
                     bad_xor || bad_div ? "FAILED" : "ok", bad_xor, bad_div, got[0], got[n], want2[0], want2[n]);
        // cost of 100 dispatches each way, to see what an emulator charges per dispatch
        const auto bench = [&](const char *what, auto &&one) {
            p_glFinish();
            const auto t0 = std::chrono::steady_clock::now();
            for (int k = 0; k < 100; ++k) one(k);
            p_glFinish();
            std::fprintf(stderr, "[gl] self-test 100 dispatches, %s: %.1f ms\n", what,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        };
        const std::uint32_t none[4] = {0, 0, 0, 0};
        p_glBindBuffer(GL_UNIFORM_BUFFER, u.id);
        cpu_write(GL_UNIFORM_BUFFER, 0, none, 16);
        p_glBindBuffer(GL_UNIFORM_BUFFER, 0);
        bind_image(0, v.id, GL_READ_WRITE);
        p_glUseProgram(prog2);
        p_glBindBufferRange(GL_UNIFORM_BUFFER, 2, u.id, 0, 256);
        bench("same program, same parameters", [&](int) { p_glDispatchCompute(1, 1, 1); p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT); });
        bench("same program, parameters rebound", [&](int k) {
            p_glBindBufferRange(GL_UNIFORM_BUFFER, 2, u.id, (k & 1) * 256, 256);
            p_glDispatchCompute(1, 1, 1);
            p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        });
        bench("same program, no barrier", [&](int) { p_glDispatchCompute(1, 1, 1); });
        bench("two small programs in turn", [&](int k) {
            p_glUseProgram(k & 1 ? prog2 : prog);
            p_glDispatchCompute(1, 1, 1);
            p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        });
        if (const GLuint big = build_compute(kGlslRaster)) {
            p_glUseProgram(big);
            p_glBindBufferRange(GL_UNIFORM_BUFFER, 2, u.id, 0, 256);
            bench("the raster program alone (returns at once)", [&](int) { p_glDispatchCompute(1, 1, 1); p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT); });
            bench("raster and a small program in turn", [&](int k) {
                p_glUseProgram(k & 1 ? prog2 : big);
                p_glDispatchCompute(1, 1, 1);
                p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            });
            p_glDeleteProgram(big);
        }
        {  // the same with eight big buffers bound, as the GS shaders have
            Buffer big[8];
            for (Buffer &x : big) x = create_buffer(16u << 20, kStorage | kCopyDst);
            p_glUseProgram(prog2);
            p_glBindBufferRange(GL_UNIFORM_BUFFER, 2, u.id, 0, 256);
            for (int s = 0; s < 8; ++s) bind_image(static_cast<GLuint>(s), big[s].id, GL_READ_WRITE);
            bench("eight 16 MB images bound", [&](int) { p_glDispatchCompute(1, 1, 1); p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT); });
            bench("eight 16 MB images bound again each time", [&](int) {
                for (int s = 0; s < 8; ++s) bind_image(static_cast<GLuint>(s), big[s].id, GL_READ_WRITE);
                p_glDispatchCompute(1, 1, 1);
                p_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            });
            for (Buffer &x : big) destroy_buffer(x);
        }
        p_glUseProgram(0);
        p_glDeleteProgram(prog2);
        destroy_buffer(v);
        destroy_buffer(u);
    }
    // What the Switch's own Mesa got wrong where an emulator did not: buffer copies right after a write, and atomics.
    {
        write_buffer(a, 0, in.data(), n * 4);
        read_direct(a);
        check("cpu write -> direct read (no copy)", in);
        write_buffer(a, 0, in.data(), n * 4);
        p_glFinish();
        copy_buffer(a, 0, c, 0, n * 4);
        read_direct(c);
        check("cpu write -> finish -> gpu copy -> direct read", in);
        // one atomic per word; two atomics on the two halves of each word (as 16-bit pixels side by side); with images
        // (what glsl_compat.py makes) and with storage buffers
        const char *const kinds[4] = {"image atomics, one per word", "image atomics, two per word (16-bit halves)",
                                      "storage-buffer atomics, one per word", "storage-buffer atomics, two per word"};
        const char *const sources[4] = {
            R"(#version 430 core
layout(local_size_x = 64) in;
layout(r32ui, binding = 0) uniform coherent uimageBuffer v;
void main() { int i = int(gl_GlobalInvocationID.x); imageAtomicXor(v, i, 0xA5A5A5A5u); })",
            R"(#version 430 core
layout(local_size_x = 64) in;
layout(r32ui, binding = 0) uniform coherent uimageBuffer v;
void main() { uint i = gl_GlobalInvocationID.x; imageAtomicXor(v, int(i >> 1), (i & 1u) != 0u ? 0xA5A50000u : 0x0000A5A5u); })",
            R"(#version 430 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer B { uint d[]; };
void main() { uint i = gl_GlobalInvocationID.x; atomicXor(d[i], 0xA5A5A5A5u); })",
            R"(#version 430 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer B { uint d[]; };
void main() { uint i = gl_GlobalInvocationID.x; atomicXor(d[i >> 1], (i & 1u) != 0u ? 0xA5A50000u : 0x0000A5A5u); })"};
        std::vector<std::uint32_t> want_x(n);
        for (int i = 0; i < n; ++i) want_x[i] = in[i] ^ 0xA5A5A5A5u;
        {  // variants of "one atomic per word", to find what loses the GPU's results
            const GLuint p = build_compute(sources[0]);
            const auto run = [&](const char *what, Buffer x, bool sub_data, bool barrier_before, bool wait_after) {
                if (sub_data) {
                    p_glFinish();
                    p_glBindBuffer(GL_COPY_WRITE_BUFFER, x.id);
                    p_glBufferSubData(GL_COPY_WRITE_BUFFER, 0, n * 4, in.data());
                    p_glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                } else {
                    write_buffer(x, 0, in.data(), n * 4);
                }
                p_glFinish();
                if (barrier_before) p_glMemoryBarrier(GL_ALL_BARRIER_BITS);
                p_glUseProgram(p);
                bind_image(0, x.id, GL_READ_WRITE);
                p_glDispatchCompute(n / 64, 1, 1);
                p_glMemoryBarrier(GL_ALL_BARRIER_BITS);
                p_glUseProgram(0);
                p_glFinish();
                if (wait_after) SDL_Delay(50);
                read_direct(x);
                check(what, want_x);
            };
            run("atomics A: as the game does it", a, false, false, false);
            run("atomics B: all barriers before the dispatch", a, false, true, false);
            run("atomics C: 50 ms wait before the read", a, false, false, true);
            run("atomics D: glBufferSubData upload", a, true, false, false);
            Buffer fresh = create_buffer(n * 4, kStorage | kCopySrc | kCopyDst);
            run("atomics E: a buffer the GPU never used", fresh, false, false, false);
            run("atomics F: the same buffer again", fresh, false, false, false);
            destroy_buffer(fresh);
            Buffer fresh2 = create_buffer(n * 4, kStorage | kCopySrc | kCopyDst);
            run("atomics G: new buffer, glBufferSubData upload", fresh2, true, false, false);
            destroy_buffer(fresh2);
            p_glDeleteProgram(p);
        }
        for (int k = 0; k < 4; ++k) {
            const GLuint p = build_compute(sources[k]);
            if (!p) {
                std::fprintf(stderr, "[gl] self-test %s: shader did not build\n", kinds[k]);
                continue;
            }
            write_buffer(a, 0, in.data(), n * 4);
            p_glFinish();
            p_glUseProgram(p);
            if (k < 2) bind_image(0, a.id, GL_READ_WRITE);
            else p_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, a.id);
            p_glDispatchCompute((k & 1) ? 2 * n / 64 : n / 64, 1, 1);
            p_glMemoryBarrier(GL_ALL_BARRIER_BITS);
            p_glUseProgram(0);
            read_direct(a);
            check(kinds[k], want_x);
            p_glDeleteProgram(p);
        }
    }
    destroy_buffer(c);
    p_glDeleteProgram(prog);
    destroy_buffer(a);
    destroy_buffer(b);
    if (std::string(std::getenv("PS2_GL_SELFTEST")) == "exit") {  // only the tests (each run on a console costs a restart)
        std::fprintf(stderr, "[gl] self-test done, exiting\n");
        std::fflush(stderr);
        std::exit(0);
    }
}

}  // namespace ps2::gs::gpu::be
