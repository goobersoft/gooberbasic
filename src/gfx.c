/* GooberBASIC graphics, input and sound (SDL2).
 *
 * This is the ONLY file that talks to SDL. Porting to SDL3 means rewriting
 * this file; the language, compiler and VM do not change.
 *
 * Resources (images, fonts, sounds) are plain Int handles, like Blitz.
 * Handle 0 is never valid, so `If img = 0` means "failed to load".
 */
#include "vm.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_mixer.h>
#include <math.h>

#define ARG_I(n) (args[n].as.i)
#define ARG_F(n) ((float)args[n].as.f)
#define ARG_S(n) (str_chars(args[n].as.s))
#define RET_VOID return v_int(0)

typedef struct { SDL_Texture *tex; int w, h; float hx, hy; } Image;
typedef struct { TTF_Font *font; int size; } Font;

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    int width, height;
    Uint8 r, g, b, a;
    Uint8 cr, cg, cb;
    char title[256];
    bool vsync;

    Image *images; int nimages;
    Font *fonts; int nfonts;
    int curFont;            /* 0 = default font */
    bool ttfReady;

    Mix_Chunk **sounds; int nsounds;
    Mix_Music *music;
    bool audioReady;

    int keyHits[SDL_NUM_SCANCODES];
    int mouseHits[4];
    int mouseX, mouseY, mouseZ;
    bool quitRequested;
    bool usesAppTerminate;
} G = {.r = 255, .g = 255, .b = 255, .a = 255, .title = "GooberBASIC"};

/* ---- text cache: rendering TTF text is slow, so remember recent strings ---- */
#define TEXT_CACHE 256
typedef struct { char *s; int font; Uint32 color; SDL_Texture *tex; int w, h; } TextEntry;
static TextEntry textCache[TEXT_CACHE];

static void text_cache_clear(void) {
    for (int i = 0; i < TEXT_CACHE; i++) {
        if (textCache[i].tex) SDL_DestroyTexture(textCache[i].tex);
        free(textCache[i].s);
        memset(&textCache[i], 0, sizeof textCache[i]);
    }
}

static void need_graphics(VM *vm, const char *what) {
    if (!G.ren) vm_error(vm, "%s needs a window: call graphics width, height first", what);
}

/* ---- events ---- */

void gfx_pump_events(void) {
    if (!G.win) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                G.quitRequested = true;
                if (!G.usesAppTerminate) vm_exit(gb_vm, 0); /* program ignores closing: just end */
                break;
            case SDL_KEYDOWN:
                if (!e.key.repeat && e.key.keysym.scancode < SDL_NUM_SCANCODES) G.keyHits[e.key.keysym.scancode]++;
                break;
            case SDL_MOUSEBUTTONDOWN: {
                int b = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2
                      : e.button.button == SDL_BUTTON_MIDDLE ? 3 : 0;
                if (b) G.mouseHits[b]++;
                break;
            }
            case SDL_MOUSEWHEEL: G.mouseZ += e.wheel.y; break;
            case SDL_MOUSEMOTION: G.mouseX = e.motion.x; G.mouseY = e.motion.y; break;
        }
    }
}

/* ---- window ---- */

static Value g_graphics(VM *vm, int argc, Value *args) {
    int w = ARG_I(0), h = ARG_I(1);
    bool full = argc > 2 && ARG_I(2);
    if (w <= 0 || h <= 0) vm_error(vm, "graphics: bad size %dx%d", w, h);
    if (G.win) {
        SDL_SetWindowSize(G.win, w, h);
        SDL_SetWindowFullscreen(G.win, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_RenderSetLogicalSize(G.ren, w, h);
        G.width = w;
        G.height = h;
        RET_VOID;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) vm_error(vm, "graphics: could not start SDL: %s", SDL_GetError());
    Uint32 flags = SDL_WINDOW_SHOWN | (full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    G.win = SDL_CreateWindow(G.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!G.win) vm_error(vm, "graphics: could not open a window: %s", SDL_GetError());
    G.ren = SDL_CreateRenderer(G.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!G.ren) G.ren = SDL_CreateRenderer(G.win, -1, SDL_RENDERER_SOFTWARE);
    if (!G.ren) vm_error(vm, "graphics: could not create a renderer: %s", SDL_GetError());
    G.vsync = true;
    SDL_RenderSetLogicalSize(G.ren, w, h);
    SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);
    G.width = w;
    G.height = h;
    SDL_SetRenderDrawColor(G.ren, 0, 0, 0, 255);
    SDL_RenderClear(G.ren);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    RET_VOID;
}

static Value g_endgraphics(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc; (void)args;
    gfx_shutdown();
    RET_VOID;
}

static Value g_apptitle(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    snprintf(G.title, sizeof G.title, "%s", ARG_S(0));
    if (G.win) SDL_SetWindowTitle(G.win, G.title);
    RET_VOID;
}

static Value g_graphicswidth(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; return v_int(G.width); }
static Value g_graphicsheight(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; return v_int(G.height); }

static Value g_appterminate(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc; (void)args;
    G.usesAppTerminate = true;
    gfx_pump_events();
    return v_int(G.quitRequested);
}

/* ---- drawing ---- */

static void apply_color(void) { SDL_SetRenderDrawColor(G.ren, G.r, G.g, G.b, G.a); }

static Uint8 clamp255(int v) { return (Uint8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static Value g_cls(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    need_graphics(vm, "cls");
    SDL_SetRenderDrawColor(G.ren, G.cr, G.cg, G.cb, 255);
    SDL_RenderClear(G.ren);
    RET_VOID;
}

static Value g_flip(VM *vm, int argc, Value *args) {
    need_graphics(vm, "flip");
    bool want = argc > 0 ? ARG_I(0) != 0 : true;
    if (want != G.vsync) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
        SDL_RenderSetVSync(G.ren, want ? 1 : 0);
#endif
        G.vsync = want;
    }
    SDL_RenderPresent(G.ren);
    net_poll();
    gfx_pump_events();
    RET_VOID;
}

static Value g_setcolor(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    G.r = clamp255(ARG_I(0));
    G.g = clamp255(ARG_I(1));
    G.b = clamp255(ARG_I(2));
    RET_VOID;
}

static Value g_setalpha(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    double a = args[0].as.f;
    G.a = clamp255((int)lround(a * 255.0));
    RET_VOID;
}

static Value g_setclscolor(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    G.cr = clamp255(ARG_I(0));
    G.cg = clamp255(ARG_I(1));
    G.cb = clamp255(ARG_I(2));
    RET_VOID;
}

static Value g_plot(VM *vm, int argc, Value *args) {
    (void)argc;
    need_graphics(vm, "plot");
    apply_color();
    SDL_RenderDrawPointF(G.ren, ARG_F(0), ARG_F(1));
    RET_VOID;
}

static Value g_drawline(VM *vm, int argc, Value *args) {
    (void)argc;
    need_graphics(vm, "draw_line");
    apply_color();
    SDL_RenderDrawLineF(G.ren, ARG_F(0), ARG_F(1), ARG_F(2), ARG_F(3));
    RET_VOID;
}

static Value g_drawrect(VM *vm, int argc, Value *args) {
    need_graphics(vm, "draw_rect");
    apply_color();
    SDL_FRect r = {ARG_F(0), ARG_F(1), ARG_F(2), ARG_F(3)};
    if (argc > 4 && !ARG_I(4)) SDL_RenderDrawRectF(G.ren, &r);
    else SDL_RenderFillRectF(G.ren, &r);
    RET_VOID;
}

static void draw_oval(float x, float y, float w, float h, bool filled) {
    apply_color();
    float rx = w / 2, ry = h / 2, cx = x + rx, cy = y + ry;
    if (rx <= 0 || ry <= 0) return;
    if (filled) {
        int rows = (int)ceilf(h);
        for (int i = 0; i < rows; i++) {
            float dy = ((float)i + 0.5f - ry) / ry;
            float t = 1.0f - dy * dy;
            if (t < 0) continue;
            float half = rx * sqrtf(t);
            SDL_RenderDrawLineF(G.ren, cx - half, y + (float)i, cx + half, y + (float)i);
        }
    } else {
        int seg = (int)fmaxf(16.0f, fminf(256.0f, (rx + ry) * 0.75f));
        SDL_FPoint pts[257];
        for (int i = 0; i <= seg; i++) {
            float a = (float)i / (float)seg * 6.2831853f;
            pts[i].x = cx + cosf(a) * rx;
            pts[i].y = cy + sinf(a) * ry;
        }
        SDL_RenderDrawLinesF(G.ren, pts, seg + 1);
    }
}

static Value g_drawoval(VM *vm, int argc, Value *args) {
    need_graphics(vm, "draw_oval");
    draw_oval(ARG_F(0), ARG_F(1), ARG_F(2), ARG_F(3), !(argc > 4 && !ARG_I(4)));
    RET_VOID;
}

static Value g_drawcircle(VM *vm, int argc, Value *args) {
    need_graphics(vm, "draw_circle");
    float r = ARG_F(2);
    draw_oval(ARG_F(0) - r, ARG_F(1) - r, r * 2, r * 2, !(argc > 3 && !ARG_I(3)));
    RET_VOID;
}

/* ---- text ---- */

static const char *default_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/Library/Fonts/Courier New.ttf",
    "C:/Windows/Fonts/consola.ttf",
    NULL,
};
static TTF_Font *defaultFont;

static void need_ttf(VM *vm) {
    if (G.ttfReady) return;
    if (TTF_Init() != 0) vm_error(vm, "Could not start font system: %s", TTF_GetError());
    G.ttfReady = true;
}

static TTF_Font *current_font(VM *vm) {
    if (G.curFont > 0) return G.fonts[G.curFont - 1].font;
    if (!defaultFont) {
        need_ttf(vm);
        const char *env = getenv("GOOBER_FONT");
        if (env) defaultFont = TTF_OpenFont(env, 16);
        for (int i = 0; !defaultFont && default_fonts[i]; i++) defaultFont = TTF_OpenFont(default_fonts[i], 16);
        if (!defaultFont)
            vm_error(vm, "No default font found. Use load_font(\"file.ttf\", size) and set_font, or set GOOBER_FONT");
    }
    return defaultFont;
}

static Value g_loadfont(VM *vm, int argc, Value *args) {
    (void)argc;
    need_ttf(vm);
    TTF_Font *f = TTF_OpenFont(ARG_S(0), ARG_I(1) > 0 ? ARG_I(1) : 16);
    if (!f) return v_int(0);
    G.fonts = gb_realloc(G.fonts, sizeof(Font) * (size_t)(G.nfonts + 1));
    G.fonts[G.nfonts].font = f;
    G.fonts[G.nfonts].size = ARG_I(1);
    return v_int(++G.nfonts);
}

static Value g_setfont(VM *vm, int argc, Value *args) {
    (void)argc;
    int h = ARG_I(0);
    if (h < 0 || h > G.nfonts || (h > 0 && !G.fonts[h - 1].font)) vm_error(vm, "set_font: invalid font handle %d", h);
    G.curFont = h;
    RET_VOID;
}

static TextEntry *render_text(VM *vm, const char *s) {
    TTF_Font *font = current_font(vm);
    Uint32 color = ((Uint32)G.r << 16) | ((Uint32)G.g << 8) | G.b;
    unsigned h = 2166136261u;
    for (const char *p = s; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ color ^ (unsigned)G.curFont * 7919u) % TEXT_CACHE;
    TextEntry *e = &textCache[h];
    if (e->s && e->font == G.curFont && e->color == color && strcmp(e->s, s) == 0) return e;
    if (e->tex) SDL_DestroyTexture(e->tex);
    free(e->s);
    memset(e, 0, sizeof *e);
    SDL_Color c = {G.r, G.g, G.b, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, c);
    if (!surf) return NULL;
    e->tex = SDL_CreateTextureFromSurface(G.ren, surf);
    e->w = surf->w;
    e->h = surf->h;
    SDL_FreeSurface(surf);
    e->s = gb_strdup(s);
    e->font = G.curFont;
    e->color = color;
    return e;
}

static Value g_drawtext(VM *vm, int argc, Value *args) {
    need_graphics(vm, "draw_text");
    const char *s = ARG_S(0);
    if (!*s) RET_VOID;
    TextEntry *e = render_text(vm, s);
    if (!e || !e->tex) RET_VOID;
    float x = ARG_F(1), y = ARG_F(2);
    if (argc > 3 && ARG_I(3)) x -= (float)e->w / 2;
    if (argc > 4 && ARG_I(4)) y -= (float)e->h / 2;
    SDL_FRect dst = {x, y, (float)e->w, (float)e->h};
    SDL_SetTextureAlphaMod(e->tex, G.a);
    SDL_RenderCopyF(G.ren, e->tex, NULL, &dst);
    RET_VOID;
}

static Value g_textwidth(VM *vm, int argc, Value *args) {
    (void)argc;
    int w = 0, h = 0;
    TTF_SizeUTF8(current_font(vm), ARG_S(0), &w, &h);
    return v_int(w);
}

static Value g_textheight(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    return v_int(TTF_FontHeight(current_font(vm)));
}

/* ---- images ---- */

static Image *get_image(VM *vm, int h, const char *who) {
    if (h <= 0 || h > G.nimages || !G.images[h - 1].tex) vm_error(vm, "%s: invalid image handle %d", who, h);
    return &G.images[h - 1];
}

static Value g_loadimage(VM *vm, int argc, Value *args) {
    (void)argc;
    need_graphics(vm, "load_image");
    SDL_Surface *s = IMG_Load(ARG_S(0));
    if (!s) return v_int(0);
    SDL_Texture *t = SDL_CreateTextureFromSurface(G.ren, s);
    int w = s->w, h = s->h;
    SDL_FreeSurface(s);
    if (!t) return v_int(0);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    G.images = gb_realloc(G.images, sizeof(Image) * (size_t)(G.nimages + 1));
    G.images[G.nimages] = (Image){t, w, h, 0, 0};
    return v_int(++G.nimages);
}

static Value g_freeimage(VM *vm, int argc, Value *args) {
    (void)argc;
    Image *im = get_image(vm, ARG_I(0), "free_image");
    SDL_DestroyTexture(im->tex);
    im->tex = NULL;
    RET_VOID;
}

static Value g_imagewidth(VM *vm, int argc, Value *args) { (void)argc; return v_int(get_image(vm, ARG_I(0), "image_width")->w); }
static Value g_imageheight(VM *vm, int argc, Value *args) { (void)argc; return v_int(get_image(vm, ARG_I(0), "image_height")->h); }

static Value g_midhandle(VM *vm, int argc, Value *args) {
    (void)argc;
    Image *im = get_image(vm, ARG_I(0), "mid_handle");
    im->hx = (float)im->w / 2;
    im->hy = (float)im->h / 2;
    RET_VOID;
}

static Value g_sethandle(VM *vm, int argc, Value *args) {
    (void)argc;
    Image *im = get_image(vm, ARG_I(0), "set_image_handle");
    im->hx = ARG_F(1);
    im->hy = ARG_F(2);
    RET_VOID;
}

static Value g_drawimage(VM *vm, int argc, Value *args) {
    (void)argc;
    need_graphics(vm, "draw_image");
    Image *im = get_image(vm, ARG_I(0), "draw_image");
    SDL_FRect dst = {ARG_F(1) - im->hx, ARG_F(2) - im->hy, (float)im->w, (float)im->h};
    SDL_SetTextureAlphaMod(im->tex, G.a);
    SDL_RenderCopyF(G.ren, im->tex, NULL, &dst);
    RET_VOID;
}

/* DrawImageEx img, x, y, angle#, scaleX#, scaleY#  (angle in degrees, around the handle) */
static Value g_drawimageex(VM *vm, int argc, Value *args) {
    need_graphics(vm, "draw_image_ex");
    Image *im = get_image(vm, ARG_I(0), "draw_image_ex");
    float ang = argc > 3 ? ARG_F(3) : 0;
    float sx = argc > 4 ? ARG_F(4) : 1;
    float sy = argc > 5 ? ARG_F(5) : sx;
    SDL_RendererFlip flip = SDL_FLIP_NONE;
    if (sx < 0) { sx = -sx; flip |= SDL_FLIP_HORIZONTAL; }
    if (sy < 0) { sy = -sy; flip |= SDL_FLIP_VERTICAL; }
    SDL_FRect dst = {ARG_F(1) - im->hx * sx, ARG_F(2) - im->hy * sy, (float)im->w * sx, (float)im->h * sy};
    SDL_FPoint center = {im->hx * sx, im->hy * sy};
    SDL_SetTextureAlphaMod(im->tex, G.a);
    SDL_RenderCopyExF(G.ren, im->tex, NULL, &dst, ang, &center, flip);
    RET_VOID;
}

/* DrawImageRect img, x, y, srcX, srcY, w, h  - draw part of an image (sprite sheets) */
static Value g_drawimagerect(VM *vm, int argc, Value *args) {
    (void)argc;
    need_graphics(vm, "draw_image_rect");
    Image *im = get_image(vm, ARG_I(0), "draw_image_rect");
    SDL_Rect src = {ARG_I(3), ARG_I(4), ARG_I(5), ARG_I(6)};
    SDL_FRect dst = {ARG_F(1), ARG_F(2), (float)src.w, (float)src.h};
    SDL_SetTextureAlphaMod(im->tex, G.a);
    SDL_RenderCopyF(G.ren, im->tex, &src, &dst);
    RET_VOID;
}

static Value g_savescreenshot(VM *vm, int argc, Value *args) {
    (void)argc;
    need_graphics(vm, "save_screenshot");
    int w, h;
    SDL_GetRendererOutputSize(G.ren, &w, &h);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return v_int(0);
    int ok = SDL_RenderReadPixels(G.ren, NULL, SDL_PIXELFORMAT_RGBA32, s->pixels, s->pitch) == 0;
    if (ok) ok = IMG_SavePNG(s, ARG_S(0)) == 0;
    SDL_FreeSurface(s);
    return v_int(ok);
}

/* ---- keyboard & mouse ---- */

static int check_key(VM *vm, int k) {
    if (k < 0 || k >= SDL_NUM_SCANCODES) vm_error(vm, "Invalid key code %d", k);
    return k;
}

static Value g_keydown(VM *vm, int argc, Value *args) {
    (void)argc;
    int k = check_key(vm, ARG_I(0));
    if (!G.win) return v_int(0);
    gfx_pump_events();
    const Uint8 *st = SDL_GetKeyboardState(NULL);
    return v_int(st[k] ? 1 : 0);
}

static Value g_keyhit(VM *vm, int argc, Value *args) {
    (void)argc;
    int k = check_key(vm, ARG_I(0));
    gfx_pump_events();
    int n = G.keyHits[k];
    G.keyHits[k] = 0;
    return v_int(n);
}

static Value g_flushkeys(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc; (void)args;
    gfx_pump_events();
    memset(G.keyHits, 0, sizeof G.keyHits);
    memset(G.mouseHits, 0, sizeof G.mouseHits);
    RET_VOID;
}

static Value g_waitkey(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    need_graphics(vm, "wait_key");
    memset(G.keyHits, 0, sizeof G.keyHits);
    for (;;) {
        SDL_Delay(5);
        gfx_pump_events();
        if (G.quitRequested) return v_int(0);
        for (int k = 0; k < SDL_NUM_SCANCODES; k++)
            if (G.keyHits[k]) { G.keyHits[k] = 0; return v_int(k); }
    }
}

static Value g_mousex(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; gfx_pump_events(); return v_int(G.mouseX); }
static Value g_mousey(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; gfx_pump_events(); return v_int(G.mouseY); }
static Value g_mousez(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; gfx_pump_events(); return v_int(G.mouseZ); }

static Value g_mousedown(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    if (!G.win) return v_int(0);
    gfx_pump_events();
    Uint32 st = SDL_GetMouseState(NULL, NULL);
    int b = ARG_I(0);
    Uint32 mask = b == 1 ? SDL_BUTTON_LMASK : b == 2 ? SDL_BUTTON_RMASK : b == 3 ? SDL_BUTTON_MMASK : 0;
    return v_int((st & mask) ? 1 : 0);
}

static Value g_mousehit(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    int b = ARG_I(0);
    if (b < 1 || b > 3) return v_int(0);
    gfx_pump_events();
    int n = G.mouseHits[b];
    G.mouseHits[b] = 0;
    return v_int(n);
}

static Value g_hidemouse(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; SDL_ShowCursor(SDL_DISABLE); RET_VOID; }
static Value g_showmouse(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; SDL_ShowCursor(SDL_ENABLE); RET_VOID; }

static Value g_movemouse(VM *vm, int argc, Value *args) {
    (void)argc;
    need_graphics(vm, "move_mouse");
    SDL_WarpMouseInWindow(G.win, ARG_I(0), ARG_I(1));
    G.mouseX = ARG_I(0);
    G.mouseY = ARG_I(1);
    RET_VOID;
}

/* ---- sound ---- */

static void need_audio(VM *vm) {
    if (G.audioReady) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) vm_error(vm, "Could not start audio: %s", SDL_GetError());
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) != 0) vm_error(vm, "Could not open audio: %s", Mix_GetError());
    Mix_AllocateChannels(32);
    G.audioReady = true;
}

static Mix_Chunk *get_sound(VM *vm, int h, const char *who) {
    if (h <= 0 || h > G.nsounds || !G.sounds[h - 1]) vm_error(vm, "%s: invalid sound handle %d", who, h);
    return G.sounds[h - 1];
}

static Value g_loadsound(VM *vm, int argc, Value *args) {
    (void)argc;
    need_audio(vm);
    Mix_Chunk *c = Mix_LoadWAV(ARG_S(0));
    if (!c) return v_int(0);
    G.sounds = gb_realloc(G.sounds, sizeof(Mix_Chunk *) * (size_t)(G.nsounds + 1));
    G.sounds[G.nsounds] = c;
    return v_int(++G.nsounds);
}

static Value g_playsound(VM *vm, int argc, Value *args) {
    Mix_Chunk *c = get_sound(vm, ARG_I(0), "play_sound");
    int loops = argc > 1 ? ARG_I(1) : 0;
    int ch = Mix_PlayChannel(-1, c, loops);
    return v_int(ch + 1); /* channel handle, 0 = failed */
}

static Value g_stopchannel(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    if (G.audioReady && ARG_I(0) > 0) Mix_HaltChannel(ARG_I(0) - 1);
    RET_VOID;
}

static Value g_channelplaying(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    if (!G.audioReady || ARG_I(0) <= 0) return v_int(0);
    return v_int(Mix_Playing(ARG_I(0) - 1) ? 1 : 0);
}

static Value g_soundvolume(VM *vm, int argc, Value *args) {
    (void)argc;
    Mix_Chunk *c = get_sound(vm, ARG_I(0), "sound_volume");
    Mix_VolumeChunk(c, (int)(args[1].as.f * MIX_MAX_VOLUME));
    RET_VOID;
}

static Value g_freesound(VM *vm, int argc, Value *args) {
    (void)argc;
    Mix_Chunk *c = get_sound(vm, ARG_I(0), "free_sound");
    Mix_FreeChunk(c);
    G.sounds[ARG_I(0) - 1] = NULL;
    RET_VOID;
}

static Value g_playmusic(VM *vm, int argc, Value *args) {
    need_audio(vm);
    if (G.music) { Mix_HaltMusic(); Mix_FreeMusic(G.music); G.music = NULL; }
    G.music = Mix_LoadMUS(ARG_S(0));
    if (!G.music) return v_int(0);
    Mix_PlayMusic(G.music, argc > 1 ? ARG_I(1) : -1);
    return v_int(1);
}

static Value g_stopmusic(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc; (void)args;
    if (G.audioReady) Mix_HaltMusic();
    RET_VOID;
}

void gfx_shutdown(void) {
    text_cache_clear();
    for (int i = 0; i < G.nimages; i++) if (G.images[i].tex) SDL_DestroyTexture(G.images[i].tex);
    free(G.images); G.images = NULL; G.nimages = 0;
    for (int i = 0; i < G.nfonts; i++) if (G.fonts[i].font) TTF_CloseFont(G.fonts[i].font);
    free(G.fonts); G.fonts = NULL; G.nfonts = 0; G.curFont = 0;
    if (defaultFont) { TTF_CloseFont(defaultFont); defaultFont = NULL; }
    if (G.ttfReady) { TTF_Quit(); G.ttfReady = false; }
    if (G.audioReady) {
        if (G.music) { Mix_FreeMusic(G.music); G.music = NULL; }
        for (int i = 0; i < G.nsounds; i++) if (G.sounds[i]) Mix_FreeChunk(G.sounds[i]);
        free(G.sounds); G.sounds = NULL; G.nsounds = 0;
        Mix_CloseAudio();
        G.audioReady = false;
    }
    if (G.ren) { SDL_DestroyRenderer(G.ren); G.ren = NULL; }
    if (G.win) { SDL_DestroyWindow(G.win); G.win = NULL; IMG_Quit(); }
    SDL_Quit();
}

NativeDef gb_gfx_natives[] = {
    {"graphics", 'v', "ii|i", g_graphics},
    {"end_graphics", 'v', "", g_endgraphics},
    {"app_title", 'v', "s", g_apptitle},
    {"graphics_width", 'i', "", g_graphicswidth},
    {"graphics_height", 'i', "", g_graphicsheight},
    {"app_terminate", 'i', "", g_appterminate},

    {"cls", 'v', "", g_cls},
    {"flip", 'v', "|i", g_flip},
    {"set_color", 'v', "iii", g_setcolor},
    {"set_alpha", 'v', "f", g_setalpha},
    {"set_cls_color", 'v', "iii", g_setclscolor},
    {"plot", 'v', "ff", g_plot},
    {"draw_line", 'v', "ffff", g_drawline},
    {"draw_rect", 'v', "ffff|i", g_drawrect},
    {"draw_oval", 'v', "ffff|i", g_drawoval},
    {"draw_circle", 'v', "fff|i", g_drawcircle},

    {"draw_text", 'v', "sff|ii", g_drawtext},
    {"text_width", 'i', "s", g_textwidth},
    {"text_height", 'i', "|s", g_textheight},
    {"load_font", 'i', "s|i", g_loadfont},
    {"set_font", 'v', "i", g_setfont},

    {"load_image", 'i', "s", g_loadimage},
    {"free_image", 'v', "i", g_freeimage},
    {"image_width", 'i', "i", g_imagewidth},
    {"image_height", 'i', "i", g_imageheight},
    {"mid_handle", 'v', "i", g_midhandle},
    {"set_image_handle", 'v', "iff", g_sethandle},
    {"draw_image", 'v', "iff", g_drawimage},
    {"draw_image_ex", 'v', "iff|fff", g_drawimageex},
    {"draw_image_rect", 'v', "iffiiii", g_drawimagerect},
    {"save_screenshot", 'i', "s", g_savescreenshot},

    {"key_down", 'i', "i", g_keydown},
    {"key_hit", 'i', "i", g_keyhit},
    {"flush_keys", 'v', "", g_flushkeys},
    {"wait_key", 'i', "", g_waitkey},
    {"mouse_x", 'i', "", g_mousex},
    {"mouse_y", 'i', "", g_mousey},
    {"mouse_z", 'i', "", g_mousez},
    {"mouse_down", 'i', "i", g_mousedown},
    {"mouse_hit", 'i', "i", g_mousehit},
    {"hide_mouse", 'v', "", g_hidemouse},
    {"show_mouse", 'v', "", g_showmouse},
    {"move_mouse", 'v', "ii", g_movemouse},

    {"load_sound", 'i', "s", g_loadsound},
    {"play_sound", 'i', "i|i", g_playsound},
    {"stop_channel", 'v', "i", g_stopchannel},
    {"channel_playing", 'i', "i", g_channelplaying},
    {"sound_volume", 'v', "if", g_soundvolume},
    {"free_sound", 'v', "i", g_freesound},
    {"play_music", 'i', "s|i", g_playmusic},
    {"stop_music", 'v', "", g_stopmusic},
    {NULL, 0, NULL, NULL},
};

#define K(name, sc) {"KEY_" #name, SDL_SCANCODE_##sc}
ConstDef gb_gfx_consts[] = {
    K(A, A), K(B, B), K(C, C), K(D, D), K(E, E), K(F, F), K(G, G), K(H, H), K(I, I),
    K(J, J), K(K, K), K(L, L), K(M, M), K(N, N), K(O, O), K(P, P), K(Q, Q), K(R, R),
    K(S, S), K(T, T), K(U, U), K(V, V), K(W, W), K(X, X), K(Y, Y), K(Z, Z),
    K(0, 0), K(1, 1), K(2, 2), K(3, 3), K(4, 4), K(5, 5), K(6, 6), K(7, 7), K(8, 8), K(9, 9),
    K(F1, F1), K(F2, F2), K(F3, F3), K(F4, F4), K(F5, F5), K(F6, F6),
    K(F7, F7), K(F8, F8), K(F9, F9), K(F10, F10), K(F11, F11), K(F12, F12),
    K(ESCAPE, ESCAPE), K(SPACE, SPACE), K(ENTER, RETURN), K(TAB, TAB), K(BACKSPACE, BACKSPACE),
    K(LEFT, LEFT), K(RIGHT, RIGHT), K(UP, UP), K(DOWN, DOWN),
    K(LSHIFT, LSHIFT), K(RSHIFT, RSHIFT), K(LCONTROL, LCTRL), K(RCONTROL, RCTRL),
    K(LALT, LALT), K(RALT, RALT), K(INSERT, INSERT), K(DELETE, DELETE),
    K(HOME, HOME), K(END, END), K(PAGEUP, PAGEUP), K(PAGEDOWN, PAGEDOWN),
    K(MINUS, MINUS), K(EQUALS, EQUALS), K(COMMA, COMMA), K(PERIOD, PERIOD), K(SLASH, SLASH),
    {"MOUSE_LEFT", 1}, {"MOUSE_RIGHT", 2}, {"MOUSE_MIDDLE", 3},
    {NULL, 0},
};
