#ifndef MINIOS_SCHEDULER_H
#define MINIOS_SCHEDULER_H

/*
 * Priority-based round-robin scheduler implementing a classic multilevel
 * ready-queue design (three priority levels, each with its own time
 * quantum: priority 0 gets the longest quantum, priority 2 the
 * shortest). The scheduler always dispatches from the highest-priority
 * non-empty queue; within a queue processes are served round-robin.
 *
 * This is a discrete-event simulation over an abstract clock -- there is
 * no real preemption via signals/timers (out of scope), but the
 * algorithm and the resulting statistics (context switches, waiting
 * time, turnaround time) are the standard ones used to evaluate
 * scheduling algorithms.
 */

#define PROC_NUM_PRIORITIES 3

typedef struct {
    int total_processes;
    int context_switches;
    int total_time;              /* simulated makespan */
    double avg_waiting_time;
    double avg_turnaround_time;
} sched_stats_t;

/* quantum[p] = time quantum granted per dispatch at priority p.
 * Pass NULL to use the defaults {10, 5, 2}. */
void sched_init(const int quantum[PROC_NUM_PRIORITIES]);

/* Add an existing (PROC_READY or PROC_NEW) process to the scheduler's
 * ready queues, in arrival order. Returns 0 on success, -1 if pid is
 * unknown or already finished. */
int sched_add(int pid);

/* Runs every process added via sched_add to completion using priority
 * round-robin, printing a trace of dispatch / preemption / completion
 * events. Returns the number of context switches performed. */
int sched_run(void);

/* Returns the statistics from the most recent sched_run(). */
sched_stats_t sched_get_stats(void);
void          sched_print_stats(void);

/* Clears queues and stats, for tests / re-runs. */
void sched_reset(void);

#endif /* MINIOS_SCHEDULER_H */
