/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#include "platform.h"
#include "serialdevices.h"
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <string.h>
#endif

/* ---- configure host scheduling ------------------------------------------ */

bool init_simulator_timing(void)
{
#if defined(__APPLE__)
    /* Without this setup, macOS sometimes delayed serial replies past the
     * host's 5 ms timeout. Raising QoS alone did not fix it. */
    int rc = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (rc) {
        fprintf(stderr, "sim: setting thread QoS: %s\n", strerror(rc));
        return false;
    }
    mach_timebase_info_data_t timebase{};
    kern_return_t status = mach_timebase_info(&timebase);
    if (status != KERN_SUCCESS || !timebase.numer) {
        fprintf(stderr, "sim: cannot determine host timing units (%d)\n", status);
        return false;
    }
    uint64_t period = 1000000ull * timebase.denom / timebase.numer;
    if (period < 4u || period > UINT32_MAX) {
        fprintf(stderr, "sim: host timing units cannot represent a 1 ms period\n");
        return false;
    }
    thread_time_constraint_policy_data_t policy{};
    policy.period = (uint32_t)period;
    policy.computation = policy.period / 4u;
    policy.constraint = policy.period;
    policy.preemptible = TRUE;
    mach_port_t thread = mach_thread_self();
    status = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                               reinterpret_cast<thread_policy_t>(&policy),
                               THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    (void)mach_port_deallocate(mach_task_self(), thread);
    if (status != KERN_SUCCESS) {
        fprintf(stderr, "sim: setting time-constraint scheduling: %s\n", mach_error_string(status));
        return false;
    }
#endif
    return true;
}

/* ---- read host time ----------------------------------------------------- */

uint64_t get_simulator_time_ns(void)
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
}

uint32_t get_board_time_ms(void)
{
    static const uint64_t start = get_simulator_time_ns();
    return (uint32_t)((get_simulator_time_ns() - start) / 1000000);
}
