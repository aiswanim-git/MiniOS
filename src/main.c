#include "../include/filesystem.h"
#include "../include/shell.h"

#include <stdio.h>

int main(void)
{
    FileSystem *fs = fs_create();
    if (!fs) {
        fprintf(stderr, "*** Failed to create virtual disk (out of memory?)\n");
        return 1;
    }

    shell_run(fs);

    fs_destroy(fs);
    return 0;
}
