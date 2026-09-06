#ifndef MINIOS_PROCESS_H
#define MINIOS_PROCESS_H

#define PROC_NAME_MAX 31
#define PROC_MAX      256

/*
 * Simulated process manager built around the classic PCB / process-state
 * model (NEW/READY/RUNNING/EXITED) used in real operating systems,
 * adapted for a single in-process simulation rather than real
 * fork/exec'd OS processes (out of scope per project requirements: no
 * ELF loading, no real scheduling of OS-level threads).
 *
 * Each simulated process has a total CPU burst time (in abstract time
 * units) that the scheduler consumes over one or more time slices.
 */

typedef enum {
    PROC_NEW = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_EXITED
} proc_state_t;

typedef struct {
    int           pid;
    char          name[PROC_NAME_MAX + 1];
    int           priority;         /* 0 = highest, PROC_NUM_PRIORITIES-1 = lowest */
    int           burst_time;       /* total CPU time required */
    int           remaining_time;   /* CPU time left */
    proc_state_t  state;
    int           arrival_time;
    int           completion_time;  /* -1 until it exits */
    int           waiting_time;
    int           turnaround_time;
} pcb_t;

/* Process table lifecycle */
void proc_manager_init(void);
void proc_manager_reset(void); /* clears the table, for tests */

/* Process operations. priority must be in [0, PROC_NUM_PRIORITIES). Returns
 * the new pid (>=0) or -1 on failure (table full / bad args). */
int  proc_create(const char *name, int priority, int burst_time);
int  proc_kill(int pid);            /* forcibly marks a process EXITED */
pcb_t *proc_get(int pid);            /* NULL if not found */
int  proc_count(void);               /* number of live (non-exited) processes */
int  proc_total(void);               /* total processes ever created (== next pid) */
void proc_list(void);                /* prints ps-style table */

#endif /* MINIOS_PROCESS_H */
