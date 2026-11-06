// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_INSTANCE_REMOVE_H
#define GREEN_CURVE_LINUX_INSTANCE_REMOVE_H
#include "linux_instance.h"
#include <errno.h>

using LinuxInstanceRemoveCommand = int (*)(char* const*);
using LinuxInstanceRemoveFile = int (*)(const char*);

// Named instances retain their recovery files until both possible writers are
// stopped. Injected operations let native fixtures exercise the actual order
// and failures without changing the host's services or installed unit files.
static inline int linux_instance_remove_units(const LinuxInstancePaths& paths,
    LinuxInstanceRemoveCommand command, LinuxInstanceRemoveFile removeFile,
    char* error, size_t errorSize) {
    if (!command || !removeFile) {
        snprintf(error, errorSize, "instance removal operations are unavailable");
        return 1;
    }
    const char* units[] = {paths.resumeName, paths.unitName};
    for (const char* unit : units) {
        char* args[] = {(char*)"systemctl", (char*)"disable", (char*)"--now",
                        const_cast<char*>(unit), nullptr};
        int status = command(args);
        if (status != 0) {
            snprintf(error, errorSize,
                "cannot stop and disable %s (exit %d); unit files and saved settings kept",
                unit, status);
            return 1;
        }
    }
    const char* files[] = {paths.resumePath, paths.unitPath};
    for (const char* file : files) {
        if (removeFile(file) != 0 && errno != ENOENT) {
            snprintf(error, errorSize, "cannot remove stopped instance unit %s: %s; saved settings kept",
                     file, strerror(errno));
            return 1;
        }
    }
    char* reload[] = {(char*)"systemctl", (char*)"daemon-reload", nullptr};
    int status = command(reload);
    if (status != 0) {
        snprintf(error, errorSize, "instance stopped but daemon-reload failed (exit %d); saved settings kept", status);
        return 1;
    }
    char* reset[] = {(char*)"systemctl", (char*)"reset-failed",
                    const_cast<char*>(paths.unitName), const_cast<char*>(paths.resumeName), nullptr};
    (void)command(reset); // Only clears bookkeeping; no running writer remains.
    return 0;
}
#endif
