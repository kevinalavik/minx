#include "input.h"

#include "cpu.h"
#include "string.h"

/* A lock-free single-producer/single-consumer ring.
 *
 * The producer is the interrupt handler (the keyboard today, a mouse later) and
 * the consumer is the console drain loop, so a spinlock would have to be taken
 * with interrupts disabled on one side and would add a scheduler interaction for
 * no benefit.  Sequence counters plus a compiler barrier are enough: the producer
 * only ever advances head, the consumer only ever advances tail, and the
 * difference of the two is the occupancy. */
static input_event_t queue[INPUT_QUEUE_SIZE];
static volatile uint32_t head_seq;   /* next index the producer will fill */
static volatile uint32_t tail_seq;   /* next index the consumer will take  */
static volatile uint32_t modifiers;
static volatile uint32_t dropped;

#define QUEUE_MASK (INPUT_QUEUE_SIZE - 1)

_Static_assert((INPUT_QUEUE_SIZE & (INPUT_QUEUE_SIZE - 1)) == 0,
               "INPUT_QUEUE_SIZE must be a power of two");

void input_init(void) {
    memset(queue, 0, sizeof(queue));
    head_seq = 0;
    tail_seq = 0;
    modifiers = 0;
    dropped = 0;
}

bool input_push(const input_event_t *event) {
    uint32_t head = head_seq;

    if ((head - tail_seq) >= INPUT_QUEUE_SIZE) {
        dropped++;
        return false;
    }

    queue[head & QUEUE_MASK] = *event;
    /* Publishing the slot before the index is what makes the event visible. */
    __atomic_signal_fence(__ATOMIC_RELEASE);
    head_seq = head + 1;

    if (event->type == INPUT_KEY_DOWN)
        modifiers = event->modifiers;

    return true;
}

bool input_pop(input_event_t *event) {
    uint32_t tail = tail_seq;

    __atomic_signal_fence(__ATOMIC_ACQUIRE);
    if (tail == head_seq)
        return false;

    *event = queue[tail & QUEUE_MASK];
    tail_seq = tail + 1;
    return true;
}

bool input_empty(void) {
    return tail_seq == head_seq;
}

size_t input_pending(void) {
    return head_seq - tail_seq;
}

uint32_t input_modifiers(void) {
    return modifiers;
}