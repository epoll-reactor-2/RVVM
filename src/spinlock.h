/*
spinlock.h - Hybrid Spinlock
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_SPINLOCK_H
#define RVVM_SPINLOCK_H

#include <stddef.h>
#include "atomics.h"

typedef struct {
    uint32_t flag;
#ifdef USE_SPINLOCK_DEBUG
    const char* location;
#endif
} spinlock_t;

// Internal locking operations
slow_path void spin_lock_wait(spinlock_t* lock, const char* location);
slow_path void spin_lock_wake(spinlock_t* lock, uint32_t prev);

// Static initialization
#define SPINLOCK_INIT {0}

// Initialize a lock
static inline void spin_init(spinlock_t* lock)
{
    atomic_store_uint32(&lock->flag, 0);
#ifdef USE_SPINLOCK_DEBUG
    lock->location = NULL;
#endif
}

// Try to claim the lock, returns true on success
static forceinline bool spin_try_lock_real(spinlock_t* lock, const char* location)
{
    bool ret = atomic_cas_uint32_ex(&lock->flag, 0, 1, false, ATOMIC_ACQUIRE, ATOMIC_ACQUIRE);
#ifdef USE_SPINLOCK_DEBUG
    if (likely(ret)) lock->location = location;
#else
    UNUSED(location);
#endif
    return ret;
}

// Perform locking on small critical section
// Reports a deadlock upon waiting for too long
static forceinline void spin_lock_real(spinlock_t* lock, const char* location)
{
    if (unlikely(!spin_try_lock_real(lock, location))) {
        spin_lock_wait(lock, location);
    }
}

// Perform locking around heavy operation, wait indefinitely
static forceinline void spin_lock_slow_real(spinlock_t* lock, const char* location)
{
    if (unlikely(!spin_try_lock_real(lock, location))) {
        spin_lock_wait(lock, NULL);
    }
}

#ifdef USE_SPINLOCK_DEBUG
#define spin_try_lock(lock) spin_try_lock_real(lock, SOURCE_LINE)
#define spin_lock(lock) spin_lock_real(lock, SOURCE_LINE)
#define spin_lock_slow(lock) spin_lock_slow_real(lock, SOURCE_LINE)
#else
#define spin_try_lock(lock) spin_try_lock_real(lock, NULL)
#define spin_lock(lock) spin_lock_real(lock, NULL)
#define spin_lock_slow(lock) spin_lock_slow_real(lock, NULL)
#endif

// Release the lock
static forceinline void spin_unlock(spinlock_t* lock)
{
    uint32_t prev = atomic_swap_uint32_ex(&lock->flag, 0, ATOMIC_RELEASE);
    if (unlikely(prev != 1)) {
        spin_lock_wake(lock, prev);
    }
}

#endif
