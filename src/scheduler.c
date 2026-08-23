#include "../include/scheduler.h"
#include "../include/process.h"

#include <stdio.h>
#include <string.h>

/* Simple FIFO ready queue, one per priority level. Sized PROC_MAX+1 so a
 * full queue can still be distinguished from an empty one. */
typedef struct {
    int items[PROC_MAX + 1];
    int head, tail, count;
} queue_t;

static queue_t   ready[PROC_NUM_PRIORITIES];
static int       quantum[PROC_NUM_PRIORITIES] = {10, 5, 2};
static sched_stats_t stats;

static void q_init(queue_t *q) { q->head = q->tail = q->count = 0; }

static void q_push(queue_t *q, int pid)
{
    if (q->count >= PROC_MAX + 1) return; /* should not happen: bounded by PROC_MAX processes */
    q->items[q->tail] = pid;
    q->tail = (q->tail + 1) % (PROC_MAX + 1);
    q->count++;
}

static int q_pop(queue_t *q)
{
    if (q->count == 0) return -1;
    int pid = q->items[q->head];
    q->head = (q->head + 1) % (PROC_MAX + 1);
    q->count--;
    return pid;
}

static int q_empty(queue_t *q) { return q->count == 0; }

void sched_init(const int q_in[PROC_NUM_PRIORITIES])
{
    if (q_in) memcpy(quantum, q_in, sizeof quantum);
    else { quantum[0] = 10; quantum[1] = 5; quantum[2] = 2; }
    sched_reset();
}

void sched_reset(void)
{
    for (int i = 0; i < PROC_NUM_PRIORITIES; ++i) q_init(&ready[i]);
    memset(&stats, 0, sizeof stats);
}

int sched_add(int pid)
{
    pcb_t *p = proc_get(pid);
    if (!p || p->state == PROC_EXITED) return -1;
    p->state = PROC_READY;
    q_push(&ready[p->priority], pid);
    stats.total_processes++;
    return 0;
}

static int any_ready(void)
{
    for (int i = 0; i < PROC_NUM_PRIORITIES; ++i) if (!q_empty(&ready[i])) return 1;
    return 0;
}

/* Picks the highest-priority non-empty queue (lowest priority number
 * wins) and pops its front process. Returns -1 if all queues empty. */
static int dispatch_next(void)
{
    for (int i = 0; i < PROC_NUM_PRIORITIES; ++i) {
        if (!q_empty(&ready[i])) return q_pop(&ready[i]);
    }
    return -1;
}

int sched_run(void)
{
    int clock = 0;

    while (any_ready()) {
        int pid = dispatch_next();
        pcb_t *p = proc_get(pid);
        if (!p) continue;

        int prio = p->priority;
        int slice = quantum[prio] < p->remaining_time ? quantum[prio] : p->remaining_time;

        p->state = PROC_RUNNING;
        printf("[t=%3d] dispatch  : pid=%-3d name=%-12s prio=%d run for %d unit(s) (remaining before: %d)\n",
               clock, p->pid, p->name, prio, slice, p->remaining_time);

        clock += slice;
        p->remaining_time -= slice;

        if (p->remaining_time == 0) {
            p->state = PROC_EXITED;
            p->completion_time = clock;
            p->turnaround_time = p->completion_time - p->arrival_time;
            p->waiting_time = p->turnaround_time - p->burst_time;
            printf("[t=%3d] complete  : pid=%-3d name=%-12s turnaround=%d waiting=%d\n",
                   clock, p->pid, p->name, p->turnaround_time, p->waiting_time);
        } else {
            p->state = PROC_READY;
            q_push(&ready[prio], pid);
            stats.context_switches++;
            printf("[t=%3d] preempt   : pid=%-3d name=%-12s remaining=%d, back of priority-%d queue\n",
                   clock, p->pid, p->name, p->remaining_time, prio);
        }
    }

    stats.total_time = clock;

    double sum_wait = 0, sum_turn = 0;
    int n = 0;
    for (int pid = 0; pid < proc_total(); ++pid) {
        pcb_t *p = proc_get(pid);
        if (p && p->completion_time >= 0) {
            sum_wait += p->waiting_time;
            sum_turn += p->turnaround_time;
            n++;
        }
    }
    stats.avg_waiting_time = n ? sum_wait / n : 0.0;
    stats.avg_turnaround_time = n ? sum_turn / n : 0.0;

    return stats.context_switches;
}

sched_stats_t sched_get_stats(void)
{
    return stats;
}

void sched_print_stats(void)
{
    printf("--- Scheduler statistics ---\n");
    printf("Processes scheduled : %d\n", stats.total_processes);
    printf("Context switches    : %d\n", stats.context_switches);
    printf("Total simulated time: %d\n", stats.total_time);
    printf("Avg waiting time    : %.2f\n", stats.avg_waiting_time);
    printf("Avg turnaround time : %.2f\n", stats.avg_turnaround_time);
}
