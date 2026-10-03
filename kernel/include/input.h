/* minx - input events.
 *
 * A single bounded queue between whatever hardware produces input and whatever
 * consumes it.  The console drains it in phase 2, the TTY line discipline in
 * phase 9, and later a mouse driver feeds the same queue, so the producers never
 * have to know who is reading.
 */
#ifndef MINX_INPUT_H
#define MINX_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_QUEUE_SIZE 256

/* Keys that have no printable form are given code points in a private plane, so
 * they can travel through the same field as ordinary Unicode. */
#define KEY_UNICODE_BASE 0x00110000u

enum {
    KEY_NONE = KEY_UNICODE_BASE,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ENTER,
    KEY_ESCAPE,
    KEY_INSERT,
    KEY_DELETE,
    KEY_HOME,
    KEY_END,
    KEY_PAGEUP,
    KEY_PAGEDOWN,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_F1,  KEY_F2,  KEY_F3,  KEY_F4,  KEY_F5,  KEY_F6,
    KEY_F7,  KEY_F8,  KEY_F9,  KEY_F10, KEY_F11, KEY_F12,
    KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_LEFTMETA,
    KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA,
    KEY_CAPSLOCK, KEY_NUMLOCK, KEY_SCROLLLOCK,
    KEY_DIVIDE,          /* keypad /  */
    KEY_MAX
};

#define MOD_SHIFT (1u << 0)
#define MOD_CTRL  (1u << 1)
#define MOD_ALT   (1u << 2)
#define MOD_META  (1u << 3)
#define MOD_CAPS  (1u << 4)
#define MOD_NUM   (1u << 5)

typedef enum {
    INPUT_KEY_DOWN,
    INPUT_KEY_UP,
    INPUT_MOUSE_MOTION,
    INPUT_MOUSE_BUTTON,
} input_type_t;

typedef struct {
    input_type_t type;
    uint32_t     key;        /* Unicode code point, or one of the KEY_* above */
    uint32_t     modifiers;  /* MOD_* bitmap at the time of the event */
    int32_t      x, y;       /* mouse position, or button coordinates */
    uint32_t     buttons;    /* mouse button bitmask */
} input_event_t;

void input_init(void);

/* Returns false when the queue is full; the event is dropped rather than
 * blocking an interrupt handler. */
bool input_push(const input_event_t *event);

/* Returns false when the queue is empty. */
bool input_pop(input_event_t *event);

bool input_empty(void);
size_t input_pending(void);

/* Most recent modifier state, for the consumer's convenience. */
uint32_t input_modifiers(void);

#endif /* MINX_INPUT_H */