/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * gmain.c: Main loop abstraction, timeouts, and idle functions
 * Copyright 1998 Owen Taylor
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Modified by the GLib Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GLib Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GLib at ftp://ftp.gtk.org/pub/gtk/.
 */

/*
 * MT safe
 */

#include "config.h"
#include "glib.h"
#include "glibconfig.h"
#include "glib_trace.h"

/* Uncomment the next line (and the corresponding line in gpoll.c) to
 * enable debugging printouts if the environment variable
 * G_MAIN_POLL_DEBUG is set to some value.
 */
/* #define G_MAIN_POLL_DEBUG */

#ifdef _WIN32
/* Always enable debugging printout on Windows, as it is more often
 * needed there...
 */
#define G_MAIN_POLL_DEBUG
#endif

/* We need to include this as early as possible, because on some
 * platforms like AIX, <poll.h> redefines the names we use for
 * GPollFD struct members.
 * See https://gitlab.gnome.org/GNOME/glib/-/issues/3500 */

#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#ifdef G_OS_UNIX
#include "glib-unix.h"
#include <pthread.h>
#ifdef HAVE_EVENTFD
#include <sys/eventfd.h>
#endif
#endif

#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <stdlib.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#ifdef G_OS_UNIX
#include <unistd.h>
#endif /* G_OS_UNIX */
#include <errno.h>
#include <string.h>

#ifdef HAVE_PIDFD
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/wait.h>  /* P_PIDFD */
#ifndef W_EXITCODE
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#endif
#ifndef W_STOPCODE
#define W_STOPCODE(sig)      ((sig) << 8 | 0x7f)
#endif
#ifndef WCOREFLAG
/* musl doesn’t define WCOREFLAG while glibc does. Unfortunately, there’s no way
 * to detect we’re building against musl, so just define it and hope.
 * See https://git.musl-libc.org/cgit/musl/tree/include/sys/wait.h#n51 */
#define WCOREFLAG 0x80
#endif
#ifndef __W_CONTINUED
/* Same as above, for musl */
#define __W_CONTINUED 0xffff
#endif
#endif  /* HAVE_PIDFD */

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifdef HAVE_MACH_MACH_TIME_H
#include <mach/mach_time.h>
#endif

#include "glib_trace.h"

#include "gmain.h"

#include "garray.h"
#include "giochannel.h"
#include "ghash.h"
#include "ghook.h"
#include "gqueue.h"
#include "gstrfuncs.h"
#include "gtestutils.h"
#include "gthreadprivate.h"
#include "gtrace-private.h"

#ifdef G_OS_WIN32
#include "gwin32.h"
#endif

#ifdef  G_MAIN_POLL_DEBUG
#include "gtimer.h"
#endif

#include "gwakeup.h"
#include "gmain-internal.h"
#include "glib-init.h"
#include "glib-private.h"

/* Types */

typedef struct _GIdleSource GIdleSource;
typedef struct _GTimeoutSource GTimeoutSource;
typedef struct _GChildWatchSource GChildWatchSource;
typedef struct _GUnixSignalWatchSource GUnixSignalWatchSource;
typedef struct _GPollRec GPollRec;
typedef struct _GSourceCallback GSourceCallback;

typedef enum
{
  G_SOURCE_READY = 1 << G_HOOK_FLAG_USER_SHIFT,
  G_SOURCE_CAN_RECURSE = 1 << (G_HOOK_FLAG_USER_SHIFT + 1),
  G_SOURCE_BLOCKED = 1 << (G_HOOK_FLAG_USER_SHIFT + 2)
} GSourceFlags;

typedef struct _GSourceList GSourceList;

struct _GSourceList
{
  GList link;
  GSource *head, *tail;
  gint priority;
};

typedef struct _GMainWaiter GMainWaiter;

struct _GMainWaiter
{
  GCond *cond;
  GMutex *mutex;
};

typedef struct _GMainDispatch GMainDispatch;

struct _GMainDispatch
{
  gint depth;
  GSource *source;
};

#ifdef G_MAIN_POLL_DEBUG
gboolean _g_main_poll_debug = FALSE;
#endif

struct _GMainContext
{
  /* The following lock is used for both the list of sources
   * and the list of poll records
   */
  GMutex mutex;
  GCond cond;
  GThread *owner;
  guint owner_count;
  GMainContextFlags flags;
  GSList *waiters;

  gint ref_count;  /* (atomic) */

  GHashTable *sources;              /* guint -> GSource */

  GPtrArray *pending_dispatches;
  gint64 timeout_usec; /* Timeout for current iteration */

  guint next_id;
  GQueue source_lists;
  gint in_check_or_prepare;

  GPollRec *poll_records;
  guint n_poll_records;
  GPollFD *cached_poll_array;
  guint cached_poll_array_size;

  GWakeup *wakeup;

  GPollFD wake_up_rec;

/* Flag indicating whether the set of fd's changed during a poll */
  gboolean poll_changed;

  GPollFunc poll_func;

  gint64   time;
  gboolean time_is_fresh;
};

struct _GSourceCallback
{
  gint ref_count;  /* (atomic) */
  GSourceFunc func;
  gpointer    data;
  GDestroyNotify notify;
};

struct _GMainLoop
{
  GMainContext *context;
  gboolean is_running; /* (atomic) */
  gint ref_count;  /* (atomic) */
};

struct _GIdleSource
{
  GSource  source;
  gboolean one_shot;
};

struct _GTimeoutSource
{
  GSource     source;
  /* Measured in seconds if 'seconds' is TRUE, or milliseconds otherwise. */
  guint       interval;
  gboolean    seconds;
  gboolean    one_shot;
};

struct _GChildWatchSource
{
  GSource     source;
  GPid        pid;
  /* @poll is always used on Windows.
   * On Unix, poll.fd will be negative if PIDFD is unavailable. */
  GPollFD     poll;
#ifndef G_OS_WIN32
  gboolean child_maybe_exited; /* (atomic) */
#endif /* G_OS_WIN32 */
};

struct _GUnixSignalWatchSource
{
  GSource     source;
  int         signum;
  gboolean    pending; /* (atomic) */
};

struct _GPollRec
{
  GPollFD *fd;
  GPollRec *prev;
  GPollRec *next;
  gint priority;
};

struct _GSourcePrivate
{
  GSList *child_sources;
  GSource *parent_source;

  gint64 ready_time;

  /* This is currently only used on UNIX, but we always declare it (and
   * let it remain empty on Windows) to avoid #ifdef all over the place.
   */
  GSList *fds;

  GSourceDisposeFunc dispose;

  gboolean static_name;
};

typedef struct _GSourceIter
{
  GMainContext *context;
  gboolean may_modify;
  GList *current_list;
  GSource *source;
} GSourceIter;

#define LOCK_CONTEXT(context) g_mutex_lock (&context->mutex)
#define UNLOCK_CONTEXT(context) g_mutex_unlock (&context->mutex)
#define G_THREAD_SELF g_thread_self ()

#define SOURCE_DESTROYED(source) \
  ((g_atomic_int_get (&((source)->flags)) & G_HOOK_FLAG_ACTIVE) == 0)
#define SOURCE_BLOCKED(source) \
  ((g_atomic_int_get (&((source)->flags)) & G_SOURCE_BLOCKED) != 0)

/* Forward declarations */

static void g_source_unref_internal             (GSource      *source,
						 GMainContext *context,
						 gboolean      have_lock);
static void g_source_destroy_internal           (GSource      *source,
						 GMainContext *context,
						 gboolean      have_lock);
static void g_source_set_priority_unlocked      (GSource      *source,
						 GMainContext *context,
						 gint          priority);
static void g_child_source_remove_internal      (GSource      *child_source,
                                                 GMainContext *context);

static gboolean g_main_context_acquire_unlocked (GMainContext *context);
static void g_main_context_release_unlocked     (GMainContext *context);
static gboolean g_main_context_prepare_unlocked (GMainContext *context,
                                                 gint         *priority);
static gint g_main_context_query_unlocked       (GMainContext *context,
                                                 gint          max_priority,
                                                 gint64       *timeout_usec,
                                                 GPollFD      *fds,
                                                 gint          n_fds);
static gboolean g_main_context_check_unlocked   (GMainContext *context,
                                                 gint          max_priority,
                                                 GPollFD      *fds,
                                                 gint          n_fds);
static void g_main_context_dispatch_unlocked    (GMainContext *context);
static void g_main_context_poll_unlocked        (GMainContext *context,
                                                 gint64        timeout_usec,
                                                 int           priority,
                                                 GPollFD      *fds,
                                                 int           n_fds);
static void g_main_context_add_poll_unlocked    (GMainContext *context,
						 gint          priority,
						 GPollFD      *fd);
static void g_main_context_remove_poll_unlocked (GMainContext *context,
						 GPollFD      *fd);

static void     g_source_iter_init  (GSourceIter   *iter,
				     GMainContext  *context,
				     gboolean       may_modify);
static gboolean g_source_iter_next  (GSourceIter   *iter,
				     GSource      **source);
static void     g_source_iter_clear (GSourceIter   *iter);

static gboolean g_timeout_dispatch (GSource     *source,
				    GSourceFunc  callback,
				    gpointer     user_data);
static gboolean g_child_watch_prepare  (GSource     *source,
				        gint        *timeout);
static gboolean g_child_watch_check    (GSource     *source);
static gboolean g_child_watch_dispatch (GSource     *source,
					GSourceFunc  callback,
					gpointer     user_data);
static void     g_child_watch_finalize (GSource     *source);

#ifndef G_OS_WIN32
static void unref_unix_signal_handler_unlocked (int signum);
#endif

#ifdef G_OS_UNIX
static void g_unix_signal_handler (int signum);
static gboolean g_unix_signal_watch_prepare  (GSource     *source,
					      gint        *timeout);
static gboolean g_unix_signal_watch_check    (GSource     *source);
static gboolean g_unix_signal_watch_dispatch (GSource     *source,
					      GSourceFunc  callback,
					      gpointer     user_data);
static void     g_unix_signal_watch_finalize  (GSource     *source);
#endif
static gboolean g_idle_prepare     (GSource     *source,
				    gint        *timeout);
static gboolean g_idle_check       (GSource     *source);
static gboolean g_idle_dispatch    (GSource     *source,
				    GSourceFunc  callback,
				    gpointer     user_data);

static void block_source (GSource      *source,
                          GMainContext *context);
static GMainContext *source_dup_main_context (GSource *source);

/* Lock for serializing access for safe execution of
 * g_main_context_unref() with concurrent use of
 * g_source_destroy() and g_source_unref().
 *
 * Locking order is source_destroy_lock, then context lock.
 */
static GRWLock source_destroy_lock;

static GMainContext *glib_worker_context;

#ifndef G_OS_WIN32


/* UNIX signals work by marking one of these variables then waking the
 * worker context to check on them and dispatch accordingly.
 *
 * Both variables must be accessed using atomic primitives, unless those atomic
 * primitives are implemented using fallback mutexes (as those aren’t safe in
 * an interrupt context).
 *
 * If using atomic primitives, the variables must be of type `int` (so they’re
 * the right size for the atomic primitives). Otherwise, use `sig_atomic_t` if
 * it’s available, which is guaranteed to be async-signal-safe (but it’s *not*
 * guaranteed to be thread-safe, which is why we use atomic primitives if
 * possible).
 *
 * Typically, `sig_atomic_t` is a typedef to `int`, but that’s not the case on
 * FreeBSD, so we can’t use it unconditionally if it’s defined.
 */
#if (defined(G_ATOMIC_LOCK_FREE) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)) || !defined(HAVE_SIG_ATOMIC_T)
static volatile int unix_signal_pending[NSIG];
static volatile int any_unix_signal_pending;
#else
static volatile sig_atomic_t unix_signal_pending[NSIG];
static volatile sig_atomic_t any_unix_signal_pending;
#endif

/* Guards all the data below */
G_LOCK_DEFINE_STATIC (unix_signal_lock);
static guint unix_signal_refcount[NSIG];
static GSList *unix_signal_watches;
static GSList *unix_child_watches;

GSourceFuncs g_unix_signal_funcs =
{
  g_unix_signal_watch_prepare,
  g_unix_signal_watch_check,
  g_unix_signal_watch_dispatch,
  g_unix_signal_watch_finalize,
  NULL, NULL
};
#endif /* !G_OS_WIN32 */

GSourceFuncs g_timeout_funcs =
{
  NULL, /* prepare */
  NULL, /* check */
  g_timeout_dispatch,
  NULL, NULL, NULL
};

GSourceFuncs g_child_watch_funcs =
{
  g_child_watch_prepare,
  g_child_watch_check,
  g_child_watch_dispatch,
  g_child_watch_finalize,
  NULL, NULL
};

GSourceFuncs g_idle_funcs =
{
  g_idle_prepare,
  g_idle_check,
  g_idle_dispatch,
  NULL, NULL, NULL
};

/**
 * g_main_context_ref:
 * @context: (not nullable): a #GMainContext
 * 
 * Increases the reference count on a [struct@GLib.MainContext] object by one.
 *
 * Returns: the @context that was passed in (since 2.6)
 **/
GMainContext *
g_main_context_ref (GMainContext *context)
{
  int old_ref_count;

  g_return_val_if_fail (context != NULL, NULL);

  old_ref_count = g_atomic_int_add (&context->ref_count, 1);
  g_return_val_if_fail (old_ref_count > 0, NULL);

  return context;
}

static inline void
poll_rec_list_free (GMainContext *context,
		    GPollRec     *list)
{
  g_slice_free_chain (GPollRec, list, next);
}

/**
 * g_main_context_unref:
 * @context: (not nullable): a #GMainContext
 * 
 * Decreases the reference count on a [struct@GLib.MainContext] object by one.
 * If
 * the result is zero, free the context and free all associated memory.
 **/
void
g_main_context_unref (GMainContext *context)
{
  GSourceIter iter;
  GSource *source;
  GList *sl_iter;
  GSList *s_iter, *remaining_sources = NULL;
  GSourceList *list;
  guint i;
  guint old_ref;
  GSource **pending_dispatches;
  gsize pending_dispatches_len;

  g_return_if_fail (context != NULL);
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0); 

retry_decrement:
  old_ref = g_atomic_int_get (&context->ref_count);
  if (old_ref > 1)
    {
      if (!g_atomic_int_compare_and_exchange (&context->ref_count, old_ref, old_ref - 1))
        goto retry_decrement;

      return;
    }

  g_rw_lock_writer_lock (&source_destroy_lock);

  /* if a weak ref got to the source_destroy lock first, we need to retry */
  old_ref = g_atomic_int_add (&context->ref_count, -1);
  if (old_ref != 1)
    {
      g_rw_lock_writer_unlock (&source_destroy_lock);
      return;
    }

  LOCK_CONTEXT (context);
  pending_dispatches = (GSource **) g_ptr_array_steal (context->pending_dispatches, &pending_dispatches_len);
  UNLOCK_CONTEXT (context);

  /* Free pending dispatches */
  for (i = 0; i < pending_dispatches_len; i++)
    g_source_unref_internal (pending_dispatches[i], context, FALSE);

  g_clear_pointer (&pending_dispatches, g_free);

  /* g_source_iter_next() assumes the context is locked. */
  LOCK_CONTEXT (context);

  /* First collect all remaining sources from the sources lists and store a
   * new reference in a separate list. Also set the context of the sources
   * to NULL so that they can't access a partially destroyed context anymore.
   *
   * We have to do this first so that we have a strong reference to all
   * sources and destroying them below does not also free them, and so that
   * none of the sources can access the context from their finalize/dispose
   * functions. */
  g_source_iter_init (&iter, context, FALSE);
  while (g_source_iter_next (&iter, &source))
    {
      source->context = NULL;
      remaining_sources = g_slist_prepend (remaining_sources, g_source_ref (source));
    }
  g_source_iter_clear (&iter);

  g_rw_lock_writer_unlock (&source_destroy_lock);

  /* Next destroy all sources. As we still hold a reference to all of them,
   * this won't cause any of them to be freed yet and especially prevents any
   * source that unrefs another source from its finalize function to be freed.
   */
  for (s_iter = remaining_sources; s_iter; s_iter = s_iter->next)
    {
      source = s_iter->data;
      g_source_destroy_internal (source, context, TRUE);
    }

  /* the context is going to die now */
  g_return_if_fail (old_ref > 0);

  sl_iter = context->source_lists.head;
  while (sl_iter != NULL)
    {
      list = sl_iter->data;
      sl_iter = sl_iter->next;
      g_slice_free (GSourceList, list);
    }

  g_hash_table_remove_all (context->sources);

  UNLOCK_CONTEXT (context);

  /* if the object has been reffed meanwhile by an internal weak ref, keep the
   * resources alive until the last reference is gone.
   */
  if (old_ref == 1)
    {
      g_mutex_clear (&context->mutex);

      g_ptr_array_free (context->pending_dispatches, TRUE);
      g_free (context->cached_poll_array);

      poll_rec_list_free (context, context->poll_records);

      g_wakeup_free (context->wakeup);
      g_cond_clear (&context->cond);

      g_hash_table_unref (context->sources);

      g_free (context);
    }

  /* And now finally get rid of our references to the sources. This will cause
   * them to be freed unless something else still has a reference to them. Due
   * to setting the context pointers in the sources to NULL above, this won't
   * ever access the context or the internal linked list inside the GSource.
   * We already removed the sources completely from the context above. */
  for (s_iter = remaining_sources; s_iter; s_iter = s_iter->next)
    {
      source = s_iter->data;
      g_source_unref_internal (source, NULL, FALSE);
    }
  g_slist_free (remaining_sources);
}

/* Helper function used by mainloop/overflow test.
 */
GMainContext *
g_main_context_new_with_next_id (guint next_id)
{
  GMainContext *ret = g_main_context_new ();
  
  ret->next_id = next_id;
  
  return ret;
}

/**
 * g_main_context_new:
 *
 * Creates a new [struct@GLib.MainContext] structure.
 *
 * Returns: the new #GMainContext
 **/
GMainContext *
g_main_context_new (void)
{
  return g_main_context_new_with_flags (G_MAIN_CONTEXT_FLAGS_NONE);
}

/**
 * g_main_context_new_with_flags:
 * @flags: a bitwise-OR combination of #GMainContextFlags flags that can only be
 *         set at creation time.
 *
 * Creates a new [struct@GLib.MainContext] structure.
 *
 * Returns: (transfer full): the new #GMainContext
 *
 * Since: 2.72
 */
GMainContext *
g_main_context_new_with_flags (GMainContextFlags flags)
{
  static gsize initialised;
  GMainContext *context;

  if (g_once_init_enter (&initialised))
    {
#ifdef G_MAIN_POLL_DEBUG
      if (g_getenv ("G_MAIN_POLL_DEBUG") != NULL)
        _g_main_poll_debug = TRUE;
#endif

      g_once_init_leave (&initialised, TRUE);
    }

  context = g_new0 (GMainContext, 1);

  TRACE (GLIB_MAIN_CONTEXT_NEW (context));

  g_mutex_init (&context->mutex);
  g_cond_init (&context->cond);

  context->sources = g_hash_table_new (g_uint_hash, g_uint_equal);
  context->owner = NULL;
  context->flags = flags;
  context->waiters = NULL;

  context->ref_count = 1;

  context->next_id = 1;

  context->poll_func = g_poll;
  
  context->cached_poll_array = NULL;
  context->cached_poll_array_size = 0;
  
  context->pending_dispatches = g_ptr_array_new ();
  
  context->time_is_fresh = FALSE;
  
  context->wakeup = g_wakeup_new ();
  g_wakeup_get_pollfd (context->wakeup, &context->wake_up_rec);
  g_main_context_add_poll_unlocked (context, 0, &context->wake_up_rec);

#ifdef G_MAIN_POLL_DEBUG
  if (_g_main_poll_debug)
    g_print ("created context=%p\n", context);
#endif

  return context;
}

/**
 * g_main_context_default:
 *
 * Returns the global-default main context. This is the main context
 * used for main loop functions when a main loop is not explicitly
 * specified, and corresponds to the "main" main loop. See also
 * [func@GLib.MainContext.get_thread_default].
 *
 * Returns: (transfer none): the global-default main context.
 **/
GMainContext *
g_main_context_default (void)
{
  static GMainContext *default_main_context = NULL;

  if (g_once_init_enter_pointer (&default_main_context))
    {
      GMainContext *context;

      context = g_main_context_new ();

      TRACE (GLIB_MAIN_CONTEXT_DEFAULT (context));

#ifdef G_MAIN_POLL_DEBUG
      if (_g_main_poll_debug)
        g_print ("global-default main context=%p\n", context);
#endif

      g_once_init_leave_pointer (&default_main_context, context);
    }

  return default_main_context;
}

static void
free_context (gpointer data)
{
  GMainContext *context = data;

  TRACE (GLIB_MAIN_CONTEXT_FREE (context));

  g_main_context_release (context);
  if (context)
    g_main_context_unref (context);
}

static void
free_context_stack (gpointer data)
{
  g_queue_free_full((GQueue *) data, (GDestroyNotify) free_context);
}

static GPrivate thread_context_stack = G_PRIVATE_INIT (free_context_stack);

/**
 * g_main_context_push_thread_default:
 * @context: (nullable): a #GMainContext, or %NULL for the global-default
 *   main context
 *
 * Acquires @context and sets it as the thread-default context for the
 * current thread. This will cause certain asynchronous operations
 * (such as most [Gio](../gio/index.html)-based I/O) which are
 * started in this thread to run under @context and deliver their
 * results to its main loop, rather than running under the global
 * default main context in the main thread. Note that calling this function
 * changes the context returned by [func@GLib.MainContext.get_thread_default],
 * not the one returned by [func@GLib.MainContext.default], so it does not
 * affect the context used by functions like [func@GLib.idle_add].
 *
 * Normally you would call this function shortly after creating a new
 * thread, passing it a [struct@GLib.MainContext] which will be run by a
 * [struct@GLib.MainLoop] in that thread, to set a new default context for all
 * async operations in that thread. In this case you may not need to
 * ever call [method@GLib.MainContext.pop_thread_default], assuming you want
 * the new [struct@GLib.MainContext] to be the default for the whole lifecycle
 * of the thread.
 *
 * If you don't have control over how the new thread was created (e.g.
 * in the new thread isn't newly created, or if the thread life
 * cycle is managed by a #GThreadPool), it is always suggested to wrap
 * the logic that needs to use the new [struct@GLib.MainContext] inside a
 * [method@GLib.MainContext.push_thread_default] /
 * [method@GLib.MainContext.pop_thread_default] pair, otherwise threads that
 * are re-used will end up never explicitly releasing the
 * [struct@GLib.MainContext] reference they hold.
 *
 * In some cases you may want to schedule a single operation in a
 * non-default context, or temporarily use a non-default context in
 * the main thread. In that case, you can wrap the call to the
 * asynchronous operation inside a
 * [method@GLib.MainContext.push_thread_default] /
 * [method@GLib.MainContext.pop_thread_default] pair, but it is up to you to
 * ensure that no other asynchronous operations accidentally get
 * started while the non-default context is active.
 *
 * Beware that libraries that predate this function may not correctly
 * handle being used from a thread with a thread-default context. Eg,
 * see g_file_supports_thread_contexts().
 *
 * Since: 2.22
 **/
void
g_main_context_push_thread_default (GMainContext *context)
{
  GQueue *stack;
  gboolean acquired_context;

  acquired_context = g_main_context_acquire (context);
  g_return_if_fail (acquired_context);

  if (context == g_main_context_default ())
    context = NULL;
  else if (context)
    g_main_context_ref (context);

  stack = g_private_get (&thread_context_stack);
  if (!stack)
    {
      stack = g_queue_new ();
      g_private_set (&thread_context_stack, stack);
    }

  g_queue_push_head (stack, context);

  TRACE (GLIB_MAIN_CONTEXT_PUSH_THREAD_DEFAULT (context));
}

/**
 * g_main_context_pop_thread_default:
 * @context: (nullable): a #GMainContext, or %NULL for the global-default
 *   main context
 *
 * Pops @context off the thread-default context stack (verifying that
 * it was on the top of the stack).
 *
 * Since: 2.22
 **/
void
g_main_context_pop_thread_default (GMainContext *context)
{
  GQueue *stack;

  if (context == g_main_context_default ())
    context = NULL;

  stack = g_private_get (&thread_context_stack);

  g_return_if_fail (stack != NULL);
  g_return_if_fail (g_queue_peek_head (stack) == context);

  TRACE (GLIB_MAIN_CONTEXT_POP_THREAD_DEFAULT (context));

  g_queue_pop_head (stack);

  g_main_context_release (context);
  if (context)
    g_main_context_unref (context);
}

/**
 * g_main_context_get_thread_default:
 *
 * Gets the thread-default #GMainContext for this thread. Asynchronous
 * operations that want to be able to be run in contexts other than
 * the default one should call this method or
 * [func@GLib.MainContext.ref_thread_default] to get a
 * [struct@GLib.MainContext] to add their [struct@GLib.Source]s to. (Note that
 * even in single-threaded programs applications may sometimes want to
 * temporarily push a non-default context, so it is not safe to assume that
 * this will always return %NULL if you are running in the default thread.)
 *
 * If you need to hold a reference on the context, use
 * [func@GLib.MainContext.ref_thread_default] instead.
 *
 * Returns: (transfer none) (nullable): the thread-default #GMainContext, or
 * %NULL if the thread-default context is the global-default main context.
 *
 * Since: 2.22
 **/
GMainContext *
g_main_context_get_thread_default (void)
{
  GQueue *stack;

  stack = g_private_get (&thread_context_stack);
  if (stack)
    return g_queue_peek_head (stack);
  else
    return NULL;
}

/**
 * g_main_context_ref_thread_default:
 *
 * Gets the thread-default [struct@GLib.MainContext] for this thread, as with
 * [func@GLib.MainContext.get_thread_default], but also adds a reference to
 * it with [method@GLib.MainContext.ref]. In addition, unlike
 * [func@GLib.MainContext.get_thread_default], if the thread-default context
 * is the global-default context, this will return that
 * [struct@GLib.MainContext] (with a ref added to it) rather than returning
 * %NULL.
 *
 * Returns: (transfer full): the thread-default #GMainContext. Unref
 *     with [method@GLib.MainContext.unref] when you are done with it.
 *
 * Since: 2.32
 */
GMainContext *
g_main_context_ref_thread_default (void)
{
  GMainContext *context;

  context = g_main_context_get_thread_default ();
  if (!context)
    context = g_main_context_default ();
  return g_main_context_ref (context);
}

/* Hooks for adding to the main loop */

/**
 * g_source_new:
 * @source_funcs: structure containing functions that implement
 *                the sources behavior.
 * @struct_size: size of the [struct@GLib.Source] structure to create.
 * 
 * Creates a new [struct@GLib.Source] structure. The size is specified to
 * allow creating structures derived from [struct@GLib.Source] that contain
 * additional data. The size passed in must be at least
 * `sizeof (GSource)`.
 * 
 * The source will not initially be associated with any #GMainContext
 * and must be added to one with [method@GLib.Source.attach] before it will be
 * executed.
 * 
 * Returns: the newly-created #GSource.
 **/
GSource *
g_source_new (GSourceFuncs *source_funcs,
	      guint         struct_size)
{
  GSource *source;

  g_return_val_if_fail (source_funcs != NULL, NULL);
  g_return_val_if_fail (struct_size >= sizeof (GSource), NULL);
  
  source = (GSource*) g_malloc0 (struct_size);
  source->priv = g_slice_new0 (GSourcePrivate);
  source->source_funcs = source_funcs;
  g_atomic_int_set (&source->ref_count, 1);
  
  source->priority = G_PRIORITY_DEFAULT;

  g_atomic_int_set (&source->flags, G_HOOK_FLAG_ACTIVE);

  source->priv->ready_time = -1;

  /* NULL/0 initialization for all other fields */

  TRACE (GLIB_SOURCE_NEW (source, source_funcs->prepare, source_funcs->check,
                          source_funcs->dispatch, source_funcs->finalize,
                          struct_size));

  return source;
}

/**
 * g_source_set_dispose_function:
 * @source: A #GSource to set the dispose function on
 * @dispose: #GSourceDisposeFunc to set on the source
 *
 * Set @dispose as dispose function on @source. @dispose will be called once
 * the reference count of @source reaches 0 but before any of the state of the
 * source is freed, especially before the finalize function is called.
 *
 * This means that at this point @source is still a valid [struct@GLib.Source]
 * and it is allow for the reference count to increase again until @dispose
 * returns.
 *
 * The dispose function can be used to clear any "weak" references to the
 * @source in other data structures in a thread-safe way where it is possible
 * for another thread to increase the reference count of @source again while
 * it is being freed.
 *
 * The finalize function can not be used for this purpose as at that point
 * @source is already partially freed and not valid anymore.
 *
 * This should only ever be called from #GSource implementations.
 *
 * Since: 2.64
 **/
void
g_source_set_dispose_function (GSource            *source,
			       GSourceDisposeFunc  dispose)
{
  gboolean was_unset G_GNUC_UNUSED;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);

  was_unset = g_atomic_pointer_compare_and_exchange (&source->priv->dispose,
                                                     NULL, dispose);
  g_return_if_fail (was_unset);
}

/* Holds context's lock */
static void
g_source_iter_init (GSourceIter  *iter,
		    GMainContext *context,
		    gboolean      may_modify)
{
  iter->context = context;
  iter->current_list = NULL;
  iter->source = NULL;
  iter->may_modify = may_modify;
}

/* Holds context's lock */
static gboolean
g_source_iter_next (GSourceIter *iter, GSource **source)
{
  GSource *next_source;

  if (iter->source)
    next_source = iter->source->next;
  else
    next_source = NULL;

  if (!next_source)
    {
      if (iter->current_list)
	iter->current_list = iter->current_list->next;
      else
	iter->current_list = iter->context->source_lists.head;

      if (iter->current_list)
	{
	  GSourceList *source_list = iter->current_list->data;

	  next_source = source_list->head;
	}
    }

  /* Note: unreffing iter->source could potentially cause its
   * GSourceList to be removed from source_lists (if iter->source is
   * the only source in its list, and it is destroyed), so we have to
   * keep it reffed until after we advance iter->current_list, above.
   *
   * Also we first have to ref the next source before unreffing the
   * previous one as unreffing the previous source can potentially
   * free the next one.
   */
  if (next_source && iter->may_modify)
    g_source_ref (next_source);

  if (iter->source && iter->may_modify)
    g_source_unref_internal (iter->source, iter->context, TRUE);
  iter->source = next_source;

  *source = iter->source;
  return *source != NULL;
}

/* Holds context's lock. Only necessary to call if you broke out of
 * the g_source_iter_next() loop early.
 */
static void
g_source_iter_clear (GSourceIter *iter)
{
  if (iter->source && iter->may_modify)
    {
      g_source_unref_internal (iter->source, iter->context, TRUE);
      iter->source = NULL;
    }
}

/* Holds context's lock
 */
static GSourceList *
find_source_list_for_priority (GMainContext *context,
			       gint          priority,
			       gboolean      create)
{
  GList *iter;
  GSourceList *source_list;

  for (iter = context->source_lists.head; iter; iter = iter->next)
    {
      source_list = iter->data;

      if (source_list->priority == priority)
	return source_list;

      if (source_list->priority > priority)
	{
	  if (!create)
	    return NULL;

	  source_list = g_slice_new0 (GSourceList);
          source_list->link.data = source_list;
	  source_list->priority = priority;
          g_queue_insert_before_link (&context->source_lists,
                                      iter,
                                      &source_list->link);
	  return source_list;
	}
    }

  if (!create)
    return NULL;

  source_list = g_slice_new0 (GSourceList);
  source_list->link.data = source_list;
  source_list->priority = priority;
  g_queue_push_tail_link (&context->source_lists, &source_list->link);

  return source_list;
}

/* Holds context's lock
 */
static void
source_add_to_context (GSource      *source,
		       GMainContext *context)
{
  GSourceList *source_list;
  GSource *prev, *next;

  source_list = find_source_list_for_priority (context, source->priority, TRUE);

  if (source->priv->parent_source)
    {
      g_assert (source_list->head != NULL);

      /* Put the source immediately before its parent */
      prev = source->priv->parent_source->prev;
      next = source->priv->parent_source;
    }
  else
    {
      prev = source_list->tail;
      next = NULL;
    }

  source->next = next;
  if (next)
    next->prev = source;
  else
    source_list->tail = source;
  
  source->prev = prev;
  if (prev)
    prev->next = source;
  else
    source_list->head = source;
}

/* Holds context's lock
 */
static void
source_remove_from_context (GSource      *source,
			    GMainContext *context)
{
  GSourceList *source_list;

  source_list = find_source_list_for_priority (context, source->priority, FALSE);
  g_return_if_fail (source_list != NULL);

  if (source->prev)
    source->prev->next = source->next;
  else
    source_list->head = source->next;

  if (source->next)
    source->next->prev = source->prev;
  else
    source_list->tail = source->prev;

  source->prev = NULL;
  source->next = NULL;

  if (source_list->head == NULL)
    {
      g_queue_unlink (&context->source_lists, &source_list->link);
      g_slice_free (GSourceList, source_list);
    }
}

static guint
g_source_attach_unlocked (GSource      *source,
                          GMainContext *context,
                          gboolean      do_wakeup)
{
  GSList *tmp_list;
  guint id;

  /* The counter may have wrapped, so we must ensure that we do not
   * reuse the source id of an existing source.
   */
  do
    id = context->next_id++;
  while (id == 0 || g_hash_table_contains (context->sources, &id));

  source->context = context;
  source->source_id = id;
  g_source_ref (source);

  g_hash_table_add (context->sources, &source->source_id);

  source_add_to_context (source, context);

  if (!SOURCE_BLOCKED (source))
    {
      tmp_list = source->poll_fds;
      while (tmp_list)
        {
          g_main_context_add_poll_unlocked (context, source->priority, tmp_list->data);
          tmp_list = tmp_list->next;
        }

      for (tmp_list = source->priv->fds; tmp_list; tmp_list = tmp_list->next)
        g_main_context_add_poll_unlocked (context, source->priority, tmp_list->data);
    }

  tmp_list = source->priv->child_sources;
  while (tmp_list)
    {
      g_source_attach_unlocked (tmp_list->data, context, FALSE);
      tmp_list = tmp_list->next;
    }

  /* If another thread has acquired the context, wake it up since it
   * might be in poll() right now.
   */
  if (do_wakeup &&
      (context->flags & G_MAIN_CONTEXT_FLAGS_OWNERLESS_POLLING ||
       (context->owner && context->owner != G_THREAD_SELF)))
    {
      g_wakeup_signal (context->wakeup);
    }

  g_trace_mark (G_TRACE_CURRENT_TIME, 0,
                "GLib", "g_source_attach",
                "%s to context %p",
                (g_source_get_name (source) != NULL) ? g_source_get_name (source) : "(unnamed)",
                context);

  return source->source_id;
}

/**
 * g_source_attach:
 * @source: a #GSource
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * 
 * Adds a [struct@GLib.Source] to a @context so that it will be executed within
 * that context. Remove it by calling [method@GLib.Source.destroy].
 *
 * This function is safe to call from any thread, regardless of which thread
 * the @context is running in.
 *
 * Returns: the ID (greater than 0) for the source within the 
 *   #GMainContext. 
 **/
guint
g_source_attach (GSource      *source,
		 GMainContext *context)
{
  guint result = 0;

  g_return_val_if_fail (source != NULL, 0);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, 0);
  g_return_val_if_fail (source->context == NULL, 0);
  g_return_val_if_fail (!SOURCE_DESTROYED (source), 0);
  
  if (!context)
    context = g_main_context_default ();

  LOCK_CONTEXT (context);

  result = g_source_attach_unlocked (source, context, TRUE);

  TRACE (GLIB_MAIN_SOURCE_ATTACH (g_source_get_name (source), source, context,
                                  result));

  UNLOCK_CONTEXT (context);

  return result;
}

static void
g_source_destroy_internal (GSource      *source,
			   GMainContext *context,
			   gboolean      have_lock)
{
  TRACE (GLIB_MAIN_SOURCE_DESTROY (g_source_get_name (source), source,
                                   context));

  if (!have_lock)
    LOCK_CONTEXT (context);
  
  if (!SOURCE_DESTROYED (source))
    {
      GSList *tmp_list;
      gpointer old_cb_data;
      GSourceCallbackFuncs *old_cb_funcs;

      g_atomic_int_and (&source->flags, ~G_HOOK_FLAG_ACTIVE);

      old_cb_data = source->callback_data;
      old_cb_funcs = source->callback_funcs;

      source->callback_data = NULL;
      source->callback_funcs = NULL;

      if (old_cb_funcs)
	{
	  UNLOCK_CONTEXT (context);
	  old_cb_funcs->unref (old_cb_data);
	  LOCK_CONTEXT (context);
	}

      if (!SOURCE_BLOCKED (source))
	{
	  tmp_list = source->poll_fds;
	  while (tmp_list)
	    {
	      g_main_context_remove_poll_unlocked (context, tmp_list->data);
	      tmp_list = tmp_list->next;
	    }

          for (tmp_list = source->priv->fds; tmp_list; tmp_list = tmp_list->next)
            g_main_context_remove_poll_unlocked (context, tmp_list->data);
	}

      while (source->priv->child_sources)
        g_child_source_remove_internal (source->priv->child_sources->data, context);

      if (source->priv->parent_source)
        g_child_source_remove_internal (source, context);
	  
      g_source_unref_internal (source, context, TRUE);
    }

  if (!have_lock)
    UNLOCK_CONTEXT (context);
}

static GMainContext *
source_dup_main_context (GSource *source)
{
  GMainContext *ret = NULL;

  g_rw_lock_reader_lock (&source_destroy_lock);

  ret = source->context;
  if (ret)
    g_atomic_int_inc (&ret->ref_count);

  g_rw_lock_reader_unlock (&source_destroy_lock);

  return ret;
}

/**
 * g_source_destroy:
 * @source: a #GSource
 *
 * Removes a source from its [struct@GLib.MainContext], if any, and mark it as
 * destroyed.  The source cannot be subsequently added to another
 * context. It is safe to call this on sources which have already been
 * removed from their context.
 *
 * This does not unref the [struct@GLib.Source]: if you still hold a reference,
 * use [method@GLib.Source.unref] to drop it.
 *
 * This function is safe to call from any thread, regardless of which thread
 * the [struct@GLib.MainContext] is running in.
 *
 * If the source is currently attached to a [struct@GLib.MainContext],
 * destroying it will effectively unset the callback similar to calling
 * [method@GLib.Source.set_callback]. This can mean, that the data's
 * #GDestroyNotify gets called right away.
 */
void
g_source_destroy (GSource *source)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  
  context = source_dup_main_context (source);

  if (context)
    {
      g_source_destroy_internal (source, context, FALSE);
      g_main_context_unref (context);
    }
  else
    g_atomic_int_and (&source->flags, ~G_HOOK_FLAG_ACTIVE);
}

/**
 * g_source_get_id:
 * @source: a #GSource
 * 
 * Returns the numeric ID for a particular source. The ID of a source
 * is a positive integer which is unique within a particular main loop 
 * context. The reverse mapping from ID to source is done by
 * [method@GLib.MainContext.find_source_by_id].
 *
 * You can only call this function while the source is associated to a
 * [struct@GLib.MainContext] instance; calling this function before
 * [method@GLib.Source.attach] or after [method@GLib.Source.destroy] yields
 * undefined behavior. The ID returned is unique within the
 * [struct@GLib.MainContext] instance passed to [method@GLib.Source.attach].
 *
 * Returns: the ID (greater than 0) for the source
 **/
guint
g_source_get_id (GSource *source)
{
  guint result;
  GMainContext *context;
  
  g_return_val_if_fail (source != NULL, 0);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, 0);
  context = source_dup_main_context (source);
  g_return_val_if_fail (context != NULL, 0);

  LOCK_CONTEXT (context);
  result = source->source_id;
  UNLOCK_CONTEXT (context);
  
  g_main_context_unref (context);

  return result;
}

/**
 * g_source_get_context:
 * @source: a #GSource
 *
 * Gets the [struct@GLib.MainContext] with which the source is associated.
 *
 * You can call this on a source that has been destroyed, provided
 * that the [struct@GLib.MainContext] it was attached to still exists (in which
 * case it will return that [struct@GLib.MainContext]). In particular, you can
 * always call this function on the source returned from
 * [func@GLib.main_current_source]. But calling this function on a source
 * whose [struct@GLib.MainContext] has been destroyed is an error.
 *
 * If the associated [struct@GLib.MainContext] could be destroy concurrently from
 * a different thread, then this function is not safe to call and
 * [method@GLib.Source.dup_context] should be used instead.
 *
 * Returns: (transfer none) (nullable): the #GMainContext with which the
 *               source is associated, or %NULL if the context has not
 *               yet been added to a source.
 **/
GMainContext *
g_source_get_context (GSource *source)
{
  g_return_val_if_fail (source != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, NULL);
  g_return_val_if_fail (source->context != NULL || !SOURCE_DESTROYED (source), NULL);

  return source->context;
}

/**
 * g_source_dup_context:
 * @source: a #GSource
 *
 * Gets the [struct@GLib.MainContext] with which the source is associated.
 *
 * You can call this on a source that has been destroyed. You can
 * always call this function on the source returned from
 * [func@GLib.main_current_source].
 *
 * Returns: (transfer full) (nullable): the [struct@GLib.MainContext] with which the
 *               source is associated, or `NULL`.
 *
 * Since: 2.86
 **/
GMainContext *
g_source_dup_context (GSource *source)
{
  g_return_val_if_fail (source != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, NULL);
  g_return_val_if_fail (source->context != NULL || !SOURCE_DESTROYED (source), NULL);

  return source_dup_main_context (source);
}

/**
 * g_source_add_poll:
 * @source:a #GSource 
 * @fd: a #GPollFD structure holding information about a file
 *      descriptor to watch.
 *
 * Adds a file descriptor to the set of file descriptors polled for
 * this source. This is usually combined with [ctor@GLib.Source.new] to add an
 * event source. The event source's check function will typically test
 * the @revents field in the #GPollFD struct and return %TRUE if events need
 * to be processed.
 *
 * This API is only intended to be used by implementations of [struct@GLib.Source].
 * Do not call this API on a [struct@GLib.Source] that you did not create.
 *
 * Using this API forces the linear scanning of event sources on each
 * main loop iteration.  Newly-written event sources should try to use
 * `g_source_add_unix_fd` instead of this API.
 **/
void
g_source_add_poll (GSource *source,
		   GPollFD *fd)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (fd != NULL);
  g_return_if_fail (!SOURCE_DESTROYED (source));
  
  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);
  
  source->poll_fds = g_slist_prepend (source->poll_fds, fd);

  if (context)
    {
      if (!SOURCE_BLOCKED (source))
	g_main_context_add_poll_unlocked (context, source->priority, fd);
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

/**
 * g_source_remove_poll:
 * @source:a #GSource 
 * @fd: a #GPollFD structure previously passed to [method@GLib.Source.add_poll].
 *
 * Removes a file descriptor from the set of file descriptors polled for
 * this source. 
 *
 * This API is only intended to be used by implementations of [struct@GLib.Source].
 * Do not call this API on a [struct@GLib.Source] that you did not create.
 **/
void
g_source_remove_poll (GSource *source,
		      GPollFD *fd)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (fd != NULL);
  g_return_if_fail (!SOURCE_DESTROYED (source));
  
  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);
  
  source->poll_fds = g_slist_remove (source->poll_fds, fd);

  if (context)
    {
      if (!SOURCE_BLOCKED (source))
	g_main_context_remove_poll_unlocked (context, fd);
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

/**
 * g_source_add_child_source:
 * @source:a #GSource
 * @child_source: a second #GSource that @source should "poll"
 *
 * Adds @child_source to @source as a "polled" source; when @source is
 * added to a [struct@GLib.MainContext], @child_source will be automatically
 * added with the same priority, when @child_source is triggered, it will
 * cause @source to dispatch (in addition to calling its own
 * callback), and when @source is destroyed, it will destroy
 * @child_source as well. (@source will also still be dispatched if
 * its own prepare/check functions indicate that it is ready.)
 *
 * If you don't need @child_source to do anything on its own when it
 * triggers, you can call g_source_set_dummy_callback() on it to set a
 * callback that does nothing (except return %TRUE if appropriate).
 *
 * @source will hold a reference on @child_source while @child_source
 * is attached to it.
 *
 * This API is only intended to be used by implementations of
 * [struct@GLib.Source]. Do not call this API on a [struct@GLib.Source] that
 * you did not create.
 *
 * Since: 2.28
 **/
void
g_source_add_child_source (GSource *source,
			   GSource *child_source)
{
  GMainContext *context;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (child_source != NULL);
  g_return_if_fail (g_atomic_int_get (&child_source->ref_count) > 0);
  g_return_if_fail (!SOURCE_DESTROYED (source));
  g_return_if_fail (!SOURCE_DESTROYED (child_source));
  g_return_if_fail (child_source->context == NULL);
  g_return_if_fail (child_source->priv->parent_source == NULL);

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);

  TRACE (GLIB_SOURCE_ADD_CHILD_SOURCE (source, child_source));

  source->priv->child_sources = g_slist_prepend (source->priv->child_sources,
						 g_source_ref (child_source));
  child_source->priv->parent_source = source;
  g_source_set_priority_unlocked (child_source, NULL, source->priority);
  if (SOURCE_BLOCKED (source))
    block_source (child_source, NULL);

  if (context)
    {
      g_source_attach_unlocked (child_source, context, TRUE);
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

static void
g_child_source_remove_internal (GSource *child_source,
                                GMainContext *context)
{
  GSource *parent_source = child_source->priv->parent_source;

  parent_source->priv->child_sources =
    g_slist_remove (parent_source->priv->child_sources, child_source);
  child_source->priv->parent_source = NULL;

  g_source_destroy_internal (child_source, context, TRUE);
  g_source_unref_internal (child_source, context, TRUE);
}

/**
 * g_source_remove_child_source:
 * @source:a #GSource
 * @child_source: a #GSource previously passed to
 *     [method@GLib.Source.add_child_source].
 *
 * Detaches @child_source from @source and destroys it.
 *
 * This API is only intended to be used by implementations of #GSource.
 * Do not call this API on a #GSource that you did not create.
 *
 * Since: 2.28
 **/
void
g_source_remove_child_source (GSource *source,
			      GSource *child_source)
{
  GMainContext *context;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (child_source != NULL);
  g_return_if_fail (g_atomic_int_get (&child_source->ref_count) > 0);
  g_return_if_fail (child_source->priv->parent_source == source);
  g_return_if_fail (!SOURCE_DESTROYED (source));
  g_return_if_fail (!SOURCE_DESTROYED (child_source));

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);

  g_child_source_remove_internal (child_source, context);

  if (context)
    {
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

static void
g_source_callback_ref (gpointer cb_data)
{
  GSourceCallback *callback = cb_data;

  g_atomic_int_inc (&callback->ref_count);
}

static void
g_source_callback_unref (gpointer cb_data)
{
  GSourceCallback *callback = cb_data;

  if (g_atomic_int_dec_and_test (&callback->ref_count))
    {
      if (callback->notify)
        callback->notify (callback->data);
      g_free (callback);
    }
}

static void
g_source_callback_get (gpointer     cb_data,
		       GSource     *source, 
		       GSourceFunc *func,
		       gpointer    *data)
{
  GSourceCallback *callback = cb_data;

  *func = callback->func;
  *data = callback->data;
}

static GSourceCallbackFuncs g_source_callback_funcs = {
  g_source_callback_ref,
  g_source_callback_unref,
  g_source_callback_get,
};

/**
 * g_source_set_callback_indirect:
 * @source: the source
 * @callback_data: pointer to callback data "object"
 * @callback_funcs: functions for reference counting @callback_data
 *                  and getting the callback and data
 *
 * Sets the callback function storing the data as a refcounted callback
 * "object". This is used internally. Note that calling 
 * [method@GLib.Source.set_callback_indirect] assumes
 * an initial reference count on @callback_data, and thus
 * @callback_funcs->unref will eventually be called once more
 * than @callback_funcs->ref.
 *
 * It is safe to call this function multiple times on a source which has already
 * been attached to a context. The changes will take effect for the next time
 * the source is dispatched after this call returns.
 **/
void
g_source_set_callback_indirect (GSource              *source,
				gpointer              callback_data,
				GSourceCallbackFuncs *callback_funcs)
{
  GMainContext *context;
  gpointer old_cb_data;
  GSourceCallbackFuncs *old_cb_funcs;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (callback_funcs != NULL || callback_data == NULL);

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);

  if (callback_funcs != &g_source_callback_funcs)
    {
      TRACE (GLIB_SOURCE_SET_CALLBACK_INDIRECT (source, callback_data,
                                                callback_funcs->ref,
                                                callback_funcs->unref,
                                                callback_funcs->get));
    }

  old_cb_data = source->callback_data;
  old_cb_funcs = source->callback_funcs;

  source->callback_data = callback_data;
  source->callback_funcs = callback_funcs;
  
  if (context)
    {
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }

  if (old_cb_funcs)
    old_cb_funcs->unref (old_cb_data);
}

/**
 * g_source_set_callback:
 * @source: the source
 * @func: a callback function
 * @data: the data to pass to callback function
 * @notify: (nullable): a function to call when @data is no longer in use, or %NULL.
 * 
 * Sets the callback function for a source. The callback for a source is
 * called from the source's dispatch function.
 *
 * The exact type of @func depends on the type of source; ie. you
 * should not count on @func being called with @data as its first
 * parameter. Cast @func with [func@GLib.SOURCE_FUNC] to avoid warnings about
 * incompatible function types.
 *
 * See [mainloop memory management](main-loop.html#memory-management-of-sources) for details
 * on how to handle memory management of @data.
 * 
 * Typically, you won't use this function. Instead use functions specific
 * to the type of source you are using, such as [func@GLib.idle_add] or
 * [func@GLib.timeout_add].
 *
 * It is safe to call this function multiple times on a source which has already
 * been attached to a context. The changes will take effect for the next time
 * the source is dispatched after this call returns.
 *
 * Note that [method@GLib.Source.destroy] for a currently attached source has the effect
 * of also unsetting the callback.
 **/
void
g_source_set_callback (GSource        *source,
		       GSourceFunc     func,
		       gpointer        data,
		       GDestroyNotify  notify)
{
  GSourceCallback *new_callback;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);

  TRACE (GLIB_SOURCE_SET_CALLBACK (source, func, data, notify));

  new_callback = g_new (GSourceCallback, 1);

  new_callback->ref_count = 1;
  new_callback->func = func;
  new_callback->data = data;
  new_callback->notify = notify;

  g_source_set_callback_indirect (source, new_callback, &g_source_callback_funcs);
}


/**
 * g_source_set_funcs:
 * @source: a #GSource
 * @funcs: the new #GSourceFuncs
 * 
 * Sets the source functions (can be used to override 
 * default implementations) of an unattached source.
 * 
 * Since: 2.12
 */
void
g_source_set_funcs (GSource     *source,
	           GSourceFuncs *funcs)
{
  g_return_if_fail (source != NULL);
  g_return_if_fail (source->context == NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (funcs != NULL);

  source->source_funcs = funcs;
}

static void
g_source_set_priority_unlocked (GSource      *source,
				GMainContext *context,
				gint          priority)
{
  GSList *tmp_list;
  
  g_return_if_fail (source->priv->parent_source == NULL ||
		    source->priv->parent_source->priority == priority);

  TRACE (GLIB_SOURCE_SET_PRIORITY (source, context, priority));

  if (context)
    {
      /* Remove the source from the context's source and then
       * add it back after so it is sorted in the correct place
       */
      source_remove_from_context (source, context);
    }

  source->priority = priority;

  if (context)
    {
      source_add_to_context (source, context);

      if (!SOURCE_BLOCKED (source))
	{
	  tmp_list = source->poll_fds;
	  while (tmp_list)
	    {
	      g_main_context_remove_poll_unlocked (context, tmp_list->data);
	      g_main_context_add_poll_unlocked (context, priority, tmp_list->data);
	      
	      tmp_list = tmp_list->next;
	    }

          for (tmp_list = source->priv->fds; tmp_list; tmp_list = tmp_list->next)
            {
              g_main_context_remove_poll_unlocked (context, tmp_list->data);
              g_main_context_add_poll_unlocked (context, priority, tmp_list->data);
            }
	}
    }

  if (source->priv->child_sources)
    {
      tmp_list = source->priv->child_sources;
      while (tmp_list)
	{
	  g_source_set_priority_unlocked (tmp_list->data, context, priority);
	  tmp_list = tmp_list->next;
	}
    }
}

/**
 * g_source_set_priority:
 * @source: a #GSource
 * @priority: the new priority.
 *
 * Sets the priority of a source. While the main loop is being run, a
 * source will be dispatched if it is ready to be dispatched and no
 * sources at a higher (numerically smaller) priority are ready to be
 * dispatched.
 *
 * A child source always has the same priority as its parent.  It is not
 * permitted to change the priority of a source once it has been added
 * as a child of another source.
 **/
void
g_source_set_priority (GSource  *source,
		       gint      priority)
{
  GMainContext *context;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (source->priv->parent_source == NULL);

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);
  g_source_set_priority_unlocked (source, context, priority);
  if (context)
    {
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

/**
 * g_source_get_priority:
 * @source: a #GSource
 * 
 * Gets the priority of a source.
 * 
 * Returns: the priority of the source
 **/
gint
g_source_get_priority (GSource *source)
{
  g_return_val_if_fail (source != NULL, 0);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, 0);

  return source->priority;
}

/**
 * g_source_set_ready_time:
 * @source: a #GSource
 * @ready_time: the monotonic time at which the source will be ready,
 *              0 for "immediately", -1 for "never"
 *
 * Sets a #GSource to be dispatched when the given monotonic time is
 * reached (or passed).  If the monotonic time is in the past (as it
 * always will be if @ready_time is 0) then the source will be
 * dispatched immediately.
 *
 * If @ready_time is -1 then the source is never woken up on the basis
 * of the passage of time.
 *
 * Dispatching the source does not reset the ready time.  You should do
 * so yourself, from the source dispatch function.
 *
 * Note that if you have a pair of sources where the ready time of one
 * suggests that it will be delivered first but the priority for the
 * other suggests that it would be delivered first, and the ready time
 * for both sources is reached during the same main context iteration,
 * then the order of dispatch is undefined.
 *
 * It is a no-op to call this function on a #GSource which has already been
 * destroyed with [method@GLib.Source.destroy].
 *
 * This API is only intended to be used by implementations of #GSource.
 * Do not call this API on a #GSource that you did not create.
 *
 * Since: 2.36
 **/
void
g_source_set_ready_time (GSource *source,
                         gint64   ready_time)
{
  GMainContext *context;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);

  if (source->priv->ready_time == ready_time)
    {
      if (context)
        {
          UNLOCK_CONTEXT (context);
          g_main_context_unref (context);
        }
      return;
    }

  source->priv->ready_time = ready_time;

  TRACE (GLIB_SOURCE_SET_READY_TIME (source, ready_time));

  if (context)
    {
      /* Quite likely that we need to change the timeout on the poll */
      if (!SOURCE_BLOCKED (source))
        g_wakeup_signal (context->wakeup);
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

/**
 * g_source_get_ready_time:
 * @source: a #GSource
 *
 * Gets the "ready time" of @source, as set by
 * [method@GLib.Source.set_ready_time].
 *
 * Any time before or equal to the current monotonic time (including 0)
 * is an indication that the source will fire immediately.
 *
 * Returns: the monotonic ready time, -1 for "never"
 **/
gint64
g_source_get_ready_time (GSource *source)
{
  g_return_val_if_fail (source != NULL, -1);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, -1);

  return source->priv->ready_time;
}

/**
 * g_source_set_can_recurse:
 * @source: a #GSource
 * @can_recurse: whether recursion is allowed for this source
 * 
 * Sets whether a source can be called recursively. If @can_recurse is
 * %TRUE, then while the source is being dispatched then this source
 * will be processed normally. Otherwise, all processing of this
 * source is blocked until the dispatch function returns.
 **/
void
g_source_set_can_recurse (GSource  *source,
			  gboolean  can_recurse)
{
  GMainContext *context;
  
  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);
  
  if (can_recurse)
    g_atomic_int_or (&source->flags, G_SOURCE_CAN_RECURSE);
  else
    g_atomic_int_and (&source->flags, ~G_SOURCE_CAN_RECURSE);

  if (context)
    {
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

/**
 * g_source_get_can_recurse:
 * @source: a #GSource
 * 
 * Checks whether a source is allowed to be called recursively.
 * see [method@GLib.Source.set_can_recurse].
 * 
 * Returns: whether recursion is allowed.
 **/
gboolean
g_source_get_can_recurse (GSource  *source)
{
  g_return_val_if_fail (source != NULL, FALSE);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, FALSE);

  return (g_atomic_int_get (&source->flags) & G_SOURCE_CAN_RECURSE) != 0;
}

static void
g_source_set_name_full (GSource    *source,
                        const char *name,
                        gboolean    is_static)
{
  GMainContext *context;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);

  TRACE (GLIB_SOURCE_SET_NAME (source, name));

  /* setting back to NULL is allowed, just because it's
   * weird if get_name can return NULL but you can't
   * set that.
   */

  if (!source->priv->static_name)
    g_free (source->name);

  if (is_static)
    source->name = (char *)name;
  else
    source->name = g_strdup (name);

  source->priv->static_name = is_static;

  if (context)
    {
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }
}

/**
 * g_source_set_name:
 * @source: a #GSource
 * @name: debug name for the source
 *
 * Sets a name for the source, used in debugging and profiling.
 * The name defaults to #NULL.
 *
 * The source name should describe in a human-readable way
 * what the source does. For example, "X11 event queue"
 * or "GTK repaint idle handler" or whatever it is.
 *
 * It is permitted to call this function multiple times, but is not
 * recommended due to the potential performance impact.  For example,
 * one could change the name in the "check" function of a #GSourceFuncs
 * to include details like the event type in the source name.
 *
 * Use caution if changing the name while another thread may be
 * accessing it with [method@GLib.Source.get_name]; that function does not copy
 * the value, and changing the value will free it while the other thread
 * may be attempting to use it.
 *
 * Also see [method@GLib.Source.set_static_name].
 *
 * Since: 2.26
 **/
void
g_source_set_name (GSource    *source,
                   const char *name)
{
  g_source_set_name_full (source, name, FALSE);
}

/**
 * g_source_set_static_name:
 * @source: a #GSource
 * @name: debug name for the source
 *
 * A variant of [method@GLib.Source.set_name] that does not
 * duplicate the @name, and can only be used with
 * string literals.
 *
 * Since: 2.70
 */
void
g_source_set_static_name (GSource    *source,
                          const char *name)
{
  g_source_set_name_full (source, name, TRUE);
}

/**
 * g_source_get_name:
 * @source: a #GSource
 *
 * Gets a name for the source, used in debugging and profiling.  The
 * name may be #NULL if it has never been set with [method@GLib.Source.set_name].
 *
 * Returns: (nullable): the name of the source
 *
 * Since: 2.26
 **/
const char *
g_source_get_name (GSource *source)
{
  g_return_val_if_fail (source != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, NULL);

  return source->name;
}

/**
 * g_source_set_name_by_id:
 * @tag: a #GSource ID
 * @name: debug name for the source
 *
 * Sets the name of a source using its ID.
 *
 * This is a convenience utility to set source names from the return
 * value of [func@GLib.idle_add], [func@GLib.timeout_add], etc.
 *
 * It is a programmer error to attempt to set the name of a non-existent
 * source.
 *
 * More specifically: source IDs can be reissued after a source has been
 * destroyed and therefore it is never valid to use this function with a
 * source ID which may have already been removed.  An example is when
 * scheduling an idle to run in another thread with [func@GLib.idle_add]: the
 * idle may already have run and been removed by the time this function
 * is called on its (now invalid) source ID.  This source ID may have
 * been reissued, leading to the operation being performed against the
 * wrong source.
 *
 * Since: 2.26
 **/
void
g_source_set_name_by_id (guint           tag,
                         const char     *name)
{
  GSource *source;

  g_return_if_fail (tag > 0);

  source = g_main_context_find_source_by_id (NULL, tag);
  if (source == NULL)
    return;

  g_source_set_name (source, name);
}


/**
 * g_source_ref:
 * @source: a #GSource
 * 
 * Increases the reference count on a source by one.
 * 
 * Returns: @source
 **/
GSource *
g_source_ref (GSource *source)
{
  int old_ref G_GNUC_UNUSED;
  g_return_val_if_fail (source != NULL, NULL);

  old_ref = g_atomic_int_add (&source->ref_count, 1);
  /* We allow ref_count == 0 here to allow the dispose function to resurrect
   * the GSource if needed */
  g_return_val_if_fail (old_ref >= 0, NULL);

  return source;
}

/* g_source_unref() but possible to call within context lock
 */
static void
g_source_unref_internal (GSource      *source,
			 GMainContext *context,
			 gboolean      have_lock)
{
  gpointer old_cb_data = NULL;
  GSourceCallbackFuncs *old_cb_funcs = NULL;
  int old_ref;

  g_return_if_fail (source != NULL);

  old_ref = g_atomic_int_get (&source->ref_count);

retry_beginning:
  if (old_ref > 1)
    {
      /* We have many references. If we can decrement the ref counter, we are done. */
      if (!g_atomic_int_compare_and_exchange_full ((int *) &source->ref_count,
                                                   old_ref, old_ref - 1,
                                                   &old_ref))
        goto retry_beginning;

      return;
    }

  g_return_if_fail (old_ref > 0);

  if (!have_lock && context)
    LOCK_CONTEXT (context);

  /* We are about to drop the last reference, there's not guarantee at this
   * point that another thread already changed the value at this point or
   * that is also entering the disposal phase, but there is no much we can do
   * and dropping the reference too early would be still risky since it could
   * lead to a preventive finalization.
   * So let's just get all the threads that reached this point to get in, while
   * the final check on whether is the case or not to continue with the
   * finalization will be done by a final unique atomic dec and test.
   */
  if (old_ref == 1)
    {
      /* If there's a dispose function, call this first */
      GSourceDisposeFunc dispose_func;

      if ((dispose_func = g_atomic_pointer_get (&source->priv->dispose)))
        {
          if (context)
            UNLOCK_CONTEXT (context);
          dispose_func (source);
          if (context)
            LOCK_CONTEXT (context);
        }

      /* At this point the source can have been revived by any of the threads
       * acting on it or it's really ready for being finalized.
       */
      if (!g_atomic_int_compare_and_exchange_full ((int *) &source->ref_count,
                                                   1, 0, &old_ref))
        {
          if (!have_lock && context)
            UNLOCK_CONTEXT (context);

          goto retry_beginning;
        }

      TRACE (GLIB_SOURCE_BEFORE_FREE (source, context,
                                      source->source_funcs->finalize));

      old_cb_data = source->callback_data;
      old_cb_funcs = source->callback_funcs;

      source->callback_data = NULL;
      source->callback_funcs = NULL;

      if (context)
	{
	  if (!SOURCE_DESTROYED (source))
	    g_warning (G_STRLOC ": ref_count == 0, but source was still attached to a context!");
	  source_remove_from_context (source, context);

	  g_hash_table_remove (context->sources, &source->source_id);
	}

      if (source->source_funcs->finalize)
	{
          gint old_ref_count;

          /* Temporarily increase the ref count again so that GSource methods
           * can be called from finalize(). */
          g_atomic_int_inc (&source->ref_count);
	  if (context)
	    UNLOCK_CONTEXT (context);
	  source->source_funcs->finalize (source);
	  if (context)
	    LOCK_CONTEXT (context);
          old_ref_count = g_atomic_int_add (&source->ref_count, -1);
          g_warn_if_fail (old_ref_count == 1);
	}

      if (old_cb_funcs)
        {
          gint old_ref_count;

          /* Temporarily increase the ref count again so that GSource methods
           * can be called from callback_funcs.unref(). */
          g_atomic_int_inc (&source->ref_count);
          if (context)
            UNLOCK_CONTEXT (context);

          old_cb_funcs->unref (old_cb_data);

          if (context)
            LOCK_CONTEXT (context);
          old_ref_count = g_atomic_int_add (&source->ref_count, -1);
          g_warn_if_fail (old_ref_count == 1);
        }

      if (!source->priv->static_name)
        g_free (source->name);
      source->name = NULL;

      g_slist_free (source->poll_fds);
      source->poll_fds = NULL;

      g_slist_free_full (source->priv->fds, g_free);

      while (source->priv->child_sources)
        {
          GSource *child_source = source->priv->child_sources->data;

          source->priv->child_sources =
            g_slist_remove (source->priv->child_sources, child_source);
          child_source->priv->parent_source = NULL;

          g_source_unref_internal (child_source, context, TRUE);
        }

      g_slice_free (GSourcePrivate, source->priv);
      source->priv = NULL;

      g_free (source);
    }

  if (!have_lock && context)
    UNLOCK_CONTEXT (context);
}

/**
 * g_source_unref:
 * @source: a #GSource
 * 
 * Decreases the reference count of a source by one. If the
 * resulting reference count is zero the source and associated
 * memory will be destroyed. 
 **/
void
g_source_unref (GSource *source)
{
  GMainContext *context;

  g_return_if_fail (source != NULL);
  /* refcount is checked inside g_source_unref_internal() */

  context = source_dup_main_context (source);

  g_source_unref_internal (source, context, FALSE);

  if (context)
    g_main_context_unref (context);
}

/**
 * g_main_context_find_source_by_id:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @source_id: the source ID, as returned by [method@GLib.Source.get_id].
 *
 * Finds a #GSource given a pair of context and ID.
 *
 * It is a programmer error to attempt to look up a non-existent source.
 *
 * More specifically: source IDs can be reissued after a source has been
 * destroyed and therefore it is never valid to use this function with a
 * source ID which may have already been removed.  An example is when
 * scheduling an idle to run in another thread with [func@GLib.idle_add]: the
 * idle may already have run and been removed by the time this function
 * is called on its (now invalid) source ID.  This source ID may have
 * been reissued, leading to the operation being performed against the
 * wrong source.
 *
 * Returns: (transfer none): the #GSource
 **/
GSource *
g_main_context_find_source_by_id (GMainContext *context,
                                  guint         source_id)
{
  GSource *source = NULL;
  gconstpointer ptr;

  g_return_val_if_fail (source_id > 0, NULL);

  if (context == NULL)
    context = g_main_context_default ();

  LOCK_CONTEXT (context);
  ptr = g_hash_table_lookup (context->sources, &source_id);
  if (ptr)
    {
      source = G_CONTAINER_OF (ptr, GSource, source_id);
      if (SOURCE_DESTROYED (source))
        source = NULL;
    }
  UNLOCK_CONTEXT (context);

  return source;
}

/**
 * g_main_context_find_source_by_funcs_user_data:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used).
 * @funcs: the @source_funcs passed to [ctor@GLib.Source.new].
 * @user_data: the user data from the callback.
 * 
 * Finds a source with the given source functions and user data.  If
 * multiple sources exist with the same source function and user data,
 * the first one found will be returned.
 * 
 * Returns: (transfer none): the source, if one was found, otherwise %NULL
 **/
GSource *
g_main_context_find_source_by_funcs_user_data (GMainContext *context,
					       GSourceFuncs *funcs,
					       gpointer      user_data)
{
  GSourceIter iter;
  GSource *source;
  
  g_return_val_if_fail (funcs != NULL, NULL);

  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  g_source_iter_init (&iter, context, FALSE);
  while (g_source_iter_next (&iter, &source))
    {
      if (!SOURCE_DESTROYED (source) &&
	  source->source_funcs == funcs &&
	  source->callback_funcs)
	{
	  GSourceFunc callback;
	  gpointer callback_data;

	  source->callback_funcs->get (source->callback_data, source, &callback, &callback_data);
	  
	  if (callback_data == user_data)
	    break;
	}
    }
  g_source_iter_clear (&iter);

  UNLOCK_CONTEXT (context);

  return source;
}

/**
 * g_main_context_find_source_by_user_data:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @user_data: the user_data for the callback.
 * 
 * Finds a source with the given user data for the callback.  If
 * multiple sources exist with the same user data, the first
 * one found will be returned.
 * 
 * Returns: (transfer none): the source, if one was found, otherwise %NULL
 **/
GSource *
g_main_context_find_source_by_user_data (GMainContext *context,
					 gpointer      user_data)
{
  GSourceIter iter;
  GSource *source;
  
  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  g_source_iter_init (&iter, context, FALSE);
  while (g_source_iter_next (&iter, &source))
    {
      if (!SOURCE_DESTROYED (source) &&
	  source->callback_funcs)
	{
	  GSourceFunc callback;
	  gpointer callback_data = NULL;

	  source->callback_funcs->get (source->callback_data, source, &callback, &callback_data);

	  if (callback_data == user_data)
	    break;
	}
    }
  g_source_iter_clear (&iter);

  UNLOCK_CONTEXT (context);

  return source;
}

/**
 * g_source_remove:
 * @tag: the ID of the source to remove.
 *
 * Removes the source with the given ID from the default main context. You must
 * use [method@GLib.Source.destroy] for sources added to a non-default main context.
 *
 * The ID of a #GSource is given by [method@GLib.Source.get_id], or will be
 * returned by the functions [method@GLib.Source.attach], [func@GLib.idle_add],
 * [func@GLib.idle_add_full], [func@GLib.timeout_add],
 * [func@GLib.timeout_add_full], [func@GLib.child_watch_add],
 * [func@GLib.child_watch_add_full], [func@GLib.io_add_watch], and
 * [func@GLib.io_add_watch_full].
 *
 * It is a programmer error to attempt to remove a non-existent source.
 *
 * More specifically: source IDs can be reissued after a source has been
 * destroyed and therefore it is never valid to use this function with a
 * source ID which may have already been removed.  An example is when
 * scheduling an idle to run in another thread with [func@GLib.idle_add]: the
 * idle may already have run and been removed by the time this function
 * is called on its (now invalid) source ID.  This source ID may have
 * been reissued, leading to the operation being performed against the
 * wrong source.
 *
 * Returns: %TRUE if the source was found and removed.
 **/
gboolean
g_source_remove (guint tag)
{
  GSource *source;

  g_return_val_if_fail (tag > 0, FALSE);

  source = g_main_context_find_source_by_id (NULL, tag);
  if (source)
    g_source_destroy (source);
  else
    g_critical ("Source ID %u was not found when attempting to remove it", tag);

  return source != NULL;
}

/**
 * g_source_remove_by_user_data:
 * @user_data: the user_data for the callback.
 * 
 * Removes a source from the default main loop context given the user
 * data for the callback. If multiple sources exist with the same user
 * data, only one will be destroyed.
 * 
 * Returns: %TRUE if a source was found and removed. 
 **/
gboolean
g_source_remove_by_user_data (gpointer user_data)
{
  GSource *source;
  
  source = g_main_context_find_source_by_user_data (NULL, user_data);
  if (source)
    {
      g_source_destroy (source);
      return TRUE;
    }
  else
    return FALSE;
}

/**
 * g_source_remove_by_funcs_user_data:
 * @funcs: The @source_funcs passed to [ctor@GLib.Source.new]
 * @user_data: the user data for the callback
 * 
 * Removes a source from the default main loop context given the
 * source functions and user data. If multiple sources exist with the
 * same source functions and user data, only one will be destroyed.
 * 
 * Returns: %TRUE if a source was found and removed. 
 **/
gboolean
g_source_remove_by_funcs_user_data (GSourceFuncs *funcs,
				    gpointer      user_data)
{
  GSource *source;

  g_return_val_if_fail (funcs != NULL, FALSE);

  source = g_main_context_find_source_by_funcs_user_data (NULL, funcs, user_data);
  if (source)
    {
      g_source_destroy (source);
      return TRUE;
    }
  else
    return FALSE;
}

/**
 * g_clear_handle_id: (skip)
 * @tag_ptr: (not nullable): a pointer to the handler ID
 * @clear_func: (not nullable): the function to call to clear the handler
 *
 * Clears a numeric handler, such as a #GSource ID.
 *
 * @tag_ptr must be a valid pointer to the variable holding the handler.
 *
 * If the ID is zero then this function does nothing.
 * Otherwise, @clear_func is called with the ID as a parameter, and the tag is
 * set to zero.
 *
 * A macro is also included that allows this function to be used without
 * pointer casts.
 *
 * Since: 2.56
 */
#undef g_clear_handle_id
void
g_clear_handle_id (guint            *tag_ptr,
                   GClearHandleFunc  clear_func)
{
  guint _handle_id;

  _handle_id = *tag_ptr;
  if (_handle_id > 0)
    {
      *tag_ptr = 0;
      clear_func (_handle_id);
    }
}

#ifdef G_OS_UNIX
/**
 * g_source_add_unix_fd:
 * @source: a #GSource
 * @fd: the fd to monitor
 * @events: an event mask
 *
 * Monitors @fd for the IO events in @events.
 *
 * The tag returned by this function can be used to remove or modify the
 * monitoring of the fd using [method@GLib.Source.remove_unix_fd] or
 * [method@GLib.Source.modify_unix_fd].
 *
 * It is not necessary to remove the fd before destroying the source; it
 * will be cleaned up automatically.
 *
 * This API is only intended to be used by implementations of #GSource.
 * Do not call this API on a #GSource that you did not create.
 *
 * As the name suggests, this function is not available on Windows.
 *
 * Returns: (not nullable): an opaque tag
 *
 * Since: 2.36
 **/
gpointer
g_source_add_unix_fd (GSource      *source,
                      gint          fd,
                      GIOCondition  events)
{
  GMainContext *context;
  GPollFD *poll_fd;

  g_return_val_if_fail (source != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, NULL);
  g_return_val_if_fail (!SOURCE_DESTROYED (source), NULL);

  poll_fd = g_new (GPollFD, 1);
  poll_fd->fd = fd;
  poll_fd->events = events;
  poll_fd->revents = 0;

  context = source_dup_main_context (source);

  if (context)
    LOCK_CONTEXT (context);

  source->priv->fds = g_slist_prepend (source->priv->fds, poll_fd);

  if (context)
    {
      if (!SOURCE_BLOCKED (source))
        g_main_context_add_poll_unlocked (context, source->priority, poll_fd);
      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }

  return poll_fd;
}

/**
 * g_source_modify_unix_fd:
 * @source: a #GSource
 * @tag: (not nullable): the tag from [method@GLib.Source.add_unix_fd]
 * @new_events: the new event mask to watch
 *
 * Updates the event mask to watch for the fd identified by @tag.
 *
 * @tag is the tag returned from [method@GLib.Source.add_unix_fd].
 *
 * If you want to remove a fd, don't set its event mask to zero.
 * Instead, call [method@GLib.Source.remove_unix_fd].
 *
 * This API is only intended to be used by implementations of #GSource.
 * Do not call this API on a #GSource that you did not create.
 *
 * As the name suggests, this function is not available on Windows.
 *
 * Since: 2.36
 **/
void
g_source_modify_unix_fd (GSource      *source,
                         gpointer      tag,
                         GIOCondition  new_events)
{
  GMainContext *context;
  GPollFD *poll_fd;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (g_slist_find (source->priv->fds, tag));

  context = source_dup_main_context (source);
  poll_fd = tag;

  poll_fd->events = new_events;

  if (context)
    {
      g_main_context_wakeup (context);
      g_main_context_unref (context);
    }
}

/**
 * g_source_remove_unix_fd:
 * @source: a #GSource
 * @tag: (not nullable): the tag from [method@GLib.Source.add_unix_fd]
 *
 * Reverses the effect of a previous call to [method@GLib.Source.add_unix_fd].
 *
 * You only need to call this if you want to remove an fd from being
 * watched while keeping the same source around.  In the normal case you
 * will just want to destroy the source.
 *
 * This API is only intended to be used by implementations of #GSource.
 * Do not call this API on a #GSource that you did not create.
 *
 * As the name suggests, this function is not available on Windows.
 *
 * Since: 2.36
 **/
void
g_source_remove_unix_fd (GSource  *source,
                         gpointer  tag)
{
  GMainContext *context;
  GPollFD *poll_fd;

  g_return_if_fail (source != NULL);
  g_return_if_fail (g_atomic_int_get (&source->ref_count) > 0);
  g_return_if_fail (g_slist_find (source->priv->fds, tag));

  context = source_dup_main_context (source);
  poll_fd = tag;

  if (context)
    LOCK_CONTEXT (context);

  source->priv->fds = g_slist_remove (source->priv->fds, poll_fd);

  if (context)
    {
      if (!SOURCE_BLOCKED (source))
        g_main_context_remove_poll_unlocked (context, poll_fd);

      UNLOCK_CONTEXT (context);
      g_main_context_unref (context);
    }

  g_free (poll_fd);
}

/**
 * g_source_query_unix_fd:
 * @source: a #GSource
 * @tag: (not nullable): the tag from [method@GLib.Source.add_unix_fd]
 *
 * Queries the events reported for the fd corresponding to @tag on
 * @source during the last poll.
 *
 * The return value of this function is only defined when the function
 * is called from the check or dispatch functions for @source.
 *
 * This API is only intended to be used by implementations of #GSource.
 * Do not call this API on a #GSource that you did not create.
 *
 * As the name suggests, this function is not available on Windows.
 *
 * Returns: the conditions reported on the fd
 *
 * Since: 2.36
 **/
GIOCondition
g_source_query_unix_fd (GSource  *source,
                        gpointer  tag)
{
  GPollFD *poll_fd;

  g_return_val_if_fail (source != NULL, 0);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, 0);
  g_return_val_if_fail (g_slist_find (source->priv->fds, tag), 0);

  poll_fd = tag;

  return poll_fd->revents;
}
#endif /* G_OS_UNIX */

/**
 * g_get_current_time:
 * @result: #GTimeVal structure in which to store current time.
 *
 * Equivalent to the UNIX gettimeofday() function, but portable.
 *
 * You may find [func@GLib.get_real_time] to be more convenient.
 *
 * Deprecated: 2.62: #GTimeVal is not year-2038-safe. Use
 *    [func@GLib.get_real_time] instead.
 **/
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
void
g_get_current_time (GTimeVal *result)
{
  gint64 tv;

  g_return_if_fail (result != NULL);

  tv = g_get_real_time ();

  result->tv_sec = tv / 1000000;
  result->tv_usec = tv % 1000000;
}
G_GNUC_END_IGNORE_DEPRECATIONS

/**
 * g_get_real_time:
 *
 * Queries the system wall-clock time.
 *
 * This call is functionally equivalent to [func@GLib.get_current_time] except
 * that the return value is often more convenient than dealing with a
 * #GTimeVal.
 *
 * You should only use this call if you are actually interested in the real
 * wall-clock time. [func@GLib.get_monotonic_time] is probably more useful for
 * measuring intervals.
 *
 * Returns: the number of microseconds since January 1, 1970 UTC.
 *
 * Since: 2.28
 **/
gint64
g_get_real_time (void)
{
#ifndef G_OS_WIN32
  struct timeval r;

  /* this is required on alpha, there the timeval structs are ints
   * not longs and a cast only would fail horribly */
  gettimeofday (&r, NULL);

  return (((gint64) r.tv_sec) * 1000000) + r.tv_usec;
#else
  FILETIME ft;
  guint64 time64;

  GetSystemTimeAsFileTime (&ft);
  memmove (&time64, &ft, sizeof (FILETIME));

  /* Convert from 100s of nanoseconds since 1601-01-01
   * to Unix epoch. This is Y2038 safe.
   */
  time64 -= G_GINT64_CONSTANT (116444736000000000);
  time64 /= 10;

  return time64;
#endif
}

/**
 * g_get_monotonic_time:
 *
 * Queries the system monotonic time.
 *
 * The monotonic clock will always increase and doesn't suffer
 * discontinuities when the user (or NTP) changes the system time.  It
 * may or may not continue to tick during times where the machine is
 * suspended.
 *
 * We try to use the clock that corresponds as closely as possible to
 * the passage of time as measured by system calls such as poll() but it
 * may not always be possible to do this.
 *
 * Returns: the monotonic time, in microseconds
 *
 * Since: 2.28
 **/
#if defined (G_OS_WIN32)
/* NOTE:
 * time_usec = ticks_since_boot * usec_per_sec / ticks_per_sec
 *
 * Doing (ticks_since_boot * usec_per_sec) before the division can overflow 64 bits
 * (ticks_since_boot  / ticks_per_sec) and then multiply would not be accurate enough.
 * So for now we calculate (usec_per_sec / ticks_per_sec) and use floating point
 */
static gdouble g_monotonic_usec_per_tick = 0;

void
g_clock_win32_init (void)
{
  LARGE_INTEGER freq;

  if (!QueryPerformanceFrequency (&freq) || freq.QuadPart == 0)
    {
      /* The documentation says that this should never happen */
      g_assert_not_reached ();
      return;
    }

  g_monotonic_usec_per_tick = (gdouble)G_USEC_PER_SEC / freq.QuadPart;
}

gint64
g_get_monotonic_time (void)
{
  if (G_LIKELY (g_monotonic_usec_per_tick != 0))
    {
      LARGE_INTEGER ticks;

      if (QueryPerformanceCounter (&ticks))
        return (gint64)(ticks.QuadPart * g_monotonic_usec_per_tick);

      g_warning ("QueryPerformanceCounter Failed (%lu)", GetLastError ());
      g_monotonic_usec_per_tick = 0;
    }

  return 0;
}
#elif defined(HAVE_MACH_MACH_TIME_H) /* Mac OS */
gint64
g_get_monotonic_time (void)
{
  mach_timebase_info_data_t timebase_info;
  guint64 val;

  /* we get nanoseconds from mach_absolute_time() using timebase_info */
  mach_timebase_info (&timebase_info);
  val = mach_absolute_time ();

  if (timebase_info.numer != timebase_info.denom)
    {
#ifdef HAVE_UINT128_T
      val = ((__uint128_t) val * (__uint128_t) timebase_info.numer) / timebase_info.denom / 1000;
#else
      guint64 t_high, t_low;
      guint64 result_high, result_low;

      /* 64 bit x 32 bit / 32 bit with 96-bit intermediate 
       * algorithm lifted from qemu */
      t_low = (val & 0xffffffffLL) * (guint64) timebase_info.numer;
      t_high = (val >> 32) * (guint64) timebase_info.numer;
      t_high += (t_low >> 32);
      result_high = t_high / (guint64) timebase_info.denom;
      result_low = (((t_high % (guint64) timebase_info.denom) << 32) +
                    (t_low & 0xffffffff)) /
                   (guint64) timebase_info.denom;
      val = ((result_high << 32) | result_low) / 1000;
#endif
    }
  else
    {
      /* nanoseconds to microseconds */
      val = val / 1000;
    }

  return val;
}
#else
gint64
g_get_monotonic_time (void)
{
  struct timespec ts;
  gint result;

  result = clock_gettime (CLOCK_MONOTONIC, &ts);

  if G_UNLIKELY (result != 0)
    g_error ("GLib requires working CLOCK_MONOTONIC");

  return (((gint64) ts.tv_sec) * 1000000) + (ts.tv_nsec / 1000);
}
#endif

static void
g_main_dispatch_free (gpointer dispatch)
{
  g_free (dispatch);
}

/* Running the main loop */

static GMainDispatch *
get_dispatch (void)
{
  static GPrivate depth_private = G_PRIVATE_INIT (g_main_dispatch_free);
  GMainDispatch *dispatch;

  dispatch = g_private_get (&depth_private);

  if (!dispatch)
    dispatch = g_private_set_alloc0 (&depth_private, sizeof (GMainDispatch));

  return dispatch;
}

/**
 * g_main_depth:
 *
 * Returns the depth of the stack of calls to
 * [method@GLib.MainContext.dispatch] on any #GMainContext in the current thread.
 * That is, when called from the toplevel, it gives 0. When
 * called from within a callback from [method@GLib.MainContext.iteration]
 * (or [method@GLib.MainLoop.run], etc.) it returns 1. When called from within 
 * a callback to a recursive call to [method@GLib.MainContext.iteration],
 * it returns 2. And so forth.
 *
 * This function is useful in a situation like the following:
 * Imagine an extremely simple "garbage collected" system.
 *
 * |[<!-- language="C" --> 
 * static GList *free_list;
 * 
 * gpointer
 * allocate_memory (gsize size)
 * { 
 *   gpointer result = g_malloc (size);
 *   free_list = g_list_prepend (free_list, result);
 *   return result;
 * }
 * 
 * void
 * free_allocated_memory (void)
 * {
 *   GList *l;
 *   for (l = free_list; l; l = l->next);
 *     g_free (l->data);
 *   g_list_free (free_list);
 *   free_list = NULL;
 *  }
 * 
 * [...]
 * 
 * while (TRUE); 
 *  {
 *    g_main_context_iteration (NULL, TRUE);
 *    free_allocated_memory();
 *   }
 * ]|
 *
 * This works from an application, however, if you want to do the same
 * thing from a library, it gets more difficult, since you no longer
 * control the main loop. You might think you can simply use an idle
 * function to make the call to free_allocated_memory(), but that
 * doesn't work, since the idle function could be called from a
 * recursive callback. This can be fixed by using [func@GLib.main_depth]
 *
 * |[<!-- language="C" --> 
 * gpointer
 * allocate_memory (gsize size)
 * { 
 *   FreeListBlock *block = g_new (FreeListBlock, 1);
 *   block->mem = g_malloc (size);
 *   block->depth = g_main_depth ();   
 *   free_list = g_list_prepend (free_list, block);
 *   return block->mem;
 * }
 * 
 * void
 * free_allocated_memory (void)
 * {
 *   GList *l;
 *   
 *   int depth = g_main_depth ();
 *   for (l = free_list; l; );
 *     {
 *       GList *next = l->next;
 *       FreeListBlock *block = l->data;
 *       if (block->depth > depth)
 *         {
 *           g_free (block->mem);
 *           g_free (block);
 *           free_list = g_list_delete_link (free_list, l);
 *         }
 *               
 *       l = next;
 *     }
 *   }
 * ]|
 *
 * There is a temptation to use [func@GLib.main_depth] to solve
 * problems with reentrancy. For instance, while waiting for data
 * to be received from the network in response to a menu item,
 * the menu item might be selected again. It might seem that
 * one could make the menu item's callback return immediately
 * and do nothing if [func@GLib.main_depth] returns a value greater than 1.
 * However, this should be avoided since the user then sees selecting
 * the menu item do nothing. Furthermore, you'll find yourself adding
 * these checks all over your code, since there are doubtless many,
 * many things that the user could do. Instead, you can use the
 * following techniques:
 *
 * 1. Use gtk_widget_set_sensitive() or modal dialogs to prevent
 *    the user from interacting with elements while the main
 *    loop is recursing.
 * 
 * 2. Avoid main loop recursion in situations where you can't handle
 *    arbitrary  callbacks. Instead, structure your code so that you
 *    simply return to the main loop and then get called again when
 *    there is more work to do.
 * 
 * Returns: The main loop recursion level in the current thread
 */
int
g_main_depth (void)
{
  GMainDispatch *dispatch = get_dispatch ();
  return dispatch->depth;
}

/**
 * g_main_current_source:
 *
 * Returns the currently firing source for this thread.
 * 
 * Returns: (transfer none) (nullable): The currently firing source or %NULL.
 *
 * Since: 2.12
 */
GSource *
g_main_current_source (void)
{
  GMainDispatch *dispatch = get_dispatch ();
  return dispatch->source;
}

/**
 * g_source_is_destroyed:
 * @source: a #GSource
 *
 * Returns whether @source has been destroyed.
 *
 * This is important when you operate upon your objects 
 * from within idle handlers, but may have freed the object 
 * before the dispatch of your idle handler.
 *
 * |[<!-- language="C" --> 
 * static gboolean 
 * idle_callback (gpointer data)
 * {
 *   SomeWidget *self = data;
 *    
 *   g_mutex_lock (&self->idle_id_mutex);
 *   // do stuff with self
 *   self->idle_id = 0;
 *   g_mutex_unlock (&self->idle_id_mutex);
 *    
 *   return G_SOURCE_REMOVE;
 * }
 *  
 * static void 
 * some_widget_do_stuff_later (SomeWidget *self)
 * {
 *   g_mutex_lock (&self->idle_id_mutex);
 *   self->idle_id = g_idle_add (idle_callback, self);
 *   g_mutex_unlock (&self->idle_id_mutex);
 * }
 *  
 * static void
 * some_widget_init (SomeWidget *self)
 * {
 *   g_mutex_init (&self->idle_id_mutex);
 *
 *   // ...
 * }
 *
 * static void 
 * some_widget_finalize (GObject *object)
 * {
 *   SomeWidget *self = SOME_WIDGET (object);
 *    
 *   if (self->idle_id)
 *     g_source_remove (self->idle_id);
 *    
 *   g_mutex_clear (&self->idle_id_mutex);
 *
 *   G_OBJECT_CLASS (parent_class)->finalize (object);
 * }
 * ]|
 *
 * This will fail in a multi-threaded application if the 
 * widget is destroyed before the idle handler fires due 
 * to the use after free in the callback. A solution, to 
 * this particular problem, is to check to if the source
 * has already been destroy within the callback.
 *
 * |[<!-- language="C" --> 
 * static gboolean 
 * idle_callback (gpointer data)
 * {
 *   SomeWidget *self = data;
 *   
 *   g_mutex_lock (&self->idle_id_mutex);
 *   if (!g_source_is_destroyed (g_main_current_source ()))
 *     {
 *       // do stuff with self
 *     }
 *   g_mutex_unlock (&self->idle_id_mutex);
 *   
 *   return FALSE;
 * }
 * ]|
 *
 * Calls to this function from a thread other than the one acquired by the
 * [struct@GLib.MainContext] the #GSource is attached to are typically
 * redundant, as the source could be destroyed immediately after this function
 * returns. However, once a source is destroyed it cannot be un-destroyed, so
 * this function can be used for opportunistic checks from any thread.
 *
 * Returns: %TRUE if the source has been destroyed
 *
 * Since: 2.12
 */
gboolean
g_source_is_destroyed (GSource *source)
{
  g_return_val_if_fail (source != NULL, TRUE);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, TRUE);
  return SOURCE_DESTROYED (source);
}

/* Temporarily remove all this source's file descriptors from the
 * poll(), so that if data comes available for one of the file descriptors
 * we don't continually spin in the poll()
 */
/* HOLDS: source->context's lock */
static void
block_source (GSource      *source,
              GMainContext *context)
{
  GSList *tmp_list;

  g_return_if_fail (!SOURCE_BLOCKED (source));

  g_atomic_int_or (&source->flags, G_SOURCE_BLOCKED);

  if (context)
    {
      tmp_list = source->poll_fds;
      while (tmp_list)
        {
          g_main_context_remove_poll_unlocked (context, tmp_list->data);
          tmp_list = tmp_list->next;
        }

      for (tmp_list = source->priv->fds; tmp_list; tmp_list = tmp_list->next)
        g_main_context_remove_poll_unlocked (context, tmp_list->data);
    }

  if (source->priv && source->priv->child_sources)
    {
      tmp_list = source->priv->child_sources;
      while (tmp_list)
	{
	  block_source (tmp_list->data, context);
	  tmp_list = tmp_list->next;
	}
    }
}

/* HOLDS: source->context's lock */
static void
unblock_source (GSource      *source,
                GMainContext *context)
{
  GSList *tmp_list;

  g_return_if_fail (SOURCE_BLOCKED (source)); /* Source already unblocked */
  g_return_if_fail (!SOURCE_DESTROYED (source));

  g_atomic_int_and (&source->flags, ~G_SOURCE_BLOCKED);

  tmp_list = source->poll_fds;
  while (tmp_list)
    {
      g_main_context_add_poll_unlocked (context, source->priority, tmp_list->data);
      tmp_list = tmp_list->next;
    }

  for (tmp_list = source->priv->fds; tmp_list; tmp_list = tmp_list->next)
    g_main_context_add_poll_unlocked (context, source->priority, tmp_list->data);

  if (source->priv && source->priv->child_sources)
    {
      tmp_list = source->priv->child_sources;
      while (tmp_list)
	{
	  unblock_source (tmp_list->data, context);
	  tmp_list = tmp_list->next;
	}
    }
}

/* HOLDS: context's lock */
static void
g_main_dispatch (GMainContext *context)
{
  GMainDispatch *current = get_dispatch ();
  guint i;

  for (i = 0; i < context->pending_dispatches->len; i++)
    {
      GSource *source = context->pending_dispatches->pdata[i];

      context->pending_dispatches->pdata[i] = NULL;
      g_assert (source);

      g_atomic_int_and (&source->flags, ~G_SOURCE_READY);

      if (!SOURCE_DESTROYED (source))
	{
	  gboolean was_in_call;
	  gpointer user_data = NULL;
	  GSourceFunc callback = NULL;
	  GSourceCallbackFuncs *cb_funcs;
	  gpointer cb_data;
	  gboolean need_destroy;

	  gboolean (*dispatch) (GSource *,
				GSourceFunc,
				gpointer);
          GSource *prev_source;
          gint64 begin_time_nsec G_GNUC_UNUSED;

	  dispatch = source->source_funcs->dispatch;
	  cb_funcs = source->callback_funcs;
	  cb_data = source->callback_data;

	  if (cb_funcs)
	    cb_funcs->ref (cb_data);
	  
	  if ((g_atomic_int_get (&source->flags) & G_SOURCE_CAN_RECURSE) == 0)
	    block_source (source, context);
	  
          was_in_call = g_atomic_int_or (&source->flags,
                                         (GSourceFlags) G_HOOK_FLAG_IN_CALL) &
                                         G_HOOK_FLAG_IN_CALL;

	  if (cb_funcs)
	    cb_funcs->get (cb_data, source, &callback, &user_data);

	  UNLOCK_CONTEXT (context);

          /* These operations are safe because 'current' is thread-local
           * and not modified from anywhere but this function.
           */
          prev_source = current->source;
          current->source = source;
          current->depth++;

          begin_time_nsec = G_TRACE_CURRENT_TIME;

          TRACE (GLIB_MAIN_BEFORE_DISPATCH (g_source_get_name (source), source,
                                            dispatch, callback, user_data));
          need_destroy = !(* dispatch) (source, callback, user_data);
          TRACE (GLIB_MAIN_AFTER_DISPATCH (g_source_get_name (source), source,
                                           dispatch, need_destroy));

          g_trace_mark (begin_time_nsec, G_TRACE_CURRENT_TIME - begin_time_nsec,
                        "GLib", "GSource.dispatch",
                        "%s ⇒ %s",
                        (g_source_get_name (source) != NULL) ? g_source_get_name (source) : "(unnamed)",
                        need_destroy ? "destroy" : "keep");

          current->source = prev_source;
          current->depth--;

	  if (cb_funcs)
	    cb_funcs->unref (cb_data);

 	  LOCK_CONTEXT (context);
	  
	  if (!was_in_call)
            g_atomic_int_and (&source->flags, ~G_HOOK_FLAG_IN_CALL);

          if (SOURCE_BLOCKED (source) && !SOURCE_DESTROYED (source))
	    unblock_source (source, context);
	  
	  /* Note: this depends on the fact that we can't switch
	   * sources from one main context to another
	   */
	  if (need_destroy && !SOURCE_DESTROYED (source))
	    {
	      g_assert (source->context == context);
	      g_source_destroy_internal (source, context, TRUE);
	    }
	}
      
      g_source_unref_internal (source, context, TRUE);
    }

  g_ptr_array_set_size (context->pending_dispatches, 0);
}

/**
 * g_main_context_acquire:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * 
 * Tries to become the owner of the specified context.
 * If some other thread is the owner of the context,
 * returns %FALSE immediately. Ownership is properly
 * recursive: the owner can require ownership again
 * and will release ownership when [method@GLib.MainContext.release]
 * is called as many times as [method@GLib.MainContext.acquire].
 *
 * You must be the owner of a context before you
 * can call [method@GLib.MainContext.prepare], [method@GLib.MainContext.query],
 * [method@GLib.MainContext.check], [method@GLib.MainContext.dispatch],
 * [method@GLib.MainContext.release].
 *
 * Since 2.76 @context can be %NULL to use the global-default
 * main context.
 * 
 * Returns: %TRUE if the operation succeeded, and
 *   this thread is now the owner of @context.
 **/
gboolean 
g_main_context_acquire (GMainContext *context)
{
  gboolean result = FALSE;

  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  result = g_main_context_acquire_unlocked (context);

  UNLOCK_CONTEXT (context); 

  return result;
}

static gboolean
g_main_context_acquire_unlocked (GMainContext *context)
{
  GThread *self = G_THREAD_SELF;

  if (!context->owner)
    {
      context->owner = self;
      g_assert (context->owner_count == 0);
      TRACE (GLIB_MAIN_CONTEXT_ACQUIRE (context, TRUE  /* success */));
    }

  if (context->owner == self)
    {
      context->owner_count++;
      return TRUE;
    }
  else
    {
      TRACE (GLIB_MAIN_CONTEXT_ACQUIRE (context, FALSE  /* failure */));
      return FALSE;
    }
}

/**
 * g_main_context_release:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * 
 * Releases ownership of a context previously acquired by this thread
 * with [method@GLib.MainContext.acquire]. If the context was acquired multiple
 * times, the ownership will be released only when [method@GLib.MainContext.release]
 * is called as many times as it was acquired.
 *
 * You must have successfully acquired the context with
 * [method@GLib.MainContext.acquire] before you may call this function.
 **/
void
g_main_context_release (GMainContext *context)
{
  if (context == NULL)
    context = g_main_context_default ();

  LOCK_CONTEXT (context);
  g_main_context_release_unlocked (context);
  UNLOCK_CONTEXT (context);
}

static void
g_main_context_release_unlocked (GMainContext *context)
{
  /* NOTE: We should also have the following assert here:
   * g_return_if_fail (context->owner == G_THREAD_SELF);
   * However, this breaks NetworkManager, which has been (non-compliantly but
   * apparently safely) releasing a #GMainContext from a thread which didn’t
   * acquire it.
   * Breaking that would be quite disruptive, so we won’t do that now. However,
   * GLib reserves the right to add that assertion in future, if doing so would
   * allow for optimisations or refactorings. By that point, NetworkManager will
   * have to have reworked its use of #GMainContext.
   *
   * See: https://gitlab.gnome.org/GNOME/glib/-/merge_requests/3513
   */
  g_return_if_fail (context->owner_count > 0);

  context->owner_count--;
  if (context->owner_count == 0)
    {
      TRACE (GLIB_MAIN_CONTEXT_RELEASE (context));

      context->owner = NULL;

      if (context->waiters)
	{
	  GMainWaiter *waiter = context->waiters->data;
	  gboolean loop_internal_waiter = (waiter->mutex == &context->mutex);
	  context->waiters = g_slist_delete_link (context->waiters,
						  context->waiters);
	  if (!loop_internal_waiter)
	    g_mutex_lock (waiter->mutex);
	  
	  g_cond_signal (waiter->cond);
	  
	  if (!loop_internal_waiter)
	    g_mutex_unlock (waiter->mutex);
	}
    }
}

static gboolean
g_main_context_wait_internal (GMainContext *context,
                              GCond        *cond,
                              GMutex       *mutex)
{
  gboolean result = FALSE;
  GThread *self = G_THREAD_SELF;
  gboolean loop_internal_waiter;
  
  loop_internal_waiter = (mutex == &context->mutex);
  
  if (!loop_internal_waiter)
    LOCK_CONTEXT (context);

  if (context->owner && context->owner != self)
    {
      GMainWaiter waiter;

      waiter.cond = cond;
      waiter.mutex = mutex;

      context->waiters = g_slist_append (context->waiters, &waiter);
      
      if (!loop_internal_waiter)
        UNLOCK_CONTEXT (context);
      g_cond_wait (cond, mutex);
      if (!loop_internal_waiter)
        LOCK_CONTEXT (context);

      context->waiters = g_slist_remove (context->waiters, &waiter);
    }

  if (!context->owner)
    {
      context->owner = self;
      g_assert (context->owner_count == 0);
    }

  if (context->owner == self)
    {
      context->owner_count++;
      result = TRUE;
    }

  if (!loop_internal_waiter)
    UNLOCK_CONTEXT (context); 
  
  return result;
}

/**
 * g_main_context_wait:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @cond: a condition variable
 * @mutex: a mutex, currently held
 *
 * Tries to become the owner of the specified context,
 * as with [method@GLib.MainContext.acquire]. But if another thread
 * is the owner, atomically drop @mutex and wait on @cond until
 * that owner releases ownership or until @cond is signaled, then
 * try again (once) to become the owner.
 *
 * Returns: %TRUE if the operation succeeded, and
 *   this thread is now the owner of @context.
 * Deprecated: 2.58: Use [method@GLib.MainContext.is_owner] and separate
 *    locking instead.
 */
gboolean
g_main_context_wait (GMainContext *context,
                     GCond        *cond,
                     GMutex       *mutex)
{
  if (context == NULL)
    context = g_main_context_default ();

  if (G_UNLIKELY (cond != &context->cond || mutex != &context->mutex))
    {
      static gboolean warned;

      if (!warned)
        {
          g_critical ("WARNING!! g_main_context_wait() will be removed in a future release.  "
                      "If you see this message, please file a bug immediately.");
          warned = TRUE;
        }
    }

  return g_main_context_wait_internal (context, cond, mutex);
}

/**
 * g_main_context_prepare:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @priority: (out) (optional): location to store priority of highest priority
 *            source already ready.
 *
 * Prepares to poll sources within a main loop. The resulting information
 * for polling is determined by calling [method@GLib.MainContext.query].
 *
 * You must have successfully acquired the context with
 * [method@GLib.MainContext.acquire] before you may call this function.
 *
 * Returns: %TRUE if some source is ready to be dispatched
 *               prior to polling.
 **/
gboolean
g_main_context_prepare (GMainContext *context,
			gint         *priority)
{
  gboolean ready;

  if (context == NULL)
    context = g_main_context_default ();
  
  LOCK_CONTEXT (context);

  ready = g_main_context_prepare_unlocked (context, priority);

  UNLOCK_CONTEXT (context);
  
  return ready;
}

static inline int
round_timeout_to_msec (gint64 timeout_usec)
{
  /* We need to round to milliseconds from our internal microseconds for
   * various external API and GPollFunc which requires milliseconds.
   *
   * However, we want to ensure a few invariants for this.
   *
   *   Return == -1 if we have no timeout specified
   *   Return ==  0 if we don't want to block at all
   *   Return  >  0 if we have any timeout to avoid spinning the CPU
   *
   * This does cause jitter if the microsecond timeout is < 1000 usec
   * because that is beyond our precision. However, using ppoll() instead
   * of poll() (when available) avoids this jitter.
   */

  if (timeout_usec == 0)
    return 0;

  if (timeout_usec > 0)
    {
      guint64 timeout_msec = (timeout_usec + 999) / 1000;

      return (int) MIN (timeout_msec, G_MAXINT);
    }

  return -1;
}

static inline gint64
extend_timeout_to_usec (int timeout_msec)
{
  if (timeout_msec >= 0)
    return (gint64) timeout_msec * 1000;

  return -1;
}

static gboolean
g_main_context_prepare_unlocked (GMainContext *context,
                                 gint         *priority)
{
  guint i;
  gint n_ready = 0;
  gint current_priority = G_MAXINT;
  GSource *source;
  GSourceIter iter;

  context->time_is_fresh = FALSE;

  if (context->in_check_or_prepare)
    {
      g_warning ("g_main_context_prepare() called recursively from within a source's check() or "
		 "prepare() member.");
      return FALSE;
    }

  TRACE (GLIB_MAIN_CONTEXT_BEFORE_PREPARE (context));

#if 0
  /* If recursing, finish up current dispatch, before starting over */
  if (context->pending_dispatches)
    {
      if (dispatch)
	g_main_dispatch (context, &current_time);
      
      return TRUE;
    }
#endif

  /* If recursing, clear list of pending dispatches */

  for (i = 0; i < context->pending_dispatches->len; i++)
    {
      if (context->pending_dispatches->pdata[i])
        g_source_unref_internal ((GSource *)context->pending_dispatches->pdata[i], context, TRUE);
    }
  g_ptr_array_set_size (context->pending_dispatches, 0);
  
  /* Prepare all sources */

  context->timeout_usec = -1;
  
  g_source_iter_init (&iter, context, TRUE);
  while (g_source_iter_next (&iter, &source))
    {
      gint64 source_timeout_usec = -1;

      if (SOURCE_DESTROYED (source) || SOURCE_BLOCKED (source))
	continue;
      if ((n_ready > 0) && (source->priority > current_priority))
	break;

      if (!(g_atomic_int_get (&source->flags) & G_SOURCE_READY))
	{
	  gboolean result;
	  gboolean (* prepare) (GSource  *source,
                                gint     *timeout);

          prepare = source->source_funcs->prepare;

          if (prepare)
            {
              gint64 begin_time_nsec G_GNUC_UNUSED;
              int source_timeout_msec = -1;

              context->in_check_or_prepare++;
              UNLOCK_CONTEXT (context);

              begin_time_nsec = G_TRACE_CURRENT_TIME;

              result = (*prepare) (source, &source_timeout_msec);
              TRACE (GLIB_MAIN_AFTER_PREPARE (source, prepare, source_timeout_msec));

              source_timeout_usec = extend_timeout_to_usec (source_timeout_msec);

              g_trace_mark (begin_time_nsec, G_TRACE_CURRENT_TIME - begin_time_nsec,
                            "GLib", "GSource.prepare",
                            "%s ⇒ %s",
                            (g_source_get_name (source) != NULL) ? g_source_get_name (source) : "(unnamed)",
                            result ? "ready" : "unready");

              LOCK_CONTEXT (context);
              context->in_check_or_prepare--;
            }
          else
            result = FALSE;

          if (result == FALSE && source->priv->ready_time != -1)
            {
              if (!context->time_is_fresh)
                {
                  context->time = g_get_monotonic_time ();
                  context->time_is_fresh = TRUE;
                }

              if (source->priv->ready_time <= context->time)
                {
                  source_timeout_usec = 0;
                  result = TRUE;
                }
              else if (source_timeout_usec < 0 ||
                       (source->priv->ready_time < context->time + source_timeout_usec))
                {
                  source_timeout_usec = MAX (0, source->priv->ready_time - context->time);
                }
            }

	  if (result)
	    {
	      GSource *ready_source = source;

	      while (ready_source)
		{
                  g_atomic_int_or (&ready_source->flags, G_SOURCE_READY);
		  ready_source = ready_source->priv->parent_source;
		}
	    }
	}

      if (g_atomic_int_get (&source->flags) & G_SOURCE_READY)
	{
	  n_ready++;
	  current_priority = source->priority;
	  context->timeout_usec = 0;
	}

      if (source_timeout_usec >= 0)
        {
          if (context->timeout_usec < 0)
            context->timeout_usec = source_timeout_usec;
          else
            context->timeout_usec = MIN (context->timeout_usec, source_timeout_usec);
        }
    }
  g_source_iter_clear (&iter);

  TRACE (GLIB_MAIN_CONTEXT_AFTER_PREPARE (context, current_priority, n_ready));
  
  if (priority)
    *priority = current_priority;
  
  return (n_ready > 0);
}

/**
 * g_main_context_query:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @max_priority: maximum priority source to check
 * @timeout_: (out): location to store timeout to be used in polling
 * @fds: (out caller-allocates) (array length=n_fds): location to
 *       store #GPollFD records that need to be polled.
 * @n_fds: (in): length of @fds.
 *
 * Determines information necessary to poll this main loop. You should
 * be careful to pass the resulting @fds array and its length @n_fds
 * as is when calling [method@GLib.MainContext.check], as this function relies
 * on assumptions made when the array is filled.
 *
 * You must have successfully acquired the context with
 * [method@GLib.MainContext.acquire] before you may call this function.
 *
 * Returns: the number of records actually stored in @fds,
 *   or, if more than @n_fds records need to be stored, the number
 *   of records that need to be stored.
 **/
gint
g_main_context_query (GMainContext *context,
		      gint          max_priority,
		      gint         *timeout_msec,
		      GPollFD      *fds,
		      gint          n_fds)
{
  gint64 timeout_usec;
  gint n_poll;

  if (context == NULL)
    context = g_main_context_default ();

  LOCK_CONTEXT (context);

  n_poll = g_main_context_query_unlocked (context, max_priority, &timeout_usec, fds, n_fds);

  UNLOCK_CONTEXT (context);

  if (timeout_msec != NULL)
    *timeout_msec = round_timeout_to_msec (timeout_usec);

  return n_poll;
}

static gint
g_main_context_query_unlocked (GMainContext *context,
                               gint          max_priority,
                               gint64       *timeout_usec,
                               GPollFD      *fds,
                               gint          n_fds)
{
  gint n_poll;
  GPollRec *pollrec, *lastpollrec;
  gushort events;
  
  TRACE (GLIB_MAIN_CONTEXT_BEFORE_QUERY (context, max_priority));

  /* fds is filled sequentially from poll_records. Since poll_records
   * are incrementally sorted by file descriptor identifier, fds will
   * also be incrementally sorted.
   */
  n_poll = 0;
  lastpollrec = NULL;
  for (pollrec = context->poll_records; pollrec; pollrec = pollrec->next)
    {
      if (pollrec->priority > max_priority)
        continue;

      /* In direct contradiction to the Unix98 spec, IRIX runs into
       * difficulty if you pass in POLLERR, POLLHUP or POLLNVAL
       * flags in the events field of the pollfd while it should
       * just ignoring them. So we mask them out here.
       */
      events = pollrec->fd->events & ~(G_IO_ERR|G_IO_HUP|G_IO_NVAL);

      /* This optimization --using the same GPollFD to poll for more
       * than one poll record-- relies on the poll records being
       * incrementally sorted.
       */
      if (lastpollrec && pollrec->fd->fd == lastpollrec->fd->fd)
        {
          if (n_poll - 1 < n_fds)
            fds[n_poll - 1].events |= events;
        }
      else
        {
          if (n_poll < n_fds)
            {
              fds[n_poll].fd = pollrec->fd->fd;
              fds[n_poll].events = events;
              fds[n_poll].revents = 0;
            }

          n_poll++;
        }

      lastpollrec = pollrec;
    }

  context->poll_changed = FALSE;

  if (timeout_usec)
    {
      *timeout_usec = context->timeout_usec;
      if (*timeout_usec != 0)
        context->time_is_fresh = FALSE;
    }

  TRACE (GLIB_MAIN_CONTEXT_AFTER_QUERY (context, context->timeout_usec,
                                        fds, n_poll));

  return n_poll;
}

/**
 * g_main_context_check:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @max_priority: the maximum numerical priority of sources to check
 * @fds: (array length=n_fds): array of #GPollFD's that was passed to
 *       the last call to [method@GLib.MainContext.query]
 * @n_fds: return value of [method@GLib.MainContext.query]
 *
 * Passes the results of polling back to the main loop. You should be
 * careful to pass @fds and its length @n_fds as received from
 * [method@GLib.MainContext.query], as this functions relies on assumptions
 * on how @fds is filled.
 *
 * You must have successfully acquired the context with
 * [method@GLib.MainContext.acquire] before you may call this function.
 *
 * Since 2.76 @context can be %NULL to use the global-default
 * main context.
 *
 * Returns: %TRUE if some sources are ready to be dispatched.
 **/
gboolean
g_main_context_check (GMainContext *context,
		      gint          max_priority,
		      GPollFD      *fds,
		      gint          n_fds)
{
  gboolean ready;
   
  LOCK_CONTEXT (context);

  ready = g_main_context_check_unlocked (context, max_priority, fds, n_fds);

  UNLOCK_CONTEXT (context);

  return ready;
}

static gboolean
g_main_context_check_unlocked (GMainContext *context,
                               gint          max_priority,
                               GPollFD      *fds,
                               gint          n_fds)
{
  GSource *source;
  GSourceIter iter;
  GPollRec *pollrec;
  gint n_ready = 0;
  gint i;

  if (context == NULL)
    context = g_main_context_default ();
   
  if (context->in_check_or_prepare)
    {
      g_warning ("g_main_context_check() called recursively from within a source's check() or "
		 "prepare() member.");
      return FALSE;
    }

  TRACE (GLIB_MAIN_CONTEXT_BEFORE_CHECK (context, max_priority, fds, n_fds));

  for (i = 0; i < n_fds; i++)
    {
      if (fds[i].fd == context->wake_up_rec.fd)
        {
          if (fds[i].revents)
            {
              TRACE (GLIB_MAIN_CONTEXT_WAKEUP_ACKNOWLEDGE (context));
              g_wakeup_acknowledge (context->wakeup);
            }
          break;
        }
    }

  /* If the set of poll file descriptors changed, bail out
   * and let the main loop rerun
   */
  if (context->poll_changed)
    {
      TRACE (GLIB_MAIN_CONTEXT_AFTER_CHECK (context, 0));

      return FALSE;
    }

  /* The linear iteration below relies on the assumption that both
   * poll records and the fds array are incrementally sorted by file
   * descriptor identifier.
   */
  pollrec = context->poll_records;
  i = 0;
  while (pollrec && i < n_fds)
    {
      /* Make sure that fds is sorted by file descriptor identifier. */
      g_assert (i <= 0 || fds[i - 1].fd < fds[i].fd);

      /* Skip until finding the first GPollRec matching the current GPollFD. */
      while (pollrec && pollrec->fd->fd != fds[i].fd)
        pollrec = pollrec->next;

      /* Update all consecutive GPollRecs that match. */
      while (pollrec && pollrec->fd->fd == fds[i].fd)
        {
          if (pollrec->priority <= max_priority)
            {
              pollrec->fd->revents =
                fds[i].revents & (pollrec->fd->events | G_IO_ERR | G_IO_HUP | G_IO_NVAL);
            }
          pollrec = pollrec->next;
        }

      /* Iterate to next GPollFD. */
      i++;
    }

  g_source_iter_init (&iter, context, TRUE);
  while (g_source_iter_next (&iter, &source))
    {
      if (SOURCE_DESTROYED (source) || SOURCE_BLOCKED (source))
	continue;
      if ((n_ready > 0) && (source->priority > max_priority))
	break;

      if (!(g_atomic_int_get (&source->flags) & G_SOURCE_READY))
        {
          gboolean result;
          gboolean (* check) (GSource *source);

          check = source->source_funcs->check;

          if (check)
            {
              gint64 begin_time_nsec G_GNUC_UNUSED;

              /* If the check function is set, call it. */
              context->in_check_or_prepare++;
              UNLOCK_CONTEXT (context);

              begin_time_nsec = G_TRACE_CURRENT_TIME;

              result = (* check) (source);

              TRACE (GLIB_MAIN_AFTER_CHECK (source, check, result));

              g_trace_mark (begin_time_nsec, G_TRACE_CURRENT_TIME - begin_time_nsec,
                            "GLib", "GSource.check",
                            "%s ⇒ %s",
                            (g_source_get_name (source) != NULL) ? g_source_get_name (source) : "(unnamed)",
                            result ? "dispatch" : "ignore");

              LOCK_CONTEXT (context);
              context->in_check_or_prepare--;
            }
          else
            result = FALSE;

          if (result == FALSE)
            {
              GSList *tmp_list;

              /* If not already explicitly flagged ready by ->check()
               * (or if we have no check) then we can still be ready if
               * any of our fds poll as ready.
               */
              for (tmp_list = source->priv->fds; tmp_list; tmp_list = tmp_list->next)
                {
                  GPollFD *pollfd = tmp_list->data;

                  if (pollfd->revents)
                    {
                      result = TRUE;
                      break;
                    }
                }
            }

          if (result == FALSE && source->priv->ready_time != -1)
            {
              if (!context->time_is_fresh)
                {
                  context->time = g_get_monotonic_time ();
                  context->time_is_fresh = TRUE;
                }

              if (source->priv->ready_time <= context->time)
                result = TRUE;
            }

	  if (result)
	    {
	      GSource *ready_source = source;

	      while (ready_source)
		{
                  g_atomic_int_or (&ready_source->flags, G_SOURCE_READY);
		  ready_source = ready_source->priv->parent_source;
		}
	    }
	}

      if (g_atomic_int_get (&source->flags) & G_SOURCE_READY)
	{
          g_source_ref (source);
	  g_ptr_array_add (context->pending_dispatches, source);

	  n_ready++;

          /* never dispatch sources with less priority than the first
           * one we choose to dispatch
           */
          max_priority = source->priority;
	}
    }
  g_source_iter_clear (&iter);

  TRACE (GLIB_MAIN_CONTEXT_AFTER_CHECK (context, n_ready));

  return n_ready > 0;
}

/**
 * g_main_context_dispatch:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 *
 * Dispatches all pending sources.
 *
 * You must have successfully acquired the context with
 * [method@GLib.MainContext.acquire] before you may call this function.
 *
 * Since 2.76 @context can be %NULL to use the global-default
 * main context.
 **/
void
g_main_context_dispatch (GMainContext *context)
{
  if (context == NULL)
    context = g_main_context_default ();

  LOCK_CONTEXT (context);

  g_main_context_dispatch_unlocked (context);

  UNLOCK_CONTEXT (context);
}

static void
g_main_context_dispatch_unlocked (GMainContext *context)
{
  TRACE (GLIB_MAIN_CONTEXT_BEFORE_DISPATCH (context));

  if (context->pending_dispatches->len > 0)
    {
      g_main_dispatch (context);
    }

  TRACE (GLIB_MAIN_CONTEXT_AFTER_DISPATCH (context));
}

/* HOLDS context lock */
static gboolean
g_main_context_iterate_unlocked (GMainContext *context,
                                 gboolean      block,
                                 gboolean      dispatch,
                                 GThread      *self)
{
  gint max_priority = 0;
  gint64 timeout_usec;
  gboolean some_ready;
  gint nfds, allocated_nfds;
  GPollFD *fds = NULL;
  gint64 begin_time_nsec G_GNUC_UNUSED;

  begin_time_nsec = G_TRACE_CURRENT_TIME;

  if (!g_main_context_acquire_unlocked (context))
    {
      gboolean got_ownership;

      if (!block)
	return FALSE;

      got_ownership = g_main_context_wait_internal (context,
                                                    &context->cond,
                                                    &context->mutex);

      if (!got_ownership)
	return FALSE;
    }
  
  if (!context->cached_poll_array)
    {
      context->cached_poll_array_size = context->n_poll_records;
      context->cached_poll_array = g_new (GPollFD, context->n_poll_records);
    }

  allocated_nfds = context->cached_poll_array_size;
  fds = context->cached_poll_array;
  
  g_main_context_prepare_unlocked (context, &max_priority);

  while ((nfds = g_main_context_query_unlocked (
              context, max_priority, &timeout_usec, fds,
              allocated_nfds)) > allocated_nfds)
    {
      g_free (fds);
      context->cached_poll_array_size = allocated_nfds = nfds;
      context->cached_poll_array = fds = g_new (GPollFD, nfds);
    }

  if (!block)
    timeout_usec = 0;

  g_main_context_poll_unlocked (context, timeout_usec, max_priority, fds, nfds);

  some_ready = g_main_context_check_unlocked (context, max_priority, fds, nfds);
  
  if (dispatch)
    g_main_context_dispatch_unlocked (context);
  
  g_main_context_release_unlocked (context);

  g_trace_mark (begin_time_nsec, G_TRACE_CURRENT_TIME - begin_time_nsec,
                "GLib", "g_main_context_iterate",
                "Context %p, %s ⇒ %s", context, block ? "blocking" : "non-blocking", some_ready ? "dispatched" : "nothing");

  return some_ready;
}

/**
 * g_main_context_pending:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 *
 * Checks if any sources have pending events for the given context.
 * 
 * Returns: %TRUE if events are pending.
 **/
gboolean 
g_main_context_pending (GMainContext *context)
{
  gboolean retval;

  if (!context)
    context = g_main_context_default();

  LOCK_CONTEXT (context);
  retval = g_main_context_iterate_unlocked (context, FALSE, FALSE, G_THREAD_SELF);
  UNLOCK_CONTEXT (context);
  
  return retval;
}

/**
 * g_main_context_iteration:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @may_block: whether the call may block.
 *
 * Runs a single iteration for the given main loop. This involves
 * checking to see if any event sources are ready to be processed,
 * then if no events sources are ready and @may_block is %TRUE, waiting
 * for a source to become ready, then dispatching the highest priority
 * events sources that are ready. Otherwise, if @may_block is %FALSE
 * sources are not waited to become ready, only those highest priority
 * events sources will be dispatched (if any), that are ready at this
 * given moment without further waiting.
 *
 * Note that even when @may_block is %TRUE, it is still possible for
 * [method@GLib.MainContext.iteration] to return %FALSE, since the wait may
 * be interrupted for other reasons than an event source becoming ready.
 *
 * Returns: %TRUE if events were dispatched.
 **/
gboolean
g_main_context_iteration (GMainContext *context, gboolean may_block)
{
  gboolean retval;

  if (!context)
    context = g_main_context_default();
  
  LOCK_CONTEXT (context);
  retval = g_main_context_iterate_unlocked (context, may_block, TRUE, G_THREAD_SELF);
  UNLOCK_CONTEXT (context);
  
  return retval;
}

/**
 * g_main_loop_new:
 * @context: (nullable): a #GMainContext  (if %NULL, the global-default
 *   main context will be used).
 * @is_running: set to %TRUE to indicate that the loop is running. This
 * is not very important since calling [method@GLib.MainLoop.run] will set this
 * to %TRUE anyway.
 * 
 * Creates a new [struct@GLib.MainLoop] structure.
 * 
 * Returns: a new #GMainLoop.
 **/
GMainLoop *
g_main_loop_new (GMainContext *context,
		 gboolean      is_running)
{
  GMainLoop *loop;

  if (!context)
    context = g_main_context_default();
  
  g_main_context_ref (context);

  loop = g_new0 (GMainLoop, 1);
  loop->context = context;
  loop->is_running = is_running != FALSE;
  loop->ref_count = 1;

  TRACE (GLIB_MAIN_LOOP_NEW (loop, context));

  return loop;
}

/**
 * g_main_loop_ref:
 * @loop: a #GMainLoop
 *
 * Increases the reference count on a [struct@GLib.MainLoop] object by one.
 *
 * Returns: @loop
 **/
GMainLoop *
g_main_loop_ref (GMainLoop *loop)
{
  g_return_val_if_fail (loop != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&loop->ref_count) > 0, NULL);

  g_atomic_int_inc (&loop->ref_count);

  return loop;
}

/**
 * g_main_loop_unref:
 * @loop: a #GMainLoop
 *
 * Decreases the reference count on a [struct@GLib.MainLoop] object by one. If
 * the result is zero, free the loop and free all associated memory.
 **/
void
g_main_loop_unref (GMainLoop *loop)
{
  g_return_if_fail (loop != NULL);
  g_return_if_fail (g_atomic_int_get (&loop->ref_count) > 0);

  if (!g_atomic_int_dec_and_test (&loop->ref_count))
    return;

  g_main_context_unref (loop->context);
  g_free (loop);
}

/**
 * g_main_loop_run:
 * @loop: a #GMainLoop
 * 
 * Runs a main loop until [method@GLib.MainLoop.quit] is called on the loop.
 * If this is called for the thread of the loop's #GMainContext,
 * it will process events from the loop, otherwise it will
 * simply wait.
 **/
void 
g_main_loop_run (GMainLoop *loop)
{
  GThread *self = G_THREAD_SELF;

  g_return_if_fail (loop != NULL);
  g_return_if_fail (g_atomic_int_get (&loop->ref_count) > 0);

  /* Hold a reference in case the loop is unreffed from a callback function */
  g_atomic_int_inc (&loop->ref_count);

  LOCK_CONTEXT (loop->context);

  if (!g_main_context_acquire_unlocked (loop->context))
    {
      gboolean got_ownership = FALSE;
      
      /* Another thread owns this context */
      g_atomic_int_set (&loop->is_running, TRUE);

      while (g_atomic_int_get (&loop->is_running) && !got_ownership)
        got_ownership = g_main_context_wait_internal (loop->context,
                                                      &loop->context->cond,
                                                      &loop->context->mutex);
      
      if (!g_atomic_int_get (&loop->is_running))
	{
	  if (got_ownership)
	    g_main_context_release_unlocked (loop->context);

	  UNLOCK_CONTEXT (loop->context);
	  g_main_loop_unref (loop);
	  return;
	}

      g_assert (got_ownership);
    }

  if G_UNLIKELY (loop->context->in_check_or_prepare)
    {
      g_warning ("g_main_loop_run(): called recursively from within a source's "
		 "check() or prepare() member, iteration not possible.");
      g_main_context_release_unlocked (loop->context);
      UNLOCK_CONTEXT (loop->context);
      g_main_loop_unref (loop);
      return;
    }

  g_atomic_int_set (&loop->is_running, TRUE);
  while (g_atomic_int_get (&loop->is_running))
    g_main_context_iterate_unlocked (loop->context, TRUE, TRUE, self);

  g_main_context_release_unlocked (loop->context);

  UNLOCK_CONTEXT (loop->context);
  
  g_main_loop_unref (loop);
}

/**
 * g_main_loop_quit:
 * @loop: a #GMainLoop
 *
 * Stops a [struct@GLib.MainLoop] from running. Any calls to
 * [method@GLib.MainLoop.run] for the loop will return.
 *
 * Note that sources that have already been dispatched when
 * [method@GLib.MainLoop.quit] is called will still be executed.
 **/
void 
g_main_loop_quit (GMainLoop *loop)
{
  g_return_if_fail (loop != NULL);
  g_return_if_fail (g_atomic_int_get (&loop->ref_count) > 0);

  LOCK_CONTEXT (loop->context);
  g_atomic_int_set (&loop->is_running, FALSE);
  g_wakeup_signal (loop->context->wakeup);

  g_cond_broadcast (&loop->context->cond);

  UNLOCK_CONTEXT (loop->context);

  TRACE (GLIB_MAIN_LOOP_QUIT (loop));
}

/**
 * g_main_loop_is_running:
 * @loop: a #GMainLoop.
 *
 * Checks to see if the main loop is currently being run via
 * [method@GLib.MainLoop.run].
 *
 * Returns: %TRUE if the mainloop is currently being run.
 **/
gboolean
g_main_loop_is_running (GMainLoop *loop)
{
  g_return_val_if_fail (loop != NULL, FALSE);
  g_return_val_if_fail (g_atomic_int_get (&loop->ref_count) > 0, FALSE);

  return g_atomic_int_get (&loop->is_running);
}

/**
 * g_main_loop_get_context:
 * @loop: a #GMainLoop.
 * 
 * Returns the [struct@GLib.MainContext] of @loop.
 * 
 * Returns: (transfer none): the [struct@GLib.MainContext] of @loop
 **/
GMainContext *
g_main_loop_get_context (GMainLoop *loop)
{
  g_return_val_if_fail (loop != NULL, NULL);
  g_return_val_if_fail (g_atomic_int_get (&loop->ref_count) > 0, NULL);
 
  return loop->context;
}

/* HOLDS: context's lock */
static void
g_main_context_poll_unlocked (GMainContext *context,
                              gint64        timeout_usec,
                              int           priority,
                              GPollFD      *fds,
                              int           n_fds)
{
#ifdef  G_MAIN_POLL_DEBUG
  GTimer *poll_timer;
  GPollRec *pollrec;
  gint i;
#endif

  GPollFunc poll_func;

  if (n_fds || timeout_usec != 0)
    {
      int ret, errsv;

#ifdef	G_MAIN_POLL_DEBUG
      poll_timer = NULL;
      if (_g_main_poll_debug)
	{
          g_print ("polling context=%p n=%d timeout_usec=%"G_GINT64_FORMAT"\n",
                   context, n_fds, timeout_usec);
          poll_timer = g_timer_new ();
	}
#endif
      poll_func = context->poll_func;

#if defined(HAVE_PPOLL) && defined(HAVE_POLL)
      if (poll_func == g_poll)
        {
          struct timespec spec;
          struct timespec *spec_p = NULL;

          if (timeout_usec > -1)
            {
              spec.tv_sec = timeout_usec / G_USEC_PER_SEC;
              spec.tv_nsec = (timeout_usec % G_USEC_PER_SEC) * 1000L;
              spec_p = &spec;
            }

          UNLOCK_CONTEXT (context);
          ret = ppoll ((struct pollfd *) fds, n_fds, spec_p, NULL);
          LOCK_CONTEXT (context);
        }
      else
#endif
        {
          int timeout_msec = round_timeout_to_msec (timeout_usec);

          UNLOCK_CONTEXT (context);
          ret = (*poll_func) (fds, n_fds, timeout_msec);
          LOCK_CONTEXT (context);
        }

      errsv = errno;
      if (ret < 0 && errsv != EINTR)
	{
#ifndef G_OS_WIN32
	  g_warning ("poll(2) failed due to: %s.",
		     g_strerror (errsv));
#else
	  /* If g_poll () returns -1, it has already called g_warning() */
#endif
	}
      
#ifdef	G_MAIN_POLL_DEBUG
      if (_g_main_poll_debug)
	{
          g_print ("g_main_poll(%d) timeout_usec: %"G_GINT64_FORMAT" - elapsed %12.10f seconds",
                   n_fds,
                   timeout_usec,
                   g_timer_elapsed (poll_timer, NULL));
          g_timer_destroy (poll_timer);
	  pollrec = context->poll_records;

	  while (pollrec != NULL)
	    {
	      i = 0;
	      while (i < n_fds)
		{
		  if (fds[i].fd == pollrec->fd->fd &&
		      pollrec->fd->events &&
		      fds[i].revents)
		    {
		      g_print (" [" G_POLLFD_FORMAT " :", fds[i].fd);
		      if (fds[i].revents & G_IO_IN)
			g_print ("i");
		      if (fds[i].revents & G_IO_OUT)
			g_print ("o");
		      if (fds[i].revents & G_IO_PRI)
			g_print ("p");
		      if (fds[i].revents & G_IO_ERR)
			g_print ("e");
		      if (fds[i].revents & G_IO_HUP)
			g_print ("h");
		      if (fds[i].revents & G_IO_NVAL)
			g_print ("n");
		      g_print ("]");
		    }
		  i++;
		}
	      pollrec = pollrec->next;
	    }
	  g_print ("\n");
	}
#endif
    } /* if (n_fds || timeout_usec != 0) */
}

/**
 * g_main_context_add_poll:
 * @context: (nullable): a #GMainContext (or %NULL for the global-default
 *   main context)
 * @fd: a #GPollFD structure holding information about a file
 *      descriptor to watch.
 * @priority: the priority for this file descriptor which should be
 *      the same as the priority used for [method@GLib.Source.attach] to ensure
 *      that the file descriptor is polled whenever the results may be needed.
 *
 * Adds a file descriptor to the set of file descriptors polled for
 * this context. This will very seldom be used directly. Instead
 * a typical event source will use `g_source_add_unix_fd` instead.
 **/
void
g_main_context_add_poll (GMainContext *context,
			 GPollFD      *fd,
			 gint          priority)
{
  if (!context)
    context = g_main_context_default ();
  
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);
  g_return_if_fail (fd);

  LOCK_CONTEXT (context);
  g_main_context_add_poll_unlocked (context, priority, fd);
  UNLOCK_CONTEXT (context);
}

/* HOLDS: main_loop_lock */
static void 
g_main_context_add_poll_unlocked (GMainContext *context,
				  gint          priority,
				  GPollFD      *fd)
{
  GPollRec *prevrec, *nextrec;
  GPollRec *newrec = g_slice_new (GPollRec);

  /* This file descriptor may be checked before we ever poll */
  fd->revents = 0;
  newrec->fd = fd;
  newrec->priority = priority;

  /* Poll records are incrementally sorted by file descriptor identifier. */
  prevrec = NULL;
  nextrec = context->poll_records;
  while (nextrec)
    {
      if (nextrec->fd->fd > fd->fd)
        break;
      prevrec = nextrec;
      nextrec = nextrec->next;
    }

  if (prevrec)
    prevrec->next = newrec;
  else
    context->poll_records = newrec;

  newrec->prev = prevrec;
  newrec->next = nextrec;

  if (nextrec)
    nextrec->prev = newrec;

  context->n_poll_records++;

  context->poll_changed = TRUE;

  /* Now wake up the main loop if it is waiting in the poll() */
  if (fd != &context->wake_up_rec)
    g_wakeup_signal (context->wakeup);
}

/**
 * g_main_context_remove_poll:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @fd: a #GPollFD descriptor previously added with
 *   [method@GLib.MainContext.add_poll]
 *
 * Removes file descriptor from the set of file descriptors to be
 * polled for a particular context.
 **/
void
g_main_context_remove_poll (GMainContext *context,
			    GPollFD      *fd)
{
  if (!context)
    context = g_main_context_default ();
  
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);
  g_return_if_fail (fd);

  LOCK_CONTEXT (context);
  g_main_context_remove_poll_unlocked (context, fd);
  UNLOCK_CONTEXT (context);
}

static void
g_main_context_remove_poll_unlocked (GMainContext *context,
				     GPollFD      *fd)
{
  GPollRec *pollrec, *prevrec, *nextrec;

  prevrec = NULL;
  pollrec = context->poll_records;

  while (pollrec)
    {
      nextrec = pollrec->next;
      if (pollrec->fd == fd)
	{
	  if (prevrec != NULL)
	    prevrec->next = nextrec;
	  else
	    context->poll_records = nextrec;

	  if (nextrec != NULL)
	    nextrec->prev = prevrec;

	  g_slice_free (GPollRec, pollrec);

	  context->n_poll_records--;
	  break;
	}
      prevrec = pollrec;
      pollrec = nextrec;
    }

  context->poll_changed = TRUE;

  /* Now wake up the main loop if it is waiting in the poll() */
  g_wakeup_signal (context->wakeup);
}

/**
 * g_source_get_current_time:
 * @source:  a #GSource
 * @timeval: #GTimeVal structure in which to store current time.
 *
 * This function ignores @source and is otherwise the same as
 * [func@GLib.get_current_time].
 *
 * Deprecated: 2.28: use [method@GLib.Source.get_time] instead
 **/
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
void
g_source_get_current_time (GSource  *source,
			   GTimeVal *timeval)
{
  g_get_current_time (timeval);
}
G_GNUC_END_IGNORE_DEPRECATIONS

/**
 * g_source_get_time:
 * @source: a #GSource
 *
 * Gets the time to be used when checking this source. The advantage of
 * calling this function over calling [func@GLib.get_monotonic_time] directly is
 * that when checking multiple sources, GLib can cache a single value
 * instead of having to repeatedly get the system monotonic time.
 *
 * The time here is the system monotonic time, if available, or some
 * other reasonable alternative otherwise.  See [func@GLib.get_monotonic_time].
 *
 * Returns: the monotonic time in microseconds
 *
 * Since: 2.28
 **/
gint64
g_source_get_time (GSource *source)
{
  GMainContext *context;
  gint64 result;

  g_return_val_if_fail (source != NULL, 0);
  g_return_val_if_fail (g_atomic_int_get (&source->ref_count) > 0, 0);
  context = source_dup_main_context (source);
  g_return_val_if_fail (context != NULL, 0);

  LOCK_CONTEXT (context);

  if (!context->time_is_fresh)
    {
      context->time = g_get_monotonic_time ();
      context->time_is_fresh = TRUE;
    }

  result = context->time;

  UNLOCK_CONTEXT (context);
  g_main_context_unref (context);

  return result;
}

/**
 * g_main_context_set_poll_func:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * @func: the function to call to poll all file descriptors
 * 
 * Sets the function to use to handle polling of file descriptors. It
 * will be used instead of the poll() system call 
 * (or GLib's replacement function, which is used where 
 * poll() isn't available).
 *
 * This function could possibly be used to integrate the GLib event
 * loop with an external event loop.
 **/
void
g_main_context_set_poll_func (GMainContext *context,
			      GPollFunc     func)
{
  if (!context)
    context = g_main_context_default ();
  
  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);

  LOCK_CONTEXT (context);
  
  if (func)
    context->poll_func = func;
  else
    context->poll_func = g_poll;

  UNLOCK_CONTEXT (context);
}

/**
 * g_main_context_get_poll_func:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * 
 * Gets the poll function set by [method@GLib.MainContext.set_poll_func].
 * 
 * Returns: the poll function
 **/
GPollFunc
g_main_context_get_poll_func (GMainContext *context)
{
  GPollFunc result;
  
  if (!context)
    context = g_main_context_default ();
  
  g_return_val_if_fail (g_atomic_int_get (&context->ref_count) > 0, NULL);

  LOCK_CONTEXT (context);
  result = context->poll_func;
  UNLOCK_CONTEXT (context);

  return result;
}

/**
 * g_main_context_wakeup:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * 
 * If @context is currently blocking in [method@GLib.MainContext.iteration]
 * waiting for a source to become ready, cause it to stop blocking
 * and return.  Otherwise, cause the next invocation of
 * [method@GLib.MainContext.iteration] to return without blocking.
 *
 * This API is useful for low-level control over [struct@GLib.MainContext]; for
 * example, integrating it with main loop implementations such as
 * [struct@GLib.MainLoop].
 *
 * Another related use for this function is when implementing a main
 * loop with a termination condition, computed from multiple threads:
 *
 * |[<!-- language="C" --> 
 *   #define NUM_TASKS 10
 *   static gint tasks_remaining = NUM_TASKS;  // (atomic)
 *   ...
 *  
 *   while (g_atomic_int_get (&tasks_remaining) != 0)
 *     g_main_context_iteration (NULL, TRUE);
 * ]|
 *  
 * Then in a thread:
 * |[<!-- language="C" --> 
 *   perform_work();
 *
 *   if (g_atomic_int_dec_and_test (&tasks_remaining))
 *     g_main_context_wakeup (NULL);
 * ]|
 **/
void
g_main_context_wakeup (GMainContext *context)
{
  if (!context)
    context = g_main_context_default ();

  g_return_if_fail (g_atomic_int_get (&context->ref_count) > 0);

  TRACE (GLIB_MAIN_CONTEXT_WAKEUP (context));

  g_wakeup_signal (context->wakeup);
}

/**
 * g_main_context_is_owner:
 * @context: (nullable): a #GMainContext (if %NULL, the global-default
 *   main context will be used)
 * 
 * Determines whether this thread holds the (recursive)
 * ownership of this [struct@GLib.MainContext]. This is useful to
 * know before waiting on another thread that may be
 * blocking to get ownership of @context.
 *
 * Returns: %TRUE if current thread is owner of @context.
 *
 * Since: 2.10
 **/
gboolean
g_main_context_is_owner (GMainContext *context)
{
  gboolean is_owner;

  if (!context)
    context = g_main_context_default ();

  LOCK_CONTEXT (context);
  is_owner = context->owner == G_THREAD_SELF;
  UNLOCK_CONTEXT (context);

  return is_owner;
}

/* Timeouts */

static void
g_timeout_set_expiration (GTimeoutSource *timeout_source,
                          gint64          current_time)
{
  gint64 expiration;

  if (timeout_source->seconds)
    {
      gint64 remainder;
      static gint timer_perturb = -1;

      if (timer_perturb == -1)
        {
          /*
           * we want a per machine/session unique 'random' value; try the dbus
           * address first, that has a UUID in it. If there is no dbus, use the
           * hostname for hashing.
           */
          const char *session_bus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
          if (!session_bus_address)
            session_bus_address = g_getenv ("HOSTNAME");
          if (session_bus_address)
            timer_perturb = ABS ((gint) g_str_hash (session_bus_address)) % 1000000;
          else
            timer_perturb = 0;
        }

      expiration = current_time + (guint64) timeout_source->interval * 1000 * 1000;

      /* We want the microseconds part of the timeout to land on the
       * 'timer_perturb' mark, but we need to make sure we don't try to
       * set the timeout in the past.  We do this by ensuring that we
       * always only *increase* the expiration time by adding a full
       * second in the case that the microsecond portion decreases.
       */
      expiration -= timer_perturb;

      remainder = expiration % 1000000;
      if (remainder >= 1000000/4)
        expiration += 1000000;

      expiration -= remainder;
      expiration += timer_perturb;
    }
  else
    {
      expiration = current_time + (guint64) timeout_source->interval * 1000;
    }

  g_source_set_ready_time ((GSource *) timeout_source, expiration);
}

static gboolean
g_timeout_dispatch (GSource     *source,
                    GSourceFunc  callback,
                    gpointer     user_data)
{
  GTimeoutSource *timeout_source = (GTimeoutSource *)source;
  gboolean again;

  if (!callback)
    {
      g_warning ("Timeout source dispatched without callback. "
                 "You must call g_source_set_callback().");
      return FALSE;
    }

  if (timeout_source->one_shot)
    {
      GSourceOnceFunc once_callback = (GSourceOnceFunc) callback;
      once_callback (user_data);
      again = G_SOURCE_REMOVE;
    }
  else
    {
      again = callback (user_data);
    }

  TRACE (GLIB_TIMEOUT_DISPATCH (source, source->context, callback, user_data, again));

  if (again)
    g_timeout_set_expiration (timeout_source, g_source_get_time (source));

  return again;
}

static GSource *
timeout_source_new (guint    interval,
                    gboolean seconds,
                    gboolean one_shot)
{
  GSource *source = g_source_new (&g_timeout_funcs, sizeof (GTimeoutSource));
  GTimeoutSource *timeout_source = (GTimeoutSource *)source;

  timeout_source->interval = interval;
  timeout_source->seconds = seconds;
  timeout_source->one_shot = one_shot;

  g_timeout_set_expiration (timeout_source, g_get_monotonic_time ());

  return source;
}

/**
 * g_timeout_source_new:
 * @interval: the timeout interval in milliseconds.
 * 
 * Creates a new timeout source.
 *
 * The source will not initially be associated with any [struct@GLib.MainContext]
 * and must be added to one with [method@GLib.Source.attach] before it will be
 * executed.
 *
 * The interval given is in terms of monotonic time, not wall clock
 * time.  See [func@GLib.get_monotonic_time].
 * 
 * Returns: the newly-created timeout source
 **/
GSource *
g_timeout_source_new (guint interval)
{
  return timeout_source_new (interval, FALSE, FALSE);
}

/**
 * g_timeout_source_new_seconds:
 * @interval: the timeout interval in seconds
 *
 * Creates a new timeout source.
 *
 * The source will not initially be associated with any
 * [struct@GLib.MainContext] and must be added to one with
 * [method@GLib.Source.attach] before it will be executed.
 *
 * The scheduling granularity/accuracy of this timeout source will be
 * in seconds.
 *
 * The interval given is in terms of monotonic time, not wall clock time.
 * See [func@GLib.get_monotonic_time].
 *
 * Returns: the newly-created timeout source
 *
 * Since: 2.14	
 **/
GSource *
g_timeout_source_new_seconds (guint interval)
{
  return timeout_source_new (interval, TRUE, FALSE);
}

static guint
timeout_add_full (gint           priority,
                  guint          interval,
                  gboolean       seconds,
                  gboolean       one_shot,
                  GSourceFunc    function,
                  gpointer       data,
                  GDestroyNotify notify)
{
  GSource *source;
  guint id;

  g_return_val_if_fail (function != NULL, 0);

  source = timeout_source_new (interval, seconds, one_shot);

  if (priority != G_PRIORITY_DEFAULT)
    g_source_set_priority (source, priority);

  g_source_set_callback (source, function, data, notify);
  id = g_source_attach (source, NULL);

  TRACE (GLIB_TIMEOUT_ADD (source, g_main_context_default (), id, priority, interval, function, data));

  g_source_unref (source);

  return id;
}

/**
 * g_timeout_add_full: (rename-to g_timeout_add)
 * @priority: the priority of the timeout source. Typically this will be in
 *   the range between [const@GLib.PRIORITY_DEFAULT] and
 *   [const@GLib.PRIORITY_HIGH].
 * @interval: the time between calls to the function, in milliseconds
 *   (1/1000ths of a second)
 * @function: function to call
 * @data: data to pass to @function
 * @notify: (nullable): function to call when the timeout is removed, or %NULL
 * 
 * Sets a function to be called at regular intervals, with the given
 * priority.  The function is called repeatedly until it returns
 * %FALSE, at which point the timeout is automatically destroyed and
 * the function will not be called again.  The @notify function is
 * called when the timeout is destroyed.  The first call to the
 * function will be at the end of the first @interval.
 *
 * Note that timeout functions may be delayed, due to the processing of other
 * event sources. Thus they should not be relied on for precise timing.
 * After each call to the timeout function, the time of the next
 * timeout is recalculated based on the current time and the given interval
 * (it does not try to 'catch up' time lost in delays).
 *
 * See [mainloop memory management](main-loop.html#memory-management-of-sources) for details
 * on how to handle the return value and memory management of @data.
 *
 * This internally creates a main loop source using
 * [func@GLib.timeout_source_new] and attaches it to the global
 * [struct@GLib.MainContext] using [method@GLib.Source.attach], so the callback
 * will be invoked in whichever thread is running that main context. You can do
 * these steps manually if you need greater control or to use a custom main
 * context.
 *
 * The interval given is in terms of monotonic time, not wall clock time.
 * See [func@GLib.get_monotonic_time].
 * 
 * Returns: the ID (greater than 0) of the event source.
 **/
guint
g_timeout_add_full (gint           priority,
		    guint          interval,
		    GSourceFunc    function,
		    gpointer       data,
		    GDestroyNotify notify)
{
  return timeout_add_full (priority, interval, FALSE, FALSE, function, data, notify);
}

/**
 * g_timeout_add:
 * @interval: the time between calls to the function, in milliseconds
 *    (1/1000ths of a second)
 * @function: function to call
 * @data: data to pass to @function
 *
 * Sets a function to be called at regular intervals, with the default
 * priority, [const@GLib.PRIORITY_DEFAULT].
 *
 * The given @function is called repeatedly until it returns
 * [const@GLib.SOURCE_REMOVE] or %FALSE, at which point the timeout is
 * automatically destroyed and the function will not be called again. The first
 * call to the function will be at the end of the first @interval.
 *
 * Note that timeout functions may be delayed, due to the processing of other
 * event sources. Thus they should not be relied on for precise timing.
 * After each call to the timeout function, the time of the next
 * timeout is recalculated based on the current time and the given interval
 * (it does not try to 'catch up' time lost in delays).
 *
 * See [mainloop memory management](main-loop.html#memory-management-of-sources) for details
 * on how to handle the return value and memory management of @data.
 *
 * If you want to have a timer in the "seconds" range and do not care
 * about the exact time of the first call of the timer, use the
 * [func@GLib.timeout_add_seconds] function; this function allows for more
 * optimizations and more efficient system power usage.
 *
 * This internally creates a main loop source using
 * [func@GLib.timeout_source_new] and attaches it to the global
 * [struct@GLib.MainContext] using [method@GLib.Source.attach], so the callback
 * will be invoked in whichever thread is running that main context. You can do
 * these steps manually if you need greater control or to use a custom main
 * context.
 *
 * It is safe to call this function from any thread.
 *
 * The interval given is in terms of monotonic time, not wall clock
 * time. See [func@GLib.get_monotonic_time].
 *
 * Returns: the ID (greater than 0) of the event source.
 **/
guint
g_timeout_add (guint32        interval,
	       GSourceFunc    function,
	       gpointer       data)
{
  return g_timeout_add_full (G_PRIORITY_DEFAULT, 
			     interval, function, data, NULL);
}

/**
 * g_timeout_add_once:
 * @interval: the time after which the function will be called, in
 *   milliseconds (1/1000ths of a second)
 * @function: function to call
 * @data: data to pass to @function
 *
 * Sets a function to be called after @interval milliseconds have elapsed,
 * with the default priority, [const@GLib.PRIORITY_DEFAULT].
 *
 * The given @function is called once and then the source will be automatically
 * removed from the main context.
 *
 * This function otherwise behaves like [func@GLib.timeout_add].
 *
 * Returns: the ID (greater than 0) of the event source
 *
 * Since: 2.74
 */
guint
g_timeout_add_once (guint32         interval,
                    GSourceOnceFunc function,
                    gpointer        data)
{
  return timeout_add_full (G_PRIORITY_DEFAULT, interval, FALSE, TRUE, (GSourceFunc) function, data, NULL);
}

/**
 * g_timeout_add_seconds_full: (rename-to g_timeout_add_seconds)
 * @priority: the priority of the timeout source. Typically this will be in
 *   the range between [const@GLib.PRIORITY_DEFAULT] and
 *   [const@GLib.PRIORITY_HIGH].
 * @interval: the time between calls to the function, in seconds
 * @function: function to call
 * @data: data to pass to @function
 * @notify: (nullable): function to call when the timeout is removed, or %NULL
 *
 * Sets a function to be called at regular intervals, with @priority.
 *
 * The function is called repeatedly until it returns [const@GLib.SOURCE_REMOVE]
 * or %FALSE, at which point the timeout is automatically destroyed and
 * the function will not be called again.
 *
 * Unlike [func@GLib.timeout_add], this function operates at whole second
 * granularity. The initial starting point of the timer is determined by the
 * implementation and the implementation is expected to group multiple timers
 * together so that they fire all at the same time. To allow this grouping, the
 * @interval to the first timer is rounded and can deviate up to one second
 * from the specified interval. Subsequent timer iterations will generally run
 * at the specified interval.
 *
 * Note that timeout functions may be delayed, due to the processing of other
 * event sources. Thus they should not be relied on for precise timing.
 * After each call to the timeout function, the time of the next
 * timeout is recalculated based on the current time and the given @interval
 *
 * See [mainloop memory management](main-loop.html#memory-management-of-sources) for details
 * on how to handle the return value and memory management of @data.
 *
 * If you want timing more precise than whole seconds, use
 * [func@GLib.timeout_add] instead.
 *
 * The grouping of timers to fire at the same time results in a more power
 * and CPU efficient behavior so if your timer is in multiples of seconds
 * and you don't require the first timer exactly one second from now, the
 * use of [func@GLib.timeout_add_seconds] is preferred over
 * [func@GLib.timeout_add].
 *
 * This internally creates a main loop source using
 * [func@GLib.timeout_source_new_seconds] and attaches it to the main loop
 * context using [method@GLib.Source.attach]. You can do these steps manually
 * if you need greater control.
 *
 * It is safe to call this function from any thread.
 *
 * The interval given is in terms of monotonic time, not wall clock
 * time. See [func@GLib.get_monotonic_time].
 *
 * Returns: the ID (greater than 0) of the event source.
 *
 * Since: 2.14
 **/
guint
g_timeout_add_seconds_full (gint           priority,
                            guint32        interval,
                            GSourceFunc    function,
                            gpointer       data,
                            GDestroyNotify notify)
{
  return timeout_add_full (priority, interval, TRUE, FALSE, function, data, notify);
}

/**
 * g_timeout_add_seconds:
 * @interval: the time between calls to the function, in seconds
 * @function: function to call
 * @data: data to pass to @function
 *
 * Sets a function to be called at regular intervals with the default
 * priority, [const@GLib.PRIORITY_DEFAULT].
 *
 * The function is called repeatedly until it returns [const@GLib.SOURCE_REMOVE]
 * or %FALSE, at which point the timeout is automatically destroyed
 * and the function will not be called again.
 *
 * This internally creates a main loop source using
 * [func@GLib.timeout_source_new_seconds] and attaches it to the main loop context
 * using [method@GLib.Source.attach]. You can do these steps manually if you need
 * greater control. Also see [func@GLib.timeout_add_seconds_full].
 *
 * It is safe to call this function from any thread.
 *
 * Note that the first call of the timer may not be precise for timeouts
 * of one second. If you need finer precision and have such a timeout,
 * you may want to use [func@GLib.timeout_add] instead.
 *
 * See [mainloop memory management](main-loop.html#memory-management-of-sources) for details
 * on how to handle the return value and memory management of @data.
 *
 * The interval given is in terms of monotonic time, not wall clock
 * time. See [func@GLib.get_monotonic_time].
 * 
 * Returns: the ID (greater than 0) of the event source.
 *
 * Since: 2.14
 **/
guint
g_timeout_add_seconds (guint       interval,
                       GSourceFunc function,
                       gpointer    data)
{
  g_return_val_if_fail (function != NULL, 0);

  return g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, interval, function, data, NULL);
}

/**
 * g_timeout_add_seconds_once:
 * @interval: the time after which the function will be called, in seconds
 * @function: function to call
 * @data: data to pass to @function
 *
 * This function behaves like [func@GLib.timeout_add_once] but with a range in
 * seconds.
 *
 * Returns: the ID (greater than 0) of the event source
 *
 * Since: 2.78
 */
guint
g_timeout_add_seconds_once (guint           interval,
                            GSourceOnceFunc function,
                            gpointer        data)
{
  return timeout_add_full (G_PRIORITY_DEFAULT, interval, TRUE, TRUE, (GSourceFunc) function, data, NULL);
}

/* Child watch functions */

#ifdef HAVE_PIDFD
static int
siginfo_t_to_wait_status (const siginfo_t *info)
{
  /* Each of these returns is essentially the inverse of WIFEXITED(),
   * WIFSIGNALED(), etc. */
  switch (info->si_code)
    {
    case CLD_EXITED:
      return W_EXITCODE (info->si_status, 0);
    case CLD_KILLED:
      return W_EXITCODE (0, info->si_status);
    case CLD_DUMPED:
      return W_EXITCODE (0, info->si_status | WCOREFLAG);
    case CLD_CONTINUED:
      return __W_CONTINUED;
    case CLD_STOPPED:
    case CLD_TRAPPED:
    default:
      return W_STOPCODE (info->si_status);
    }
}
#endif /* HAVE_PIDFD */

static gboolean
g_child_watch_prepare (GSource *source,
		       gint    *timeout)
{
#ifdef G_OS_WIN32
  return FALSE;
#else  /* G_OS_WIN32 */
  {
    GChildWatchSource *child_watch_source;

    child_watch_source = (GChildWatchSource *) source;

    if (child_watch_source->poll.fd >= 0)
      return FALSE;

    return g_atomic_int_get (&child_watch_source->child_maybe_exited);
  }
#endif /* G_OS_WIN32 */
}

static gboolean
g_child_watch_check (GSource *source)
{
  GChildWatchSource *child_watch_source;
  gboolean child_exited;

  child_watch_source = (GChildWatchSource *) source;

#ifdef G_OS_WIN32
  child_exited = !!(child_watch_source->poll.revents & G_IO_IN);
#else /* G_OS_WIN32 */
#ifdef HAVE_PIDFD
  if (child_watch_source->poll.fd >= 0)
    {
      child_exited = !!(child_watch_source->poll.revents & G_IO_IN);
      return child_exited;
    }
#endif /* HAVE_PIDFD */
  child_exited = g_atomic_int_get (&child_watch_source->child_maybe_exited);
#endif /* G_OS_WIN32 */

  return child_exited;
}

static void
g_child_watch_finalize (GSource *source)
{
#ifndef G_OS_WIN32
  GChildWatchSource *child_watch_source = (GChildWatchSource *) source;

  if (child_watch_source->poll.fd >= 0)
    {
      close (child_watch_source->poll.fd);
      return;
    }

  G_LOCK (unix_signal_lock);
  unix_child_watches = g_slist_remove (unix_child_watches, source);
  unref_unix_signal_handler_unlocked (SIGCHLD);
  G_UNLOCK (unix_signal_lock);
#endif /* G_OS_WIN32 */
}

#ifndef G_OS_WIN32

static void
wake_source (GSource *source)
{
  GMainContext *context;

  /* This should be thread-safe:
   *
   *  - if the source is currently being added to a context, that
   *    context will be woken up anyway
   *
   *  - if the source is currently being destroyed, we simply need not
   *    to crash:
   *
   *    - the memory for the source will remain valid until after the
   *      source finalize function was called (which would remove the
   *      source from the global list which we are currently holding the
   *      lock for)
   *
   *    - the GMainContext will either be NULL or point to a live
   *      GMainContext
   *
   *    - the GMainContext will remain valid since source_dup_main_context()
   *      gave us a ref or NULL
   *
   *  Since we are holding a lot of locks here, don't try to enter any
   *  more GMainContext functions for fear of dealock -- just hit the
   *  GWakeup and run.  Even if that's safe now, it could easily become
   *  unsafe with some very minor changes in the future, and signal
   *  handling is not the most well-tested codepath.
   */
  context = source_dup_main_context (source);
  if (context)
    g_wakeup_signal (context->wakeup);

  if (context)
    g_main_context_unref (context);
}

static void
dispatch_unix_signals_unlocked (void)
{
  gboolean pending[NSIG];
  GSList *node;
  gint i;

  /* clear this first in case another one arrives while we're processing */
  g_atomic_int_set (&any_unix_signal_pending, 0);

  /* We atomically test/clear the bit from the global array in case
   * other signals arrive while we are dispatching.
   *
   * We then can safely use our own array below without worrying about
   * races.
   */
  for (i = 0; i < NSIG; i++)
    {
      /* Be very careful with (the volatile) unix_signal_pending.
       *
       * We must ensure that it's not possible that we clear it without
       * handling the signal.  We therefore must ensure that our pending
       * array has a field set (ie: we will do something about the
       * signal) before we clear the item in unix_signal_pending.
       *
       * Note specifically: we must check _our_ array.
       */
      pending[i] = g_atomic_int_compare_and_exchange (&unix_signal_pending[i], 1, 0);
    }

  /* handle GChildWatchSource instances */
  if (pending[SIGCHLD])
    {
      /* The only way we can do this is to scan all of the children.
       *
       * The docs promise that we will not reap children that we are not
       * explicitly watching, so that ties our hands from calling
       * waitpid(-1).  We also can't use siginfo's si_pid field since if
       * multiple SIGCHLD arrive at the same time, one of them can be
       * dropped (since a given UNIX signal can only be pending once).
       */
      for (node = unix_child_watches; node; node = node->next)
        {
          GChildWatchSource *source = node->data;

          if (g_atomic_int_compare_and_exchange (&source->child_maybe_exited, FALSE, TRUE))
            wake_source ((GSource *) source);
        }
    }

  /* handle GUnixSignalWatchSource instances */
  for (node = unix_signal_watches; node; node = node->next)
    {
      GUnixSignalWatchSource *source = node->data;

      if (pending[source->signum] &&
          g_atomic_int_compare_and_exchange (&source->pending, FALSE, TRUE))
        {
          wake_source ((GSource *) source);
        }
    }

}

static void
dispatch_unix_signals (void)
{
  G_LOCK(unix_signal_lock);
  dispatch_unix_signals_unlocked ();
  G_UNLOCK(unix_signal_lock);
}

static gboolean
g_unix_signal_watch_prepare (GSource *source,
			     gint    *timeout)
{
  GUnixSignalWatchSource *unix_signal_source;

  unix_signal_source = (GUnixSignalWatchSource *) source;

  return g_atomic_int_get (&unix_signal_source->pending);
}

static gboolean
g_unix_signal_watch_check (GSource  *source)
{
  GUnixSignalWatchSource *unix_signal_source;

  unix_signal_source = (GUnixSignalWatchSource *) source;

  return g_atomic_int_get (&unix_signal_source->pending);
}

static gboolean
g_unix_signal_watch_dispatch (GSource    *source, 
			      GSourceFunc callback,
			      gpointer    user_data)
{
  GUnixSignalWatchSource *unix_signal_source;
  gboolean again;

  unix_signal_source = (GUnixSignalWatchSource *) source;

  if (!callback)
    {
      g_warning ("Unix signal source dispatched without callback. "
		 "You must call g_source_set_callback().");
      return FALSE;
    }

  g_atomic_int_set (&unix_signal_source->pending, FALSE);

  again = (callback) (user_data);

  return again;
}

static void
ref_unix_signal_handler_unlocked (int signum)
{
  /* Ensure we have the worker context */
  g_get_worker_context ();
  unix_signal_refcount[signum]++;
  if (unix_signal_refcount[signum] == 1)
    {
      struct sigaction action;
      action.sa_handler = g_unix_signal_handler;
      sigemptyset (&action.sa_mask);
#ifdef SA_RESTART
      action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
#else
      action.sa_flags = SA_NOCLDSTOP;
#endif
#ifdef SA_ONSTACK
      action.sa_flags |= SA_ONSTACK;
#endif
      sigaction (signum, &action, NULL);
    }
}

static void
unref_unix_signal_handler_unlocked (int signum)
{
  unix_signal_refcount[signum]--;
  if (unix_signal_refcount[signum] == 0)
    {
      struct sigaction action;
      memset (&action, 0, sizeof (action));
      action.sa_handler = SIG_DFL;
      sigemptyset (&action.sa_mask);
      sigaction (signum, &action, NULL);
    }
}

/* Return a const string to avoid allocations. We lose precision in the case the
 * @signum is unrecognised, but that’ll do. */
static const gchar *
signum_to_string (int signum)
{
  /* See `man 0P signal.h` */
#define SIGNAL(s) \
    case (s): \
      return ("GUnixSignalSource: " #s);
  switch (signum)
    {
    /* These signals are guaranteed to exist by POSIX. */
    SIGNAL (SIGABRT)
    SIGNAL (SIGFPE)
    SIGNAL (SIGILL)
    SIGNAL (SIGINT)
    SIGNAL (SIGSEGV)
    SIGNAL (SIGTERM)
    /* Frustratingly, these are not, and hence for brevity the list is
     * incomplete. */
#ifdef SIGALRM
    SIGNAL (SIGALRM)
#endif
#ifdef SIGCHLD
    SIGNAL (SIGCHLD)
#endif
#ifdef SIGHUP
    SIGNAL (SIGHUP)
#endif
#ifdef SIGKILL
    SIGNAL (SIGKILL)
#endif
#ifdef SIGPIPE
    SIGNAL (SIGPIPE)
#endif
#ifdef SIGQUIT
    SIGNAL (SIGQUIT)
#endif
#ifdef SIGSTOP
    SIGNAL (SIGSTOP)
#endif
#ifdef SIGUSR1
    SIGNAL (SIGUSR1)
#endif
#ifdef SIGUSR2
    SIGNAL (SIGUSR2)
#endif
#ifdef SIGPOLL
    SIGNAL (SIGPOLL)
#endif
#ifdef SIGPROF
    SIGNAL (SIGPROF)
#endif
#ifdef SIGTRAP
    SIGNAL (SIGTRAP)
#endif
    default:
      return "GUnixSignalSource: Unrecognized signal";
    }
#undef SIGNAL
}

GSource *
_g_main_create_unix_signal_watch (int signum)
{
  GSource *source;
  GUnixSignalWatchSource *unix_signal_source;

  source = g_source_new (&g_unix_signal_funcs, sizeof (GUnixSignalWatchSource));
  unix_signal_source = (GUnixSignalWatchSource *) source;

  unix_signal_source->signum = signum;
  unix_signal_source->pending = FALSE;

  /* Set a default name on the source, just in case the caller does not. */
  g_source_set_static_name (source, signum_to_string (signum));

  G_LOCK (unix_signal_lock);
  ref_unix_signal_handler_unlocked (signum);
  unix_signal_watches = g_slist_prepend (unix_signal_watches, unix_signal_source);
  dispatch_unix_signals_unlocked ();
  G_UNLOCK (unix_signal_lock);

  return source;
}

static void
g_unix_signal_watch_finalize (GSource    *source)
{
  GUnixSignalWatchSource *unix_signal_source;

  unix_signal_source = (GUnixSignalWatchSource *) source;

  G_LOCK (unix_signal_lock);
  unref_unix_signal_handler_unlocked (unix_signal_source->signum);
  unix_signal_watches = g_slist_remove (unix_signal_watches, source);
  G_UNLOCK (unix_signal_lock);
}

#endif /* G_OS_WIN32 */

static gboolean
g_child_watch_dispatch (GSource    *source, 
			GSourceFunc callback,
			gpointer    user_data)
{
  GChildWatchSource *child_watch_source;
  GChildWatchFunc child_watch_callback = (GChildWatchFunc) callback;
  int wait_status;

  child_watch_source = (GChildWatchSource *) source;

  /* We only (try to) reap the child process right before dispatching the callback.
   * That way, the caller can rely that the process is there until the callback
   * is invoked; or, if the caller calls g_source_destroy() without the callback
   * being dispatched, the process is still not reaped. */

#ifdef G_OS_WIN32
  {
    DWORD child_status;

    /*
     * Note: We do _not_ check for the special value of STILL_ACTIVE
     * since we know that the process has exited and doing so runs into
     * problems if the child process "happens to return STILL_ACTIVE(259)"
     * as Microsoft's Platform SDK puts it.
     */
    if (!GetExitCodeProcess (child_watch_source->pid, &child_status))
      {
        gchar *emsg = g_win32_error_message (GetLastError ());
        g_warning (G_STRLOC ": GetExitCodeProcess() failed: %s", emsg);
        g_free (emsg);

        /* Unknown error. We got signaled that the process might be exited,
         * but now we failed to reap it? Assume the process is gone and proceed. */
        wait_status = -1;
      }
    else
      wait_status = child_status;
  }
#else /* G_OS_WIN32 */
  {
    gboolean child_exited = FALSE;

    wait_status = -1;

#ifdef HAVE_PIDFD
    if (child_watch_source->poll.fd >= 0)
      {
        siginfo_t child_info = {
          0,
        };

        /* Get the exit status */
        if (waitid (P_PIDFD, child_watch_source->poll.fd, &child_info, WEXITED | WNOHANG) >= 0)
          {
            if (child_info.si_pid != 0)
              {
                /* waitid() helpfully provides the wait status in a decomposed
                 * form which is quite useful. Unfortunately we have to report it
                 * to the #GChildWatchFunc as a waitpid()-style platform-specific
                 * wait status, so that the user code in #GChildWatchFunc can then
                 * call WIFEXITED() (etc.) on it. That means re-composing the
                 * status information. */
                wait_status = siginfo_t_to_wait_status (&child_info);
                child_exited = TRUE;
              }
            else
              {
                g_debug (G_STRLOC ": pidfd signaled but pid %" G_PID_FORMAT " didn't exit",
                         child_watch_source->pid);
                return TRUE;
              }
          }
        else
          {
            int errsv = errno;

            g_warning (G_STRLOC ": waitid(pid:%" G_PID_FORMAT ", pidfd=%d) failed: %s (%d). %s",
                       child_watch_source->pid, child_watch_source->poll.fd, g_strerror (errsv), errsv,
                       "See documentation of g_child_watch_source_new() for possible causes.");

            /* Assume the process is gone and proceed. */
            child_exited = TRUE;
          }
      }
#endif /* HAVE_PIDFD*/

    if (!child_exited)
      {
        pid_t pid;
        int wstatus;

      waitpid_again:

        /* We must reset the flag before waitpid(). Otherwise, there would be a
         * race. */
        g_atomic_int_set (&child_watch_source->child_maybe_exited, FALSE);

        pid = waitpid (child_watch_source->pid, &wstatus, WNOHANG);

        if (G_UNLIKELY (pid < 0 && errno == EINTR))
          goto waitpid_again;

        if (pid == 0)
          {
            /* Not exited yet. Wait longer. */
            return TRUE;
          }

        if (pid > 0)
          wait_status = wstatus;
        else
          {
            int errsv = errno;

            g_warning (G_STRLOC ": waitpid(pid:%" G_PID_FORMAT ") failed: %s (%d). %s",
                       child_watch_source->pid, g_strerror (errsv), errsv,
                       "See documentation of g_child_watch_source_new() for possible causes.");

            /* Assume the process is gone and proceed. */
          }
      }
  }
#endif /* G_OS_WIN32 */

  if (!callback)
    {
      g_warning ("Child watch source dispatched without callback. "
		 "You must call g_source_set_callback().");
      return FALSE;
    }

  (child_watch_callback) (child_watch_source->pid, wait_status, user_data);

  /* We never keep a child watch source around as the child is gone */
  return FALSE;
}

#ifndef G_OS_WIN32

static void
g_unix_signal_handler (int signum)
{
  gint saved_errno = errno;

#if defined(G_ATOMIC_LOCK_FREE) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
  g_atomic_int_set (&unix_signal_pending[signum], 1);
  g_atomic_int_set (&any_unix_signal_pending, 1);
#else
#warning "Can't use atomics in g_unix_signal_handler(): Unix signal handling will be racy"
  unix_signal_pending[signum] = 1;
  any_unix_signal_pending = 1;
#endif

  g_wakeup_signal (glib_worker_context->wakeup);

  errno = saved_errno;
}

#endif /* !G_OS_WIN32 */

/**
 * g_child_watch_source_new:
 * @pid: process to watch. On POSIX the positive pid of a child process. On
 * Windows a handle for a process (which doesn't have to be a child).
 * 
 * Creates a new child_watch source.
 *
 * The source will not initially be associated with any
 * [struct@GLib.MainContext] and must be added to one with
 * [method@GLib.Source.attach] before it will be executed.
 *
 * Note that child watch sources can only be used in conjunction with
 * `g_spawn...` when the %G_SPAWN_DO_NOT_REAP_CHILD flag is used.
 *
 * Note that on platforms where #GPid must be explicitly closed
 * (see [func@GLib.spawn_close_pid]) @pid must not be closed while the
 * source is still active. Typically, you will want to call
 * [func@GLib.spawn_close_pid] in the callback function for the source.
 *
 * On POSIX platforms, the following restrictions apply to this API
 * due to limitations in POSIX process interfaces:
 *
 * * @pid must be a child of this process
 * * @pid must be positive
 * * the application must not call `waitpid` with a non-positive
 *   first argument, for instance in another thread
 * * the application must not wait for @pid to exit by any other
 *   mechanism, including `waitpid(pid, ...)` or a second child-watch
 *   source for the same @pid
 * * the application must not ignore `SIGCHLD`
 * * Before 2.78, the application could not send a signal (`kill()`) to the
 *   watched @pid in a race free manner. Since 2.78, you can do that while the
 *   associated [struct@GLib.MainContext] is acquired.
 * * Before 2.78, even after destroying the [struct@GLib.Source], you could not
 *   be sure that @pid wasn't already reaped. Hence, it was also not
 *   safe to `kill()` or `waitpid()` on the process ID after the child watch
 *   source was gone. Destroying the source before it fired made it
 *   impossible to reliably reap the process.
 *
 * If any of those conditions are not met, this and related APIs will
 * not work correctly. This can often be diagnosed via a GLib warning
 * stating that `ECHILD` was received by `waitpid`.
 *
 * Calling `waitpid` for specific processes other than @pid remains a
 * valid thing to do.
 *
 * Returns: the newly-created child watch source
 *
 * Since: 2.4
 **/
GSource *
g_child_watch_source_new (GPid pid)
{
  GSource *source;
  GChildWatchSource *child_watch_source;
#ifdef HAVE_PIDFD
  int errsv;
#endif

#ifndef G_OS_WIN32
  g_return_val_if_fail (pid > 0, NULL);
#endif

  source = g_source_new (&g_child_watch_funcs, sizeof (GChildWatchSource));
  child_watch_source = (GChildWatchSource *)source;

  /* Set a default name on the source, just in case the caller does not. */
  g_source_set_static_name (source, "GChildWatchSource");

  child_watch_source->pid = pid;

#ifdef G_OS_WIN32
  child_watch_source->poll.fd = (gintptr) pid;
  child_watch_source->poll.events = G_IO_IN;

  g_source_add_poll (source, &child_watch_source->poll);
#else /* !G_OS_WIN32 */

#ifdef HAVE_PIDFD
  /* Use a pidfd, if possible, to avoid having to install a global SIGCHLD
   * handler and potentially competing with any other library/code which wants
   * to install one.
   *
   * Unfortunately this use of pidfd isn’t race-free (the PID could be recycled
   * between the caller calling g_child_watch_source_new() and here), but it’s
   * better than SIGCHLD.
   */
  child_watch_source->poll.fd = (int) syscall (SYS_pidfd_open, pid, 0);

  if (child_watch_source->poll.fd >= 0)
    {
      child_watch_source->poll.events = G_IO_IN;
      g_source_add_poll (source, &child_watch_source->poll);
      return source;
    }

  errsv = errno;
  g_debug ("pidfd_open(%" G_PID_FORMAT ") failed with error: %s",
           pid, g_strerror (errsv));
  /* Fall through; likely the kernel isn’t new enough to support pidfd_open() */
#endif /* HAVE_PIDFD */

  /* We can do that without atomic, as the source is not yet added in
   * unix_child_watches (which we do next under a lock). */
  child_watch_source->child_maybe_exited = TRUE;
  child_watch_source->poll.fd = -1;

  G_LOCK (unix_signal_lock);
  ref_unix_signal_handler_unlocked (SIGCHLD);
  unix_child_watches = g_slist_prepend (unix_child_watches, child_watch_source);
  G_UNLOCK (unix_signal_lock);
#endif /* !G_OS_WIN32 */

  return source;
}

/**
 * g_child_watch_add_full: (rename-to g_child_watch_add)
 * @priority: the priority of the idle source. Typically this will be in the
 *   range between [const@GLib.PRIORITY_DEFAULT_IDLE] and
 *   [const@GLib.PRIORITY_HIGH_IDLE].
 * @pid: process to watch. On POSIX the positive pid of a child process. On
 * Windows a handle for a process (which doesn't have to be a child).
 * @function: function to call
 * @data: data to pass to @function
 * @notify: (nullable): function to call when the idle is removed, or %NULL
 * 
 * Sets a function to be called when the child indicated by @pid 
 * exits, at the priority @priority.
 *
 * If you obtain @pid from [func@GLib.spawn_async] or
 * [func@GLib.spawn_async_with_pipes] you will need to pass
 * %G_SPAWN_DO_NOT_REAP_CHILD as flag to the spawn function for the child
 * watching to work.
 *
 * In many programs, you will want to call [func@GLib.spawn_check_wait_status]
 * in the callback to determine whether or not the child exited
 * successfully.
 *
 * Also, note that on platforms where #GPid must be explicitly closed
 * (see [func@GLib.spawn_close_pid]) @pid must not be closed while the source
 * is still active.  Typically, you should invoke [func@GLib.spawn_close_pid]
 * in the callback function for the source.
 * 
 * GLib supports only a single callback per process id.
 * On POSIX platforms, the same restrictions mentioned for
 * [func@GLib.child_watch_source_new] apply to this function.
 *
 * This internally creates a main loop source using 
 * [func@GLib.child_watch_source_new] and attaches it to the main loop context
 * using [method@GLib.Source.attach]. You can do these steps manually if you
 * need greater control.
 *
 * Returns: the ID (greater than 0) of the event source.
 *
 * Since: 2.4
 **/
guint
g_child_watch_add_full (gint            priority,
			GPid            pid,
			GChildWatchFunc function,
			gpointer        data,
			GDestroyNotify  notify)
{
  GSource *source;
  guint id;
  
  g_return_val_if_fail (function != NULL, 0);
#ifndef G_OS_WIN32
  g_return_val_if_fail (pid > 0, 0);
#endif

  source = g_child_watch_source_new (pid);

  if (priority != G_PRIORITY_DEFAULT)
    g_source_set_priority (source, priority);

  g_source_set_callback (source, (GSourceFunc) function, data, notify);
  id = g_source_attach (source, NULL);
  g_source_unref (source);

  return id;
}

/**
 * g_child_watch_add:
 * @pid: process id to watch. On POSIX the positive pid of a child
 *   process. On Windows a handle for a process (which doesn't have
 *   to be a child).
 * @function: function to call
 * @data: data to pass to @function
 *
 * Sets a function to be called when the child indicated by @pid 
 * exits, at a default priority, [const@GLib.PRIORITY_DEFAULT].
 *
 * If you obtain @pid from [func@GLib.spawn_async] or
 * [func@GLib.spawn_async_with_pipes] you will need to pass
 * %G_SPAWN_DO_NOT_REAP_CHILD as flag to the spawn function for the child
 * watching to work.
 *
 * Note that on platforms where #GPid must be explicitly closed
 * (see [func@GLib.spawn_close_pid]) @pid must not be closed while the
 * source is still active. Typically, you will want to call
 * [func@GLib.spawn_close_pid] in the callback function for the source.
 *
 * GLib supports only a single callback per process id.
 * On POSIX platforms, the same restrictions mentioned for
 * [func@GLib.child_watch_source_new] apply to this function.
 *
 * This internally creates a main loop source using 
 * [func@GLib.child_watch_source_new] and attaches it to the main loop context
 * using [method@GLib.Source.attach]. You can do these steps manually if you
 * need greater control.
 *
 * Returns: the ID (greater than 0) of the event source.
 *
 * Since: 2.4
 **/
guint 
g_child_watch_add (GPid            pid,
		   GChildWatchFunc function,
		   gpointer        data)
{
  return g_child_watch_add_full (G_PRIORITY_DEFAULT, pid, function, data, NULL);
}


/* Idle functions */

static gboolean 
g_idle_prepare  (GSource  *source,
		 gint     *timeout)
{
  *timeout = 0;

  return TRUE;
}

static gboolean 
g_idle_check    (GSource  *source)
{
  return TRUE;
}

static gboolean
g_idle_dispatch (GSource    *source, 
		 GSourceFunc callback,
		 gpointer    user_data)
{
  GIdleSource *idle_source = (GIdleSource *)source;
  gboolean again;

  if (!callback)
    {
      g_warning ("Idle source dispatched without callback. "
		 "You must call g_source_set_callback().");
      return FALSE;
    }

  if (idle_source->one_shot)
    {
      GSourceOnceFunc once_callback = (GSourceOnceFunc) callback;
      once_callback (user_data);
      again = G_SOURCE_REMOVE;
    }
  else
    {
      again = callback (user_data);
    }

  TRACE (GLIB_IDLE_DISPATCH (source, source->context, callback, user_data, again));

  return again;
}

static GSource *
idle_source_new (gboolean one_shot)
{
  GSource *source;
  GIdleSource *idle_source;

  source = g_source_new (&g_idle_funcs, sizeof (GIdleSource));
  idle_source = (GIdleSource *) source;

  idle_source->one_shot = one_shot;

  g_source_set_priority (source, G_PRIORITY_DEFAULT_IDLE);

  /* Set a default name on the source, just in case the caller does not. */
  g_source_set_static_name (source, "GIdleSource");

  return source;
}

/**
 * g_idle_source_new:
 * 
 * Creates a new idle source.
 *
 * The source will not initially be associated with any
 * [struct@GLib.MainContext] and must be added to one with
 * [method@GLib.Source.attach] before it will be executed. Note that the
 * default priority for idle sources is [const@GLib.PRIORITY_DEFAULT_IDLE], as
 * compared to other sources which have a default priority of
 * [const@GLib.PRIORITY_DEFAULT].
 *
 * Returns: the newly-created idle source
 **/
GSource *
g_idle_source_new (void)
{
  return idle_source_new (FALSE);
}

static guint
idle_add_full (gint           priority,
               gboolean       one_shot,
               GSourceFunc    function,
               gpointer       data,
               GDestroyNotify notify)
{
  GSource *source;
  guint id;

  g_return_val_if_fail (function != NULL, 0);

  source = idle_source_new (one_shot);

  if (priority != G_PRIORITY_DEFAULT_IDLE)
    g_source_set_priority (source, priority);

  g_source_set_callback (source, function, data, notify);
  id = g_source_attach (source, NULL);

  TRACE (GLIB_IDLE_ADD (source, g_main_context_default (), id, priority, function, data));

  g_source_unref (source);

  return id;
}

/**
 * g_idle_add_full: (rename-to g_idle_add)
 * @priority: the priority of the idle source. Typically this will be in the
 *   range between [const@GLib.PRIORITY_DEFAULT_IDLE] and
 *   [const@GLib.PRIORITY_HIGH_IDLE].
 * @function: function to call
 * @data: data to pass to @function
 * @notify: (nullable): function to call when the idle is removed, or %NULL
 * 
 * Adds a function to be called whenever there are no higher priority
 * events pending.
 *
 * If the function returns [const@GLib.SOURCE_REMOVE] or %FALSE it is automatically
 * removed from the list of event sources and will not be called again.
 *
 * See [mainloop memory management](main-loop.html#memory-management-of-sources) for details
 * on how to handle the return value and memory management of @data.
 *
 * This internally creates a main loop source using [func@GLib.idle_source_new]
 * and attaches it to the global [struct@GLib.MainContext] using
 * [method@GLib.Source.attach], so the callback will be invoked in whichever
 * thread is running that main context. You can do these steps manually if you
 * need greater control or to use a custom main context.
 *
 * Returns: the ID (greater than 0) of the event source.
 **/
guint 
g_idle_add_full (gint           priority,
		 GSourceFunc    function,
		 gpointer       data,
		 GDestroyNotify notify)
{
  return idle_add_full (priority, FALSE, function, data, notify);
}

/**
 * g_idle_add:
 * @function: function to call 
 * @data: data to pass to @function.
 * 
 * Adds a function to be called whenever there are no higher priority
 * events pending to the default main loop. The function is given the
 * default idle priority, [const@GLib.PRIORITY_DEFAULT_IDLE].  If the function
 * returns %FALSE it is automatically removed from the list of event
 * sources and will not be called again.
 *
 * See [mainloop memory management](main-loop.html#memory-management-of-sources) for details
 * on how to handle the return value and memory management of @data.
 *
 * This internally creates a main loop source using [func@GLib.idle_source_new]
 * and attaches it to the global [struct@GLib.MainContext] using
 * [method@GLib.Source.attach], so the callback will be invoked in whichever
 * thread is running that main context. You can do these steps manually if you
 * need greater control or to use a custom main context.
 *
 * Returns: the ID (greater than 0) of the event source.
 **/
guint 
g_idle_add (GSourceFunc    function,
	    gpointer       data)
{
  return g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, function, data, NULL);
}

/**
 * g_idle_add_once:
 * @function: function to call
 * @data: data to pass to @function
 *
 * Adds a function to be called whenever there are no higher priority
 * events pending to the default main loop. The function is given the
 * default idle priority, [const@GLib.PRIORITY_DEFAULT_IDLE].
 *
 * The function will only be called once and then the source will be
 * automatically removed from the main context.
 *
 * This function otherwise behaves like [func@GLib.idle_add].
 *
 * Returns: the ID (greater than 0) of the event source
 *
 * Since: 2.74
 */
guint
g_idle_add_once (GSourceOnceFunc function,
                 gpointer        data)
{
  return idle_add_full (G_PRIORITY_DEFAULT_IDLE, TRUE, (GSourceFunc) function, data, NULL);
}

/**
 * g_idle_remove_by_data:
 * @data: the data for the idle source's callback.
 * 
 * Removes the idle function with the given data.
 * 
 * Returns: %TRUE if an idle source was found and removed.
 **/
gboolean
g_idle_remove_by_data (gpointer data)
{
  return g_source_remove_by_funcs_user_data (&g_idle_funcs, data);
}

/**
 * g_main_context_invoke:
 * @context: (nullable): a #GMainContext, or %NULL for the global-default
 *   main context
 * @function: function to call
 * @data: data to pass to @function
 *
 * Invokes a function in such a way that @context is owned during the
 * invocation of @function.
 *
 * If @context is %NULL then the global-default main context — as
 * returned by [func@GLib.MainContext.default] — is used.
 *
 * If @context is owned by the current thread, @function is called
 * directly.  Otherwise, if @context is the thread-default main context
 * of the current thread and [method@GLib.MainContext.acquire] succeeds, then
 * @function is called and [method@GLib.MainContext.release] is called
 * afterwards.
 *
 * In any other case, an idle source is created to call @function and
 * that source is attached to @context (presumably to be run in another
 * thread).  The idle source is attached with [const@GLib.PRIORITY_DEFAULT]
 * priority.  If you want a different priority, use
 * [method@GLib.MainContext.invoke_full].
 *
 * Note that, as with normal idle functions, @function should probably
 * return %FALSE.  If it returns %TRUE, it will be continuously run in a
 * loop (and may prevent this call from returning).
 *
 * Since: 2.28
 **/
void
g_main_context_invoke (GMainContext *context,
                       GSourceFunc   function,
                       gpointer      data)
{
  g_main_context_invoke_full (context,
                              G_PRIORITY_DEFAULT,
                              function, data, NULL);
}

/**
 * g_main_context_invoke_full:
 * @context: (nullable): a #GMainContext, or %NULL for the global-default
 *   main context
 * @priority: the priority at which to run @function
 * @function: function to call
 * @data: data to pass to @function
 * @notify: (nullable): a function to call when @data is no longer in use, or %NULL.
 *
 * Invokes a function in such a way that @context is owned during the
 * invocation of @function.
 *
 * This function is the same as [method@GLib.MainContext.invoke] except that it
 * lets you specify the priority in case @function ends up being
 * scheduled as an idle and also lets you give a #GDestroyNotify for @data.
 *
 * @notify should not assume that it is called from any particular
 * thread or with any particular context acquired.
 *
 * Since: 2.28
 **/
void
g_main_context_invoke_full (GMainContext   *context,
                            gint            priority,
                            GSourceFunc     function,
                            gpointer        data,
                            GDestroyNotify  notify)
{
  g_return_if_fail (function != NULL);

  if (!context)
    context = g_main_context_default ();

  if (g_main_context_is_owner (context))
    {
      while (function (data));
      if (notify != NULL)
        notify (data);
    }

  else
    {
      GMainContext *thread_default;

      thread_default = g_main_context_get_thread_default ();

      if (!thread_default)
        thread_default = g_main_context_default ();

      if (thread_default == context && g_main_context_acquire (context))
        {
          while (function (data));

          g_main_context_release (context);

          if (notify != NULL)
            notify (data);
        }
      else
        {
          GSource *source;

          source = g_idle_source_new ();
          g_source_set_priority (source, priority);
          g_source_set_callback (source, function, data, notify);
          g_source_attach (source, context);
          g_source_unref (source);
        }
    }
}

static gpointer
glib_worker_main (gpointer data)
{
  while (TRUE)
    {
      g_main_context_iteration (glib_worker_context, TRUE);

#ifdef G_OS_UNIX
      if (g_atomic_int_get (&any_unix_signal_pending))
        dispatch_unix_signals ();
#endif
    }

  return NULL; /* worst GCC warning message ever... */
}

GMainContext *
g_get_worker_context (void)
{
  static gsize initialised;

  if (g_once_init_enter (&initialised))
    {
      /* mask all signals in the worker thread */
#ifdef G_OS_UNIX
      sigset_t prev_mask;
      sigset_t all;

      sigfillset (&all);
      pthread_sigmask (SIG_SETMASK, &all, &prev_mask);
#endif
      glib_worker_context = g_main_context_new ();
      g_thread_new ("gmain", glib_worker_main, NULL);
#ifdef G_OS_UNIX
      pthread_sigmask (SIG_SETMASK, &prev_mask, NULL);
#endif
      g_once_init_leave (&initialised, TRUE);
    }

  return glib_worker_context;
}

