/* Integration tests: exercise the filesystem, process manager and
 * scheduler together through their public APIs only, the same way
 * shell.c does. This does not drive the interactive shell's stdin loop
 * directly (that would require faking a TTY); instead it performs the
 * same sequence of API calls the shell would make for a realistic
 * session, which is what actually needs to be verified end-to-end. */

#include "../include/filesystem.h"
#include "../include/process.h"
#include "../include/scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

/* A realistic "session": build a small directory tree, write some
 * files, run a batch of processes through the scheduler, and persist a
 * summary log back into the virtual filesystem -- then verify
 * everything is consistent and fsck-clean. */
static void test_full_session(void)
{
    printf("test_full_session\n");

    FileSystem *fs = fs_create();
    proc_manager_reset();
    sched_reset();
    sched_init(NULL);

    CHECK(fs_mkdir(fs, "/logs") == FS_OK, "create /logs");
    CHECK(fs_mkdir(fs, "/bin") == FS_OK, "create /bin");
    CHECK(fs_write(fs, "/bin/hello", "print('hi')", 11) == FS_OK, "write a fake program file");

    const char *names[] = {"init", "shell", "worker1", "worker2", "worker3"};
    int priorities[]    = {0, 0, 1, 1, 2};
    int bursts[]         = {4, 6, 12, 9, 20};
    int pids[5];

    for (int i = 0; i < 5; ++i) {
        pids[i] = proc_create(names[i], priorities[i], bursts[i]);
        CHECK(pids[i] == i, "sequential pid assignment");
        CHECK(sched_add(pids[i]) == 0, "process admitted to scheduler");
    }

    int switches = sched_run();
    CHECK(switches >= 0, "scheduler produced a non-negative switch count");

    for (int i = 0; i < 5; ++i) {
        pcb_t *p = proc_get(pids[i]);
        CHECK(p->state == PROC_EXITED, "every spawned process eventually exits");
        CHECK(p->remaining_time == 0, "no leftover burst time");
    }

    sched_stats_t stats = sched_get_stats();
    char log[512];
    int n = snprintf(log, sizeof log,
        "processes=%d context_switches=%d total_time=%d avg_wait=%.2f avg_turnaround=%.2f\n",
        stats.total_processes, stats.context_switches, stats.total_time,
        stats.avg_waiting_time, stats.avg_turnaround_time);
    CHECK(fs_write(fs, "/logs/run1.log", log, (size_t)n) == FS_OK, "persist scheduler summary to vfs");

    void *buf; size_t len;
    CHECK(fs_read(fs, "/logs/run1.log", &buf, &len) == FS_OK, "read log back");
    CHECK(len == (size_t)n, "log length round-trips");
    free(buf);

    CHECK(fs_fsck(fs, 0) == 0, "filesystem is consistent after full session");

    fs_stat_t st;
    CHECK(fs_stat(fs, "/bin/hello", &st) == FS_OK, "unrelated earlier file untouched by scheduler work");
    CHECK(st.size == 11, "unrelated file content untouched");

    fs_destroy(fs);
}

/* Filesystem and process/scheduler state must be independent: resetting
 * one should not disturb the other, and multiple filesystems/scheduler
 * runs can be created and torn down in sequence without leaking state. */
static void test_independent_subsystem_lifecycles(void)
{
    printf("test_independent_subsystem_lifecycles\n");

    FileSystem *fs1 = fs_create();
    fs_write(fs1, "/a.txt", "first disk", 10);

    proc_manager_reset();
    sched_reset();
    sched_init(NULL);
    int pid = proc_create("p", 0, 3);
    sched_add(pid);
    sched_run();
    CHECK(proc_get(pid)->state == PROC_EXITED, "process finished on first disk's session");

    /* Swap in a second, independent filesystem; process/scheduler state
     * must be unaffected by filesystem lifecycle. */
    FileSystem *fs2 = fs_create();
    CHECK(fs_fsck(fs2, 0) == 0, "second filesystem starts clean");
    void *buf; size_t len;
    CHECK(fs_read(fs2, "/a.txt", &buf, &len) == FS_ERR_NOT_FOUND,
          "second filesystem does not see first filesystem's files");
    CHECK(proc_get(pid)->state == PROC_EXITED,
          "process table state survives across filesystem swap (subsystems are independent)");

    fs_destroy(fs1);
    fs_destroy(fs2);
}

int main(void)
{
    test_full_session();
    test_independent_subsystem_lifecycles();

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
