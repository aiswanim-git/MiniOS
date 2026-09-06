#ifndef MINIOS_FILESYSTEM_H
#define MINIOS_FILESYSTEM_H

#include <stddef.h>
#include <stdint.h>

/*
 * MiniOS FAT-based virtual filesystem.
 *
 * The virtual disk is a single 64 MB in-memory buffer, laid out in
 * 1024-byte blocks:
 *
 *   block 0        : superblock (nblocks, nfreeblocks, root block, magic)
 *   blocks 1-8      : free-block bitmap (65536 bits -> covers all blocks)
 *   blocks 9-264    : FAT (65536 4-byte next-block pointers, one per block)
 *   block 265       : root directory
 *   blocks 266..N   : data blocks (directories and file data)
 *
 * Directory entries are 32-byte fixed records (type, name, size,
 * firstblock). A directory that outgrows 32 entries in its first block
 * chains additional blocks via the FAT, exactly like file data does.
 *
 * The filesystem is intentionally single-process / in-process (a plain
 * heap buffer, not a SysV shared-memory segment). MiniOS is one program
 * embedding filesystem + process manager + scheduler + shell, so there
 * is no need for multiple processes to share the disk image; using
 * shared memory here would only add complexity without benefit. See
 * docs/design.md for the full rationale.
 */

#define FS_BLOCK_SIZE      1024u
#define FS_DISK_SIZE_BYTES (64u * 1024u * 1024u) /* 64 MB */
#define FS_NAME_MAX        22   /* + 1 for NUL, fits the 23-byte name field */

typedef enum {
    FS_OK = 0,
    FS_ERR_NOT_FOUND,
    FS_ERR_EXISTS,
    FS_ERR_NOT_DIR,
    FS_ERR_NOT_FILE,
    FS_ERR_NO_SPACE,
    FS_ERR_INVALID,
    FS_ERR_IO,
    FS_ERR_NOT_EMPTY
} fs_result_t;

typedef struct filesystem FileSystem; /* opaque */

typedef struct {
    char     name[FS_NAME_MAX + 1];
    char     type;      /* 'f' or 'd' */
    uint32_t size;       /* bytes for files, entry count for directories */
    uint32_t firstblock;
} fs_stat_t;

/* Lifecycle */
FileSystem *fs_create(void);
void        fs_destroy(FileSystem *fs);

/* Informational */
const char *fs_strerror(fs_result_t err);
const char *fs_pwd(FileSystem *fs);
void        fs_df(FileSystem *fs); /* prints total/free/used blocks */

/* Navigation & directory operations */
fs_result_t fs_cd(FileSystem *fs, const char *path);
fs_result_t fs_mkdir(FileSystem *fs, const char *path);
fs_result_t fs_ls(FileSystem *fs, const char *path);   /* prints listing   */
fs_result_t fs_tree(FileSystem *fs, const char *path);  /* prints subtree   */
fs_result_t fs_stat(FileSystem *fs, const char *path, fs_stat_t *out);
fs_result_t fs_rm(FileSystem *fs, const char *path);    /* file or empty dir */

/* File data operations (host <-> virtual disk and vd <-> vd) */
fs_result_t fs_write(FileSystem *fs, const char *path, const void *data, size_t len);
fs_result_t fs_read(FileSystem *fs, const char *path, void **out_buf, size_t *out_len);
fs_result_t fs_cat(FileSystem *fs, const char *path);   /* prints file content */
fs_result_t fs_import(FileSystem *fs, const char *host_path, const char *vd_path);
fs_result_t fs_export(FileSystem *fs, const char *vd_path, const char *host_path);
fs_result_t fs_cp(FileSystem *fs, const char *src_vd_path, const char *dst_vd_path);

/* Consistency checker. Prints a report. If repair != 0, fixes any
 * leaked (allocated-but-unreachable) blocks it finds. Returns the number
 * of problems found (0 == clean). */
int fs_fsck(FileSystem *fs, int repair);

#endif /* MINIOS_FILESYSTEM_H */
