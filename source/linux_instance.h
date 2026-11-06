// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_INSTANCE_H
#define GREEN_CURVE_LINUX_INSTANCE_H

#include "linux_gpu_uuid.h"
#include <stdio.h>
#include <string.h>

// One immutable physical GPU per process. Empty identity preserves the
// original singleton endpoint and on-disk formats.
struct LinuxInstancePaths {
    char socketDir[96];
    char socketPath[108];
    char stateDir[112];
    char active[144], operation[144], startup[144], guard[144], fan[144];
    char unitName[96], unitPath[144], resumeName[104], resumePath[152];
    char directoryName[72];
};

static inline LinuxInstancePaths linux_instance_build_paths(const LinuxGpuUuid* uuid) {
    LinuxInstancePaths p = {};
    const char* id = linux_gpu_uuid_valid(uuid) ? uuid->text : "";
    const char* slash = id[0] ? "/" : "";
    snprintf(p.directoryName, sizeof(p.directoryName), "greencurve%s%s", slash, id);
    snprintf(p.socketDir, sizeof(p.socketDir), "/run/%s", p.directoryName);
    snprintf(p.socketPath, sizeof(p.socketPath), "%s/greencurve.sock", p.socketDir);
    snprintf(p.stateDir, sizeof(p.stateDir), "/var/lib/%s", p.directoryName);
    snprintf(p.active, sizeof(p.active), "%s/active.bin", p.stateDir);
    snprintf(p.operation, sizeof(p.operation), "%s/operation.bin", p.stateDir);
    snprintf(p.startup, sizeof(p.startup), "%s/startup.bin", p.stateDir);
    snprintf(p.guard, sizeof(p.guard), "%s/restore-guard.bin", p.stateDir);
    snprintf(p.fan, sizeof(p.fan), "%s/fan-owned.bin", p.stateDir);
    snprintf(p.unitName, sizeof(p.unitName), "greencurve%s%s.service", id[0] ? "@" : "", id);
    snprintf(p.unitPath, sizeof(p.unitPath), "/etc/systemd/system/%s", p.unitName);
    snprintf(p.resumeName, sizeof(p.resumeName), "greencurve-resume%s%s.service", id[0] ? "@" : "", id);
    snprintf(p.resumePath, sizeof(p.resumePath), "/etc/systemd/system/%s", p.resumeName);
    return p;
}

struct LinuxInstanceContext {
    LinuxGpuUuid uuid;
    LinuxInstancePaths paths;
    bool daemon;
    int ownerFd;
    LinuxGpuUuid ownerUuid;
    int ownerPciFd;
    char ownerPciName[48];
};

inline LinuxInstanceContext& linux_instance() {
    static LinuxInstanceContext value = {{}, linux_instance_build_paths(nullptr), false, -1, {}, -1, {}};
    return value;
}

static inline bool linux_instance_enabled() {
    return linux_gpu_uuid_valid(&linux_instance().uuid);
}

static inline bool linux_instance_configure(const char* text) {
    LinuxGpuUuid uuid = linux_gpu_uuid_parse(text);
    if (!linux_gpu_uuid_valid(&uuid)) return false;
    linux_instance().uuid = uuid;
    linux_instance().paths = linux_instance_build_paths(&uuid);
    return true;
}

// Exact UUID is required in instance mode; unlike optional restore metadata,
// an unreadable physical identity must never fall back to another card.
static inline int linux_instance_gpu_index(const LinuxGpuUuid* wanted,
    const LinuxGpuUuid* observed, unsigned int count) {
    if (!linux_gpu_uuid_valid(wanted) || !observed) return -1;
    int found = -1;
    for (unsigned int i = 0; i < count; ++i) {
        if (!linux_gpu_uuid_valid(&observed[i]) ||
            strcmp(wanted->text, observed[i].text) != 0) continue;
        if (found >= 0) return -2;
        found = (int)i;
    }
    return found;
}

static inline bool linux_instance_target_allowed(const GpuAdapterInfo* requested,
    const GpuAdapterInfo* bound) {
    return !requested || !requested->valid ||
        linux_gpu_bdf_equal(requested, bound);
}

static inline bool linux_instance_restore_allowed(const LinuxGpuUuid* instance,
    const GpuAdapterInfo* requested, const GpuAdapterInfo* bound,
    const LinuxGpuUuid* savedUuid) {
    if (!linux_gpu_uuid_valid(instance)) return true;
    if (linux_gpu_uuid_valid(savedUuid))
        return strcmp(instance->text, savedUuid->text) == 0;
    return linux_instance_target_allowed(requested, bound);
}

static inline bool linux_instance_observed_identity(const LinuxGpuUuid* instance,
    LinuxGetGpuUuid query, nvmlDevice_t device, bool writeResolved) {
    if (!linux_gpu_uuid_valid(instance)) return true;
    if (!writeResolved) return false;
    const LinuxGpuUuid observed = linux_gpu_read_uuid(query, device);
    return linux_gpu_uuid_valid(&observed) && strcmp(observed.text, instance->text) == 0;
}

#endif
