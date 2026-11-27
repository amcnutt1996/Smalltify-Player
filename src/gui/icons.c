/*
 * icons.c: the SVG of every icon (drawn for this application, on a 24 x 24
 * grid: strokes of 1.8 with round ends, fills for the transport) and their
 * textures.
 */
#include "icons.h"

#include <string.h>

#define SVG(body) "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24'>" body "</svg>"
#define STROKE(body) \
    "<g fill='none' stroke='#ffffff' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>" body "</g>"
#define FILL(body) "<g fill='#ffffff' stroke='none'>" body "</g>"

/* the speaker of the volume icons */
#define SPEAKER_BODY \
    "<path d='M4 9.6C4 9 4.4 8.6 5 8.6H7.6L11 5.6C11.6 5.1 12.5 5.5 12.5 6.3V17.7C12.5 18.5 11.6 18.9 " \
    "11 18.4L7.6 15.4H5C4.4 15.4 4 15 4 14.4Z'/>"
/* the two arrows of repeat */
#define REPEAT_LOOP \
    "<path d='M4 11V9.5A3 3 0 0 1 7 6.5H19.5'/><path d='M17 4L19.5 6.5L17 9'/>" \
    "<path d='M20 13V14.5A3 3 0 0 1 17 17.5H4.5'/><path d='M7 20L4.5 17.5L7 15'/>"
#define HEART_PATH \
    "<path d='M12 19.5C12 19.5 3.5 14.6 3.5 9.1C3.5 6.5 5.4 4.6 7.9 4.6C9.6 4.6 11.1 5.6 12 7.1C12.9 " \
    "5.6 14.4 4.6 16.1 4.6C18.6 4.6 20.5 6.5 20.5 9.1C20.5 14.6 12 19.5 12 19.5Z'/>"

static const char *const g_svg[ICON_COUNT] = {
    [ICON_PLAY] = SVG(FILL("<path d='M8 5.6C8 4.8 8.9 4.3 9.6 4.8L19.2 11.2C19.8 11.6 19.8 12.4 19.2 "
                           "12.8L9.6 19.2C8.9 19.7 8 19.2 8 18.4Z'/>")),
    [ICON_PAUSE] = SVG(FILL("<rect x='6.5' y='5' width='3.8' height='14' rx='1.2'/>"
                            "<rect x='13.7' y='5' width='3.8' height='14' rx='1.2'/>")),
    [ICON_PREVIOUS] = SVG(FILL("<rect x='5' y='5.5' width='2.4' height='13' rx='1.1'/>"
                               "<path d='M19 6.6C19 5.8 18.1 5.3 17.4 5.8L9.6 11.2C9 11.6 9 12.4 9.6 "
                               "12.8L17.4 18.2C18.1 18.7 19 18.2 19 17.4Z'/>")),
    [ICON_NEXT] = SVG(FILL("<rect x='16.6' y='5.5' width='2.4' height='13' rx='1.1'/>"
                           "<path d='M5 6.6C5 5.8 5.9 5.3 6.6 5.8L14.4 11.2C15 11.6 15 12.4 14.4 "
                           "12.8L6.6 18.2C5.9 18.7 5 18.2 5 17.4Z'/>")),
    [ICON_SHUFFLE] = SVG(STROKE("<path d='M3.5 7.5H6.4C8 7.5 9 8.2 9.9 9.5L13.1 14.5C14 15.8 15 16.5 16.6 16.5H20'/>"
                                "<path d='M3.5 16.5H6.4C7.4 16.5 8.2 16.2 8.9 15.6'/>"
                                "<path d='M15.1 8.4C15.8 7.8 16.6 7.5 17.6 7.5H20'/>"
                                "<path d='M17.5 5L20 7.5L17.5 10'/><path d='M17.5 14L20 16.5L17.5 19'/>")),
    [ICON_REPEAT] = SVG(STROKE(REPEAT_LOOP)),
    [ICON_REPEAT_ONE] = SVG(STROKE(REPEAT_LOOP "<path d='M11 10.6L12.4 9.8V14.2'/>")),
    [ICON_HEART] = SVG(STROKE(HEART_PATH)),
    [ICON_HEART_FILLED] = SVG(FILL(HEART_PATH)),
    [ICON_VOLUME] = SVG(FILL(SPEAKER_BODY) STROKE("<path d='M15.5 9.3A3.8 3.8 0 0 1 15.5 14.7'/>"
                                                  "<path d='M18 6.8A7.4 7.4 0 0 1 18 17.2'/>")),
    [ICON_VOLUME_OFF] = SVG(FILL(SPEAKER_BODY) STROKE("<path d='M16 9.5L21 14.5M21 9.5L16 14.5'/>")),
    [ICON_SIDEBAR] = SVG(STROKE("<rect x='3.5' y='5' width='17' height='14' rx='3'/><path d='M9.5 5V19'/>")),
    [ICON_GEAR] = SVG(STROKE("<path d='M9.46 5.26L10.13 2.79H13.87L14.54 5.26L14.97 5.44L17.19 4.17L19.83 "
                             "6.81L18.56 9.03L18.74 9.46L21.21 10.13V13.87L18.74 14.54L18.56 14.97L19.83 "
                             "17.19L17.19 19.83L14.97 18.56L14.54 18.74L13.87 21.21H10.13L9.46 18.74L9.03 "
                             "18.56L6.81 19.83L4.17 17.19L5.44 14.97L5.26 14.54L2.79 13.87V10.13L5.26 "
                             "9.46L5.44 9.03L4.17 6.81L6.81 4.17L9.03 5.44Z'/>"
                             "<circle cx='12' cy='12' r='3'/>")),
    [ICON_LOG_OUT] = SVG(STROKE("<path d='M10 4.5H7A2.5 2.5 0 0 0 4.5 7V17A2.5 2.5 0 0 0 7 19.5H10'/>"
                                "<path d='M15 8L19 12L15 16'/><path d='M19 12H9.5'/>")),
    [ICON_LOG_IN] = SVG(STROKE("<path d='M14 4.5H17A2.5 2.5 0 0 1 19.5 7V17A2.5 2.5 0 0 1 17 19.5H14'/>"
                               "<path d='M10 8L14 12L10 16'/><path d='M14 12H4.5'/>")),
    [ICON_SEARCH] = SVG(STROKE("<circle cx='10.5' cy='10.5' r='6'/><path d='M15 15L20 20'/>")),
    [ICON_CHEVRON_DOWN] = SVG(STROKE("<path d='M6.5 9.5L12 15L17.5 9.5'/>")),
    [ICON_CHEVRON_RIGHT] = SVG(STROKE("<path d='M9.5 6.5L15 12L9.5 17.5'/>")),
    [ICON_BACK] = SVG(STROKE("<path d='M14.5 5.5L8 12L14.5 18.5'/>")),
    [ICON_PLUS] = SVG(STROKE("<path d='M12 5.5V18.5M5.5 12H18.5'/>")),
    [ICON_QUEUE] = SVG(STROKE("<path d='M4.5 7H15.5M4.5 12H15.5M4.5 17H11'/><path d='M17.5 13.5V20.5M14 17H21'/>")),
    [ICON_DEVICES] = SVG(STROKE("<rect x='2.5' y='4.5' width='13' height='10' rx='1.5'/>"
                                "<path d='M6 19H11M8.5 14.5V19'/>"
                                "<rect x='16.5' y='9' width='5' height='10.5' rx='1.2'/>")),
    [ICON_COMPUTER] = SVG(STROKE("<path d='M5 15.5V7A1.5 1.5 0 0 1 6.5 5.5H17.5A1.5 1.5 0 0 1 19 7V15.5'/>"
                                 "<path d='M2.5 18.5H21.5'/>")),
    [ICON_PHONE] = SVG(STROKE("<rect x='7' y='3' width='10' height='18' rx='2.5'/><path d='M11 17.8H13'/>")),
    [ICON_SPEAKER] = SVG(STROKE("<rect x='6' y='3' width='12' height='18' rx='3'/>"
                                "<circle cx='12' cy='14' r='3'/><path d='M12 7.4V7.6'/>")),
    [ICON_TV] = SVG(STROKE("<rect x='3' y='5' width='18' height='12' rx='2'/><path d='M8.5 20H15.5'/>")),
    [ICON_CLOSE] = SVG(STROKE("<path d='M7 7L17 17M17 7L7 17'/>")),
    [ICON_REFRESH] = SVG(STROKE("<path d='M16.13 6.10A7.2 7.2 0 1 1 7.87 6.10'/>"
                                "<path d='M4.27 6.10L7.87 6.10L6.64 9.48'/>")),
    [ICON_NOTE] = SVG(STROKE("<path d='M9.5 17.5V6.5L18.5 4.5V15.5'/>"
                             "<circle cx='7.5' cy='17.5' r='2'/><circle cx='16.5' cy='15.5' r='2'/>")),
};

static Rid g_texture[ICON_COUNT];
static int g_made[ICON_COUNT];

Rid icon_texture(Icon icon)
{
    if ((unsigned)icon >= ICON_COUNT)
        return R_INVALID_ID;
    /* Raven's textures go with Rshutdown: one made in an earlier session
     * of the library is not valid any more. */
    if (!g_made[icon] || !Rtexture_is_valid(g_texture[icon])) {
        /* scale 0: drawn at the size it is shown, again when that changes */
        g_texture[icon] = Rtexture_create_from_svg(g_svg[icon], strlen(g_svg[icon]), 0.0f);
        g_made[icon] = 1;
    }
    return g_texture[icon];
}

Rid icon_image(Rid parent, Icon icon, float size)
{
    Rid image = Rwidget_create(R_WIDGET_IMAGE);
    Rimage_set_stretch(image, R_STRETCH_FIT);
    Rimage_set_tint_text(image, R_TRUE);
    Rwidget_set_size(image, size, size);
    Rwidget_set_hit_testable(image, R_FALSE);
    Rwidget_set_accessible_hidden(image, R_TRUE);
    icon_set(image, icon);
    if (parent != R_INVALID_ID)
        Rwidget_add_child(parent, image);
    return image;
}

void icon_set(Rid image, Icon icon)
{
    Rimage_set_texture(image, icon_texture(icon));
}
