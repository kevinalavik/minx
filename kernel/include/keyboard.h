/* minx - PS/2 keyboard.
 *
 * Drives the 8042 controller on IRQ1, decodes scancode set 2 (what the
 * controller emits) into Unicode plus modifier state, and pushes key events into
 * the input queue.
 */
#ifndef MINX_KEYBOARD_H
#define MINX_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Returns 0 on success, negative when no keyboard is present. */
int keyboard_init(void);
bool keyboard_present(void);

/* True once the controller has reported a keyboard that passed its self test. */
bool keyboard_ready(void);

/* Diagnostic counters, used by the self test to prove the IRQ path works. */
uint64_t keyboard_scancodes(void);
uint64_t keyboard_events(void);
uint32_t keyboard_overruns(void);
uint8_t  keyboard_saved_byte(void);
bool     keyboard_numlock(void);
bool     keyboard_capslock(void);

/* True when the keyboard emits scancode set 2 and the 8042 is translating it
 * into set 1 for us, rather than the keyboard speaking set 1 natively. */
bool     keyboard_uses_8042_translation(void);

/* Raw controller access, for the PS/2 mouse later. */
uint8_t ps2_read_data(void);
bool    ps2_write_command(uint8_t cmd);
bool    ps2_write_data(uint8_t value);

#endif /* MINX_KEYBOARD_H */