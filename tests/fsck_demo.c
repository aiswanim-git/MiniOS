/* Standalone fault-injection demo for fs_fsck(). Not part of `make test`;
 * this is a throwaway tool to produce a screenshot of fsck catching and
 * repairing a real inconsistency. It #includes filesystem.c directly so
 * it can reach the internal block_alloc() helper and deliberately create
 * a "leaked block" (allocated in the bitmap, but not linked into any
 * directory or file chain) -- exactly the case fs_fsck() is designed to
 * detect and repair. */
#include "../src/filesystem.c"
#include <stdio.h>

int main(void)
{
    FileSystem *fs = fs_create();
    fs_mkdir(fs, "/home");
    fs_write(fs, "/home/notes.txt", "hello world", 11);

    printf("=== fsck on a clean filesystem ===\n");
    fs_fsck(fs, 0);

    uint32_t leaked = block_alloc(fs);
    printf("\n[demo] manually allocated block %u directly via block_alloc(),\n"
           "       without linking it into any file or directory --\n"
           "       simulating a bug that leaks a block.\n", leaked);

    printf("\n=== fsck detects it (no repair) ===\n");
    fs_fsck(fs, 0);

    printf("\n=== fsck -r repairs it ===\n");
    fs_fsck(fs, 1);

    printf("\n=== fsck confirms clean after repair ===\n");
    fs_fsck(fs, 0);

    fs_destroy(fs);
    return 0;
}
