/* Thread management routine
 * Copyright (C) 1998, 2000 Kunihiro Ishiguro <kunihiro@zebra.org>
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* #define DEBUG */

#include <zebra.h>
#include <sys/resource.h>

#include "thread.h"
#include "memory.h"
#include "log.h"
#include "hash.h"
#include "pqueue.h"
#include "command.h"
#include "sigevent.h"
#include "network.h"

DEFINE_MTYPE_STATIC(LIB, THREAD,        "Thread")
DEFINE_MTYPE_STATIC(LIB, THREAD_MASTER, "Thread master")
DEFINE_MTYPE_STATIC(LIB, THREAD_STATS,  "Thread stats")

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#define AWAKEN(m) \
  do { \
      static unsigned char wakebyte = 0x01; \
      write (m->io_pipe[1], &wakebyte, 1); \
  } while (0);

static pthread_mutex_t cpu_record_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct hash *cpu_record = NULL;

static unsigned long
timeval_elapsed (struct timeval a, struct timeval b)
{
  return (((a.tv_sec - b.tv_sec) * TIMER_SECOND_MICRO)
	  + (a.tv_usec - b.tv_usec));
}

static unsigned int
cpu_record_hash_key (struct cpu_thread_history *a)
{
  return (uintptr_t) a->func;
}

static int 
cpu_record_hash_cmp (const struct cpu_thread_history *a,
		     const struct cpu_thread_history *b)
{
  return a->func == b->func;
}

static void *
cpu_record_hash_alloc (struct cpu_thread_history *a)
{
  struct cpu_thread_history *new;
  new = XCALLOC (MTYPE_THREAD_STATS, sizeof (struct cpu_thread_history));
  new->func = a->func;
  new->funcname = a->funcname;
  return new;
}

static void
cpu_record_hash_free (void *a)
{
  struct cpu_thread_history *hist = a;
 
  XFREE (MTYPE_THREAD_STATS, hist);
}

static void 
vty_out_cpu_thread_history(struct vty* vty,
			   struct cpu_thread_history *a)
{
  vty_out(vty, "%5d %10ld.%03ld %9d %8ld %9ld %8ld %9ld",
	  a->total_active, a->cpu.total/1000, a->cpu.total%1000, a->total_calls,
	  a->cpu.total/a->total_calls, a->cpu.max,
	  a->real.total/a->total_calls, a->real.max);
  vty_out(vty, " %c%c%c%c%c%c %s%s",
	  a->types & (1 << THREAD_READ) ? 'R':' ',
	  a->types & (1 << THREAD_WRITE) ? 'W':' ',
	  a->types & (1 << THREAD_TIMER) ? 'T':' ',
	  a->types & (1 << THREAD_EVENT) ? 'E':' ',
	  a->types & (1 << THREAD_EXECUTE) ? 'X':' ',
	  a->types & (1 << THREAD_BACKGROUND) ? 'B' : ' ',
	  a->funcname, VTY_NEWLINE);
}

static void
cpu_record_hash_print(struct hash_backet *bucket, 
		      void *args[])
{
  struct cpu_thread_history *totals = args[0];
  struct vty *vty = args[1];
  thread_type *filter = args[2];
  struct cpu_thread_history *a = bucket->data;

  if ( !(a->types & *filter) )
       return;
  vty_out_cpu_thread_history(vty,a);
  totals->total_active += a->total_active;
  totals->total_calls += a->total_calls;
  totals->real.total += a->real.total;
  if (totals->real.max < a->real.max)
    totals->real.max = a->real.max;
  totals->cpu.total += a->cpu.total;
  if (totals->cpu.max < a->cpu.max)
    totals->cpu.max = a->cpu.max;
}

static void
cpu_record_print(struct vty *vty, thread_type filter)
{
  struct cpu_thread_history tmp;
  void *args[3] = {&tmp, vty, &filter};

  memset(&tmp, 0, sizeof tmp);
  tmp.funcname = "TOTAL";
  tmp.types = filter;

  vty_out(vty, "%21s %18s %18s%s",
	  "", "CPU (user+system):", "Real (wall-clock):", VTY_NEWLINE);
  vty_out(vty, "Active   Runtime(ms)   Invoked Avg uSec Max uSecs");
  vty_out(vty, " Avg uSec Max uSecs");
  vty_out(vty, "  Type  Thread%s", VTY_NEWLINE);

  pthread_mutex_lock (&cpu_record_mtx);
  {
    hash_iterate(cpu_record,
                 (void(*)(struct hash_backet*,void*))cpu_record_hash_print,
                 args);
  }
  pthread_mutex_unlock (&cpu_record_mtx);

  if (tmp.total_calls > 0)
    vty_out_cpu_thread_history(vty, &tmp);
}

DEFUN (show_thread_cpu,
       show_thread_cpu_cmd,
       "show thread cpu [FILTER]",
       SHOW_STR
       "Thread information\n"
       "Thread CPU usage\n"
       "Display filter (rwtexb)\n")
{
  int idx_filter = 3;
  int i = 0;
  thread_type filter = (thread_type) -1U;

  if (argc > 3)
    {
      filter = 0;
      while (argv[idx_filter]->arg[i] != '\0')
	{
	  switch ( argv[idx_filter]->arg[i] )
	    {
	    case 'r':
	    case 'R':
	      filter |= (1 << THREAD_READ);
	      break;
	    case 'w':
	    case 'W':
	      filter |= (1 << THREAD_WRITE);
	      break;
	    case 't':
	    case 'T':
	      filter |= (1 << THREAD_TIMER);
	      break;
	    case 'e':
	    case 'E':
	      filter |= (1 << THREAD_EVENT);
	      break;
	    case 'x':
	    case 'X':
	      filter |= (1 << THREAD_EXECUTE);
	      break;
	    case 'b':
	    case 'B':
	      filter |= (1 << THREAD_BACKGROUND);
	      break;
	    default:
	      break;
	    }
	  ++i;
	}
      if (filter == 0)
	{
	  vty_out(vty, "Invalid filter \"%s\" specified,"
                  " must contain at least one of 'RWTEXB'%s",
		  argv[idx_filter]->arg, VTY_NEWLINE);
	  return CMD_WARNING;
	}
    }

  cpu_record_print(vty, filter);
  return CMD_SUCCESS;
}

static void
cpu_record_hash_clear (struct hash_backet *bucket, 
		      void *args)
{
  thread_type *filter = args;
  struct cpu_thread_history *a = bucket->data;

  if ( !(a->types & *filter) )
       return;
  
  pthread_mutex_lock (&cpu_record_mtx);
  {
    hash_release (cpu_record, bucket->data);
  }
  pthread_mutex_unlock (&cpu_record_mtx);
}

static void
cpu_record_clear (thread_type filter)
{
  thread_type *tmp = &filter;

  pthread_mutex_lock (&cpu_record_mtx);
  {
    hash_iterate (cpu_record,
                  (void (*) (struct hash_backet*,void*)) cpu_record_hash_clear,
                  tmp);
  }
  pthread_mutex_unlock (&cpu_record_mtx);
}

DEFUN (clear_thread_cpu,
       clear_thread_cpu_cmd,
       "clear thread cpu [FILTER]",
       "Clear stored data\n"
       "Thread information\n"
       "Thread CPU usage\n"
       "Display filter (rwtexb)\n")
{
  int idx_filter = 3;
  int i = 0;
  thread_type filter = (thread_type) -1U;

  if (argc > 3)
    {
      filter = 0;
      while (argv[idx_filter]->arg[i] != '\0')
	{
	  switch ( argv[idx_filter]->arg[i] )
	    {
	    case 'r':
	    case 'R':
	      filter |= (1 << THREAD_READ);
	      break;
	    case 'w':
	    case 'W':
	      filter |= (1 << THREAD_WRITE);
	      break;
	    case 't':
	    case 'T':
	      filter |= (1 << THREAD_TIMER);
	      break;
	    case 'e':
	    case 'E':
	      filter |= (1 << THREAD_EVENT);
	      break;
	    case 'x':
	    case 'X':
	      filter |= (1 << THREAD_EXECUTE);
	      break;
	    case 'b':
	    case 'B':
	      filter |= (1 << THREAD_BACKGROUND);
	      break;
	    default:
	      break;
	    }
	  ++i;
	}
      if (filter == 0)
	{
	  vty_out(vty, "Invalid filter \"%s\" specified,"
                  " must contain at least one of 'RWTEXB'%s",
		  argv[idx_filter]->arg, VTY_NEWLINE);
	  return CMD_WARNING;
	}
    }

  cpu_record_clear (filter);
  return CMD_SUCCESS;
}

void
thread_cmd_init (void)
{
  install_element (VIEW_NODE, &show_thread_cpu_cmd);
  install_element (ENABLE_NODE, &clear_thread_cpu_cmd);
}

static int
thread_timer_cmp(void *a, void *b)
{
  struct thread *thread_a = a;
  struct thread *thread_b = b;

  if (timercmp (&thread_a->u.sands, &thread_b->u.sands, <))
    return -1;
  if (timercmp (&thread_a->u.sands, &thread_b->u.sands, >))
    return 1;
  return 0;
}

static void
thread_timer_update(void *node, int actual_position)
{
  struct thread *thread = node;

  thread->index = actual_position;
}

/* Allocate new thread master.  */
struct thread_master *
thread_master_create (void)
{
  struct thread_master *rv;
  struct rlimit limit;

  getrlimit(RLIMIT_NOFILE, &limit);

  pthread_mutex_lock (&cpu_record_mtx);
  {
    if (cpu_record == NULL)
      cpu_record = hash_create ((unsigned int (*) (void *))cpu_record_hash_key,
                                (int (*) (const void *, const void *))
                                cpu_record_hash_cmp);
  }
  pthread_mutex_unlock (&cpu_record_mtx);

  rv = XCALLOC (MTYPE_THREAD_MASTER, sizeof (struct thread_master));
  if (rv == NULL)
    return NULL;

  pthread_mutex_init (&rv->mtx, NULL);

  rv->fd_limit = (int)limit.rlim_cur;
  rv->read = XCALLOC (MTYPE_THREAD, sizeof (struct thread *) * rv->fd_limit);
  if (rv->read == NULL)
    {
      XFREE (MTYPE_THREAD_MASTER, rv);
      return NULL;
    }

  rv->write = XCALLOC (MTYPE_THREAD, sizeof (struct thread *) * rv->fd_limit);
  if (rv->write == NULL)
    {
      XFREE (MTYPE_THREAD, rv->read);
      XFREE (MTYPE_THREAD_MASTER, rv);
      return NULL;
    }

  /* Initialize the timer queues */
  rv->timer = pqueue_create();
  rv->background = pqueue_create();
  rv->timer->cmp = rv->background->cmp = thread_timer_cmp;
  rv->timer->update = rv->background->update = thread_timer_update;
  rv->spin = true;
  rv->handle_signals = true;
  rv->owner = pthread_self();
  pipe (rv->io_pipe);
  set_nonblocking (rv->io_pipe[0]);
  set_nonblocking (rv->io_pipe[1]);

  rv->handler.pfdsize = rv->fd_limit;
  rv->handler.pfdcount = 0;
  rv->handler.pfds = XCALLOC (MTYPE_THREAD_MASTER,
                              sizeof (struct pollfd) * rv->handler.pfdsize);

  return rv;
}

/* Add a new thread to the list.  */
static void
thread_list_add (struct thread_list *list, struct thread *thread)
{
  thread->next = NULL;
  thread->prev = list->tail;
  if (list->tail)
    list->tail->next = thread;
  else
    list->head = thread;
  list->tail = thread;
  list->count++;
}

/* Delete a thread from the list. */
static struct thread *
thread_list_delete (struct thread_list *list, struct thread *thread)
{
  if (thread->next)
    thread->next->prev = thread->prev;
  else
    list->tail = thread->prev;
  if (thread->prev)
    thread->prev->next = thread->next;
  else
    list->head = thread->next;
  thread->next = thread->prev = NULL;
  list->count--;
  return thread;
}

/* Thread list is empty or not.  */
static int
thread_empty (struct thread_list *list)
{
  return  list->head ? 0 : 1;
}

/* Delete top of the list and return it. */
static struct thread *
thread_trim_head (struct thread_list *list)
{
  if (!thread_empty (list))
    return thread_list_delete (list, list->head);
  return NULL;
}

/* Move thread to unuse list. */
static void
thread_add_unuse (struct thread_master *m, struct thread *thread)
{
  assert (m != NULL && thread != NULL);
  assert (thread->next == NULL);
  assert (thread->prev == NULL);
  thread->ref = NULL;

  thread->type = THREAD_UNUSED;
  thread->hist->total_active--;
  thread_list_add (&m->unuse, thread);
}

/* Free all unused thread. */
static void
thread_list_free (struct thread_master *m, struct thread_list *list)
{
  struct thread *t;
  struct thread *next;

  for (t = list->head; t; t = next)
    {
      next = t->next;
      XFREE (MTYPE_THREAD, t);
      list->count--;
      m->alloc--;
    }
}

static void
thread_array_free (struct thread_master *m, struct thread **thread_array)
{
  struct thread *t;
  int index;

  for (index = 0; index < m->fd_limit; ++index)
    {
      t = thread_array[index];
      if (t)
        {
          thread_array[index] = NULL;
          XFREE (MTYPE_THREAD, t);
          m->alloc--;
        }
    }
  XFREE (MTYPE_THREAD, thread_array);
}

static void
thread_queue_free (struct thread_master *m, struct pqueue *queue)
{
  int i;

  for (i = 0; i < queue->size; i++)
    XFREE(MTYPE_THREAD, queue->array[i]);

  m->alloc -= queue->size;
  pqueue_delete(queue);
}

/*
 * thread_master_free_unused
 *
 * As threads are finished with they are put on the
 * unuse list for later reuse.
 * If we are shutting down, Free up unused threads
 * So we can see if we forget to shut anything off
 */
void
thread_master_free_unused (struct thread_master *m)
{
  pthread_mutex_lock (&m->mtx);
  {
    struct thread *t;
    while ((t = thread_trim_head(&m->unuse)) != NULL)
      {
        pthread_mutex_destroy (&t->mtx);
        XFREE(MTYPE_THREAD, t);
      }
  }
  pthread_mutex_unlock (&m->mtx);
}

/* Stop thread scheduler. */
void
thread_master_free (struct thread_master *m)
{
  thread_array_free (m, m->read);
  thread_array_free (m, m->write);
  thread_queue_free (m, m->timer);
  thread_list_free (m, &m->event);
  thread_list_free (m, &m->ready);
  thread_list_free (m, &m->unuse);
  thread_queue_free (m, m->background);
  pthread_mutex_destroy (&m->mtx);
  close (m->io_pipe[0]);
  close (m->io_pipe[1]);

  XFREE (MTYPE_THREAD_MASTER, m->handler.pfds);
  XFREE (MTYPE_THREAD_MASTER, m);

  pthread_mutex_lock (&cpu_record_mtx);
  {
    if (cpu_record)
      {
        hash_clean (cpu_record, cpu_record_hash_free);
        hash_free (cpu_record);
        cpu_record = NULL;
      }
  }
  pthread_mutex_unlock (&cpu_record_mtx);
}

/* Return remain time in second. */
unsigned long
thread_timer_remain_second (struct thread *thread)
{
  int64_t remain;

  pthread_mutex_lock (&thread->mtx);
  {
    remain = monotime_until(&thread->u.sands, NULL) / 1000000LL;
  }
  pthread_mutex_unlock (&thread->mtx);

  return remain < 0 ? 0 : remain;
}

#define debugargdef  const char *funcname, const char *schedfrom, int fromln
#define debugargpass funcname, schedfrom, fromln

struct timeval
thread_timer_remain(struct thread *thread)
{
  struct timeval remain;
  pthread_mutex_lock (&thread->mtx);
  {
    monotime_until(&thread->u.sands, &remain);
  }
  pthread_mutex_unlock (&thread->mtx);
  return remain;
}

/* Get new thread.  */
static struct thread *
thread_get (struct thread_master *m, u_char type,
	    int (*func) (struct thread *), void *arg, debugargdef)
{
  struct thread *thread = thread_trim_head (&m->unuse);
  struct cpu_thread_history tmp;

  if (! thread)
    {
      thread = XCALLOC (MTYPE_THREAD, sizeof (struct thread));
      /* mutex only needs to be initialized at struct creation. */
      pthread_mutex_init (&thread->mtx, NULL);
      m->alloc++;
    }

  thread->type = type;
  thread->add_type = type;
  thread->master = m;
  thread->arg = arg;
  thread->index = -1;
  thread->yield = THREAD_YIELD_TIME_SLOT; /* default */
  thread->ref = NULL;

  /*
   * So if the passed in funcname is not what we have
   * stored that means the thread->hist needs to be
   * updated.  We keep the last one around in unused
   * under the assumption that we are probably
   * going to immediately allocate the same
   * type of thread.
   * This hopefully saves us some serious
   * hash_get lookups.
   */
  if (thread->funcname != funcname ||
      thread->func != func)
    {
      tmp.func = func;
      tmp.funcname = funcname;
      pthread_mutex_lock (&cpu_record_mtx);
      {
        thread->hist = hash_get (cpu_record, &tmp,
                                 (void * (*) (void *))cpu_record_hash_alloc);
      }
      pthread_mutex_unlock (&cpu_record_mtx);
    }
  thread->hist->total_active++;
  thread->func = func;
  thread->funcname = funcname;
  thread->schedfrom = schedfrom;
  thread->schedfrom_line = fromln;

  return thread;
}

static int
fd_poll (struct thread_master *m, struct pollfd *pfds, nfds_t pfdsize,
         nfds_t count, struct timeval *timer_wait)
{
  if (count == 0)
    return 0;

  /* If timer_wait is null here, that means poll() should block indefinitely,
   * unless the thread_master has overriden it by setting ->selectpoll_timeout.
   * If the value is positive, it specifies the maximum number of milliseconds
   * to wait. If the timeout is -1, it specifies that we should never wait and
   * always return immediately even if no event is detected. If the value is
   * zero, the behavior is default. */
  int timeout = -1;

  /* number of file descriptors with events */
  int num;

  if (timer_wait != NULL && m->selectpoll_timeout == 0) // use the default value
    timeout = (timer_wait->tv_sec*1000) + (timer_wait->tv_usec/1000);
  else if (m->selectpoll_timeout > 0) // use the user's timeout
    timeout = m->selectpoll_timeout;
  else if (m->selectpoll_timeout < 0) // effect a poll (return immediately)
    timeout = 0;

  /* add poll pipe poker */
  assert (count + 1 < pfdsize);
  pfds[count].fd = m->io_pipe[0];
  pfds[count].events = POLLIN;
  pfds[count].revents = 0x00;

  num = poll (pfds, count + 1, timeout);

  static unsigned char trash[64];
  if (num > 0 && pfds[count].revents != 0 && num--)
    while (read (m->io_pipe[0], &trash, sizeof (trash)) > 0);

  return num;
}

/* Add new read thread. */
struct thread *
funcname_thread_add_read_write (int dir, struct thread_master *m,
        int (*func) (struct thread *), void *arg, int fd, struct thread **t_ptr,
        debugargdef)
{
  struct thread *thread = NULL;

  pthread_mutex_lock (&m->mtx);
  {
    if (t_ptr && *t_ptr) // thread is already scheduled; don't reschedule
      {
        pthread_mutex_unlock (&m->mtx);
        return NULL;
      }

    /* default to a new pollfd */
    nfds_t queuepos = m->handler.pfdcount;

    /* if we already have a pollfd for our file descriptor, find and use it */
    for (nfds_t i = 0; i < m->handler.pfdcount; i++)
      if (m->handler.pfds[i].fd == fd)
        {
          queuepos = i;
          break;
        }

    /* make sure we have room for this fd + pipe poker fd */
    assert (queuepos + 1 < m->handler.pfdsize);

    thread = thread_get (m, dir, func, arg, debugargpass);

    m->handler.pfds[queuepos].fd = fd;
    m->handler.pfds[queuepos].events |= (dir == THREAD_READ ? POLLIN : POLLOUT);

    if (queuepos == m->handler.pfdcount)
      m->handler.pfdcount++;

    if (thread)
      {
        pthread_mutex_lock (&thread->mtx);
        {
          thread->u.fd = fd;
          if (dir == THREAD_READ)
            m->read[thread->u.fd] = thread;
          else
            m->write[thread->u.fd] = thread;
        }
        pthread_mutex_unlock (&thread->mtx);

        if (t_ptr)
          {
            *t_ptr = thread;
            thread->ref = t_ptr;
          }
      }

    AWAKEN (m);
  }
  pthread_mutex_unlock (&m->mtx);

  return thread;
}

static struct thread *
funcname_thread_add_timer_timeval (struct thread_master *m,
         int (*func) (struct thread *), int type, void *arg,
         struct timeval *time_relative, struct thread **t_ptr, debugargdef)
{
  struct thread *thread;
  struct pqueue *queue;

  assert (m != NULL);

  assert (type == THREAD_TIMER || type == THREAD_BACKGROUND);
  assert (time_relative);
  
  pthread_mutex_lock (&m->mtx);
  {
    if (t_ptr && *t_ptr) // thread is already scheduled; don't reschedule
      {
        pthread_mutex_unlock (&m->mtx);
        return NULL;
      }

    queue = ((type == THREAD_TIMER) ? m->timer : m->background);
    thread = thread_get (m, type, func, arg, debugargpass);

    pthread_mutex_lock (&thread->mtx);
    {
      monotime(&thread->u.sands);
      timeradd(&thread->u.sands, time_relative, &thread->u.sands);
      pqueue_enqueue(thread, queue);
      if (t_ptr)
        {
          *t_ptr = thread;
          thread->ref = t_ptr;
        }
    }
    pthread_mutex_unlock (&thread->mtx);

    AWAKEN (m);
  }
  pthread_mutex_unlock (&m->mtx);

  return thread;
}


/* Add timer event thread. */
struct thread *
funcname_thread_add_timer (struct thread_master *m,
        int (*func) (struct thread *), void *arg, long timer,
        struct thread **t_ptr, debugargdef)
{
  struct timeval trel;

  assert (m != NULL);

  trel.tv_sec = timer;
  trel.tv_usec = 0;

  return funcname_thread_add_timer_timeval (m, func, THREAD_TIMER, arg, &trel,
                                            t_ptr, debugargpass);
}

/* Add timer event thread with "millisecond" resolution */
struct thread *
funcname_thread_add_timer_msec (struct thread_master *m,
        int (*func) (struct thread *), void *arg, long timer,
        struct thread **t_ptr, debugargdef)
{
  struct timeval trel;

  assert (m != NULL);

  trel.tv_sec = timer / 1000;
  trel.tv_usec = 1000*(timer % 1000);

  return funcname_thread_add_timer_timeval (m, func, THREAD_TIMER, arg, &trel,
                                            t_ptr, debugargpass);
}

/* Add timer event thread with "millisecond" resolution */
struct thread *
funcname_thread_add_timer_tv (struct thread_master *m,
        int (*func) (struct thread *), void *arg, struct timeval *tv,
        struct thread **t_ptr, debugargdef)
{
  return funcname_thread_add_timer_timeval (m, func, THREAD_TIMER, arg, tv,
                                            t_ptr, debugargpass);
}

/* Add a background thread, with an optional millisec delay */
struct thread *
funcname_thread_add_background (struct thread_master *m,
        int (*func) (struct thread *), void *arg, long delay,
        struct thread **t_ptr, debugargdef)
{
  struct timeval trel;
  
  assert (m != NULL);
  
  if (delay)
    {
      trel.tv_sec = delay / 1000;
      trel.tv_usec = 1000*(delay % 1000);
    }
  else
    {
      trel.tv_sec = 0;
      trel.tv_usec = 0;
    }

  return funcname_thread_add_timer_timeval (m, func, THREAD_BACKGROUND, arg, &trel,
                                            t_ptr, debugargpass);
}

/* Add simple event thread. */
struct thread *
funcname_thread_add_event (struct thread_master *m,
        int (*func) (struct thread *), void *arg, int val,
        struct thread **t_ptr, debugargdef)
{
  struct thread *thread;

  assert (m != NULL);

  pthread_mutex_lock (&m->mtx);
  {
    if (t_ptr && *t_ptr) // thread is already scheduled; don't reschedule
      {
        pthread_mutex_unlock (&m->mtx);
        return NULL;
      }

    thread = thread_get (m, THREAD_EVENT, func, arg, debugargpass);
    pthread_mutex_lock (&thread->mtx);
    {
      thread->u.val = val;
      thread_list_add (&m->event, thread);
    }
    pthread_mutex_unlock (&thread->mtx);

    if (t_ptr)
      {
        *t_ptr = thread;
        thread->ref = t_ptr;
      }

    AWAKEN (m);
  }
  pthread_mutex_unlock (&m->mtx);

  return thread;
}

static void
thread_cancel_read_or_write (struct thread *thread, short int state)
{
  for (nfds_t i = 0; i < thread->master->handler.pfdcount; ++i)
    if (thread->master->handler.pfds[i].fd == thread->u.fd)
      {
        thread->master->handler.pfds[i].events &= ~(state);

        /* remove thread fds from pfd list */
        if (thread->master->handler.pfds[i].events == 0)
          {
            memmove(thread->master->handler.pfds+i,
                    thread->master->handler.pfds+i+1,
                    (thread->master->handler.pfdsize-i-1) * sizeof(struct pollfd));
            thread->master->handler.pfdcount--;
            return;
          }
      }
}

/**
 * Cancel thread from scheduler.
 *
 * This function is *NOT* MT-safe. DO NOT call it from any other pthread except
 * the one which owns thread->master. You will crash.
 */
void
thread_cancel (struct thread *thread)
{
  struct thread_list *list = NULL;
  struct pqueue *queue = NULL;
  struct thread **thread_array = NULL;

  pthread_mutex_lock (&thread->mtx);
  pthread_mutex_lock (&thread->master->mtx);

  assert (pthread_self() == thread->master->owner);

  switch (thread->type)
    {
    case THREAD_READ:
      thread_cancel_read_or_write (thread, POLLIN | POLLHUP);
      thread_array = thread->master->read;
      break;
    case THREAD_WRITE:
      thread_cancel_read_or_write (thread, POLLOUT | POLLHUP);
      thread_array = thread->master->write;
      break;
    case THREAD_TIMER:
      queue = thread->master->timer;
      break;
    case THREAD_EVENT:
      list = &thread->master->event;
      break;
    case THREAD_READY:
      list = &thread->master->ready;
      break;
    case THREAD_BACKGROUND:
      queue = thread->master->background;
      break;
    default:
      goto done;
      break;
    }

  if (queue)
    {
      assert(thread->index >= 0);
      pqueue_remove (thread, queue);
    }
  else if (list)
    {
      thread_list_delete (list, thread);
    }
  else if (thread_array)
    {
      thread_array[thread->u.fd] = NULL;
    }
  else
    {
      assert(!"Thread should be either in queue or list or array!");
    }

  if (thread->ref)
    *thread->ref = NULL;

  thread_add_unuse (thread->master, thread);

done:
  pthread_mutex_unlock (&thread->master->mtx);
  pthread_mutex_unlock (&thread->mtx);
}

/* Delete all events which has argument value arg. */
unsigned int
thread_cancel_event (struct thread_master *m, void *arg)
{
  unsigned int ret = 0;
  struct thread *thread;
  struct thread *t;

  pthread_mutex_lock (&m->mtx);
  {
    thread = m->event.head;
    while (thread)
      {
        t = thread;
        pthread_mutex_lock (&t->mtx);
        {
          thread = t->next;

          if (t->arg == arg)
            {
              ret++;
              thread_list_delete (&m->event, t);
              if (t->ref)
                *t->ref = NULL;
              thread_add_unuse (m, t);
            }
        }
        pthread_mutex_unlock (&t->mtx);
      }

    /* thread can be on the ready list too */
    thread = m->ready.head;
    while (thread)
      {
        t = thread;
        pthread_mutex_lock (&t->mtx);
        {
          thread = t->next;

          if (t->arg == arg)
            {
              ret++;
              thread_list_delete (&m->ready, t);
              if (t->ref)
                *t->ref = NULL;
              thread_add_unuse (m, t);
            }
        }
        pthread_mutex_unlock (&t->mtx);
      }
  }
  pthread_mutex_unlock (&m->mtx);
  return ret;
}

static struct timeval *
thread_timer_wait (struct pqueue *queue, struct timeval *timer_val)
{
  if (queue->size)
    {
      struct thread *next_timer = queue->array[0];
      monotime_until(&next_timer->u.sands, timer_val);
      return timer_val;
    }
  return NULL;
}

static struct thread *
thread_run (struct thread_master *m, struct thread *thread,
        struct thread *fetch)
{
  *fetch = *thread;
  thread_add_unuse (m, thread);
  return fetch;
}

static int
thread_process_io_helper (struct thread_master *m, struct thread *thread,
        short state, int pos)
{
  struct thread **thread_array;

  if (!thread)
    return 0;

  if (thread->type == THREAD_READ)
    thread_array = m->read;
  else
    thread_array = m->write;

  thread_array[thread->u.fd] = NULL;
  thread_list_add (&m->ready, thread);
  thread->type = THREAD_READY;
  /* if another pthread scheduled this file descriptor for the event we're
   * responding to, no problem; we're getting to it now */
  thread->master->handler.pfds[pos].events &= ~(state);
  return 1;
}

static void
thread_process_io (struct thread_master *m, struct pollfd *pfds,
        unsigned int num, unsigned int count)
{
  unsigned int ready = 0;

  for (nfds_t i = 0; i < count && ready < num ; ++i)
    {
      /* no event for current fd? immediately continue */
      if (pfds[i].revents == 0)
        continue;

      ready++;

      /* Unless someone has called thread_cancel from another pthread, the only
       * thing that could have changed in m->handler.pfds while we were
       * asleep is the .events field in a given pollfd. Barring thread_cancel()
       * that value should be a superset of the values we have in our copy, so
       * there's no need to update it. Similarily, barring deletion, the fd
       * should still be a valid index into the master's pfds. */
      if (pfds[i].revents & (POLLIN | POLLHUP))
        thread_process_io_helper(m, m->read[pfds[i].fd], POLLIN, i);
      if (pfds[i].revents & POLLOUT)
        thread_process_io_helper(m, m->write[pfds[i].fd], POLLOUT, i);

      /* if one of our file descriptors is garbage, remove the same from
       * both pfds + update sizes and index */
      if (pfds[i].revents & POLLNVAL)
        {
          memmove (m->handler.pfds + i,
                   m->handler.pfds + i + 1,
                   (m->handler.pfdcount - i - 1) * sizeof(struct pollfd));
          m->handler.pfdcount--;

          memmove (pfds + i, pfds + i + 1,
                   (count - i - 1) * sizeof(struct pollfd));
          count--;
          i--;
        }
    }
}

/* Add all timers that have popped to the ready list. */
static unsigned int
thread_process_timers (struct pqueue *queue, struct timeval *timenow)
{
  struct thread *thread;
  unsigned int ready = 0;
  
  while (queue->size)
    {
      thread = queue->array[0];
      if (timercmp (timenow, &thread->u.sands, <))
        return ready;
      pqueue_dequeue(queue);
      thread->type = THREAD_READY;
      thread_list_add (&thread->master->ready, thread);
      ready++;
    }
  return ready;
}

/* process a list en masse, e.g. for event thread lists */
static unsigned int
thread_process (struct thread_list *list)
{
  struct thread *thread;
  struct thread *next;
  unsigned int ready = 0;
  
  for (thread = list->head; thread; thread = next)
    {
      next = thread->next;
      thread_list_delete (list, thread);
      thread->type = THREAD_READY;
      thread_list_add (&thread->master->ready, thread);
      ready++;
    }
  return ready;
}


/* Fetch next ready thread. */
struct thread *
thread_fetch (struct thread_master *m, struct thread *fetch)
{
  struct thread *thread;
  struct timeval now;
  struct timeval timer_val = { .tv_sec = 0, .tv_usec = 0 };
  struct timeval timer_val_bg;
  struct timeval *timer_wait = &timer_val;
  struct timeval *timer_wait_bg;

  do
    {
      int num = 0;

      /* Signals pre-empt everything */
      if (m->handle_signals)
        quagga_sigevent_process ();
       
      pthread_mutex_lock (&m->mtx);
      /* Drain the ready queue of already scheduled jobs, before scheduling
       * more.
       */
      if ((thread = thread_trim_head (&m->ready)) != NULL)
        {
          fetch = thread_run (m, thread, fetch);
          if (fetch->ref)
            *fetch->ref = NULL;
          pthread_mutex_unlock (&m->mtx);
          return fetch;
        }
      
      /* To be fair to all kinds of threads, and avoid starvation, we
       * need to be careful to consider all thread types for scheduling
       * in each quanta. I.e. we should not return early from here on.
       */
       
      /* Normal event are the next highest priority.  */
      thread_process (&m->event);
      
      /* Calculate select wait timer if nothing else to do */
      if (m->ready.count == 0)
        {
          timer_wait = thread_timer_wait (m->timer, &timer_val);
          timer_wait_bg = thread_timer_wait (m->background, &timer_val_bg);
          
          if (timer_wait_bg &&
              (!timer_wait || (timercmp (timer_wait, timer_wait_bg, >))))
            timer_wait = timer_wait_bg;
        }

      if (timer_wait && timer_wait->tv_sec < 0)
        {
          timerclear(&timer_val);
          timer_wait = &timer_val;
        }

      /* copy pollfds so we can unlock during blocking calls to poll() */
      struct pollfd pfds[m->handler.pfdsize];
      unsigned int count = m->handler.pfdcount + m->handler.pfdcountsnmp;
      memcpy (pfds, m->handler.pfds, count * sizeof (struct pollfd));

      pthread_mutex_unlock (&m->mtx);
      {
        num = fd_poll (m, pfds, m->handler.pfdsize, count, timer_wait);
      }
      pthread_mutex_lock (&m->mtx);
      
      /* Signals should get quick treatment */
      if (num < 0)
        {
          if (errno == EINTR)
            {
              pthread_mutex_unlock (&m->mtx);
              continue; /* signal received - process it */
            }
          zlog_warn ("poll() error: %s", safe_strerror (errno));
          pthread_mutex_unlock (&m->mtx);
          return NULL;
        }

      /* Check foreground timers.  Historically, they have had higher
       * priority than I/O threads, so let's push them onto the ready
       * list in front of the I/O threads. */
      monotime(&now);
      thread_process_timers (m->timer, &now);
      
      /* Got IO, process it */
      if (num > 0)
        thread_process_io (m, pfds, num, count);

#if 0
      /* If any threads were made ready above (I/O or foreground timer),
         perhaps we should avoid adding background timers to the ready
	 list at this time.  If this is code is uncommented, then background
	 timer threads will not run unless there is nothing else to do. */
      if ((thread = thread_trim_head (&m->ready)) != NULL)
        {
          fetch = thread_run (m, thread, fetch);
          if (fetch->ref)
            *fetch->ref = NULL;
          pthread_mutex_unlock (&m->mtx);
          return fetch;
        }
#endif

      /* Background timer/events, lowest priority */
      thread_process_timers (m->background, &now);
      
      if ((thread = thread_trim_head (&m->ready)) != NULL)
        {
          fetch = thread_run (m, thread, fetch);
          if (fetch->ref)
            *fetch->ref = NULL;
          pthread_mutex_unlock (&m->mtx);
          return fetch;
        }

      pthread_mutex_unlock (&m->mtx);

    } while (m->spin);

  return NULL;
}

unsigned long
thread_consumed_time (RUSAGE_T *now, RUSAGE_T *start, unsigned long *cputime)
{
  /* This is 'user + sys' time.  */
  *cputime = timeval_elapsed (now->cpu.ru_utime, start->cpu.ru_utime) +
	     timeval_elapsed (now->cpu.ru_stime, start->cpu.ru_stime);
  return timeval_elapsed (now->real, start->real);
}

/* We should aim to yield after yield milliseconds, which defaults
   to THREAD_YIELD_TIME_SLOT .
   Note: we are using real (wall clock) time for this calculation.
   It could be argued that CPU time may make more sense in certain
   contexts.  The things to consider are whether the thread may have
   blocked (in which case wall time increases, but CPU time does not),
   or whether the system is heavily loaded with other processes competing
   for CPU time.  On balance, wall clock time seems to make sense. 
   Plus it has the added benefit that gettimeofday should be faster
   than calling getrusage. */
int
thread_should_yield (struct thread *thread)
{
  int result;
  pthread_mutex_lock (&thread->mtx);
  {
    result = monotime_since(&thread->real, NULL) > (int64_t)thread->yield;
  }
  pthread_mutex_unlock (&thread->mtx);
  return result;
}

void
thread_set_yield_time (struct thread *thread, unsigned long yield_time)
{
  pthread_mutex_lock (&thread->mtx);
  {
    thread->yield = yield_time;
  }
  pthread_mutex_unlock (&thread->mtx);
}

void
thread_getrusage (RUSAGE_T *r)
{
  monotime(&r->real);
  getrusage(RUSAGE_SELF, &(r->cpu));
}

struct thread *thread_current = NULL;

/* We check thread consumed time. If the system has getrusage, we'll
   use that to get in-depth stats on the performance of the thread in addition
   to wall clock time stats from gettimeofday. */
void
thread_call (struct thread *thread)
{
  unsigned long realtime, cputime;
  RUSAGE_T before, after;

  GETRUSAGE (&before);
  thread->real = before.real;

  thread_current = thread;
  (*thread->func) (thread);
  thread_current = NULL;

  GETRUSAGE (&after);

  realtime = thread_consumed_time (&after, &before, &cputime);
  thread->hist->real.total += realtime;
  if (thread->hist->real.max < realtime)
    thread->hist->real.max = realtime;
  thread->hist->cpu.total += cputime;
  if (thread->hist->cpu.max < cputime)
    thread->hist->cpu.max = cputime;

  ++(thread->hist->total_calls);
  thread->hist->types |= (1 << thread->add_type);

#ifdef CONSUMED_TIME_CHECK
  if (realtime > CONSUMED_TIME_CHECK)
    {
      /*
       * We have a CPU Hog on our hands.
       * Whinge about it now, so we're aware this is yet another task
       * to fix.
       */
      zlog_warn ("SLOW THREAD: task %s (%lx) ran for %lums (cpu time %lums)",
		 thread->funcname,
		 (unsigned long) thread->func,
		 realtime/1000, cputime/1000);
    }
#endif /* CONSUMED_TIME_CHECK */
}

/* Execute thread */
void
funcname_thread_execute (struct thread_master *m,
                int (*func)(struct thread *), 
                void *arg,
                int val,
		debugargdef)
{
  struct cpu_thread_history tmp;
  struct thread dummy;

  memset (&dummy, 0, sizeof (struct thread));

  pthread_mutex_init (&dummy.mtx, NULL);
  dummy.type = THREAD_EVENT;
  dummy.add_type = THREAD_EXECUTE;
  dummy.master = NULL;
  dummy.arg = arg;
  dummy.u.val = val;

  tmp.func = dummy.func = func;
  tmp.funcname = dummy.funcname = funcname;
  pthread_mutex_lock (&cpu_record_mtx);
  {
    dummy.hist = hash_get (cpu_record, &tmp,
                           (void * (*) (void *))cpu_record_hash_alloc);
  }
  pthread_mutex_unlock (&cpu_record_mtx);

  dummy.schedfrom = schedfrom;
  dummy.schedfrom_line = fromln;

  thread_call (&dummy);
}
