#ifndef INSTALL_PROCESS_H
#define INSTALL_PROCESS_H

#include <sys/types.h>

/* Launch the helper ELF via elfldr and establish the IPC connection.
 * On success, *ipc_fd is set to the connected socket and returns 0.
 * On failure, returns -1. */
int install_process_start(int *ipc_fd);

#endif
