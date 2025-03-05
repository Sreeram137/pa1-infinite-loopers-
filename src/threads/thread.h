/* Updated thread_unblock to insert threads based on priority. */
void thread_unblock (struct thread *t) {
  enum intr_level old_level;
  ASSERT (is_thread (t));
  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem, thread_priority_compare, NULL);
  t->status = THREAD_READY;
  check_preemption();
  intr_set_level (old_level);
}

/* Checks for preemption based on priority. */
void check_preemption (void) {
  if (!list_empty(&ready_list)) {
    struct thread *highest = list_entry(list_front(&ready_list), struct thread, elem);
    if (highest->priority > thread_current()->priority) {
      thread_yield();
    }
  }
}

/* Updated thread_set_priority to handle preemption and reordering. */
void thread_set_priority (int new_priority) {
  enum intr_level old_level = intr_disable();
  thread_current()->priority = new_priority;
  list_remove(&thread_current()->elem);
  list_insert_ordered(&ready_list, &thread_current()->elem, thread_priority_compare, NULL);
  check_preemption();
  intr_set_level(old_level);
}
