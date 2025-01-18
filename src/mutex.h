#ifndef DNAFX_MUTEX_H
#define DNAFX_MUTEX_H

#include "debug.h"

extern gboolean dnafx_lock_debug;

/* Mutex implementation */
typedef GMutex dnafx_mutex;
#define dnafx_mutex_init(a) g_mutex_init(a)
#define DNAFX_MUTEX_INITIALIZER {0}
#define dnafx_mutex_destroy(a) g_mutex_clear(a)
#define dnafx_mutex_lock_nodebug(a) g_mutex_lock(a)
#define dnafx_mutex_lock_debug(a) { DNAFX_PRINT("[%s:%s:%d:lock] %p\n", __FILE__, __FUNCTION__, __LINE__, a); g_mutex_lock(a); }
#define dnafx_mutex_lock(a) { if(!dnafx_lock_debug) { dnafx_mutex_lock_nodebug(a); } else { dnafx_mutex_lock_debug(a); } }
#define dnafx_mutex_trylock_nodebug(a) { ret = g_mutex_trylock(a); }
#define dnafx_mutex_trylock_debug(a) { DNAFX_PRINT("[%s:%s:%d:trylock] %p\n", __FILE__, __FUNCTION__, __LINE__, a); ret = g_mutex_trylock(a); }
#define dnafx_mutex_trylock(a) ({ gboolean ret; if(!dnafx_lock_debug) { dnafx_mutex_trylock_nodebug(a); } else { dnafx_mutex_trylock_debug(a); } ret; })
#define dnafx_mutex_unlock_nodebug(a) g_mutex_unlock(a)
#define dnafx_mutex_unlock_debug(a) { DNAFX_PRINT("[%s:%s:%d:unlock] %p\n", __FILE__, __FUNCTION__, __LINE__, a); g_mutex_unlock(a); }
#define dnafx_mutex_unlock(a) { if(!dnafx_lock_debug) { dnafx_mutex_unlock_nodebug(a); } else { dnafx_mutex_unlock_debug(a); } }

/* Condition/signal implementation */
typedef GCond dnafx_condition;
#define dnafx_condition_init(a) g_cond_init(a)
#define dnafx_condition_destroy(a) g_cond_clear(a)
#define dnafx_condition_wait(a, b) g_cond_wait(a, b);
#define dnafx_condition_wait_until(a, b, c) g_cond_wait_until(a, b, c);
#define dnafx_condition_signal(a) g_cond_signal(a);
#define dnafx_condition_broadcast(a) g_cond_broadcast(a);

#endif
