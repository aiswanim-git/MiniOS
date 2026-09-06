# MiniOS Design Notes

## Virtual disk layout

The virtual disk is a single 64 MB (`67108864` byte) in-memory buffer,
divided into `65536` blocks of `1024` bytes each. The bitmap and FAT
regions are sized to exactly cover this: 8 blocks of bitmap give
65536 bits (1 per block), and 256 blocks of FAT give 65536 4-byte
"next block" entries (1 per block).

| Blocks        | Count | Purpose                                            |
|---------------|-------|-----------------------------------------------------|
| 0             | 1     | Superblock: magic, `nblocks`, `nfreeblocks`, root block |
| 1 – 8         | 8     | Free-block bitmap (`8*1024*8 = 65536` bits, 1 bit/block) |
| 9 – 264       | 256   | FAT: `256*1024/4 = 65536` 4-byte "next block" entries |
| 265           | 1     | Root directory's first block                        |
| 266 – 65535   | 65270 | Data blocks (directories and file contents)          |

Blocks `0..265` are marked permanently allocated in the bitmap at
format time so the allocator never hands them out as data blocks.

## Bitmap

One bit per block, `1` = allocated, `0` = free. `block_alloc()`
performs a linear scan starting at the first data block (`266`) for a
free bit; this is simple, obviously correct, and fast enough at this
scale (a full disk holds ~65 000 blocks, i.e. worst case ~65 000
byte-tests, negligible compared to any real I/O this project would
otherwise be doing).

## FAT (File Allocation Table)

`FAT[b]` stores the block number that follows block `b` in whatever
chain `b` belongs to, or `0` if `b` is the last block of its chain.
Both file data and directories that outgrow their first block use the
same chaining mechanism — a directory is, structurally, just a file
whose "content" is a sequence of 32-byte directory-entry records.

## Directory / entry design

Each directory entry is a fixed 32-byte record:

```c
typedef struct {
    char     type;                 /* 'd' or 'f' */
    char     name[23];             /* NUL-terminated, <=22 chars */
    uint32_t size;                  /* bytes (file) / entry count (dir) */
    uint32_t firstblock;
} dirent_t;                         /* 1 + 23 + 4 + 4 = 32 bytes */
```

32 entries fit in one 1024-byte block; a directory chains additional
blocks via the FAT exactly like a file does once it exceeds 32
entries (exercised by `test_tree_and_many_entries`, which creates 70
files in one directory).

Entry `0` of every directory is a self-referential `"."` entry whose
`size` field doubles as **the directory's live entry count** — a
compact convention that avoids needing a separate "directory header"
struct. Entry `1` is always `".."`, pointing at the parent's
first block. `dir_add()` appends after the current count and bumps it;
`dir_remove()` deletes an entry by swapping the last entry into its
slot (O(1), and safe because directory entries are unordered).

## Path resolution

`path_resolve()` walks a `/`-or-relative path component by component
starting from either the root block (`path[0] == '/'`) or the
filesystem's current working-directory block. It returns the parent
block, the target block (`0` if the last component doesn't exist), the
target's type (`'d'`, `'f'`, or `'X'` for "doesn't exist"), and the
final path component's name — which lets a single resolution serve
`mkdir`, `rm`, `fs_write`, etc. without re-parsing the path.

`fs_cd()` does not store a separate parallel path string incrementally;
instead, after changing `cwd_block`, `recompute_cwd_path()` walks `..`
entries back to the root and rebuilds the absolute path string from
scratch. This is O(depth) per `cd`, trivially bounded, and avoids an
entire class of bugs around keeping a cached path string in sync with
`.`/`..` traversal.

## PCB / process model

```c
typedef struct {
    int pid, priority, burst_time, remaining_time;
    proc_state_t state;             /* NEW / READY / RUNNING / EXITED */
    int arrival_time, completion_time, waiting_time, turnaround_time;
    char name[32];
} pcb_t;
```

MiniOS processes are **simulated**, not real fork/exec'd OS processes:
each one is defined by a single total CPU burst requirement and a
priority. A more elaborate design could add a real multi-process
launcher/timer/signal setup (e.g. `SIGUSR1`-driven preemption across
real Unix processes synchronized with semaphores) to demonstrate
*actual* OS-level context switching across `fork()`ed processes, but
that's explicitly out of scope here (no ELF loading, no real
preemption). What matters for this project is the part that's actually
the point of it: the PCB fields, process states, and — most
importantly — the **scheduling algorithm**.

## Priority round-robin scheduling

Three ready queues (priorities `0` = highest .. `2` = lowest), each a
simple bounded FIFO. `sched_run()` always dispatches from the
highest-priority non-empty queue. The dispatched process runs for
`min(quantum[priority], remaining_time)` simulated time units:

- If that empties `remaining_time`, the process transitions to
  `EXITED` and its `completion_time`/`turnaround_time`/`waiting_time`
  are recorded.
- Otherwise it is re-enqueued at the back of *its own* priority queue
  (round robin) and a context switch is counted.

Default quanta are `{10, 5, 2}` for priorities `{0, 1, 2}` — a
descending schedule so higher-priority processes get more CPU time per
dispatch — but `sched_init()` accepts a custom 3-element array so this
is configurable, as required.

This is a classic multilevel queue with round robin *within* each
level, not true multilevel *feedback* queue (processes never change
priority). That matches "priority-based round-robin scheduling" as
scoped, without the added complexity of dynamic priority aging.

## fsck approach

`fs_fsck()` performs a mark-and-sweep consistency check:

1. Recursively walk the directory tree starting at the root, following
   every directory's own FAT chain and every file's FAT chain, marking
   every block visited in a scratch bitmap (`visited[]`).
2. Compare `visited[]` against the on-disk bitmap for every data block:
   - **Allocated but unreachable ("leaked")** — the bitmap says the
     block is in use, but nothing in the directory tree points to it
     (e.g. left over from a bug in an allocator/free path). If `repair`
     is set, these blocks are freed.
   - **Reachable but marked free ("corrupt")** — the directory tree
     points at a block the bitmap thinks is free; this indicates
     corruption and is reported (not auto-repaired, since guessing
     which side — bitmap or tree — is "correct" could destroy data).
3. Cross-check the superblock's cached `nfreeblocks` against a fresh
   count of unset bitmap bits; repair re-derives it from the bitmap if
   `repair` is set.
4. Along the way, flag directory entries with out-of-range
   `firstblock` values or an unrecognised `type`.

This intentionally does not attempt deeper repairs (e.g. re-linking a
broken FAT chain, or guessing at a directory entry's original name) —
those require heuristics or data recovery techniques beyond a basic
consistency checker, which is what the project scope calls for.

## Other notable decisions

- **Fixed 64 MB disk, no runtime resizing.** The bitmap/FAT region
  sizes are compile-time constants sized exactly for 65536 blocks, per
  the stated requirement ("64 MB virtual/in-memory disk"). Making the
  size runtime-configurable would mean dynamically sizing the bitmap
  and FAT regions (and therefore where the root directory and first
  data block start), which adds real complexity for no requirement
  this project actually has.
- **In-process heap buffer instead of SysV shared memory.** See
  `docs/architecture.md` for the full rationale — this is the most
  consequential design decision in the project, and the one most worth
  being able to defend.
- **No multi-binary split.** Some OS-course filesystem exercises split
  disk creation, the shell, and shared helpers into separate compiled
  programs communicating via shared memory. Since MiniOS is a single
  process, that split collapses naturally into `filesystem.c` (disk
  logic) plus `shell.c` (the command loop).
