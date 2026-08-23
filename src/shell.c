#include "../include/shell.h"
#include "../include/process.h"
#include "../include/scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 1024

static void print_help(void)
{
    printf(
        "Filesystem commands:\n"
        "  ls [path]              list a directory (default: cwd)\n"
        "  cd [path]               change directory (no arg: go to /)\n"
        "  pwd                     print working directory\n"
        "  mkdir <path>            create a directory\n"
        "  rm <path>               remove a file or empty directory\n"
        "  cat <path>               print a file's contents\n"
        "  tree [path]              print a directory subtree\n"
        "  stat <path>              print metadata for a path\n"
        "  cp <src> <dst>           copy a file within the virtual disk\n"
        "  import <hostfile> <vd>   copy a host file into the virtual disk\n"
        "  export <vd> <hostfile>   copy a virtual-disk file to the host\n"
        "  write <path> <text...>   create/overwrite a file with literal text\n"
        "  df                       show block usage\n"
        "  fsck [-r]                check (and optionally repair) the disk\n"
        "\n"
        "Process / scheduler commands:\n"
        "  spawn <name> <prio 0-2> <burst>   create a process\n"
        "  kill <pid>                         terminate a process\n"
        "  ps                                  list all processes\n"
        "  run                                  run all READY/NEW processes to completion\n"
        "  stats                                print last run's scheduler statistics\n"
        "\n"
        "  help                     show this message\n"
        "  exit | quit              leave the shell\n");
}

static void report(fs_result_t rc, const char *what)
{
    if (rc != FS_OK) fprintf(stderr, "*** %s: %s\n", what, fs_strerror(rc));
}

void shell_run(FileSystem *fs)
{
    char line[LINE_MAX];

    proc_manager_init();
    sched_init(NULL);

    printf("MiniOS shell. Type 'help' for a list of commands.\n");
    while (1) {
        printf("miniOS:%s> ", fs_pwd(fs));
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { printf("\n"); break; }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        char cmd[64] = {0};
        char *rest = line;
        sscanf(line, "%63s", cmd);
        if (cmd[0] == '\0') continue;
        rest += strlen(cmd);
        while (*rest == ' ') ++rest;

        if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit")) {
            break;

        } else if (!strcmp(cmd, "help")) {
            print_help();

        } else if (!strcmp(cmd, "ls")) {
            report(fs_ls(fs, rest), "ls");

        } else if (!strcmp(cmd, "cd")) {
            report(fs_cd(fs, rest), "cd");

        } else if (!strcmp(cmd, "pwd")) {
            printf("%s\n", fs_pwd(fs));

        } else if (!strcmp(cmd, "mkdir")) {
            if (*rest == '\0') fprintf(stderr, "*** mkdir: missing operand\n");
            else report(fs_mkdir(fs, rest), "mkdir");

        } else if (!strcmp(cmd, "rm")) {
            if (*rest == '\0') fprintf(stderr, "*** rm: missing operand\n");
            else report(fs_rm(fs, rest), "rm");

        } else if (!strcmp(cmd, "cat")) {
            if (*rest == '\0') fprintf(stderr, "*** cat: missing operand\n");
            else report(fs_cat(fs, rest), "cat");

        } else if (!strcmp(cmd, "tree")) {
            report(fs_tree(fs, rest), "tree");

        } else if (!strcmp(cmd, "stat")) {
            if (*rest == '\0') { fprintf(stderr, "*** stat: missing operand\n"); continue; }
            fs_stat_t st;
            fs_result_t rc = fs_stat(fs, rest, &st);
            if (rc != FS_OK) report(rc, "stat");
            else printf("name=%s type=%c size=%u firstblock=%u\n",
                        st.name, st.type, st.size, st.firstblock);

        } else if (!strcmp(cmd, "cp")) {
            char src[512], dst[512];
            if (sscanf(rest, "%511s %511s", src, dst) != 2) {
                fprintf(stderr, "*** cp: usage: cp <src> <dst>\n");
            } else report(fs_cp(fs, src, dst), "cp");

        } else if (!strcmp(cmd, "import")) {
            char src[512], dst[512];
            if (sscanf(rest, "%511s %511s", src, dst) != 2) {
                fprintf(stderr, "*** import: usage: import <hostfile> <vdpath>\n");
            } else report(fs_import(fs, src, dst), "import");

        } else if (!strcmp(cmd, "export")) {
            char src[512], dst[512];
            if (sscanf(rest, "%511s %511s", src, dst) != 2) {
                fprintf(stderr, "*** export: usage: export <vdpath> <hostfile>\n");
            } else report(fs_export(fs, src, dst), "export");

        } else if (!strcmp(cmd, "write")) {
            char path[512];
            int n = sscanf(rest, "%511s", path);
            if (n != 1) {
                fprintf(stderr, "*** write: usage: write <path> <text...>\n");
            } else {
                char *text = rest + strlen(path);
                while (*text == ' ') ++text;
                report(fs_write(fs, path, text, strlen(text)), "write");
            }

        } else if (!strcmp(cmd, "df")) {
            fs_df(fs);

        } else if (!strcmp(cmd, "fsck")) {
            int repair = !strcmp(rest, "-r");
            fs_fsck(fs, repair);

        } else if (!strcmp(cmd, "spawn")) {
            char name[64]; int prio, burst;
            if (sscanf(rest, "%63s %d %d", name, &prio, &burst) != 3) {
                fprintf(stderr, "*** spawn: usage: spawn <name> <priority 0-2> <burst>\n");
            } else {
                int pid = proc_create(name, prio, burst);
                if (pid < 0) fprintf(stderr, "*** spawn: failed (bad args or table full)\n");
                else { printf("spawned pid %d\n", pid); sched_add(pid); }
            }

        } else if (!strcmp(cmd, "kill")) {
            int pid;
            if (sscanf(rest, "%d", &pid) != 1 || proc_kill(pid) != 0)
                fprintf(stderr, "*** kill: no such live process\n");

        } else if (!strcmp(cmd, "ps")) {
            proc_list();

        } else if (!strcmp(cmd, "run")) {
            sched_run();

        } else if (!strcmp(cmd, "stats")) {
            sched_print_stats();

        } else {
            fprintf(stderr, "*** Unknown command: %s (type 'help')\n", cmd);
        }
    }
}
