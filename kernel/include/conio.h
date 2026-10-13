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

/* How many keystrokes are waiting for the terminal right now.
 *
 * The heartbeat reports it, because "the keyboard is not answering" and "the keyboard is
 * answering and nobody is reading" look identical from the outside -- the counters in the
 * heartbeat (`irq1`, `scancodes`, `events`) all keep climbing in both cases.  This is the
 * number that tells them apart, and it is the reason the pump has to run unconditionally:
 * a queue that grows and is never drained is the signature of a machine whose keyboard is
 * fine and whose shell is deaf. */
unsigned conio_tty_input_pending(void);

/* Drain the input queue.  Returns how many events were handled. */
unsigned conio_pump(void);

/* Print the ready-to-use banner and reset the edit line. */
void conio_banner(void);

#endif /* MINX_CONIO_H */