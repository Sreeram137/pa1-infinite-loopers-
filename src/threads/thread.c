#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "fixedpoint.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Constants */
#define THREAD_MAGIC_NUMBER 0xcd6abf4b
#define TIME_QUANTUM 4
#define LOAD_AVG_UPDATE_INTERVAL 100
#define PRIORITY_RECALC_INTERVAL 4

/* Global Data Structures */
static struct {
    struct list ready_threads;
    struct list all_threads;
    struct lock tid_alloc_lock;
    struct thread *idle;
    struct thread *main;
} thread_system;

/* Statistics */
static struct {
    long long idle_count;
    long long kernel_count;
    long long user_count;
} thread_stats;

/* Scheduling State */
static struct {
    unsigned ticks_since_yield;
    bool mlfq_enabled;
} scheduler_state;

/* Forward Declarations */
static void setup_idle_thread(void *);
static void execute_kernel_thread(thread_func *, void *);
static void prepare_new_thread_stack(struct thread *, thread_func *, void *);
static void update_mlfq_metrics(void);
static void recalculate_thread_priorities(void);

/* Thread Management Core */

void thread_bootstrap(void) {
    ASSERT(intr_get_level() == INTR_OFF);
    
    lock_init(&thread_system.tid_alloc_lock);
    list_init(&thread_system.ready_threads);
    list_init(&thread_system.all_threads);

    thread_system.main = get_executing_thread();
    initialize_thread_struct(thread_system.main, "main", PRI_DEFAULT);
    thread_system.main->state = THREAD_RUNNING;
    thread_system.main->tid = generate_new_tid();
}

void thread_begin(void) {
    struct semaphore sync_sem;
    sema_init(&sync_sem, 0);
    
    create_thread("idle", PRI_MIN, setup_idle_thread, &sync_sem);
    intr_enable();
    sema_down(&sync_sem);
}

/* Thread Lifecycle Operations */

tid_t create_thread(const char *name, int priority, thread_func *func, void *aux) {
    struct thread *new_thread;
    enum intr_level old_level;

    if (func == NULL) return TID_ERROR;

    new_thread = allocate_thread_memory();
    if (new_thread == NULL) return TID_ERROR;

    initialize_thread_struct(new_thread, name, priority);
    new_thread->tid = generate_new_tid();

    old_level = intr_disable();
    prepare_new_thread_stack(new_thread, func, aux);
    make_thread_ready(new_thread);

    if (new_thread->priority > get_current_thread()->priority) {
        relinquish_cpu();
    }
    intr_set_level(old_level);

    return new_thread->tid;
}

void terminate_thread(void) {
    ASSERT(!intr_context());

#ifdef USERPROG
    cleanup_process();
#endif

    intr_disable();
    remove_from_all_threads(get_current_thread());
    get_current_thread()->state = THREAD_DYING;
    schedule_next_thread();
    NOT_REACHED();
}

/* Scheduler Operations */

void timer_interrupt_handler(void) {
    struct thread *current = get_current_thread();
    
    update_thread_stats(current);
    
    if (scheduler_state.mlfq_enabled) {
        handle_mlfq_scheduling(current);
    }

    if (++scheduler_state.ticks_since_yield >= TIME_QUANTUM) {
        trigger_context_switch();
    }
}

static void handle_mlfq_scheduling(struct thread *current) {
    if (strcmp(current->name, "idle") != 0) {
        current->cpu_usage = fixed_add(current->cpu_usage, 1);
    }

    if (timer_ticks() % LOAD_AVG_UPDATE_INTERVAL == 0) {
        update_mlfq_metrics();
    }

    if (timer_ticks() % PRIORITY_RECALC_INTERVAL == 0) {
        recalculate_thread_priorities();
    }
}

/* Thread State Management */

void block_current_thread(void) {
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);

    get_current_thread()->state = THREAD_BLOCKED;
    schedule_next_thread();
}

void unblock_thread(struct thread *t) {
    enum intr_level old_level = intr_disable();
    
    ASSERT(is_valid_thread(t));
    ASSERT(t->state == THREAD_BLOCKED);
    
    add_to_ready_queue(t);
    t->state = THREAD_READY;
    
    intr_set_level(old_level);
}

void yield_cpu(void) {
    struct thread *current = get_current_thread();
    enum intr_level old_level;
    
    ASSERT(!intr_context());
    old_level = intr_disable();
    
    if (current != thread_system.idle) {
        add_to_ready_queue(current);
    }
    current->state = THREAD_READY;
    schedule_next_thread();
    intr_set_level(old_level);
}

/* Priority Management */

void adjust_thread_priority(int new_priority) {
    enum intr_level old_level = intr_disable();
    struct thread *current = get_current_thread();

    if (list_empty(&current->priority_donors)) {
        current->priority = new_priority;
        current->base_priority = new_priority;
    }
    else if (new_priority > current->priority) {
        current->priority = new_priority;
        current->base_priority = new_priority;
    }
    else {
        current->base_priority = new_priority;
    }

    if (!list_empty(&thread_system.ready_threads)) {
        struct thread *highest_ready = get_highest_priority_ready_thread();
        if (highest_ready->priority > current->priority) {
            relinquish_cpu();
        }
    }
    intr_set_level(old_level);
}

/* Helper Functions */

static void initialize_thread_struct(struct thread *t, const char *name, int priority) {
    ASSERT(t != NULL && name != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);

    memset(t, 0, sizeof *t);
    t->state = THREAD_BLOCKED;
    strlcpy(t->name, name, sizeof t->name);
    t->stack = (uint8_t *)t + PGSIZE;

    if (scheduler_state.mlfq_enabled) {
        t->cpu_usage = (strcmp(t->name, "main") == 0) ? 0 : 
                       fixed_div(get_current_cpu_usage(), 100);
        priority = PRI_MAX - fixed_round(fixed_div(t->cpu_usage, 4)) - (t->nice * 2);
    }
    
    t->magic = THREAD_MAGIC_NUMBER;
    t->priority = priority;
    t->base_priority = priority;
    list_init(&t->priority_donors);
    add_to_all_threads(t);
}

static void prepare_new_thread_stack(struct thread *t, thread_func *func, void *aux) {
    struct {
        void *ret_addr;
        thread_func *start_func;
        void *arg;
    } *kernel_frame = allocate_stack_frame(t, sizeof(*kernel_frame));
    
    kernel_frame->ret_addr = NULL;
    kernel_frame->start_func = func;
    kernel_frame->arg = aux;

    struct {
        void (*entry_point)(void);
    } *switch_frame = allocate_stack_frame(t, sizeof(*switch_frame));
    
    switch_frame->entry_point = execute_kernel_thread;
}

/* Idle Thread Implementation */

static void setup_idle_thread(void *sync_sem) {
    thread_system.idle = get_current_thread();
    sema_up(sync_sem);

    while (true) {
        intr_disable();
        block_current_thread();
        asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Comparison Functions */

bool compare_wakeup_times(struct list_elem *a, struct list_elem *b, void *aux) {
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    return ta->wake_time < tb->wake_time;
}

bool compare_thread_priorities(struct list_elem *a, struct list_elem *b, void *aux) {
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    return ta->priority > tb->priority;
}
