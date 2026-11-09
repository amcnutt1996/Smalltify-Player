/* library.h: the Library panel over the player area. */
#ifndef SMP_SCREENS_LIBRARY_H
#define SMP_SCREENS_LIBRARY_H

#include "../ui_state.h"

/* The Library: a stack of screens. Back or Esc goes up one; Esc at the root
 * closes the panel. */
void draw_library(Ui *u, GuiRect area);

#endif /* SMP_SCREENS_LIBRARY_H */
