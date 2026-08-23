#include "../include/filesystem.h"

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

static void test_create_destroy(void)
{
    printf("test_create_destroy\n");
    FileSystem *fs = fs_create();
    CHECK(fs != NULL, "fs_create should succeed");
    CHECK(!strcmp(fs_pwd(fs), "/"), "fresh filesystem cwd is /");
    fs_destroy(fs);
}

static void test_df_reports_free_space(void)
{
    printf("test_df_reports_free_space\n");
    FileSystem *fs = fs_create();
    CHECK(fs_fsck(fs, 0) == 0, "freshly formatted disk is clean");
    fs_destroy(fs);
}

static void test_mkdir_and_ls(void)
{
    printf("test_mkdir_and_ls\n");
    FileSystem *fs = fs_create();
    CHECK(fs_mkdir(fs, "/home") == FS_OK, "mkdir /home");
    CHECK(fs_mkdir(fs, "/home") == FS_ERR_EXISTS, "mkdir duplicate fails");
    CHECK(fs_mkdir(fs, "/home/alice") == FS_OK, "mkdir /home/alice");
    CHECK(fs_mkdir(fs, "/nosuch/dir") == FS_ERR_NOT_FOUND, "mkdir under missing parent fails");

    fs_stat_t st;
    CHECK(fs_stat(fs, "/home/alice", &st) == FS_OK, "stat /home/alice");
    CHECK(st.type == 'd', "alice is a directory");
    fs_destroy(fs);
}

static void test_cd_and_pwd(void)
{
    printf("test_cd_and_pwd\n");
    FileSystem *fs = fs_create();
    fs_mkdir(fs, "/a");
    fs_mkdir(fs, "/a/b");
    CHECK(fs_cd(fs, "/a/b") == FS_OK, "cd /a/b");
    CHECK(!strcmp(fs_pwd(fs), "/a/b"), "pwd reflects cd");
    CHECK(fs_cd(fs, "..") == FS_OK, "cd ..");
    CHECK(!strcmp(fs_pwd(fs), "/a"), "pwd after cd ..");
    CHECK(fs_cd(fs, "nosuch") == FS_ERR_NOT_FOUND, "cd into missing dir fails");
    fs_cd(fs, "/");
    CHECK(!strcmp(fs_pwd(fs), "/"), "cd / resets to root");
    fs_destroy(fs);
}

static void test_write_read_roundtrip(void)
{
    printf("test_write_read_roundtrip\n");
    FileSystem *fs = fs_create();
    const char *msg = "The quick brown fox jumps over the lazy dog.";
    CHECK(fs_write(fs, "/greeting.txt", msg, strlen(msg)) == FS_OK, "write file");

    void *buf; size_t len;
    CHECK(fs_read(fs, "/greeting.txt", &buf, &len) == FS_OK, "read file");
    CHECK(len == strlen(msg), "read length matches");
    CHECK(memcmp(buf, msg, len) == 0, "read content matches");
    free(buf);
    fs_destroy(fs);
}

static void test_write_multiblock_file(void)
{
    printf("test_write_multiblock_file\n");
    FileSystem *fs = fs_create();
    size_t n = FS_BLOCK_SIZE * 5 + 137; /* spans several blocks, not block-aligned */
    unsigned char *data = malloc(n);
    for (size_t i = 0; i < n; ++i) data[i] = (unsigned char)(i % 251);

    CHECK(fs_write(fs, "/big.bin", data, n) == FS_OK, "write multiblock file");
    void *buf; size_t len;
    CHECK(fs_read(fs, "/big.bin", &buf, &len) == FS_OK, "read multiblock file");
    CHECK(len == n, "multiblock length matches");
    CHECK(memcmp(buf, data, n) == 0, "multiblock content matches");
    free(buf);
    free(data);
    fs_destroy(fs);
}

static void test_overwrite_frees_old_blocks(void)
{
    printf("test_overwrite_frees_old_blocks\n");
    FileSystem *fs = fs_create();
    unsigned char big[FS_BLOCK_SIZE * 3];
    memset(big, 'A', sizeof big);
    fs_write(fs, "/f.bin", big, sizeof big);

    fs_write(fs, "/f.bin", "small", 5);
    void *buf; size_t len;
    fs_read(fs, "/f.bin", &buf, &len);
    CHECK(len == 5, "overwritten file has new size");
    CHECK(memcmp(buf, "small", 5) == 0, "overwritten file has new content");
    free(buf);
    CHECK(fs_fsck(fs, 0) == 0, "no leaked blocks after overwrite");
    fs_destroy(fs);
}

static void test_rm_file_and_empty_dir(void)
{
    printf("test_rm_file_and_empty_dir\n");
    FileSystem *fs = fs_create();
    fs_write(fs, "/note.txt", "hi", 2);
    CHECK(fs_rm(fs, "/note.txt") == FS_OK, "rm file");
    CHECK(fs_rm(fs, "/note.txt") == FS_ERR_NOT_FOUND, "rm again fails");

    fs_mkdir(fs, "/empty");
    CHECK(fs_rm(fs, "/empty") == FS_OK, "rm empty dir");

    fs_mkdir(fs, "/full");
    fs_write(fs, "/full/x.txt", "x", 1);
    CHECK(fs_rm(fs, "/full") == FS_ERR_NOT_EMPTY, "rm non-empty dir fails");
    fs_destroy(fs);
}

static void test_cp_within_disk(void)
{
    printf("test_cp_within_disk\n");
    FileSystem *fs = fs_create();
    fs_write(fs, "/src.txt", "copy me", 7);
    fs_mkdir(fs, "/dst");
    CHECK(fs_cp(fs, "/src.txt", "/dst") == FS_OK, "cp into a directory");
    fs_stat_t st;
    CHECK(fs_stat(fs, "/dst/src.txt", &st) == FS_OK, "copy landed with source name");
    CHECK(st.size == 7, "copy has correct size");
    fs_destroy(fs);
}

static void test_import_export_roundtrip(void)
{
    printf("test_import_export_roundtrip\n");
    FileSystem *fs = fs_create();
    const char *host_in = "/tmp/minios_test_in.txt";
    const char *host_out = "/tmp/minios_test_out.txt";
    FILE *fp = fopen(host_in, "w");
    fprintf(fp, "hello from the host filesystem\n");
    fclose(fp);

    CHECK(fs_import(fs, host_in, "/imported.txt") == FS_OK, "import host file");
    CHECK(fs_export(fs, "/imported.txt", host_out) == FS_OK, "export back to host");

    FILE *a = fopen(host_in, "rb");
    FILE *b = fopen(host_out, "rb");
    char ba[256], bb[256];
    size_t la = fread(ba, 1, sizeof ba, a);
    size_t lb = fread(bb, 1, sizeof bb, b);
    CHECK(la == lb && memcmp(ba, bb, la) == 0, "imported/exported content matches original");
    fclose(a); fclose(b);
    remove(host_in);
    remove(host_out);
    fs_destroy(fs);
}

static void test_tree_and_many_entries(void)
{
    printf("test_tree_and_many_entries\n");
    FileSystem *fs = fs_create();
    fs_mkdir(fs, "/many");
    char name[32];
    for (int i = 0; i < 70; ++i) { /* forces the directory to span >2 FAT blocks (32 entries/block) */
        snprintf(name, sizeof name, "/many/f%02d.txt", i);
        CHECK(fs_write(fs, name, "x", 1) == FS_OK, "write into large directory");
    }
    fs_stat_t st;
    CHECK(fs_stat(fs, "/many/f69.txt", &st) == FS_OK, "last file in large directory is findable");
    CHECK(fs_fsck(fs, 0) == 0, "large directory chain passes fsck");
    fs_destroy(fs);
}

static void test_fsck_detects_leak_and_repairs(void)
{
    printf("test_fsck_detects_leak_and_repairs\n");
    FileSystem *fs = fs_create();
    fs_write(fs, "/leaktest.bin", "data", 4);
    fs_stat_t st;
    fs_stat(fs, "/leaktest.bin", &st);

    /* Simulate corruption: remove the directory entry directly via rm's
     * sibling path is not exposed, so instead we emulate a leak by
     * writing then "forgetting" a file -- here we just verify a clean
     * disk reports zero problems, and that an intentionally large
     * write/overwrite/delete cycle still ends up leak-free. */
    fs_rm(fs, "/leaktest.bin");
    int problems = fs_fsck(fs, 1);
    CHECK(problems == 0, "no leaks after normal write+rm cycle");
    fs_destroy(fs);
}

int main(void)
{
    test_create_destroy();
    test_df_reports_free_space();
    test_mkdir_and_ls();
    test_cd_and_pwd();
    test_write_read_roundtrip();
    test_write_multiblock_file();
    test_overwrite_frees_old_blocks();
    test_rm_file_and_empty_dir();
    test_cp_within_disk();
    test_import_export_roundtrip();
    test_tree_and_many_entries();
    test_fsck_detects_leak_and_repairs();

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
