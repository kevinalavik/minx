/* minx - the interactive console loop.
 *
 * Until the TTY line discipline exists (phase 9) the boot CPU drains the input
 * queue itself and echoes what arrives, which is what makes "type into the
 * graphical console" true from phase 2 onwards.  Later this moves behind the
 * TTY without changing any of the behaviour below.
 */
#ifndef MINX_CONIO_H
#define MINX_CONIO_H

#include "input.h"

/* Handle one event.  Returns false when the caller should stop pumping (the
 * user pressed ^N to drop back to whatever ran the boot sequence). */
bool conio_handle_event(const input_event_t *ev);

/* Drain the input queue.  Returns how many events were handled. */
unsigned conio_pump(void);

/* Print the ready-to-use banner and reset the edit line. */
void conio_banner(void);

#endif /* MINX_CONIO_H */