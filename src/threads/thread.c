#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <kernel/list.h>

/* Hardware specifications for the 8254 timer */
#if TIMER_FREQ < 19
#error Minimum timer frequency is 19Hz
#endif
#if TIMER_FREQ > 1000
#error Maximum recommended timer frequency is 1000Hz
#endif

/* System tick counter since initialization */
static int64_t system_ticks;

/* List for suspended threads waiting on timeouts */
struct list suspended_threads_queue;

/* Calibration value for precise delays */
static unsigned calibration_loops;

/* Function prototypes */
static intr_handler_func timer_isr;
static bool is_loop_excessive(unsigned iterations);
static void perform_busy_wait(int64_t cycles);
static void precise_sleep(int64_t numerator, int32_t denominator);
static void precise_delay(int64_t numerator, int32_t denominator);

/* Initialize timer hardware and interrupt handler */
void timer_initialize(void) 
{
    pit_configure_channel(0, 2, TIMER_FREQ);
    intr_register_ext(0x20, timer_isr, "8254 Timer");
    list_init(&suspended_threads_queue);
}

/* Calibrate the delay loop counter */
void timer_calibrate_delay(void) 
{
    unsigned high_mark, test_mark;

    ASSERT(intr_get_level() == INTR_ON);
    printf("Calibrating delay loop... ");

    /* Find approximate loops per tick */
    calibration_loops = 1u << 10;
    while (!is_loop_excessive(calibration_loops << 1)) 
    {
        calibration_loops <<= 1;
        ASSERT(calibration_loops != 0);
    }

    /* Fine-tune the lower bits */
    high_mark = calibration_loops;
    for (test_mark = high_mark >> 1; test_mark != high_mark >> 10; test_mark >>= 1)
        if (!is_loop_excessive(high_mark | test_mark))
            calibration_loops |= test_mark;

    printf("%'"PRIu64" cycles/second\n", (uint64_t)calibration_loops * TIMER_FREQ);
}

/* Get current system tick count */
int64_t get_system_ticks(void) 
{
    enum intr_level prev_state = intr_disable();
    int64_t current = system_ticks;
    intr_set_level(prev_state);
    return current;
}

/* Calculate ticks elapsed since reference point */
int64_t ticks_since(int64_t reference) 
{
    return get_system_ticks() - reference;
}

/* Suspend execution for specified ticks */
void timer_wait_ticks(int64_t wait_ticks) 
{
    struct thread* current;
    enum intr_level prev_state;

    ASSERT(intr_get_level() == INTR_ON);

    prev_state = intr_disable();
    current = thread_current();
    current->resume_tick = get_system_ticks() + wait_ticks;
    list_insert_ordered(&suspended_threads_queue, &current->elem, compare_wakeup_time, NULL);
    thread_block();
    intr_set_level(prev_state);
}

/* Millisecond sleep wrapper */
void timer_wait_ms(int64_t milliseconds) 
{
    precise_sleep(milliseconds, 1000);
}

/* Microsecond sleep wrapper */
void timer_wait_us(int64_t microseconds) 
{
    precise_sleep(microseconds, 1000 * 1000);
}

/* Nanosecond sleep wrapper */
void timer_wait_ns(int64_t nanoseconds) 
{
    precise_sleep(nanoseconds, 1000 * 1000 * 1000);
}

/* Busy-wait millisecond delay */
void timer_busy_wait_ms(int64_t milliseconds) 
{
    precise_delay(milliseconds, 1000);
}

/* Busy-wait microsecond delay */
void timer_busy_wait_us(int64_t microseconds) 
{
    precise_delay(microseconds, 1000 * 1000);
}

/* Busy-wait nanosecond delay */
void timer_busy_wait_ns(int64_t nanoseconds) 
{
    precise_delay(nanoseconds, 1000 * 1000 * 1000);
}

/* Display timer statistics */
void show_timer_stats(void) 
{
    printf("Timer ticks: %"PRId64"\n", get_system_ticks());
}

/* Timer interrupt service routine */
static void timer_isr(struct intr_frame *context UNUSED)
{
    struct list_elem *first;
    struct thread *thread;

    system_ticks++;
    thread_tick_update();

    while(!list_empty(&suspended_threads_queue))
    {
        first = list_front(&suspended_threads_queue);
        thread = list_entry(first, struct thread, elem);

        if(thread->resume_tick > system_ticks)
            break;

        list_remove(first);
        thread_resume(thread);
    }
}

/* Check if loop count exceeds one tick */
static bool is_loop_excessive(unsigned iterations) 
{
    int64_t start = system_ticks;
    while (system_ticks == start)
        barrier();

    start = system_ticks;
    perform_busy_wait(iterations);

    barrier();
    return start != system_ticks;
}

/* Tight loop for precise delays */
static void NO_INLINE perform_busy_wait(int64_t cycles) 
{
    while (cycles-- > 0)
        barrier();
}

/* High precision sleep implementation */
static void precise_sleep(int64_t num, int32_t denom) 
{
    int64_t tick_count = num * TIMER_FREQ / denom;

    ASSERT(intr_get_level() == INTR_ON);
    if (tick_count > 0)
    {
        timer_wait_ticks(tick_count);
    }
    else 
    {
        precise_delay(num, denom);
    }
}

/* High precision busy-wait */
static void precise_delay(int64_t num, int32_t denom)
{
    ASSERT(denom % 1000 == 0);
    perform_busy_wait(calibration_loops * num / 1000 * TIMER_FREQ / (denom / 1000));
}
