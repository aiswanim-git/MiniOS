#include "../include/process.h"

#include <stdio.h>
#include <string.h>

static pcb_t table[PROC_MAX];
static int   next_pid;
static int   live_count;

void proc_manager_init(void)
{
    memset(table, 0, sizeof table);
    next_pid = 0;
    live_count = 0;
}

void proc_manager_reset(void)
{
    proc_manager_init();
}

int proc_create(const char *name, int priority, int burst_time)
{
    if (!name || priority < 0 || priority > 2 || burst_time <= 0) return -1;
    if (next_pid >= PROC_MAX) return -1;

    pcb_t *p = &table[next_pid];
    p->pid = next_pid;
    strncpy(p->name, name, PROC_NAME_MAX);
    p->name[PROC_NAME_MAX] = '\0';
    p->priority = priority;
    p->burst_time = burst_time;
    p->remaining_time = burst_time;
    p->state = PROC_NEW;
    p->arrival_time = 0;
    p->completion_time = -1;
    p->waiting_time = 0;
    p->turnaround_time = 0;

    live_count++;
    return next_pid++;
}

int proc_kill(int pid)
{
    if (pid < 0 || pid >= next_pid) return -1;
    if (table[pid].state == PROC_EXITED) return -1;
    table[pid].state = PROC_EXITED;
    table[pid].remaining_time = 0;
    live_count--;
    return 0;
}

pcb_t *proc_get(int pid)
{
    if (pid < 0 || pid >= next_pid) return NULL;
    return &table[pid];
}

int proc_count(void)
{
    return live_count;
}

int proc_total(void)
{
    return next_pid;
}

static const char *state_name(proc_state_t s)
{
    switch (s) {
        case PROC_NEW:     return "NEW";
        case PROC_READY:   return "READY";
        case PROC_RUNNING: return "RUNNING";
        case PROC_EXITED:  return "EXITED";
    }
    return "?";
}

void proc_list(void)
{
    printf("%-5s %-16s %-8s %-8s %-9s %-10s %-10s\n",
           "PID", "NAME", "PRIO", "BURST", "REMAIN", "STATE", "COMPLETE");
    for (int i = 0; i < next_pid; ++i) {
        pcb_t *p = &table[i];
        if (p->completion_time >= 0)
            printf("%-5d %-16s %-8d %-8d %-9d %-10s %-10d\n",
                   p->pid, p->name, p->priority, p->burst_time,
                   p->remaining_time, state_name(p->state), p->completion_time);
        else
            printf("%-5d %-16s %-8d %-8d %-9d %-10s %-10s\n",
                   p->pid, p->name, p->priority, p->burst_time,
                   p->remaining_time, state_name(p->state), "-");
    }
}
