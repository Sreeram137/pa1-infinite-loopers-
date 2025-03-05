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
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow. */
#define THREAD_MAGIC 0xab7f3c9d

/* List of threads in THREAD_READY state. */
static struct list ready_threads;

/* List of all threads. */
static struct list all_threads;

/* Idle thread. */
static struct thread *idle_task;

/* Initial thread, running init.c:main(). */
static struct thread *main_thread;

/* Lock for thread ID allocation. */
static struct lock tid_mutex;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
    void *ret_addr;             /* Return address. */
    thread_func *func;          /* Function to call. */
    void *args;                 /* Function arguments. */
};

/* Timer tick counters. */
static long long idle_time;      /* Time spent idle. */
static long long kernel_time;    /* Time in kernel threads. */
static long long user_time;      /* Time in user programs. */

/* Scheduling. */
#define TIME_QUANTUM 4           /* Timer ticks per thread. */
static unsigned tick_count;      /* Ticks since last yield. */

/* If false, use round-robin scheduler.
   If true, use multi-level feedback queue scheduler. */
bool use_mlfqs;

static void kernel_thread(thread_func *, void *args);

static void idle_task_func(void *args UNUSED);
static struct thread *current_thread(void);
static struct thread *next_ready_thread(void);
static void setup_thread(struct thread *, const char *name, int priority);
static bool valid_thread(struct thread *) UNUSED;
static void *allocate_stack_frame(struct thread *, size_t size);
static void schedule_thread(void);
void thread_schedule_finalize(struct thread *prev);
static tid_t generate_tid(void);

/* Initializes the threading system. */
void thread_init(void) {
    ASSERT(intr_get_level() == INTR_OFF);

    lock_init(&tid_mutex);
    list_init(&ready_threads);
    list_init(&all_threads);

    /* Set up the main thread. */
    main_thread = current_thread();
    setup_thread(main_thread, "main", PRI_DEFAULT);
    main_thread->status = THREAD_RUNNING;
    main_thread->tid = generate_tid();
}

/* Starts preemptive thread scheduling. */
void thread_start(void) {
    /* Create the idle thread. */
    struct semaphore idle_sema;
    sema_init(&idle_sema, 0);
    thread_create("idle", PRI_MIN, idle_task_func, &idle_sema);

    /* Enable interrupts for scheduling. */
    intr_enable();

    /* Wait for idle thread initialization. */
    sema_down(&idle_sema);
}

/* Called by the timer interrupt handler at each tick. */
void thread_tick(void) {
    struct thread *t = current_thread();

    /* Update tick counters. */
    if (t == idle_task)
        idle_time++;
#ifdef USERPROG
    else if (t->pagedir != NULL)
        user_time++;
#endif
    else
        kernel_time++;

    /* Enforce preemption. */
    if (++tick_count >= TIME_QUANTUM)
        intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
    printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
           idle_time, kernel_time, user_time);
}

/* Creates a new kernel thread. */
tid_t thread_create(const char *name, int priority, thread_func *func, void *args) {
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;

    ASSERT(func != NULL);

    /* Allocate thread. */
    t = palloc_get_page(PAL_ZERO);
    if (t == NULL)
        return TID_ERROR;

    /* Initialize thread. */
    setup_thread(t, name, priority);
    tid = t->tid = generate_tid();

    /* Stack frame for kernel_thread(). */
    kf = allocate_stack_frame(t, sizeof *kf);
    kf->ret_addr = NULL;
    kf->func = func;
    kf->args = args;

    /* Stack frame for switch_entry(). */
    ef = allocate_stack_frame(t, sizeof *ef);
    ef->ret_addr = (void (*)(void)) kernel_thread;

    /* Stack frame for switch_threads(). */
    sf = allocate_stack_frame(t, sizeof *sf);
    sf->ret_addr = switch_entry;
    sf->ebp = 0;

    /* Add to ready queue. */
    thread_unblock(t);
    thread_yield();
    return tid;
}

/* Blocks the current thread. */
void thread_block(void) {
    ASSERT(!intr_context());
    ASSERT(intr_get_level() == INTR_OFF);

    current_thread()->status = THREAD_BLOCKED;
    schedule_thread();
}

/* Unblocks a thread. */
void thread_unblock(struct thread *t) {
    enum intr_level old_level;

    ASSERT(valid_thread(t));

    old_level = intr_disable();
    ASSERT(t->status == THREAD_BLOCKED);
    list_insert_ordered(&ready_threads, &t->elem, compare_priority, 0);
    t->status = THREAD_READY;
    intr_set_level(old_level);
}

/* Returns the name of the current thread. */
const char *thread_name(void) {
    return current_thread()->name;
}

/* Returns the current thread. */
struct thread *thread_current(void) {
    struct thread *t = current_thread();
    ASSERT(valid_thread(t));
    ASSERT(t->status == THREAD_RUNNING);
    return t;
}

/* Returns the current thread's tid. */
tid_t thread_tid(void) {
    return current_thread()->tid;
}

/* Exits the current thread. */
void thread_exit(void) {
    ASSERT(!intr_context());

#ifdef USERPROG
    process_exit();
#endif

    intr_disable();
    list_remove(&current_thread()->allelem);
    current_thread()->status = THREAD_DYING;
    schedule_thread();
    NOT_REACHED();
}

/* Yields the CPU. */
void thread_yield(void) {
    struct thread *cur = current_thread();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();
    if (cur != idle_task)
        list_insert_ordered(&ready_threads, &cur->elem, compare_priority, 0);
    cur->status = THREAD_READY;
    schedule_thread();
    intr_set_level(old_level);
}

/* Applies a function to all threads. */
void thread_foreach(thread_action_func *func, void *aux) {
    struct list_elem *e;

    ASSERT(intr_get_level() == INTR_OFF);

    for (e = list_begin(&all_threads); e != list_end(&all_threads); e = list_next(e)) {
        struct thread *t = list_entry(e, struct thread, allelem);
        func(t, aux);
    }
}

/* Sets the current thread's priority. */
void thread_set_priority(int new_priority) {
    current_thread()->priority = new_priority;
    thread_yield();
}

/* Returns the current thread's priority. */
int thread_get_priority(void) {
    return current_thread()->priority;
}

/* Sets the current thread's nice value. */
void thread_set_nice(int nice UNUSED) {
    /* Not implemented. */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
    /* Not implemented. */
    return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
    /* Not implemented. */
    return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
    /* Not implemented. */
    return 0;
}

/* Idle thread function. */
static void idle_task_func(void *idle_started_ UNUSED) {
    struct semaphore *idle_started = idle_started_;
    idle_task = current_thread();
    sema_up(idle_started);

    for (;;) {
        intr_disable();
        thread_block();
        asm volatile("sti; hlt" : : : "memory");
    }
}

/* Kernel thread function. */
static void kernel_thread(thread_func *func, void *args) {
    ASSERT(func != NULL);
    intr_enable();
    func(args);
    thread_exit();
}

/* Returns the current thread. */
struct thread *current_thread(void) {
    uint32_t *esp;
    asm("mov %%esp, %0" : "=g"(esp));
    return pg_round_down(esp);
}

/* Validates a thread. */
static bool valid_thread(struct thread *t) {
    return t != NULL && t->magic == THREAD_MAGIC;
}

/* Initializes a thread. */
static void setup_thread(struct thread *t, const char *name, int priority) {
    enum intr_level old_level;

    ASSERT(t != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT(name != NULL);

    memset(t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;
    strlcpy(t->name, name, sizeof t->name);
    t->stack = (uint8_t *) t + PGSIZE;
    t->priority = priority;
    t->magic = THREAD_MAGIC;

    old_level = intr_disable();
    list_push_back(&all_threads, &t->allelem);
    intr_set_level(old_level);
}

/* Allocates a stack frame. */
static void *allocate_stack_frame(struct thread *t, size_t size) {
    ASSERT(valid_thread(t));
    ASSERT(size % sizeof(uint32_t) == 0);

    t->stack -= size;
    return t->stack;
}

/* Chooses the next thread to run. */
static struct thread *next_ready_thread(void) {
    if (list_empty(&ready_threads))
        return idle_task;
    else
        return list_entry(list_pop_front(&ready_threads), struct thread, elem);
}

/* Finalizes a thread switch. */
void thread_schedule_finalize(struct thread *prev) {
    struct thread *cur = current_thread();
    ASSERT(intr_get_level() == INTR_OFF);

    cur->status = THREAD_RUNNING;
    tick_count = 0;

#ifdef USERPROG
    process_activate();
#endif

    if (prev != NULL && prev->status == THREAD_DYING && prev != main_thread) {
        ASSERT(prev != cur);
        palloc_free_page(prev);
    }
}

/* Schedules a new thread. */
static void schedule_thread(void) {
    struct thread *cur = current_thread();
    struct thread *next = next_ready_thread();
    struct thread *prev = NULL;

    ASSERT(intr_get_level() == INTR_OFF);
    ASSERT(cur->status != THREAD_RUNNING);
    ASSERT(valid_thread(next));

    if (cur != next)
        prev = switch_threads(cur, next);
    thread_schedule_finalize(prev);
}

/* Generates a new thread ID. */
static tid_t generate_tid(void) {
    static tid_t next_tid = 1;
    tid_t tid;

    lock_acquire(&tid_mutex);
    tid = next_tid++;
    lock_release(&tid_mutex);

    return tid;
}

/* Offset of `stack' member within `struct thread'. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

/* Compares thread priorities. */
bool compare_priority(struct list_elem *l1, struct list_elem *l2, void *aux) {
    struct thread *t1 = list_entry(l1, struct thread, elem);
    struct thread *t2 = list_entry(l2, struct thread, elem);
    return t1->priority > t2->priority;
}
