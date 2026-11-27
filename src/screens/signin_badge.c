/* signin_badge.c: the application's mark (signin_badge.h). */
#include "screens/signin_badge.h"

/* src/gui/logo.png, compiled in by CMakeLists.txt (ui_logo.c). */
extern const unsigned char ui_logo_png[];
extern const unsigned int ui_logo_png_len;

static Rid g_logo;
static int g_made;

/* The logo's texture, decoded on first use and shared. Raven's textures go
 * with Rshutdown: one made in an earlier session of the library is not
 * valid any more. */
static Rid logo_texture(void)
{
    if (!g_made || !Rtexture_is_valid(g_logo)) {
        g_logo = Rtexture_create((const char *)ui_logo_png, ui_logo_png_len);
        g_made = 1;
    }
    return g_logo;
}

Rid signin_badge(Rid parent, float side)
{
    Rid image = Rwidget_create(R_WIDGET_IMAGE);
    Rwidget_add_child(parent, image);
    Rimage_set_stretch(image, R_STRETCH_FIT);
    Rimage_set_texture(image, logo_texture());
    Rwidget_set_size(image, side, side);
    Rwidget_set_hit_testable(image, R_FALSE);
    Rwidget_set_accessible_hidden(image, R_TRUE);
    return image;
}
