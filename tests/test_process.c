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

static void test_create_basic(void)
{
    printf("test_create_basic\n");
    proc_manager_reset();
    int pid = proc_create("alpha", 0, 10);
    CHECK(pid == 0, "first pid is 0");
    pcb_t *p = proc_get(pid);
    CHECK(p != NULL, "proc_get finds it");
    CHECK(p->state == PROC_NEW, "new process starts in PROC_NEW");
    CHECK(p->remaining_time == 10, "remaining_time == burst_time initially");
    CHECK(proc_count() == 1, "one live process");
    CHECK(proc_total() == 1, "one process ever created");
}

static void test_invalid_create_rejected(void)
{
    printf("test_invalid_create_rejected\n");
    proc_manager_reset();
    CHECK(proc_create(NULL, 0, 5) == -1, "NULL name rejected");
    CHECK(proc_create("x", -1, 5) == -1, "negative priority rejected");
    CHECK(proc_create("x", 3, 5) == -1, "out-of-range priority rejected");
    CHECK(proc_create("x", 0, 0) == -1, "zero burst rejected");
    CHECK(proc_create("x", 0, -5) == -1, "negative burst rejected");
    CHECK(proc_total() == 0, "no processes were actually created");
}

static void test_kill(void)
{
    printf("test_kill\n");
    proc_manager_reset();
    int pid = proc_create("victim", 1, 20);
    CHECK(proc_count() == 1, "one live process before kill");
    CHECK(proc_kill(pid) == 0, "kill succeeds");
    CHECK(proc_get(pid)->state == PROC_EXITED, "state becomes EXITED");
    CHECK(proc_count() == 0, "no live processes after kill");
    CHECK(proc_kill(pid) == -1, "killing twice fails");
    CHECK(proc_kill(9999) == -1, "killing unknown pid fails");
}

static void test_multiple_processes(void)
{
    printf("test_multiple_processes\n");
    proc_manager_reset();
    int a = proc_create("a", 0, 5);
    int b = proc_create("b", 1, 5);
    int c = proc_create("c", 2, 5);
    CHECK(a == 0 && b == 1 && c == 2, "pids are assigned sequentially");
    CHECK(proc_count() == 3, "three live processes");
    proc_kill(b);
    CHECK(proc_count() == 2, "killing one leaves two live");
    CHECK(proc_total() == 3, "total count unaffected by kill");
}

int main(void)
{
    test_create_basic();
    test_invalid_create_rejected();
    test_kill();
    test_multiple_processes();

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
