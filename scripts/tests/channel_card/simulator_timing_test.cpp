/* Exercise macOS scheduler setup failures without changing this test thread's
 * scheduling policy. The production implementation is included below. */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <assert.h>
#include <errno.h>

static int qos_result;
static kern_return_t timebase_result, policy_result;
static unsigned qos_calls, timebase_calls, policy_calls, releases;
static uint32_t numerator = 125, denominator = 3;

static int set_qos(qos_class_t qos, int priority)
{
    assert(qos == QOS_CLASS_USER_INTERACTIVE && priority == 0);
    ++qos_calls;
    return qos_result;
}

static kern_return_t read_timebase(mach_timebase_info_t info)
{
    ++timebase_calls;
    info->numer = numerator;
    info->denom = denominator;
    return timebase_result;
}

static kern_return_t set_policy(thread_t thread, thread_policy_flavor_t flavor,
                                thread_policy_t value, mach_msg_type_number_t count)
{
    assert(thread == 42 && flavor == THREAD_TIME_CONSTRAINT_POLICY);
    assert(count == THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    auto *policy = reinterpret_cast<thread_time_constraint_policy_t>(value);
    assert(policy->period == 24000 && policy->computation == 6000);
    assert(policy->constraint == policy->period && policy->preemptible == TRUE);
    ++policy_calls;
    return policy_result;
}

static mach_port_t current_thread() { return 42; }
static kern_return_t release_thread(mach_port_t task, mach_port_name_t thread)
{
    assert(task == mach_task_self() && thread == 42);
    ++releases;
    return KERN_SUCCESS;
}

#define pthread_set_qos_class_self_np set_qos
#define mach_timebase_info read_timebase
#define thread_policy_set set_policy
#define mach_thread_self current_thread
#define mach_port_deallocate release_thread
#include "clock.cpp"
#undef pthread_set_qos_class_self_np
#undef mach_timebase_info
#undef thread_policy_set
#undef mach_thread_self
#undef mach_port_deallocate

static void reset()
{
    qos_result = 0;
    timebase_result = policy_result = KERN_SUCCESS;
    qos_calls = timebase_calls = policy_calls = releases = 0;
    numerator = 125;
    denominator = 3;
}

int main()
{
    const uint32_t block_sizes[] = {32, 48, 64, 96, 256};
    for (uint32_t samples : block_sizes) {
        uint64_t first = start_audio_clock(samples, 48000);
        for (uint64_t block = 2; block <= 48001; ++block) {
            uint64_t expected = block * samples * 1000000000ULL / 48000
                              - (uint64_t)samples * 1000000000ULL / 48000;
            assert(schedule_next_audio_block() - first == expected);
        }
    }

    reset();
    assert(init_simulator_timing());
    assert(qos_calls == 1 && timebase_calls == 1 && policy_calls == 1 && releases == 1);

    reset(); qos_result = EPERM;
    assert(!init_simulator_timing());
    assert(qos_calls == 1 && timebase_calls == 0 && policy_calls == 0 && releases == 0);

    reset(); timebase_result = KERN_FAILURE;
    assert(!init_simulator_timing());
    assert(timebase_calls == 1 && policy_calls == 0 && releases == 0);

    reset(); numerator = 0;
    assert(!init_simulator_timing() && policy_calls == 0);
    reset(); denominator = 0;
    assert(!init_simulator_timing() && policy_calls == 0);
    reset(); numerator = 1; denominator = UINT32_MAX;
    assert(!init_simulator_timing() && policy_calls == 0);

    reset(); policy_result = KERN_FAILURE;
    assert(!init_simulator_timing());
    assert(policy_calls == 1 && releases == 1);
    return 0;
}
