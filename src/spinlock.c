/*
spinlock.c - Hybrid Spinlock
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "spinlock.h"
#include "stacktrace.h"
#include "threading.h"
#include "rvtimer.h"
#include "utils.h"

// Maximum allowed lock time, warns and recovers the lock upon expiration
#define SPINLOCK_MAX_MS 5000

// Attemts to claim the lock before blocking in the kernel
#define SPINLOCK_RETRIES 60

static cond_var_t* global_cond = NULL;

static void spin_deinit(void)
{
    cond_var_t* cond = global_cond;
    global_cond = NULL;
    // Make sure no use-after-free happens on running threads
    atomic_fence();
    condvar_free(cond);
}

static void spin_global_init(void)
{
    DO_ONCE ({
        global_cond = condvar_create();
        call_at_deinit(spin_deinit);
    });
}

static void spin_lock_debug_report(spinlock_t* lock)
{
#ifdef USE_SPINLOCK_DEBUG
    rvvm_warn("The lock was previously held at %s", lock->location);
#else
    UNUSED(lock);
#endif
#ifdef RVVM_VERSION
    rvvm_warn("Version: RVVM v"RVVM_VERSION);
#endif
    stacktrace_print();
}

slow_path void spin_lock_wait(spinlock_t* lock, const char* location)
{
    for (size_t i = 0; i < SPINLOCK_RETRIES; ++i) {
        // Read lock flag until there's any chance to grab it
        // Improves performance due to cacheline bouncing elimination
        if (atomic_load_uint32_ex(&lock->flag, ATOMIC_ACQUIRE) == 0) {
            if (spin_try_lock_real(lock, location)) {
                return;
            }
            break;
        }
    }

    spin_global_init();

    rvtimer_t timer = {0};
    rvtimer_init(&timer, 1000);
    do {
        uint32_t flag = atomic_load_uint32_ex(&lock->flag, ATOMIC_ACQUIRE);
        if (flag == 0 && spin_try_lock_real(lock, location)) {
            // Succesfully grabbed the lock
            return;
        }
        // Someone else grabbed the lock, indicate that we are still waiting
        if (flag != 2 && !atomic_cas_uint32(&lock->flag, 1, 2)) {
            // Failed to indicate lock as waiting, retry grabbing
            continue;
        }
        // Wait upon wakeup from lock owner
        bool woken = condvar_wait(global_cond, 10);
        if (woken || flag != 2) {
            // Reset deadlock timer upon noticing any forward progress
            rvtimer_init(&timer, 1000);
        }
    } while (location == NULL || rvtimer_get(&timer) < SPINLOCK_MAX_MS);

    rvvm_warn("Possible deadlock at %s", location);

    spin_lock_debug_report(lock);

    rvvm_warn("Attempting to recover execution...\n * * * * * * *\n");
}

slow_path void spin_lock_wake(spinlock_t* lock, uint32_t prev)
{
    if (prev > 1) {
        spin_global_init();
        condvar_wake_all(global_cond);
    } else {
        rvvm_warn("Unlock of a non-locked lock!");
        spin_lock_debug_report(lock);
    }
}
