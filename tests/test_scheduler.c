#include "../include/scheduler.h"
#include "../include/process.h"

#include <stdio.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

static void setup(void)
{
    proc_manager_reset();
    sched_reset();
    sched_init(NULL); /* default quantum {10, 5, 2} */
}

static void test_single_process_runs_to_completion(void)
{
    printf("test_single_process_runs_to_completion\n");
    setup();
    int pid = proc_create("solo", 0, 7); /* shorter than quantum(0)=10 */
    sched_add(pid);
    int switches = sched_run();
    pcb_t *p = proc_get(pid);
    CHECK(p->state == PROC_EXITED, "process exits");
    CHECK(p->remaining_time == 0, "no remaining time left");
    CHECK(p->completion_time == 7, "completes at t=7 (burst < quantum, no preemption)");
    CHECK(switches == 0, "no context switches needed for a single short burst");
}

static void test_preemption_for_long_burst(void)
{
    printf("test_preemption_for_long_burst\n");
    setup();
    int pid = proc_create("long", 0, 25); /* quantum(0)=10 -> needs 3 slices */
    sched_add(pid);
    int switches = sched_run();
    pcb_t *p = proc_get(pid);
    CHECK(p->completion_time == 25, "completes after full burst");
    CHECK(switches == 2, "two preemptions for a 25-unit burst at quantum 10");
}

static void test_priority_ordering(void)
{
    printf("test_priority_ordering\n");
    setup();
    int low  = proc_create("low",  2, 3);
    int high = proc_create("high", 0, 3);
    int mid  = proc_create("mid",  1, 3);
    /* add in low/high/mid order -- scheduler must still run high first */
    sched_add(low);
    sched_add(high);
    sched_add(mid);
    sched_run();

    pcb_t *ph = proc_get(high);
    pcb_t *pm = proc_get(mid);
    pcb_t *pl = proc_get(low);
    CHECK(ph->completion_time < pm->completion_time, "high priority finishes before mid");
    CHECK(pm->completion_time < pl->completion_time, "mid priority finishes before low");
}

static void test_round_robin_within_same_priority(void)
{
    printf("test_round_robin_within_same_priority\n");
    setup();
    int a = proc_create("a", 1, 8); /* quantum(1) = 5 */
    int b = proc_create("b", 1, 8);
    sched_add(a);
    sched_add(b);
    sched_run();
    pcb_t *pa = proc_get(a);
    pcb_t *pb = proc_get(b);
    /* Round robin: a runs 5 (t=5), b runs 5 (t=10), a runs remaining 3 (t=13, exits),
     * b runs remaining 3 (t=16, exits). */
    CHECK(pa->completion_time == 13, "first process finishes at t=13");
    CHECK(pb->completion_time == 16, "second process finishes at t=16");
}

static void test_stats_after_run(void)
{
    printf("test_stats_after_run\n");
    setup();
    sched_add(proc_create("a", 0, 5));
    sched_add(proc_create("b", 0, 5));
    sched_run();
    sched_stats_t s = sched_get_stats();
    CHECK(s.total_processes == 2, "stats count processes added");
    CHECK(s.total_time == 10, "total simulated time matches sum of bursts (no preemption)");
    CHECK(s.avg_turnaround_time > 0, "average turnaround computed");
}

static void test_sched_add_rejects_bad_pid(void)
{
    printf("test_sched_add_rejects_bad_pid\n");
    setup();
    CHECK(sched_add(999) == -1, "adding an unknown pid fails");
    int pid = proc_create("x", 0, 1);
    proc_kill(pid);
    CHECK(sched_add(pid) == -1, "adding an already-exited process fails");
}

int main(void)
{
    test_single_process_runs_to_completion();
    test_preemption_for_long_burst();
    test_priority_ordering();
    test_round_robin_within_same_priority();
    test_stats_after_run();
    test_sched_add_rejects_bad_pid();

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
