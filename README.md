# MiniOS — FAT File System, Process Management & CPU Scheduling

MiniOS is a small, self-contained C program that combines:

- a **FAT-based virtual filesystem** on a 64 MB in-memory disk
- a **process manager** (PCB table, process states)
- a **priority round-robin CPU scheduler**
- an interactive **shell**
- a basic **fsck** consistency checker

I built it as a self-directed project to go deeper into OS internals
than my undergraduate operating systems course covered — designing and
implementing the on-disk layout, path resolution, process table, and
scheduler myself, then testing it thoroughly. See `docs/architecture.md`
and `docs/design.md` for the design rationale behind each subsystem.

## Key features

- 64 MB virtual disk: bitmap-based free-block tracking, FAT-style
  block chaining, hierarchical directories.
- File & directory operations: `mkdir`, `rm`, `ls`, `tree`, `stat`,
  `cd`/`pwd`, `cat`, `cp` (within the virtual disk), `import`/`export`
  (host filesystem ↔ virtual disk).
- `fsck`: detects leaked (allocated-but-unreachable) blocks, bitmap/FAT
  inconsistencies, and corrupt directory entries; can repair the
  recoverable ones.
- Process manager: PCB table with `NEW`/`READY`/`RUNNING`/`EXITED`
  states, `spawn`/`kill`/`ps`.
- Scheduler: three priority levels, round robin within each level,
  configurable time quantum per level, reports context switches,
  average waiting time, and average turnaround time.
- Interactive `miniOS> ` shell tying it all together, plus unit and
  integration tests for every subsystem.

## Architecture at a glance

```
include/filesystem.h  <-- src/filesystem.c   (disk, bitmap, FAT, dirs, fsck)
include/process.h     <-- src/process.c      (PCB table)
include/scheduler.h   <-- src/scheduler.c    (ready queues, dispatch loop)
include/shell.h       <-- src/shell.c        (REPL, uses only the headers above)
                           src/main.c         (creates the disk, runs the shell)
```

Full details: [`docs/architecture.md`](docs/architecture.md).

## Build & run

Requires `clang` and `make` (developed/targeted for macOS; no
Linux-only APIs are used, so it also builds with `gcc`).

```sh
make          # optimized build -> ./minios
make debug    # -g -O0 build
make asan     # AddressSanitizer + UndefinedBehaviorSanitizer build
make test     # builds and runs all unit/integration tests
make clean    # removes build/ and the minios binary
./minios
```

## Shell commands

**Filesystem**

| Command | Description |
|---|---|
| `ls [path]` | list a directory (default: current directory) |
| `cd [path]` | change directory (no argument: go to `/`) |
| `pwd` | print the current working directory |
| `mkdir <path>` | create a directory |
| `rm <path>` | remove a file or an empty directory |
| `cat <path>` | print a file's contents |
| `tree [path]` | print a directory subtree |
| `stat <path>` | print type/size/first-block metadata |
| `cp <src> <dst>` | copy a file within the virtual disk |
| `import <hostfile> <vdpath>` | copy a file from the host machine into the virtual disk |
| `export <vdpath> <hostfile>` | copy a virtual-disk file out to the host machine |
| `write <path> <text...>` | create/overwrite a file with literal text |
| `df` | show total/free/used block counts |
| `fsck [-r]` | check (and, with `-r`, repair) the virtual disk |

**Process manager / scheduler**

| Command | Description |
|---|---|
| `spawn <name> <priority 0-2> <burst>` | create a simulated process |
| `kill <pid>` | terminate a process |
| `ps` | list all processes and their state |
| `run` | run every `READY`/`NEW` process to completion via priority round robin |
| `stats` | print the last run's scheduler statistics |

`help` lists all commands from within the shell; `exit`/`quit` leaves it.

## Example session

```
$ ./minios
MiniOS shell. Type 'help' for a list of commands.
miniOS:/> mkdir home
miniOS:/> mkdir home/alice
miniOS:/> cd home/alice
miniOS:/home/alice> write notes.txt Hello MiniOS filesystem
miniOS:/home/alice> cat notes.txt
Hello MiniOS filesystem
miniOS:/home/alice> cd /
miniOS:/> tree
.
  home/
    alice/
      notes.txt
miniOS:/> fsck
Running fsck on virtual disk (65536 blocks)...
fsck: filesystem is clean.
miniOS:/> spawn A 0 15
spawned pid 0
miniOS:/> spawn B 1 8
spawned pid 1
miniOS:/> run
[t=  0] dispatch  : pid=0   name=A            prio=0 run for 10 unit(s) (remaining before: 15)
[t= 10] preempt   : pid=0   name=A            remaining=5, back of priority-0 queue
[t= 10] dispatch  : pid=0   name=A            prio=0 run for 5 unit(s) (remaining before: 5)
[t= 15] complete  : pid=0   name=A            turnaround=15 waiting=0
[t= 15] dispatch  : pid=1   name=B            prio=1 run for 5 unit(s) (remaining before: 8)
[t= 20] preempt   : pid=1   name=B            remaining=3, back of priority-1 queue
[t= 20] dispatch  : pid=1   name=B            prio=1 run for 3 unit(s) (remaining before: 3)
[t= 23] complete  : pid=1   name=B            turnaround=23 waiting=15
miniOS:/> stats
--- Scheduler statistics ---
Processes scheduled : 2
Context switches    : 2
Total simulated time: 23
Avg waiting time    : 7.50
Avg turnaround time : 19.00
miniOS:/> exit
```

## Testing

```sh
make test
```

This builds and runs four standalone test binaries (each linking the
core sources directly, no test framework dependency):

- `tests/test_filesystem.c` — disk formatting, mkdir/ls/cd, file
  read/write round-trips (including multi-block files), overwrite,
  rm, cp, host import/export, large directories that span multiple
  FAT-chained blocks, and fsck.
- `tests/test_process.c` — PCB creation/validation, kill, multiple
  processes.
- `tests/test_scheduler.c` — single-process runs, preemption at the
  quantum boundary, priority ordering, round robin within a priority
  level, statistics, and input validation.
- `tests/test_integration.c` — a full simulated session exercising
  the filesystem, process manager, and scheduler together (including
  persisting a scheduler run's summary back into the virtual disk),
  plus a check that independently created filesystems/process states
  don't leak into each other.

All 180 checks pass, including under `make asan` (AddressSanitizer +
UndefinedBehaviorSanitizer), which reports no memory errors or
undefined behavior across the full test suite and a scripted
interactive session covering every shell command.

## Limitations

- The 64 MB disk size is fixed at compile time (see `docs/design.md`
  for why); there is no runtime resize.
- File/directory names are capped at 22 characters.
- `fsck` detects and repairs leaked blocks and a stale free-block
  count, and reports (without guessing a fix for) directory-tree vs.
  bitmap contradictions and corrupt block chains; it does not attempt
  data recovery.
- Processes are simulated PCBs with a single total CPU burst each —
  there is no real `fork`/`exec`, no I/O bursts, and no real
  timer-driven preemption (all out of scope; see `docs/design.md`).
- The scheduler is a static 3-level priority round robin (quanta
  `{10, 5, 2}` by default, configurable via `sched_init`); priorities
  never change dynamically (no aging / feedback).
- Single-threaded, single-user: there is no concurrent access to the
  virtual disk from multiple shells/processes. This is a deliberate
  simplification — see `docs/architecture.md` for the reasoning.
