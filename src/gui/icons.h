/*
 * icons.h: the icons of the application, SVG compiled into the binary
 * (icons.c). White on transparent, 24 x 24 units, drawn by Raven at the
 * size they are shown, and tinted with the text color around them: an icon
 * in a button with the class "on" or "current" is in the accent.
 */
#ifndef SMP_GUI_ICONS_H
#define SMP_GUI_ICONS_H

#include <Raven.h>

typedef enum {
    ICON_PLAY,
    ICON_PAUSE,
    ICON_PREVIOUS,
    ICON_NEXT,
    ICON_SHUFFLE,
    ICON_REPEAT,
    ICON_REPEAT_ONE,
    ICON_HEART,
    ICON_HEART_FILLED,
    ICON_VOLUME,       /* speaker with waves */
    ICON_VOLUME_OFF,   /* speaker, crossed out */
    ICON_SIDEBAR,      /* show / hide the sidebar */
    ICON_GEAR,         /* settings */
    ICON_LOG_OUT,
    ICON_LOG_IN,
    ICON_SEARCH,
    ICON_CHEVRON_DOWN,
    ICON_CHEVRON_RIGHT,
    ICON_BACK,         /* chevron to the left */
    ICON_PLUS,
    ICON_QUEUE,        /* add to the queue */
    ICON_DEVICES,      /* the Devices tab */
    ICON_COMPUTER,
    ICON_PHONE,
    ICON_SPEAKER,      /* a speaker device */
    ICON_TV,           /* a TV, a console, any other device */
    ICON_CLOSE,
    ICON_REFRESH,
    ICON_NOTE,         /* music: the place of missing art */
    ICON_COUNT
} Icon;

/* The texture of an icon, made on first use and shared. R_INVALID_ID when
 * Raven cannot draw SVG. */
Rid icon_texture(Icon icon);

/* An Image showing the icon, size x size units, tinted with the text color
 * around it; it lets clicks through to what holds it. Added to parent
 * unless parent is R_INVALID_ID. */
Rid icon_image(Rid parent, Icon icon, float size);

/* Shows another icon in an Image made by icon_image. */
void icon_set(Rid image, Icon icon);

#endif /* SMP_GUI_ICONS_H */
