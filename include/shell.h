#ifndef MINIOS_SHELL_H
#define MINIOS_SHELL_H

#include "filesystem.h"

/* Runs the interactive "miniOS> " command loop against the given
 * filesystem until the user types "exit"/"quit" or EOF is reached.
 * The shell talks to the filesystem, process manager and scheduler
 * exclusively through their public headers -- it never touches
 * filesystem/process/scheduler internals directly. */
void shell_run(FileSystem *fs);

#endif /* MINIOS_SHELL_H */
