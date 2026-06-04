#include "game/game.h"
#include "audio/audio.h"
#include "rendering/pixel_renderer.h"
#include "rendering/embedded_icon.h"
#include "emu/jsbeeb_bridge.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include <memory>
#include <cstdio>

#ifdef _WIN32
// Request 1 ms scheduler resolution so std::this_thread::sleep_for in
// Game::step can actually wake up close to its requested 40 ms budget.
// Default Windows granularity is 15.625 ms, which floors the loop at
// ~32 fps. winmm.lib is already linked via the audio backend.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif

// Owned by main(); raw pointers are aliased into sokol_app callbacks via
// TU-local globals because sapp_desc's callbacks are plain function pointers
// with no userdata in the basic flavour we use here.
namespace {
    std::unique_ptr<Game> g_game;
    PixelRenderer*        g_renderer = nullptr;

    // sokol_gfx pipeline state for the CPU-framebuffer-as-texture present.
    // Pipeline samples `fb_img` (RGBA8, BGRA-byte-order matching the CPU
    // buffer's 0xAARRGGBB packing) with a 4-vertex full-screen quad.
    sg_image    fb_img    = {};
    sg_view     fb_view   = {};   // texture-view over fb_img for the FS bind
    sg_sampler  fb_smp    = {};
    sg_buffer   fb_vbuf   = {};
    sg_buffer   fb_ibuf   = {};
    sg_shader   fb_shader = {};
    sg_pipeline fb_pip    = {};
    int         fb_w      = 0;
    int         fb_h      = 0;

#if defined(_WIN32)
    // HLSL passthrough (D3D11 backend): float2 pos in clip space, uv in [0,1].
    const char* fb_vs_src =
        "struct vs_in  { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "vs_out main(vs_in inp) {\n"
        "    vs_out outp;\n"
        "    outp.pos = float4(inp.pos, 0.0, 1.0);\n"
        "    outp.uv  = inp.uv;\n"
        "    return outp;\n"
        "}\n";

    const char* fb_fs_src =
        "Texture2D    tex : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 main(vs_out inp) : SV_Target0 {\n"
        "    return tex.Sample(smp, inp.uv);\n"
        "}\n";
#else
    // GLSL 410 passthrough (GLCORE/GLX backend on Linux & co.). Mirrors the
    // HLSL above one-for-one. Attribute names (`pos`, `uv`) and the combined
    // sampler name (`tex_smp`) are wired to the shader desc below via
    // glsl_name fields — required by sokol's GL 4.1 path.
    const char* fb_vs_src =
        "#version 410\n"
        "in vec2 pos;\n"
        "in vec2 uv;\n"
        "out vec2 uv_frag;\n"
        "void main() {\n"
        "    gl_Position = vec4(pos, 0.0, 1.0);\n"
        "    uv_frag = uv;\n"
        "}\n";

    const char* fb_fs_src =
        "#version 410\n"
        "uniform sampler2D tex_smp;\n"
        "in vec2 uv_frag;\n"
        "out vec4 frag_color;\n"
        "void main() {\n"
        // RGBA8 texture holds the CPU buffer's B,G,R,A bytes, so the
        // sampled .rgba is actually (B,G,R,A) — .bgra restores (R,G,B,A).
        "    frag_color = texture(tex_smp, uv_frag).bgra;\n"
        "}\n";
#endif
}

// (Re)create the framebuffer texture and its sampling view at (w,h). Called
// from init_cb and from the RESIZED event handler in task #8.
static void recreate_fb_image(int w, int h) {
    if (fb_view.id != SG_INVALID_ID) sg_destroy_view(fb_view);
    if (fb_img.id  != SG_INVALID_ID) sg_destroy_image(fb_img);
    sg_image_desc id = {};
    id.width  = w;
    id.height = h;
    // CPU buf is uint32_t 0xAARRGGBB = bytes [B,G,R,A] in little-endian.
    // D3D11 takes that verbatim as BGRA8. Desktop GL has no renderable
    // BGRA8 internal format (BGRA is upload-only there), so on GL we store
    // the bytes in an RGBA8 texture and undo the channel order with a
    // `.bgra` swizzle in the fragment shader (see fb_fs_src).
#if defined(_WIN32)
    id.pixel_format = SG_PIXELFORMAT_BGRA8;
#else
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
#endif
    id.usage.stream_update = true;            // updated every frame
    id.label = "exile-framebuffer";
    fb_img = sg_make_image(&id);

    sg_view_desc vd = {};
    vd.texture.image = fb_img;
    vd.label = "exile-framebuffer-view";
    fb_view = sg_make_view(&vd);

    fb_w = w;
    fb_h = h;
}

static void init_cb() {
    // 1. sokol_gfx setup, attached to sokol_app's swapchain.
    sg_desc d = {};
    d.environment = sglue_environment();
    d.logger.func = slog_func;
    sg_setup(&d);

    // 2. Texture sized to the current window. Resized on SAPP_EVENTTYPE_RESIZED.
    recreate_fb_image(sapp_width(), sapp_height());

    // 3. Linear sampler with clamp (no wraparound at edges).
    sg_sampler_desc sd = {};
    sd.min_filter = SG_FILTER_LINEAR;
    sd.mag_filter = SG_FILTER_LINEAR;
    sd.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sd.label = "exile-fb-sampler";
    fb_smp = sg_make_sampler(&sd);

    // 4. Vertex + index buffers for a full-screen quad. UV (0,0) = top-left
    // to match the CPU framebuffer's row-0-at-top layout.
    static const float verts[] = {
        // pos          uv
        -1.0f,  1.0f,   0.0f, 0.0f,   // top-left
         1.0f,  1.0f,   1.0f, 0.0f,   // top-right
        -1.0f, -1.0f,   0.0f, 1.0f,   // bottom-left
         1.0f, -1.0f,   1.0f, 1.0f,   // bottom-right
    };
    sg_buffer_desc vbd = {};
    vbd.data = SG_RANGE(verts);
    vbd.label = "exile-fb-vbuf";
    fb_vbuf = sg_make_buffer(&vbd);

    static const uint16_t indices[] = { 0, 2, 1,  1, 2, 3 };
    sg_buffer_desc ibd = {};
    ibd.usage.index_buffer = true;
    ibd.data = SG_RANGE(indices);
    ibd.label = "exile-fb-ibuf";
    fb_ibuf = sg_make_buffer(&ibd);

    // 5. Shader: passthrough VS, single-texture FS. D3D11 backend compiles
    // the HLSL strings via D3DCompiler at runtime.
    sg_shader_desc sh = {};
    sh.vertex_func.source = fb_vs_src;
    sh.vertex_func.entry  = "main";
    sh.fragment_func.source = fb_fs_src;
    sh.fragment_func.entry  = "main";
#if defined(_WIN32)
    // D3D11: attributes bind by HLSL semantic, texture/sampler by register.
    sh.attrs[0].hlsl_sem_name = "POSITION";
    sh.attrs[0].hlsl_sem_index = 0;
    sh.attrs[1].hlsl_sem_name = "TEXCOORD";
    sh.attrs[1].hlsl_sem_index = 0;
    sh.views[0].texture.hlsl_register_t_n = 0;
    sh.samplers[0].hlsl_register_s_n = 0;
#else
    // GL 4.1: attributes bind by name; the texture+sampler pair is a single
    // combined `sampler2D` uniform, named via texture_sampler_pairs.glsl_name.
    sh.attrs[0].glsl_name = "pos";
    sh.attrs[1].glsl_name = "uv";
    sh.texture_sampler_pairs[0].glsl_name = "tex_smp";
#endif
    sh.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    sh.views[0].texture.image_type = SG_IMAGETYPE_2D;
    sh.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    sh.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    sh.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    sh.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    sh.texture_sampler_pairs[0].view_slot = 0;
    sh.texture_sampler_pairs[0].sampler_slot = 0;
    sh.label = "exile-fb-shader";
    fb_shader = sg_make_shader(&sh);

    // 6. Pipeline: float2 pos + float2 uv, indexed triangles, no culling.
    sg_pipeline_desc pd = {};
    pd.shader = fb_shader;
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    pd.index_type = SG_INDEXTYPE_UINT16;
    pd.cull_mode = SG_CULLMODE_NONE;
    pd.label = "exile-fb-pipeline";
    fb_pip = sg_make_pipeline(&pd);
}

static void frame_cb() {
    if (!g_game || !g_game->is_running()) {
        sapp_request_quit();
        return;
    }

    // 1. Run game logic + per-pixel CPU rendering into the renderer's buf.
    g_game->step();
    if (!g_game->is_running()) { sapp_request_quit(); return; }

    // 2. If the CPU framebuffer's size has drifted from the GPU texture's
    // (e.g. resize event already grew buf but we haven't recreated the
    // image yet), rebuild the texture before upload. The full RESIZED-
    // event handler in task #8 calls this explicitly; this is the safety net.
    const int w = g_renderer->win_w();
    const int h = g_renderer->win_h();
    if (w != fb_w || h != fb_h) recreate_fb_image(w, h);

    // 3. Upload the whole framebuffer to the GPU texture.
    sg_image_data id = {};
    id.mip_levels[0].ptr  = g_renderer->framebuffer();
    id.mip_levels[0].size = (size_t)w * h * sizeof(uint32_t);
    sg_update_image(fb_img, &id);

    // 4. Default pass: clear black, draw the textured quad covering the
    // swapchain, present.
    sg_pass pass = {};
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 1.0f };
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);

    sg_apply_pipeline(fb_pip);
    sg_bindings b = {};
    b.vertex_buffers[0] = fb_vbuf;
    b.index_buffer      = fb_ibuf;
    b.views[0]          = fb_view;
    b.samplers[0]       = fb_smp;
    sg_apply_bindings(&b);
    sg_draw(0, 6, 1);

    sg_end_pass();
    sg_commit();
}

static void event_cb(const sapp_event* ev) {
    if (g_renderer) g_renderer->handle_event(ev);
}

static void cleanup_cb() {
    sg_destroy_pipeline(fb_pip);
    sg_destroy_shader(fb_shader);
    sg_destroy_buffer(fb_ibuf);
    sg_destroy_buffer(fb_vbuf);
    sg_destroy_sampler(fb_smp);
    sg_destroy_view(fb_view);
    sg_destroy_image(fb_img);
    sg_shutdown();
    Audio::close();
    // Join the jsbeeb HTTP acceptor thread (started on J toggle). If
    // never started, stop() is a no-op. If skipped, the still-joinable
    // std::thread destructor at process exit calls terminate().
    JsbeebBridge::stop();
    g_game.reset();
    g_renderer = nullptr;
}

int main() {
    auto renderer = std::make_unique<PixelRenderer>();
    g_renderer = renderer.get();
    g_game = std::make_unique<Game>(std::move(renderer));

    if (!g_game->init()) {
        std::fprintf(stderr, "Failed to initialize game\n");
        return 1;
    }

#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    sapp_desc d{};
    d.init_cb     = init_cb;
    d.frame_cb    = frame_cb;
    d.event_cb    = event_cb;
    d.cleanup_cb  = cleanup_cb;
    d.width       = INITIAL_W;
    d.height      = INITIAL_H;
    d.window_title = "Exile";
    d.icon.sokol_default = false;
    d.icon.images[0].width  = kAppIconWidth;
    d.icon.images[0].height = kAppIconHeight;
    d.icon.images[0].pixels.ptr  = kAppIconRGBA;
    d.icon.images[0].pixels.size = sizeof(kAppIconRGBA);
    d.logger.func = slog_func;
    sapp_run(&d);

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return 0;
}
