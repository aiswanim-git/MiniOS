#include "../include/filesystem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- On-disk layout constants (see filesystem.h for the diagram) ---- */

#define NBLOCKS       (FS_DISK_SIZE_BYTES / FS_BLOCK_SIZE) /* 65536 */
#define BITMAP_START  1u
#define BITMAP_BLOCKS 8u                      /* 8*1024*8 = 65536 bits    */
#define FAT_START     (BITMAP_START + BITMAP_BLOCKS)   /* block 9        */
#define FAT_BLOCKS    256u                    /* 256*256 entries = 65536 */
#define FAT_ENTRIES_PER_BLOCK (FS_BLOCK_SIZE / 4u)
#define ROOT_BLOCK    (FAT_START + FAT_BLOCKS) /* block 265 */
#define FIRST_DATA_BLOCK (ROOT_BLOCK + 1u)     /* block 266 */
#define DENT_SIZE     32u
#define DENTS_PER_BLOCK (FS_BLOCK_SIZE / DENT_SIZE) /* 32 */
#define FS_MAGIC      0x4D494E4Fu /* "MINO" */

/* On-disk directory-entry record, packed to exactly 32 bytes. */
typedef struct {
    char     type;                 /* 'd' or 'f' */
    char     name[FS_NAME_MAX + 1]; /* 23 bytes, NUL-terminated */
    uint32_t size;
    uint32_t firstblock;
} __attribute__((packed)) dirent_t;

_Static_assert(sizeof(dirent_t) == DENT_SIZE, "dirent_t must be 32 bytes");

/* Copies at most FS_NAME_MAX characters of src into a (FS_NAME_MAX+1)-byte
 * dst buffer and always NUL-terminates it. Wraps strncpy to avoid
 * -Wstringop-truncation noise from the (intentional) truncating copy. */
static void name_copy(char *dst, const char *src)
{
    size_t i = 0;
    for (; i < FS_NAME_MAX && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

struct filesystem {
    unsigned char *disk;      /* FS_DISK_SIZE_BYTES bytes */
    uint32_t       nblocks;
    uint32_t       nfree;
    uint32_t       root_block;
    uint32_t       cwd_block; /* current-directory block number */
    char           cwd_path[4096];
};

/* ---------------------------------------------------------------------
 * Low-level block/bitmap/FAT helpers
 * ------------------------------------------------------------------- */

static unsigned char *blk(FileSystem *fs, uint32_t bno)
{
    return fs->disk + (size_t)bno * FS_BLOCK_SIZE;
}

static int bitmap_test(FileSystem *fs, uint32_t bno)
{
    unsigned char byte = fs->disk[BITMAP_START * FS_BLOCK_SIZE + bno / 8];
    return (byte >> (bno % 8)) & 1;
}

static void bitmap_set(FileSystem *fs, uint32_t bno, int used)
{
    size_t off = (size_t)BITMAP_START * FS_BLOCK_SIZE + bno / 8;
    unsigned char byte = fs->disk[off];
    if (used) byte |= (unsigned char)(1u << (bno % 8));
    else      byte &= (unsigned char)~(1u << (bno % 8));
    fs->disk[off] = byte;
}

static uint32_t fat_get(FileSystem *fs, uint32_t bno)
{
    uint32_t next;
    memcpy(&next, fs->disk + (size_t)FAT_START * FS_BLOCK_SIZE + 4u * bno, 4);
    return next;
}

static void fat_set(FileSystem *fs, uint32_t bno, uint32_t next)
{
    memcpy(fs->disk + (size_t)FAT_START * FS_BLOCK_SIZE + 4u * bno, &next, 4);
}

/* Allocates one free data block (>= FIRST_DATA_BLOCK), marks it used in
 * the bitmap, and returns its block number, or 0 if the disk is full. */
static uint32_t block_alloc(FileSystem *fs)
{
    for (uint32_t b = FIRST_DATA_BLOCK; b < fs->nblocks; ++b) {
        if (!bitmap_test(fs, b)) {
            bitmap_set(fs, b, 1);
            fat_set(fs, b, 0);
            fs->nfree--;
            return b;
        }
    }
    return 0;
}

static void block_free(FileSystem *fs, uint32_t b)
{
    if (b < FIRST_DATA_BLOCK || b >= fs->nblocks) return;
    bitmap_set(fs, b, 0);
    fat_set(fs, b, 0);
    fs->nfree++;
}

/* Frees an entire FAT chain starting at block b (b itself included). */
static void chain_free(FileSystem *fs, uint32_t b)
{
    while (b != 0) {
        uint32_t next = fat_get(fs, b);
        block_free(fs, b);
        b = next;
    }
}

/* Follows a directory/file's FAT chain to the block containing logical
 * entry/byte-group `index_of_32B_unit`, extending the chain with newly
 * allocated blocks if `extend` is non-zero and the chain isn't long
 * enough yet. Returns the block number, or 0 on failure. */
static uint32_t chain_walk(FileSystem *fs, uint32_t first, uint32_t unit_index, int extend)
{
    uint32_t block = first;
    uint32_t units_per_block = FS_BLOCK_SIZE / 32u; /* only used for dirents;
                                                         data blocks use 1024B
                                                         units handled by caller */
    (void)units_per_block;
    uint32_t idx = unit_index;
    while (idx >= 1) {
        uint32_t next = fat_get(fs, block);
        if (next == 0) {
            if (!extend) return 0;
            next = block_alloc(fs);
            if (next == 0) return 0;
            fat_set(fs, block, next);
        }
        block = next;
        idx--;
    }
    return block;
}

/* ---------------------------------------------------------------------
 * Directory entry helpers (32-byte records, 32 per block, chained)
 * ------------------------------------------------------------------- */

static void dent_read(FileSystem *fs, uint32_t first_block, uint32_t entry_no, dirent_t *out)
{
    uint32_t block_idx = entry_no / DENTS_PER_BLOCK;
    uint32_t offset     = entry_no % DENTS_PER_BLOCK;
    uint32_t b = chain_walk(fs, first_block, block_idx, 0);
    if (b == 0) { memset(out, 0, sizeof *out); return; }
    memcpy(out, blk(fs, b) + offset * DENT_SIZE, DENT_SIZE);
}

static void dent_write(FileSystem *fs, uint32_t first_block, uint32_t entry_no, const dirent_t *in)
{
    uint32_t block_idx = entry_no / DENTS_PER_BLOCK;
    uint32_t offset     = entry_no % DENTS_PER_BLOCK;
    uint32_t b = chain_walk(fs, first_block, block_idx, 1);
    if (b == 0) return; /* out of space; caller should have checked */
    memcpy(blk(fs, b) + offset * DENT_SIZE, in, DENT_SIZE);
}

/* Directory entry 0 of every directory is itself: it stores the
 * directory's own metadata, notably `size` = number of entries in use.
 * This is a common convention for FAT-style directory encoding. */
static uint32_t dir_entry_count(FileSystem *fs, uint32_t dir_block)
{
    dirent_t self;
    dent_read(fs, dir_block, 0, &self);
    return self.size;
}

static void dir_set_entry_count(FileSystem *fs, uint32_t dir_block, uint32_t n)
{
    dirent_t self;
    dent_read(fs, dir_block, 0, &self);
    self.size = n;
    dent_write(fs, dir_block, 0, &self);
}

/* Looks up `name` as a direct child of directory `dir_block`.
 * Returns 1 and fills the output params if found, else 0. Entries 0 and 1
 * are always "." and ".." and are matched specially. */
static int dir_lookup(FileSystem *fs, uint32_t dir_block, const char *name,
                       dirent_t *out, uint32_t *entry_no)
{
    if (!strcmp(name, ".")) {
        dent_read(fs, dir_block, 0, out);
        strcpy(out->name, ".");
        out->type = 'd';
        out->firstblock = dir_block;
        if (entry_no) *entry_no = 0;
        return 1;
    }
    if (!strcmp(name, "..")) {
        dent_read(fs, dir_block, 1, out);
        if (entry_no) *entry_no = 1;
        return 1;
    }
    uint32_t n = dir_entry_count(fs, dir_block);
    for (uint32_t i = 2; i < n; ++i) {
        dirent_t d;
        dent_read(fs, dir_block, i, &d);
        if (!strcmp(d.name, name)) {
            *out = d;
            if (entry_no) *entry_no = i;
            return 1;
        }
    }
    return 0;
}

/* Appends a new entry to directory `dir_block`. Returns 0 on success. */
static int dir_add(FileSystem *fs, uint32_t dir_block, char type, const char *name,
                    uint32_t size, uint32_t firstblock)
{
    uint32_t n = dir_entry_count(fs, dir_block);
    dirent_t d;
    memset(&d, 0, sizeof d);
    d.type = type;
    name_copy(d.name, name);
    d.size = size;
    d.firstblock = firstblock;
    dent_write(fs, dir_block, n, &d);
    dir_set_entry_count(fs, dir_block, n + 1);
    return 0;
}

/* Removes entry `entry_no` from directory `dir_block` by swapping the
 * last entry into its place (order of "." and ".." is preserved; only
 * entries >= 2 may be removed). */
static void dir_remove(FileSystem *fs, uint32_t dir_block, uint32_t entry_no)
{
    uint32_t n = dir_entry_count(fs, dir_block);
    if (n == 0 || entry_no >= n) return;
    if (entry_no != n - 1) {
        dirent_t last;
        dent_read(fs, dir_block, n - 1, &last);
        dent_write(fs, dir_block, entry_no, &last);
    }
    dir_set_entry_count(fs, dir_block, n - 1);
}

static int dir_update(FileSystem *fs, uint32_t dir_block, const char *name,
                       uint32_t new_size, uint32_t new_firstblock)
{
    uint32_t n = dir_entry_count(fs, dir_block);
    for (uint32_t i = 2; i < n; ++i) {
        dirent_t d;
        dent_read(fs, dir_block, i, &d);
        if (!strcmp(d.name, name)) {
            d.size = new_size;
            d.firstblock = new_firstblock;
            dent_write(fs, dir_block, i, &d);
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Path resolution
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t parent_block; /* 0 if unresolved */
    uint32_t self_block;   /* 0 if the final component doesn't exist */
    char     self_type;    /* 'd', 'f', or 'X' (doesn't exist) */
    uint32_t self_size;
    char     last_name[FS_NAME_MAX + 1];
} resolved_t;

/* Resolves `path` (absolute if it starts with '/', else relative to
 * fs->cwd_block). Splits off the final component into last_name so
 * callers can create/remove/rename it. Intermediate components that
 * don't exist or aren't directories cause resolution to stop with
 * parent_block = 0. */
static resolved_t path_resolve(FileSystem *fs, const char *path)
{
    resolved_t r;
    memset(&r, 0, sizeof r);

    if (path == NULL || path[0] == '\0' || !strcmp(path, "/")) {
        r.self_block = (path && path[0] == '/') ? fs->root_block : fs->cwd_block;
        r.self_type = 'd';
        r.parent_block = r.self_block; /* root's parent is itself here */
        dirent_t self;
        dent_read(fs, r.self_block, 0, &self);
        r.self_size = self.size;
        r.last_name[0] = '\0';
        return r;
    }

    char buf[4096];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    uint32_t block;
    char *p;
    if (buf[0] == '/') { block = fs->root_block; p = buf + 1; }
    else                { block = fs->cwd_block;  p = buf; }

    while (1) {
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
            if (*p == '\0') { p = slash + 1; continue; } /* collapse "//" */
            dirent_t d;
            if (!dir_lookup(fs, block, p, &d, NULL) || d.type != 'd') {
                r.parent_block = 0;
                r.self_type = 'X';
                return r;
            }
            block = d.firstblock;
            p = slash + 1;
        } else {
            r.parent_block = block;
            dirent_t d;
            uint32_t entry_no;
            if (dir_lookup(fs, block, p, &d, &entry_no)) {
                r.self_block = d.firstblock;
                r.self_type = d.type;
                r.self_size = d.size;
            } else {
                r.self_block = 0;
                r.self_type = 'X';
            }
            name_copy(r.last_name, p);
            return r;
        }
    }
}

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

static void format(FileSystem *fs)
{
    memset(fs->disk, 0, FS_DISK_SIZE_BYTES);
    fs->nblocks = NBLOCKS;
    fs->nfree = NBLOCKS - FIRST_DATA_BLOCK;
    fs->root_block = ROOT_BLOCK;

    uint32_t magic = FS_MAGIC;
    memcpy(fs->disk, &magic, 4);
    memcpy(fs->disk + 4, &fs->nblocks, 4);
    memcpy(fs->disk + 8, &fs->nfree, 4);
    memcpy(fs->disk + 12, &fs->root_block, 4);

    /* Reserve blocks [0 .. FIRST_DATA_BLOCK-1] in the bitmap so the
     * allocator never hands them out. */
    for (uint32_t b = 0; b < FIRST_DATA_BLOCK; ++b) bitmap_set(fs, b, 1);

    /* Root directory: "." and ".." both point at itself. */
    dirent_t self, parent;
    memset(&self, 0, sizeof self);
    self.type = 'd'; strcpy(self.name, "."); self.size = 2; self.firstblock = ROOT_BLOCK;
    memset(&parent, 0, sizeof parent);
    parent.type = 'd'; strcpy(parent.name, ".."); parent.size = 0; parent.firstblock = ROOT_BLOCK;
    memcpy(blk(fs, ROOT_BLOCK), &self, DENT_SIZE);
    memcpy(blk(fs, ROOT_BLOCK) + DENT_SIZE, &parent, DENT_SIZE);
}

FileSystem *fs_create(void)
{
    FileSystem *fs = calloc(1, sizeof *fs);
    if (!fs) return NULL;
    fs->disk = malloc(FS_DISK_SIZE_BYTES);
    if (!fs->disk) { free(fs); return NULL; }
    format(fs);
    fs->cwd_block = fs->root_block;
    fs->cwd_path[0] = '\0';
    return fs;
}

void fs_destroy(FileSystem *fs)
{
    if (!fs) return;
    free(fs->disk);
    free(fs);
}

const char *fs_strerror(fs_result_t err)
{
    switch (err) {
        case FS_OK:            return "success";
        case FS_ERR_NOT_FOUND:  return "no such file or directory";
        case FS_ERR_EXISTS:     return "file or directory already exists";
        case FS_ERR_NOT_DIR:    return "not a directory";
        case FS_ERR_NOT_FILE:   return "not a file";
        case FS_ERR_NO_SPACE:   return "no space left on virtual disk";
        case FS_ERR_INVALID:    return "invalid argument";
        case FS_ERR_IO:         return "I/O error";
        case FS_ERR_NOT_EMPTY:  return "directory not empty";
    }
    return "unknown error";
}

const char *fs_pwd(FileSystem *fs)
{
    return (fs->cwd_path[0] == '\0') ? "/" : fs->cwd_path;
}

void fs_df(FileSystem *fs)
{
    printf("Total blocks: %u  Free blocks: %u  Used blocks: %u  Block size: %u bytes\n",
           fs->nblocks, fs->nfree, fs->nblocks - fs->nfree, FS_BLOCK_SIZE);
}

/* ---------------------------------------------------------------------
 * Navigation
 * ------------------------------------------------------------------- */

/* Recomputes fs->cwd_path from scratch by walking ".." up to the root,
 * collecting names, then reversing them. */
static void recompute_cwd_path(FileSystem *fs)
{
    char names[128][FS_NAME_MAX + 1];
    int depth = 0;
    uint32_t block = fs->cwd_block;

    while (block != fs->root_block && depth < 128) {
        dirent_t parent;
        dent_read(fs, block, 1, &parent); /* ".." */
        uint32_t parent_block = parent.firstblock;
        /* find our own name inside parent_block */
        uint32_t n = dir_entry_count(fs, parent_block);
        const char *found = NULL;
        static char namebuf[FS_NAME_MAX + 1];
        for (uint32_t i = 2; i < n; ++i) {
            dirent_t d;
            dent_read(fs, parent_block, i, &d);
            if (d.type == 'd' && d.firstblock == block) {
                strcpy(namebuf, d.name);
                found = namebuf;
                break;
            }
        }
        if (!found) break; /* shouldn't happen on a consistent fs */
        strcpy(names[depth++], found);
        block = parent_block;
    }

    char *p = fs->cwd_path;
    *p = '\0';
    for (int i = depth - 1; i >= 0; --i) {
        *p++ = '/';
        strcpy(p, names[i]);
        p += strlen(names[i]);
    }
    *p = '\0';
}

fs_result_t fs_cd(FileSystem *fs, const char *path)
{
    if (path == NULL || path[0] == '\0') {
        fs->cwd_block = fs->root_block;
        fs->cwd_path[0] = '\0';
        return FS_OK;
    }
    resolved_t r = path_resolve(fs, path);
    if (r.self_type == 'X') return FS_ERR_NOT_FOUND;
    if (r.self_type != 'd') return FS_ERR_NOT_DIR;
    fs->cwd_block = r.self_block;
    recompute_cwd_path(fs);
    return FS_OK;
}

fs_result_t fs_mkdir(FileSystem *fs, const char *path)
{
    if (path == NULL || path[0] == '\0') return FS_ERR_INVALID;
    resolved_t r = path_resolve(fs, path);
    if (r.parent_block == 0) return FS_ERR_NOT_FOUND;
    if (r.self_type != 'X') return FS_ERR_EXISTS;
    if (r.last_name[0] == '\0') return FS_ERR_INVALID;

    uint32_t nb = block_alloc(fs);
    if (nb == 0) return FS_ERR_NO_SPACE;

    dirent_t self, parent;
    memset(&self, 0, sizeof self);
    self.type = 'd'; strcpy(self.name, "."); self.size = 2; self.firstblock = nb;
    memset(&parent, 0, sizeof parent);
    parent.type = 'd'; strcpy(parent.name, ".."); parent.size = 0; parent.firstblock = r.parent_block;
    memcpy(blk(fs, nb), &self, DENT_SIZE);
    memcpy(blk(fs, nb) + DENT_SIZE, &parent, DENT_SIZE);

    dir_add(fs, r.parent_block, 'd', r.last_name, 2, nb);
    return FS_OK;
}

fs_result_t fs_ls(FileSystem *fs, const char *path)
{
    resolved_t r = path_resolve(fs, (path && path[0]) ? path : ".");
    if (r.self_type == 'X') return FS_ERR_NOT_FOUND;
    if (r.self_type != 'd') {
        printf("%-24s %8u\n", r.last_name, r.self_size);
        return FS_OK;
    }
    uint32_t n = dir_entry_count(fs, r.self_block);
    printf("Total %u entries\n", n);
    printf("%-24s %-6s %10s %12s\n", "NAME", "TYPE", "SIZE", "FIRSTBLOCK");
    for (uint32_t i = 0; i < n; ++i) {
        dirent_t d;
        dent_read(fs, r.self_block, i, &d);
        printf("%-24s %-6c %10u %12u\n", d.name, d.type, d.size, d.firstblock);
    }
    return FS_OK;
}

static void tree_recurse(FileSystem *fs, uint32_t block, int depth)
{
    uint32_t n = dir_entry_count(fs, block);
    for (uint32_t i = 2; i < n; ++i) {
        dirent_t d;
        dent_read(fs, block, i, &d);
        for (int k = 0; k < depth; ++k) printf("  ");
        printf("%s%s\n", d.name, d.type == 'd' ? "/" : "");
        if (d.type == 'd') tree_recurse(fs, d.firstblock, depth + 1);
    }
}

fs_result_t fs_tree(FileSystem *fs, const char *path)
{
    resolved_t r = path_resolve(fs, (path && path[0]) ? path : ".");
    if (r.self_type == 'X') return FS_ERR_NOT_FOUND;
    if (r.self_type != 'd') return FS_ERR_NOT_DIR;
    printf(".\n");
    tree_recurse(fs, r.self_block, 1);
    return FS_OK;
}

fs_result_t fs_stat(FileSystem *fs, const char *path, fs_stat_t *out)
{
    if (!out) return FS_ERR_INVALID;
    resolved_t r = path_resolve(fs, path);
    if (r.self_type == 'X') return FS_ERR_NOT_FOUND;
    memset(out, 0, sizeof *out);
    name_copy(out->name, r.last_name[0] ? r.last_name : "/");
    out->type = r.self_type;
    out->size = r.self_size;
    out->firstblock = r.self_block;
    return FS_OK;
}

fs_result_t fs_rm(FileSystem *fs, const char *path)
{
    resolved_t r = path_resolve(fs, path);
    if (r.self_type == 'X') return FS_ERR_NOT_FOUND;
    if (r.parent_block == 0 || r.last_name[0] == '\0') return FS_ERR_INVALID;

    if (r.self_type == 'd') {
        if (dir_entry_count(fs, r.self_block) != 2) return FS_ERR_NOT_EMPTY;
        chain_free(fs, r.self_block);
    } else {
        chain_free(fs, r.self_block);
    }

    dirent_t d; uint32_t entry_no;
    dir_lookup(fs, r.parent_block, r.last_name, &d, &entry_no);
    dir_remove(fs, r.parent_block, entry_no);
    return FS_OK;
}

/* ---------------------------------------------------------------------
 * File data operations
 * ------------------------------------------------------------------- */

/* Resolves dst to (parent_block, name) for a write, whether dst names an
 * existing file, a directory (write with source's basename), or a new
 * path. `base_name_hint` is used when dst resolves to a directory. */
static fs_result_t resolve_write_target(FileSystem *fs, const char *dst,
                                         const char *base_name_hint,
                                         uint32_t *out_parent, char *out_name)
{
    resolved_t r = path_resolve(fs, dst);
    if (r.self_type == 'd') {
        *out_parent = r.self_block;
        name_copy(out_name, base_name_hint);
        return FS_OK;
    }
    if (r.parent_block == 0) return FS_ERR_NOT_FOUND;
    *out_parent = r.parent_block;
    name_copy(out_name, r.last_name);
    return FS_OK;
}

fs_result_t fs_write(FileSystem *fs, const char *path, const void *data, size_t len)
{
    char name[FS_NAME_MAX + 1];
    uint32_t parent;
    /* basename fallback used only if path itself is a directory, which
     * doesn't make sense for a raw fs_write -- require a concrete path. */
    resolved_t probe = path_resolve(fs, path);
    if (probe.parent_block == 0) return FS_ERR_NOT_FOUND;
    if (probe.self_type == 'd') return FS_ERR_EXISTS; /* can't overwrite a dir */
    parent = probe.parent_block;
    name_copy(name, probe.last_name);
    if (name[0] == '\0') return FS_ERR_INVALID;

    dirent_t existing; uint32_t entry_no = 0;
    int had = dir_lookup(fs, parent, name, &existing, &entry_no);
    if (had) {
        if (existing.type != 'f') return FS_ERR_NOT_FILE;
        chain_free(fs, existing.firstblock);
    }

    uint32_t first = 0, prev = 0;
    size_t remaining = len;
    const unsigned char *src = data;
    while (remaining > 0) {
        uint32_t b = block_alloc(fs);
        if (b == 0) { if (first) chain_free(fs, first); return FS_ERR_NO_SPACE; }
        if (first == 0) first = b; else fat_set(fs, prev, b);
        size_t chunk = remaining < FS_BLOCK_SIZE ? remaining : FS_BLOCK_SIZE;
        memcpy(blk(fs, b), src, chunk);
        if (chunk < FS_BLOCK_SIZE) memset(blk(fs, b) + chunk, 0, FS_BLOCK_SIZE - chunk);
        src += chunk;
        remaining -= chunk;
        prev = b;
    }

    if (had) dir_update(fs, parent, name, (uint32_t)len, first);
    else     dir_add(fs, parent, 'f', name, (uint32_t)len, first);
    return FS_OK;
}

fs_result_t fs_read(FileSystem *fs, const char *path, void **out_buf, size_t *out_len)
{
    resolved_t r = path_resolve(fs, path);
    if (r.self_type == 'X') return FS_ERR_NOT_FOUND;
    if (r.self_type != 'f') return FS_ERR_NOT_FILE;

    unsigned char *buf = malloc(r.self_size ? r.self_size : 1);
    if (!buf) return FS_ERR_IO;
    size_t copied = 0;
    uint32_t b = r.self_block;
    while (copied < r.self_size && b != 0) {
        size_t chunk = (r.self_size - copied) < FS_BLOCK_SIZE ? (r.self_size - copied) : FS_BLOCK_SIZE;
        memcpy(buf + copied, blk(fs, b), chunk);
        copied += chunk;
        b = fat_get(fs, b);
    }
    *out_buf = buf;
    *out_len = r.self_size;
    return FS_OK;
}

fs_result_t fs_cat(FileSystem *fs, const char *path)
{
    void *buf; size_t len;
    fs_result_t rc = fs_read(fs, path, &buf, &len);
    if (rc != FS_OK) return rc;
    fwrite(buf, 1, len, stdout);
    if (len > 0 && ((char *)buf)[len - 1] != '\n') printf("\n");
    free(buf);
    return FS_OK;
}

fs_result_t fs_import(FileSystem *fs, const char *host_path, const char *vd_path)
{
    FILE *fp = fopen(host_path, "rb");
    if (!fp) return FS_ERR_IO;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return FS_ERR_IO; }
    fseek(fp, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (!data) { fclose(fp); return FS_ERR_IO; }
    if (sz > 0 && fread(data, 1, (size_t)sz, fp) != (size_t)sz) {
        free(data); fclose(fp); return FS_ERR_IO;
    }
    fclose(fp);

    const char *base = strrchr(host_path, '/');
    base = base ? base + 1 : host_path;

    char name[FS_NAME_MAX + 1];
    uint32_t parent;
    fs_result_t rc = resolve_write_target(fs, vd_path, base, &parent, name);
    if (rc != FS_OK) { free(data); return rc; }
    (void)parent; /* fs_write below re-resolves the target path itself */

    resolved_t r = path_resolve(fs, vd_path);
    char target[4096];
    if (r.self_type == 'd') {
        if (!strcmp(vd_path, "/")) snprintf(target, sizeof target, "/%s", name);
        else snprintf(target, sizeof target, "%s/%s", vd_path, name);
    } else {
        strncpy(target, vd_path, sizeof target - 1);
        target[sizeof target - 1] = '\0';
    }
    rc = fs_write(fs, target, data, (size_t)sz);
    free(data);
    return rc;
}

fs_result_t fs_export(FileSystem *fs, const char *vd_path, const char *host_path)
{
    void *buf; size_t len;
    fs_result_t rc = fs_read(fs, vd_path, &buf, &len);
    if (rc != FS_OK) return rc;
    FILE *fp = fopen(host_path, "wb");
    if (!fp) { free(buf); return FS_ERR_IO; }
    if (len > 0) fwrite(buf, 1, len, fp);
    fclose(fp);
    free(buf);
    return FS_OK;
}

fs_result_t fs_cp(FileSystem *fs, const char *src_vd_path, const char *dst_vd_path)
{
    void *buf; size_t len;
    fs_result_t rc = fs_read(fs, src_vd_path, &buf, &len);
    if (rc != FS_OK) return rc;

    resolved_t sr = path_resolve(fs, src_vd_path);
    char target[4096];
    resolved_t dr = path_resolve(fs, dst_vd_path);
    if (dr.self_type == 'd') {
        if (!strcmp(dst_vd_path, "/")) snprintf(target, sizeof target, "/%s", sr.last_name);
        else snprintf(target, sizeof target, "%s/%s", dst_vd_path, sr.last_name);
    } else {
        strncpy(target, dst_vd_path, sizeof target - 1);
        target[sizeof target - 1] = '\0';
    }
    rc = fs_write(fs, target, buf, len);
    free(buf);
    return rc;
}

/* ---------------------------------------------------------------------
 * fsck: filesystem consistency checker
 * ------------------------------------------------------------------- */

/* Marks every block reachable from the directory tree (directory chain
 * blocks themselves, plus file data chain blocks) as visited. */
static void fsck_mark(FileSystem *fs, unsigned char *visited, uint32_t dir_block, int *problems)
{
    /* mark the directory's own chain */
    for (uint32_t b = dir_block; b != 0; b = fat_get(fs, b)) {
        if (b >= fs->nblocks) { (*problems)++; break; }
        visited[b] = 1;
    }

    uint32_t n = dir_entry_count(fs, dir_block);
    for (uint32_t i = 2; i < n; ++i) {
        dirent_t d;
        dent_read(fs, dir_block, i, &d);
        if (d.type == 'd') {
            if (d.firstblock < FIRST_DATA_BLOCK || d.firstblock >= fs->nblocks) {
                printf("  [!] directory entry \"%s\" has invalid firstblock %u\n", d.name, d.firstblock);
                (*problems)++;
                continue;
            }
            fsck_mark(fs, visited, d.firstblock, problems);
        } else if (d.type == 'f') {
            for (uint32_t b = d.firstblock; b != 0; b = fat_get(fs, b)) {
                if (b >= fs->nblocks) {
                    printf("  [!] file \"%s\" has a corrupt block chain\n", d.name);
                    (*problems)++;
                    break;
                }
                visited[b] = 1;
            }
        } else {
            printf("  [!] entry \"%s\" has unknown type '%c'\n", d.name, d.type);
            (*problems)++;
        }
    }
}

int fs_fsck(FileSystem *fs, int repair)
{
    printf("Running fsck on virtual disk (%u blocks)...\n", fs->nblocks);
    int problems = 0;
    unsigned char *visited = calloc(fs->nblocks, 1);
    if (!visited) { printf("  [!] fsck: out of memory\n"); return -1; }

    for (uint32_t b = 0; b < FIRST_DATA_BLOCK; ++b) visited[b] = 1; /* reserved area */
    fsck_mark(fs, visited, fs->root_block, &problems);

    uint32_t leaked = 0, corrupt_free = 0;
    for (uint32_t b = FIRST_DATA_BLOCK; b < fs->nblocks; ++b) {
        int alloc = bitmap_test(fs, b);
        if (alloc && !visited[b]) {
            leaked++;
            if (repair) { block_free(fs, b); }
        } else if (!alloc && visited[b]) {
            corrupt_free++; /* reachable block that bitmap thinks is free */
        }
    }

    uint32_t counted_free = 0;
    for (uint32_t b = FIRST_DATA_BLOCK; b < fs->nblocks; ++b) if (!bitmap_test(fs, b)) counted_free++;
    if (counted_free != fs->nfree) {
        printf("  [!] superblock free-block count (%u) does not match bitmap (%u)\n",
               fs->nfree, counted_free);
        problems++;
        if (repair) { fs->nfree = counted_free; memcpy(fs->disk + 8, &fs->nfree, 4); }
    }

    if (leaked) {
        printf("  [!] %u block(s) allocated but unreachable from the root directory%s\n",
               leaked, repair ? " (freed)" : "");
        problems += (int)leaked;
    }
    if (corrupt_free) {
        printf("  [!] %u block(s) reachable from directory tree but marked free in bitmap\n",
               corrupt_free);
        problems += (int)corrupt_free;
    }

    free(visited);
    if (problems == 0) printf("fsck: filesystem is clean.\n");
    else printf("fsck: %d problem(s) found%s.\n", problems, repair ? " (repairable ones fixed)" : "");
    return problems;
}
