// launcher_platform_sdl3.c — SDL3 implementation of the shared platform layer.

#include "launcher_platform.h"
#include "launcher_boot_timing.h"
#include "launcher_gl.h"

#include <stdio.h>
#include <stdlib.h>

static bool s_quit_sdl = true;

void launcher_platform_set_quit_sdl(bool quit_sdl) {
    s_quit_sdl = quit_sdl;
}

static bool forced_usable_bounds(SDL_Rect* out) {
    const char* value = getenv("LNG_FORCE_USABLE_BOUNDS");
    if (!value || !value[0] || !out) return false;
    int w = 0, h = 0;
    if (sscanf(value, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
        return false;
    out->x = 0;
    out->y = 0;
    out->w = w;
    out->h = h;
    return true;
}

static void fit_initial_window_to_display(int* width, int* height) {
    if (!width || !height || *width <= 0 || *height <= 0) return;

    SDL_Rect usable = {0};
    bool have_bounds = forced_usable_bounds(&usable);
    if (!have_bounds) {
        SDL_DisplayID display = SDL_GetPrimaryDisplay();
        have_bounds = display != 0 && SDL_GetDisplayUsableBounds(display, &usable);
    }
    if (!have_bounds || usable.w <= 0 || usable.h <= 0) return;

    // Leave room for desktop panels/title bars. Normal desktop sizes keep the
    // default 1100x880 window; only cramped displays get a smaller launch size.
    const int margin = 64;
    int max_w = usable.w - margin;
    int max_h = usable.h - margin;
    if (max_w <= 0 || max_h <= 0) return;
    if (*width <= max_w && *height <= max_h) return;

    float scale_w = (float)max_w / (float)*width;
    float scale_h = (float)max_h / (float)*height;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale <= 0.0f || scale >= 1.0f) return;

    const int min_w = 820;
    const int min_h = 600;
    int fitted_w = (int)((float)*width * scale);
    int fitted_h = (int)((float)*height * scale);
    if (fitted_w < min_w && max_w >= min_w) fitted_w = min_w;
    if (fitted_h < min_h && max_h >= min_h) fitted_h = min_h;
    if (fitted_w > max_w) fitted_w = max_w;
    if (fitted_h > max_h) fitted_h = max_h;
    if (fitted_w > 0) *width = fitted_w;
    if (fitted_h > 0) *height = fitted_h;
}

// Usable (work-area) bounds of the display the window is actually on, honouring
// the LNG_FORCE_USABLE_BOUNDS override the initial fit already understands.
static bool window_usable_bounds(SDL_Window* window, SDL_Rect* out) {
    SDL_DisplayID display;
    if (!out) return false;
    if (forced_usable_bounds(out)) return true;
    display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    if (!display || !SDL_GetDisplayUsableBounds(display, out)) return false;
    return out->w > 0 && out->h > 0;
}

// Centre the whole FRAME in the work area, never letting the caption cross the
// top edge. Called before a maximize too, so Restore lands somewhere sane.
static void place_window(SDL_Window* window, const SDL_Rect* usable, int w, int h,
                         int frame_t, int frame_l, int frame_b, int frame_r) {
    int x, y;
    SDL_SetWindowSize(window, w, h);
    x = usable->x + frame_l + (usable->w - (w + frame_l + frame_r)) / 2;
    y = usable->y + frame_t + (usable->h - (h + frame_t + frame_b)) / 2;
    if (x < usable->x + frame_l) x = usable->x + frame_l;
    if (y < usable->y + frame_t) y = usable->y + frame_t;
    SDL_SetWindowPosition(window, x, y);
}

// Size and place the window for the resolved scale, keeping every part the user
// has to grab — title bar, borders — inside the desktop's work area.
//
// The size the host asks for is LOGICAL, so where the point/pixel split is
// synthesized an 1100x880 launcher needs 1650x1320 pixels on a 150% desktop and
// 3300x2640 on a 300% one — usually more than the monitor has. Clamping those
// numbers to the work area and centring the result is not enough, and is how a
// window ends up unusable: SDL sizes and positions the CLIENT area, so the
// caption lives above y and has to be paid for here, and SDL_WINDOWPOS_CENTERED
// centres on the DISPLAY bounds rather than the work area — a window as tall as
// the work area is then pushed up by half the taskbar's height and its title bar
// leaves the top of the screen, with no grabbable edges either because it
// already spans the work area.
static void fit_window_to_display(LauncherPlatform* p, int logical_w, int logical_h) {
    SDL_Rect usable = {0};
    int frame_t = 0, frame_l = 0, frame_b = 0, frame_r = 0;
    int want_w, want_h, max_w, max_h, margin;
    float scale;

    if (!p || !p->window || logical_w <= 0 || logical_h <= 0) return;
    scale = p->input_scale > 0.0f ? p->input_scale : 1.0f;
    want_w = (int)((float)logical_w * scale);
    want_h = (int)((float)logical_h * scale);

    if (!window_usable_bounds(p->window, &usable)) {
        // No work area to reason about: honour the scale and let the window
        // manager place it, which is all this could do before any of the above.
        if (scale != 1.0f) {
            SDL_SetWindowSize(p->window, want_w, want_h);
            SDL_SetWindowPosition(p->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            launcher_platform_refresh_metrics(p);
        }
        return;
    }

    // Frame extents come from the live window, so they already carry the
    // display's own scale. Platforms without server-side decorations report
    // nothing and leave these at 0, where the margin alone is the slack.
    SDL_GetWindowBordersSize(p->window, &frame_t, &frame_l, &frame_b, &frame_r);
    margin = (int)(16.0f * scale);
    max_w = usable.w - frame_l - frame_r - margin * 2;
    max_h = usable.h - frame_t - frame_b - margin * 2;
    if (max_w <= 0 || max_h <= 0) {   // a work area smaller than its own chrome
        place_window(p->window, &usable, want_w, want_h,
                     frame_t, frame_l, frame_b, frame_r);
    } else if (want_w > max_w || want_h > max_h) {
        // Too big to place by hand. Maximizing is the honest version of what a
        // clamp was reaching for: the OS owns the geometry, the title bar stays
        // reachable, snapping works, and Restore gives back the size set here.
        place_window(p->window, &usable,
                     want_w < max_w ? want_w : max_w,
                     want_h < max_h ? want_h : max_h,
                     frame_t, frame_l, frame_b, frame_r);
        SDL_MaximizeWindow(p->window);
    } else {
        place_window(p->window, &usable, want_w, want_h,
                     frame_t, frame_l, frame_b, frame_r);
    }
    launcher_platform_refresh_metrics(p);
}

/* Desktop GL: a 3.3 core context first, then any 3.0+ context. Intel's
 * Sandy Bridge driver stops at OpenGL 3.1, and ImGui's OpenGL3 backend runs
 * on 3.0 with GLSL 130 (it checks 3.2/3.3 features at runtime). Below 3.0 --
 * SDL returns Microsoft's GDI OpenGL 1.1 when the driver lacks
 * WGL_ARB_create_context -- the backend cannot load and its first font
 * rebuild used to dereference NULL; refuse it so the host boots without the
 * launcher. */
#if !defined(LNG_SDL_RENDERER)
static bool create_launcher_gl_context(LauncherPlatform* p) {
    static const struct { int major, minor, profile; } kTries[] = {
        { 3, 3, SDL_GL_CONTEXT_PROFILE_CORE },
        { 3, 0, 0 },
    };
    for (size_t i = 0; i < SDL_arraysize(kTries); ++i) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kTries[i].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, kTries[i].minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, kTries[i].profile);
        SDL_GLContext ctx = SDL_GL_CreateContext(p->window);
        if (!ctx) {
            fprintf(stderr, "[launcher] SDL_GL_CreateContext(%d.%d) failed: %s\n",
                    kTries[i].major, kTries[i].minor, SDL_GetError());
            continue;
        }
        SDL_GL_MakeCurrent(p->window, ctx);
        char version[128];
        int major = 0, minor = 0;
        if (launcher_gl_context_version(&major, &minor, version, sizeof(version)) &&
            major >= 3) {
            p->gl = ctx;
            p->gl_major = major;
            p->gl_minor = minor;
            fprintf(stderr, "[launcher] OpenGL %s\n", version);
            return true;
        }
        fprintf(stderr, "[launcher] OpenGL context below 3.0 (GL_VERSION=%s)\n",
                version);
        SDL_GL_MakeCurrent(p->window, NULL);
        SDL_GL_DestroyContext(ctx);
    }
    return false;
}
#endif

#if defined(LNG_SDL_RENDERER)
static SDL_Renderer* s_renderer;
void* launcher_platform_active_renderer(void) { return s_renderer; }

/* SDL_Renderer instead of a GL context. On Windows Direct3D 11 is asked for
 * by name (it is also SDL's own first choice there), so a machine whose GPU
 * has no usable OpenGL driver -- Microsoft's Basic Display Adapter, old Intel
 * parts -- still gets a hardware launcher; any other driver SDL offers is the
 * fallback, then its software renderer. LNG_RENDER_DRIVER=<name> overrides
 * (e.g. "opengl", "vulkan", "software") for tests. */
static bool create_launcher_renderer(LauncherPlatform* p) {
    const char* forced = getenv("LNG_RENDER_DRIVER");
    const char* tries[3] = { NULL, NULL, NULL };
    int n = 0;
    if (forced && forced[0]) tries[n++] = forced;
#if defined(_WIN32)
    else tries[n++] = "direct3d11";
#endif
    tries[n++] = NULL;   /* SDL's default order */
    for (int i = 0; i < n; ++i) {
        SDL_Renderer* r = SDL_CreateRenderer(p->window, tries[i]);
        if (!r) {
            fprintf(stderr, "[launcher] SDL_CreateRenderer(%s) failed: %s\n",
                    tries[i] ? tries[i] : "default", SDL_GetError());
            continue;
        }
        s_renderer = r;
        p->renderer = r;
        p->renderer_name = SDL_GetRendererName(r);
        SDL_SetRenderVSync(r, 1);   // vsync -- a launcher has no reason to spin
        fprintf(stderr, "[launcher] renderer %s\n",
                p->renderer_name ? p->renderer_name : "?");
        return true;
    }
    return false;
}
#else
void* launcher_platform_active_renderer(void) { return NULL; }
#endif

bool launcher_platform_open(LauncherPlatform* p, const char* title,
                            int logical_w, int logical_h) {
    if (!p) return false;
    SDL_zerop(p);
    launcher_boot_timing_mark("rui:platform_open:begin");

    SDL_SetMainReady();   // we built with SDL_MAIN_HANDLED (real main() is entry)
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
        fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    {
        const int version = SDL_GetVersion();
        fprintf(stderr, "[launcher] SDL runtime %d.%d.%d (%s)\n",
                SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version),
                SDL_VERSIONNUM_MICRO(version), SDL_GetRevision());
    }

#if !defined(LNG_SDL_RENDERER)
    // Context version/profile: create_launcher_gl_context().
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
#endif

    // HIGH_PIXEL_DENSITY is the linchpin: it asks SDL for a native-resolution
    // backbuffer on fractional-scale displays (esp. Wayland) instead of a
    // logical-size buffer the compositor blurs up. RESIZABLE lets us exercise
    // the live-resize requirement.
    SDL_WindowFlags flags =
#if !defined(LNG_SDL_RENDERER)
        SDL_WINDOW_OPENGL |
#endif
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    /* Automated launcher checks (LNG_SCRIPT + LNG_TEST_HIDDEN, as on SDL2)
     * must not steal the user's desktop/focus. */
    if (getenv("LNG_SCRIPT") && getenv("LNG_TEST_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;

    int win_w = logical_w;
    int win_h = logical_h;
    fit_initial_window_to_display(&win_w, &win_h);

    p->window = SDL_CreateWindow(title ? title : "Launcher",
                                 win_w, win_h, flags);
    if (!p->window) {
        fprintf(stderr, "[launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (s_quit_sdl) SDL_Quit();
        return false;
    }

#if defined(LNG_SDL_RENDERER)
    if (!create_launcher_renderer(p)) {
#else
    if (!create_launcher_gl_context(p)) {
#endif
        SDL_DestroyWindow(p->window);
        p->window = NULL;
        if (s_quit_sdl) SDL_Quit();
        return false;
    }

#if !defined(LNG_SDL_RENDERER)
    SDL_GL_SetSwapInterval(1);   // vsync — a launcher has no reason to spin
#endif

    launcher_platform_refresh_metrics(p);

    // The window was created `logical_w` PIXELS wide. Where the point/pixel
    // split is synthesized (Windows, X11) that is only logical_w/scale logical
    // units — a 1100-unit layout arriving in 733 of them drops straight to the
    // narrow one-column breakpoint on a 150% desktop — so this asks for the
    // pixels the requested logical size really needs, and keeps the result
    // reachable on displays that cannot show them all.
    fit_window_to_display(p, logical_w, logical_h);
    launcher_boot_timing_mark("rui:platform_open:window+gl_ready");
    return true;
}

void launcher_platform_set_icon(LauncherPlatform* p, const char* image_path) {
    if (!p || !p->window || !image_path || !image_path[0]) return;
    int w = 0, h = 0;
    unsigned char* pixels = launcher_image_load_rgba(image_path, &w, &h);
    if (!pixels) return;
    /* SDL copies the pixels into its own icon storage, so the decoded buffer
     * is ours to free as soon as SetWindowIcon returns. */
    SDL_Surface* surf =
        SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (surf) {
        SDL_SetWindowIcon(p->window, surf);
        SDL_DestroySurface(surf);
    }
    launcher_image_free(pixels);
}

// Test hook: LNG_FORCE_SCALE=<1..4> pins the display scale, so the HiDPI path
// can be exercised end to end on a 100% display. 0 when unset or out of range.
static float forced_display_scale(void) {
    const char* fs = getenv("LNG_FORCE_SCALE");
    float v;
    if (!fs || !fs[0]) return 0.0f;
    v = (float)atof(fs);
    return (v > 1.0f && v <= 4.0f) ? v : 0.0f;
}

void launcher_platform_refresh_metrics(LauncherPlatform* p) {
    int win_w = 0, win_h = 0;
    float s, forced;
    if (!p || !p->window) return;

    SDL_GetWindowSize(p->window, &win_w, &win_h);
    SDL_GetWindowSizeInPixels(p->window, &p->pixel_w, &p->pixel_h);

    s = SDL_GetWindowDisplayScale(p->window);
    if (s <= 0.0f) s = 1.0f;
    forced = forced_display_scale();
    if (forced > 0.0f) s = forced;

    p->logical_w = win_w;
    p->logical_h = win_h;
    p->display_scale = s;
    p->input_scale = 1.0f;

    // Where SDL reports a point/pixel split of its own — macOS retina, Wayland
    // under HIGH_PIXEL_DENSITY — the window size already IS the logical size
    // and the density is carried by the pixel size. Windows and X11 report
    // both in pixels, so there is no split to read and the launcher drew at
    // 100% on a 150% desktop. Synthesize one: the layout gets pixels/scale
    // logical units and the backend renders it at pixel density, which is the
    // same shape as the Retina model rather than a second scaling scheme.
    if (s > 1.0f && win_w > 0 && p->pixel_w == win_w) {
        p->logical_w = (int)((float)p->pixel_w / s);
        p->logical_h = (int)((float)p->pixel_h / s);
    }
    if (p->logical_w > 0)
        p->input_scale = (float)win_w / (float)p->logical_w;
}

void launcher_platform_present(LauncherPlatform* p) {
#if defined(LNG_SDL_RENDERER)
    if (p && p->renderer) SDL_RenderPresent((SDL_Renderer*)p->renderer);
#else
    if (p && p->window) SDL_GL_SwapWindow(p->window);
#endif
}

void launcher_platform_close(LauncherPlatform* p) {
    if (!p) return;
#if defined(LNG_SDL_RENDERER)
    if (p->renderer) {
        SDL_DestroyRenderer((SDL_Renderer*)p->renderer);
        p->renderer = NULL; p->renderer_name = NULL; s_renderer = NULL;
    }
#endif
    if (p->gl)     { SDL_GL_DestroyContext(p->gl); p->gl = NULL; }
    if (p->window) { SDL_DestroyWindow(p->window); p->window = NULL; }
    /* Soft-return rematch hosts that skip SDL_Quit (s_quit_sdl=false) keep the
     * subsystem alive across launcher↔game transitions. When we do quit, hosts
     * MUST re-SDL_Init (video+audio+gamecontroller) before recreating the game
     * window — see docs/HOST_NETPLAY.md. */
    if (s_quit_sdl)
        SDL_Quit();
}
