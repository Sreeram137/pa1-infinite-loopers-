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

/* A unique identifier for the thread structure to detect stack overflow.
   Refer to the detailed explanation in thread.h for more information. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of threads in the THREAD_READY state, i.e., threads that are prepared to run but not currently executing. */
static struct list ready_list;

/* List of all threads. Threads are added to this list when scheduled and removed upon exit. */
static struct list all_list;

/* Pointer to the idle thread. */
static struct thread *idle_thread;

/* Pointer to the initial thread, which runs the main function in init.c. */
static struct thread *initial_thread;

/* Lock for managing thread ID allocation. */
static struct lock tid_lock;

/* Stack frame structure for kernel_thread(). */
struct kernel_thread_frame 
{
    void *eip;                  /* Return address for the function. */
    thread_func *function;      /* Function to be executed. */
    void *aux;                  /* Additional data for the function. */
};

/* System statistics. */
static long long idle_ticks;    /* Time spent in the idle thread. */
static long long kernel_ticks;  /* Time spent in kernel threads. */
static long long user_ticks;    /* Time spent in user programs. */

/* Scheduling parameters. */
#define TIME_SLICE 4            /* Time allocated to each thread before yielding. */
static unsigned thread_ticks;   /* Timer ticks since the last yield. */

/* Flag to control the scheduler type. If false, round-robin is used; if true, MLFQS is used. */
bool thread_mlfqs;

/* Function prototypes. */
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by converting the current execution context into a thread.
   This is only possible because loader.S ensures the stack starts at a page boundary.
   Also initializes the ready queue and the thread ID lock.
   Note: thread_current() should not be called until this function completes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up the thread structure for the currently running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Enables preemptive scheduling by turning on interrupts and creates the idle thread. */
void
thread_start (void) 
{
  /* Initialize the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Enable interrupts to start preemptive scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each tick. Runs in an interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update system statistics based on the current thread. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption if the time slice is exhausted. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints system statistics related to thread execution. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new thread with the given name, priority, and function, and adds it to the ready queue.
   Returns the thread ID or TID_ERROR if creation fails. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate memory for the new thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize the thread structure. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Set up the stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Set up the stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Set up the stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add the thread to the ready queue and yield the CPU. */
  thread_unblock (t);
  thread_yield();
  return tid;
}

/* Blocks the current thread, preventing it from being scheduled until unblocked. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Unblocks a thread, allowing it to be scheduled again. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, compare_priority, 0);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the currently running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the currently running thread after performing sanity checks. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Ensure the thread is valid and not experiencing stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the thread ID of the currently running thread. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Terminates the current thread and schedules another thread to run. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove the thread from the all_list, mark it as dying, and schedule another thread. */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU, allowing another thread to run. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered (&ready_list, &cur->elem, compare_priority, 0);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Applies a function to all threads in the system. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the priority of the current thread. */
void
thread_set_priority (int new_priority) 
{
  thread_current ()->priority = new_priority;
  thread_yield();
}

/* Returns the priority of the current thread. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the nice value of the current thread. */
void
thread_set_nice (int nice UNUSED) 
{
  /* To be implemented in future updates. */
}

/* Returns the nice value of the current thread. */
int
thread_get_nice (void) 
{
  /* To be implemented in future updates. */
  return 0;
}

/* Returns the system load average multiplied by 100. */
int
thread_get_load_avg (void) 
{
  /* To be implemented in future updates. */
  return 0;
}

/* Returns the recent CPU usage of the current thread multiplied by 100. */
int
thread_get_recent_cpu (void) 
{
  /* To be implemented in future updates. */
  return 0;
}

/* The idle thread runs when no other threads are ready. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Allow other threads to run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one. */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* The base function for kernel threads. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* Enable interrupts for scheduling. */
  function (aux);       /* Execute the thread's function. */
  thread_exit ();       /* Terminate the thread if the function returns. */
}

/* Returns the currently running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Calculate the thread's address based on the stack pointer. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Checks if a thread structure is valid. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Initializes a thread structure with the given name and priority. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a frame on the thread's stack. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Selects the next thread to run from the ready queue. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page tables and cleaning up the previous thread. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark the current thread as running. */
  cur->status = THREAD_RUNNING;

  /* Reset the timer ticks for the new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new thread's address space. */
  process_activate ();
#endif

  /* Clean up the previous thread if it is dying. */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new thread to run. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Allocates a unique thread ID. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of the stack member within the thread structure. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

/* Compares the priority of two threads for ordered insertion into the ready list. */
bool compare_priority(struct list_elem *l1, struct list_elem *l2,void *aux)
{
  struct thread *t1 = list_entry(l1,struct thread,elem);
  struct thread *t2 = list_entry(l2,struct thread,elem);
  if( t1->priority > t2->priority)
    return true;
  return false;
}
