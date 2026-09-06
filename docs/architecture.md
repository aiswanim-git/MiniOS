# MiniOS Architecture

## Subsystem overview

MiniOS is a single executable made of four largely independent
subsystems, each with a public header and exactly one implementation
file. The shell is the only piece that talks to more than one
subsystem, and it only ever does so through their public headers.

```
                     +--------------------+
                     |      shell.c       |
                     |  (command parsing) |
                     +----+-----+-----+---+
                          |     |     |
        include/filesystem.h  include/process.h
                          |     |     |
                          v     |     v
                +-----------+   |   +------------+
                | filesystem|   |   |  process   |
                |   .c      |   |   |   .c       |
                +-----------+   |   +-----+------+
                                v         |
                        include/scheduler.h
                                |         |
                                v         v
                          +-------------------+
                          |   scheduler.c     |
                          | (reads/writes PCB |
                          |  via process.h)   |
                          +-------------------+

                +---------------------+
                |       main.c        |
                | creates FileSystem, |
                | calls shell_run()   |
                +---------------------+
```

## Responsibilities of each file

- **include/filesystem.h / src/filesystem.c** — Owns the entire virtual
  disk: block allocation (bitmap), block chaining (FAT), directories,
  path resolution, file read/write, host import/export, and `fsck`. The
  `FileSystem` type is opaque; every other file only sees the functions
  declared in the header.

- **include/process.h / src/process.c** — Owns the process table
  (`pcb_t[]`), pid allocation, and process state transitions
  (`NEW -> READY -> RUNNING -> EXITED`). Has no notion of scheduling
  policy — it only stores and reports state.

- **include/scheduler.h / src/scheduler.c** — Owns the three
  priority-level ready queues and the round-robin dispatch loop. It
  reads and mutates `pcb_t` fields (via `proc_get()`) but never touches
  the process table's storage directly, and knows nothing about the
  filesystem.

- **include/shell.h / src/shell.c** — The interactive `miniOS> ` REPL.
  Parses one line at a time into a command + argument string and
  dispatches to `fs_*`, `proc_*`, and `sched_*` calls. Contains no
  filesystem/FAT/PCB internals of its own.

- **src/main.c** — Creates one `FileSystem`, hands it to `shell_run()`,
  and destroys it on exit. The entire program's persistent state lives
  in `main`'s stack frame (the `FileSystem*`) plus the process
  table/scheduler queues, which are process-global by design (see
  design.md).

## Data flow: a typical shell command

1. `shell_run()` reads a line from stdin and splits it into `cmd` +
   `rest`.
2. For filesystem commands (`ls`, `cd`, `mkdir`, `cat`, ...), `rest` is
   passed straight to the matching `fs_*` function, which resolves the
   path against the current working directory stored inside the
   `FileSystem` struct, walks the FAT/bitmap/directory structures, and
   returns an `fs_result_t`.
3. For process/scheduler commands (`spawn`, `kill`, `ps`, `run`,
   `stats`), `shell_run()` calls `proc_create()`/`proc_kill()` (which
   only touch the process table) or `sched_add()`/`sched_run()` (which
   read/mutate PCBs by pid and drive the ready queues).
4. Errors are never fatal to the shell: every `fs_*` call returns an
   `fs_result_t` that the shell turns into a `*** <cmd>: <message>`
   line on stderr, and the REPL loop continues.

## Why the filesystem is an in-process heap buffer, not shared memory

A common way to build this kind of exercise is to split the filesystem
across multiple independent Unix processes (a disk-manager process, a
shell process, ...) that attach to a shared-memory segment, because
the disk image needs to outlive any single process and be visible to
several processes at once. MiniOS has no such requirement: filesystem,
process manager, scheduler and shell all live in one program, and the
disk only needs to persist for the lifetime of that program. A single
`malloc`'d 64 MB buffer gives the exact same block-based semantics
without the extra machinery (`shmget`/`shmat`/`ftok`, and the platform
quirks of SysV IPC key generation) — and it is what makes the
filesystem trivially unit testable (`fs_create()`/`fs_destroy()` per
test, no leftover IPC segments to clean up between test runs). See
`docs/design.md` for further discussion of this and other design
trade-offs.
