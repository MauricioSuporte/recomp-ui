// launcher_gl.h — tiny image->GL-texture loader shared by both backends.
//
// Uses stb_image (public domain) to decode TGA/PNG and uploads an RGBA texture
// via GL 1.1 calls (available from opengl32 / libGL). Backend-agnostic: the
// ImGui backend feeds the id to ImGui::Image; the Clay renderer will bind it
// directly.

#ifndef LAUNCHER_NG_GL_H
#define LAUNCHER_NG_GL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int id;   // GL texture name (0 = not loaded; 1 with `handle`)
    int          w, h;
    void*        handle; // SDL_Texture* in LNG_SDL_RENDERER builds, else NULL
} LauncherTexture;

// Load an image file into a freshly created GL texture (linear filtered,
// clamped). Returns a texture with id==0 on failure. Requires a current GL
// context.
LauncherTexture launcher_texture_load(const char* path);

// As above, but for images that carry no alpha channel (e.g. 24-bit TGA art
// with a flat backdrop baked in): samples the top-left pixel and makes every
// pixel within `tolerance` (0-255 per channel) fully transparent. Keeps the
// art asset-agnostic instead of hand-editing each game's images.
LauncherTexture launcher_texture_load_colorkey(const char* path, int tolerance);

void launcher_texture_free(LauncherTexture* t);

// Version of the CURRENT desktop GL context, parsed from GL_VERSION. Returns
// false when there is no usable version string. `out_version` (optional)
// receives GL_VERSION for logs. On Windows, SDL hands back a legacy context
// (Microsoft's "GDI Generic" OpenGL 1.1) instead of failing when the driver
// lacks WGL_ARB_create_context -- e.g. a GPU with no vendor driver running on
// the Microsoft Basic Display Adapter -- so the platform layer checks this
// before accepting a context.
bool launcher_gl_context_version(int* major, int* minor,
                                 char* out_version, size_t out_version_len);

// Decode an image file to a tightly-packed RGBA8 buffer, WITHOUT touching GL.
// For the one consumer that needs pixels rather than a texture: the window /
// taskbar icon, which SDL wants as a surface. Returns NULL on failure; on
// success *w and *h are the image size and the caller must release the buffer
// with launcher_image_free().
//
// This lives here rather than in the caller because launcher_gl.c owns the
// launcher's PRIVATE stb_image instance (STB_IMAGE_STATIC — see the comment
// there): a second copy in another translation unit is exactly the symbol
// collision that build was set up to avoid.
unsigned char* launcher_image_load_rgba(const char* path, int* w, int* h);
void           launcher_image_free(unsigned char* pixels);

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_GL_H
