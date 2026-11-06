// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_INSTANCE_OWNERSHIP_H
#define GREEN_CURVE_LINUX_INSTANCE_OWNERSHIP_H

#include "linux_instance.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static inline bool linux_instance_prepare_directory_for_owner(const char* path,
    uid_t owner, bool socketDirectory) {
    bool created = mkdir(path, 0755) == 0;
    if (!created && errno != EEXIST) return false;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st = {};
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == owner &&
        (st.st_mode & 0022) == 0;
    // mkdir obeys the daemon's restrictive umask. Normalize through the
    // verified descriptor, never by following a pathname after inspection.
    if (ok && created) ok = fchmod(fd, 0755) == 0 && fstat(fd, &st) == 0;
    if (ok && socketDirectory) ok = (st.st_mode & 0777) == 0755;
    if (fd >= 0) close(fd);
    if (!ok) errno = EPERM;
    return ok;
}

static inline bool linux_instance_prepare_daemon(char* err, size_t errSize) {
    if (geteuid() != 0) {
        snprintf(err, errSize, "--daemon requires root");
        return false;
    }
    const char* dirs[] = {"/run/greencurve", linux_instance().paths.socketDir,
        "/var/lib/greencurve", linux_instance().paths.stateDir};
    for (unsigned int i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        if (!linux_instance_prepare_directory_for_owner(dirs[i], 0, i < 2)) {
            snprintf(err, errSize, "cannot prepare protected daemon directories: %s", strerror(errno));
            return false;
        }
    }
    return true;
}

// Separate from per-instance state: singleton and instance daemons contend on
// the same physical-card lock, regardless of their socket or config paths.
static inline int linux_instance_owner_named_at(int directory, const char* name, uid_t owner) {
    if (directory < 0 || !name || !*name || strlen(name) > 64 ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) { errno = EINVAL; return -1; }
    for (const char* c = name; *c; ++c) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == ':' || *c == '_')) {
            errno = EINVAL; return -1;
        }
    }
    int fd = openat(directory, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st = {};
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != owner || (st.st_mode & 0077) != 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno ? errno : EPERM;
        close(fd); errno = saved; return -1;
    }
    return fd;
}

static inline int linux_instance_owner_open_at(int directory, const LinuxGpuUuid* uuid, uid_t owner) {
    if (!linux_gpu_uuid_valid(uuid)) { errno = EINVAL; return -1; }
    char name[56] = {};
    snprintf(name, sizeof(name), "%s.lock", uuid->text);
    return linux_instance_owner_named_at(directory, name, owner);
}

static inline bool linux_instance_acquire_owner(const LinuxGpuUuid* uuid,
                                               char* err, size_t errSize,
                                               const GpuAdapterInfo* target = nullptr) {
    LinuxInstanceContext& instance = linux_instance();
    if (!instance.daemon) return true;
    if (!linux_gpu_uuid_valid(uuid)) {
        if (linux_instance_enabled()) {
            snprintf(err, errSize, "instance GPU physical identity is unavailable");
            return false;
        }
    }
    char pciName[48] = {};
    if (target && linux_gpu_bdf_valid(target))
        snprintf(pciName, sizeof(pciName), "pci-%04x:%02x:%02x.%u.lock",
                 target->pciDomain, target->pciBus, target->pciDevice, target->pciFunction);
    const bool reuseUuid = linux_gpu_uuid_valid(uuid) && instance.ownerFd >= 0 &&
        strcmp(instance.ownerUuid.text, uuid->text) == 0;
    const bool reusePci = pciName[0] && instance.ownerPciFd >= 0 &&
        strcmp(instance.ownerPciName, pciName) == 0;
    if ((reuseUuid || !linux_gpu_uuid_valid(uuid)) && (reusePci || !pciName[0]))
        return true;
    const char* dirs[] = {"/run/greencurve", "/run/greencurve/owners"};
    int directory = -1;
    for (const char* path : dirs) {
        if (directory >= 0) close(directory);
        if (mkdir(path, 0755) != 0 && errno != EEXIST) { directory = -1; break; }
        directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st = {};
        if (directory < 0 || fstat(directory, &st) != 0 || st.st_uid != 0 ||
            (st.st_mode & 0022) != 0) {
            if (directory >= 0) close(directory);
            directory = -1; break;
        }
    }
    int fd = reuseUuid ? instance.ownerFd :
        linux_gpu_uuid_valid(uuid) ? linux_instance_owner_open_at(directory, uuid, 0) : -1;
    int pciFd = reusePci ? instance.ownerPciFd :
        pciName[0] ? linux_instance_owner_named_at(directory, pciName, 0) : -1;
    if (directory >= 0) close(directory);
    if ((linux_gpu_uuid_valid(uuid) && fd < 0) || (pciName[0] && pciFd < 0)) {
        if (!reuseUuid && fd >= 0) close(fd);
        if (!reusePci && pciFd >= 0) close(pciFd);
        snprintf(err, errSize, "GPU ownership unavailable (another daemon may own this card): %s", strerror(errno));
        return false;
    }
    if (!reuseUuid && instance.ownerFd >= 0) close(instance.ownerFd);
    if (!reusePci && instance.ownerPciFd >= 0) close(instance.ownerPciFd);
    instance.ownerFd = fd;
    instance.ownerUuid = linux_gpu_uuid_valid(uuid) ? *uuid : LinuxGpuUuid{};
    instance.ownerPciFd = pciFd;
    snprintf(instance.ownerPciName, sizeof(instance.ownerPciName), "%s", pciName);
    return true;
}

static inline bool linux_instance_start_daemon() {
    linux_instance().daemon = true;
    char error[256] = {};
    if (!linux_instance_prepare_daemon(error, sizeof(error)) ||
        (linux_instance_enabled() && !linux_instance_acquire_owner(
             &linux_instance().uuid, error, sizeof(error)))) {
        fprintf(stderr, "%s\n", error);
        return false;
    }
    return true;
}

#endif
