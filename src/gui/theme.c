/*
 * theme.c: Raven Glass Dark with the classes of the application, the font,
 * the text scale, and the accent and wallpaper taken from the album art.
 */
#include "theme.h"

#include <math.h>
#include <string.h>

#include "../config.h"
#include "window.h"

#ifndef __APPLE__
/* The GUI font off macOS: a subset of Inter (third_party/inter), compiled
 * in by CMakeLists.txt (ui_font.c in the build folder). */
extern const unsigned char ui_font_ttf[];
extern const unsigned int ui_font_ttf_len;
#endif

/* The text roles: class, size at scale 1 (the theme's FONT_SIZE is 14), bold. */
static const struct {
    const char *cls;
    float size;
    int bold;
} g_roles[TEXT_ROLES] = {
    [TEXT_BODY] = { "", 14.0f, 0 },
    [TEXT_CAPTION] = { "caption", 12.0f, 0 },
    [TEXT_HEADLINE] = { "headline", 17.0f, 1 },
    [TEXT_TITLE] = { "title", 22.0f, 1 },
    [TEXT_LARGE] = { "large-title", 28.0f, 1 },
};

/* Text scale for each CFG_FONT_* setting: the body text is 12, 14, 16 or
 * 19 units, as the setting always made it. */
static const float g_text_scale[CFG_FONT_SIZES] = { 12.0f / 14.0f, 1.0f, 16.0f / 14.0f, 19.0f / 14.0f };

/* Spotify green, until there is art to take a color from. */
static const float g_default_accent[3] = { 30 / 255.0f, 215 / 255.0f, 96 / 255.0f };

static Rid g_font = R_INVALID_ID, g_bold = R_INVALID_ID;
static int g_text_size = -1;
static float g_accent[3];
static Rid g_wallpaper = R_INVALID_ID;         /* the window's background, from the art */
static Rid g_default_wallpaper = R_INVALID_ID; /* the one before any art, made once */

/* ---- accent ------------------------------------------------------------- */

static void apply_accent(const float rgb[3])
{
    memcpy(g_accent, rgb, sizeof g_accent);
    Rtheme_set_accent(rgb[0], rgb[1], rgb[2]);
    Rtheme_set_color(R_COLOR_SWITCH, rgb[0], rgb[1], rgb[2], 1.0f);
    /* toggles that are on, and the item playing: their text and icons */
    Rtheme_set_style_color(NULL, "on", R_COLOR_TEXT, rgb[0], rgb[1], rgb[2], 1.0f);
    Rtheme_set_style_color(NULL, "current", R_COLOR_TEXT, rgb[0], rgb[1], rgb[2], 1.0f);
}

void theme_accent(float rgb[3])
{
    memcpy(rgb, g_accent, sizeof g_accent);
}

/* ---- setup -------------------------------------------------------------- */

static void load_fonts(void)
{
#ifdef __APPLE__
    /* San Francisco: the system's font, which may not be shipped with the
     * app. Without it Raven's own font stays. */
    g_font = Rfont_create_from_file("/System/Library/Fonts/SFNS.ttf");
#else
    g_font = Rfont_create((const char *)ui_font_ttf, ui_font_ttf_len);
#endif
    if (g_font == R_INVALID_ID) {
        g_font = Rtheme_get_font();
    } else {
        Rtheme_set_font(g_font);
    }
    /* SF is a variable font of which Raven reads the regular instance; its
     * bold is drawn thicker by Raven. */
    g_bold = Rfont_get_variant(g_font, R_FONT_BOLD);
}

void theme_init(void)
{
    Rlook_set(R_LOOK_RAVEN_GLASS_DARK);
    load_fonts();
    for (int i = 0; i < TEXT_ROLES; i++)
        if (g_roles[i].cls[0])
            Rtheme_set_style_metric(NULL, g_roles[i].cls, R_METRIC_FONT_SIZE, g_roles[i].size);
    Rtheme_set_style_color(NULL, "muted", R_COLOR_TEXT, 1.0f, 1.0f, 1.0f, THEME_TEXT_DIM_ALPHA);
    Rtheme_set_style_color(NULL, "error", R_COLOR_TEXT, 1.0f, 0.47f, 0.45f, 1.0f);
    Rtheme_set_style_metric("Button", "round", R_METRIC_CORNER_RADIUS, 999.0f);
    Rtheme_set_style_metric("Button", "row", R_METRIC_CORNER_RADIUS, 10.0f);
    /* a text input inside a field component: the field draws the frame */
    Rtheme_set_style_material("TextInput", "bare", R_MATERIAL_SOLID);
    Rtheme_set_style_color("TextInput", "bare", R_COLOR_INPUT, 0.0f, 0.0f, 0.0f, 0.0f);
    Rtheme_set_style_color("TextInput", "bare", R_COLOR_CONTROL_BORDER, 0.0f, 0.0f, 0.0f, 0.0f);
    Rtheme_set_style_color("TextInput", "bare", R_COLOR_BORDER, 0.0f, 0.0f, 0.0f, 0.0f);
    Rtheme_set_style_color("TextInput", "bare", R_COLOR_FOCUS, 0.0f, 0.0f, 0.0f, 0.0f);
    apply_accent(g_default_accent);
    g_text_size = -1;
    g_wallpaper = g_default_wallpaper = R_INVALID_ID; /* textures of a former Raven session */
}

void theme_set_text_size(int size)
{
    if (size < 0 || size >= CFG_FONT_SIZES)
        size = CFG_FONT_DEFAULT;
    if (size == g_text_size)
        return;
    g_text_size = size;
    Raccessibility_set_text_scale(g_text_scale[size]);
}

float theme_text_size(TextRole role)
{
    return g_roles[role].size;
}

const char *theme_text_class(TextRole role)
{
    return g_roles[role].cls;
}

int theme_text_bold(TextRole role)
{
    return g_roles[role].bold;
}

Rid theme_font(int bold)
{
    return bold ? g_bold : g_font;
}

/* ---- art: accent and wallpaper ---------------------------------------- */

#define ART_N 96 /* the art is boiled down to ART_N x ART_N */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Box average of the RGBA image into an ART_N x ART_N grid of 0..1 colors. */
static void shrink(const uint8_t *px, int w, int h, float out[ART_N][ART_N][3])
{
    for (int y = 0; y < ART_N; y++) {
        int y0 = y * h / ART_N, y1 = (y + 1) * h / ART_N;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < ART_N; x++) {
            int x0 = x * w / ART_N, x1 = (x + 1) * w / ART_N;
            if (x1 <= x0)
                x1 = x0 + 1;
            float sum[3] = { 0, 0, 0 };
            int n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    const uint8_t *p = px + ((size_t)yy * (size_t)w + (size_t)xx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    n++;
                }
            for (int k = 0; k < 3; k++)
                out[y][x][k] = n ? sum[k] / (255.0f * (float)n) : 0.0f;
        }
    }
}

static void rgb_to_hsv(const float c[3], float *h, float *s, float *v)
{
    float mx = fmaxf(c[0], fmaxf(c[1], c[2])), mn = fminf(c[0], fminf(c[1], c[2])), d = mx - mn;
    *v = mx;
    *s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f)
        *h = 0.0f;
    else if (mx == c[0])
        *h = fmodf((c[1] - c[2]) / d + 6.0f, 6.0f);
    else if (mx == c[1])
        *h = (c[2] - c[0]) / d + 2.0f;
    else
        *h = (c[0] - c[1]) / d + 4.0f;
}

static void hsv_to_rgb(float h, float s, float v, float c[3])
{
    float f = h - floorf(h), p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch ((int)floorf(h) % 6) {
    case 0: c[0] = v, c[1] = t, c[2] = p; break;
    case 1: c[0] = q, c[1] = v, c[2] = p; break;
    case 2: c[0] = p, c[1] = v, c[2] = t; break;
    case 3: c[0] = p, c[1] = q, c[2] = v; break;
    case 4: c[0] = t, c[1] = p, c[2] = v; break;
    default: c[0] = v, c[1] = p, c[2] = q; break;
    }
}

/* The vivid color that covers the most of the art: hues weighted by
 * saturation and brightness, the strongest hue (with its neighbours) wins,
 * then made bright enough to read on the dark glass. 0 when the art has no
 * color to speak of (black and white). */
#define HUES 24
static int vivid_color(float img[ART_N][ART_N][3], float out[3])
{
    float weight[HUES] = { 0 }, sum[HUES][3] = { { 0 } };
    float total = 0.0f;
    for (int y = 0; y < ART_N; y++)
        for (int x = 0; x < ART_N; x++) {
            float h, s, v;
            rgb_to_hsv(img[y][x], &h, &s, &v);
            if (s < 0.25f || v < 0.22f)
                continue;
            float w = s * s * v;
            int bin = (int)(h / 6.0f * HUES) % HUES;
            weight[bin] += w;
            for (int k = 0; k < 3; k++)
                sum[bin][k] += img[y][x][k] * w;
            total += w;
        }
    int best = -1;
    float best_w = 0.0f;
    for (int i = 0; i < HUES; i++) {
        float w = weight[i] + 0.5f * (weight[(i + HUES - 1) % HUES] + weight[(i + 1) % HUES]);
        if (w > best_w) {
            best_w = w;
            best = i;
        }
    }
    /* a few specks of color in a gray picture are not its color */
    if (best < 0 || weight[best] <= 0.0f || total < 0.004f * ART_N * ART_N)
        return 0;
    float c[3], h, s, v;
    for (int k = 0; k < 3; k++)
        c[k] = sum[best][k] / weight[best];
    rgb_to_hsv(c, &h, &s, &v);
    hsv_to_rgb(h, clampf(s, 0.55f, 0.85f), clampf(v, 0.80f, 0.96f), out);
    return 1;
}

/* Three box blurs make a Gaussian; edges are clamped. */
static void blur(float img[ART_N][ART_N][3], int radius)
{
    static float tmp[ART_N][ART_N][3];
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < ART_N; y++)
            for (int x = 0; x < ART_N; x++) {
                float s[3] = { 0, 0, 0 };
                for (int k = -radius; k <= radius; k++) {
                    int xx = x + k < 0 ? 0 : x + k >= ART_N ? ART_N - 1 : x + k;
                    for (int c = 0; c < 3; c++)
                        s[c] += img[y][xx][c];
                }
                for (int c = 0; c < 3; c++)
                    tmp[y][x][c] = s[c] / (float)(2 * radius + 1);
            }
        for (int y = 0; y < ART_N; y++)
            for (int x = 0; x < ART_N; x++) {
                float s[3] = { 0, 0, 0 };
                for (int k = -radius; k <= radius; k++) {
                    int yy = y + k < 0 ? 0 : y + k >= ART_N ? ART_N - 1 : y + k;
                    for (int c = 0; c < 3; c++)
                        s[c] += tmp[yy][x][c];
                }
                for (int c = 0; c < 3; c++)
                    img[y][x][c] = s[c] / (float)(2 * radius + 1);
            }
    }
}

/* The art blurred to its colors, more saturated, and dark enough for white
 * text and glass to read on it, darker still towards the bottom. */
static Rid make_wallpaper(float img[ART_N][ART_N][3])
{
    static uint8_t out[ART_N * ART_N * 4];
    blur(img, 7);
    for (int y = 0; y < ART_N; y++) {
        float shade = 0.56f - 0.16f * (float)y / (ART_N - 1);
        for (int x = 0; x < ART_N; x++) {
            float *c = img[y][x];
            float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            uint8_t *o = out + ((size_t)y * ART_N + (size_t)x) * 4;
            for (int k = 0; k < 3; k++) {
                float v = (l + (c[k] - l) * 1.35f) * shade + 0.015f;
                o[k] = (uint8_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            o[3] = 255;
        }
    }
    Rid tex = Rtexture_create_from_pixels(out, ART_N, ART_N);
    if (tex != R_INVALID_ID)
        Rtexture_set_filter(tex, R_FILTER_LINEAR);
    return tex;
}

/* Before any art: soft green, violet and blue light in the dark, graded
 * like a wallpaper from art, so the glass has colors to bend. */
static Rid make_default_wallpaper(void)
{
    static const struct {
        float x, y, r, c[3];
    } glow[] = {
        { 0.12f, 0.18f, 0.50f, { 0.12f, 0.62f, 0.44f } },
        { 0.72f, 0.28f, 0.42f, { 0.48f, 0.24f, 0.70f } },
        { 0.86f, 0.88f, 0.55f, { 0.16f, 0.30f, 0.78f } },
    };
    static float img[ART_N][ART_N][3];
    for (int y = 0; y < ART_N; y++)
        for (int x = 0; x < ART_N; x++) {
            float c[3] = { 0.10f, 0.11f, 0.15f };
            for (size_t i = 0; i < sizeof glow / sizeof glow[0]; i++) {
                float dx = (float)x / ART_N - glow[i].x, dy = (float)y / ART_N - glow[i].y;
                float w = expf(-2.5f * (dx * dx + dy * dy) / (glow[i].r * glow[i].r));
                for (int k = 0; k < 3; k++)
                    c[k] += glow[i].c[k] * w;
            }
            memcpy(img[y][x], c, sizeof c);
        }
    return make_wallpaper(img);
}

void theme_set_art(const unsigned char *data, size_t len)
{
    static float img[ART_N][ART_N][3];
    float accent[3];
    int w = 0, h = 0;
    uint8_t *px = data && len ? Rimage_decode(data, len, &w, &h) : NULL;
    Rid wall = R_INVALID_ID;
    int vivid = 0;
    if (px && w > 0 && h > 0) {
        shrink(px, w, h, img);
        vivid = vivid_color(img, accent);
        wall = make_wallpaper(img);
    }
    if (px)
        Rpixels_free(px);
    if (wall == R_INVALID_ID && !Rtexture_is_valid(g_default_wallpaper))
        g_default_wallpaper = make_default_wallpaper();
    Rwindow_set_background(window_id(), wall != R_INVALID_ID ? wall : g_default_wallpaper);
    if (Rtexture_is_valid(g_wallpaper))
        Rtexture_destroy(g_wallpaper);
    g_wallpaper = wall;
    apply_accent(vivid ? accent : g_default_accent);
}
