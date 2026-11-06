// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_MULTI_GPU_RUNTIME_H
#define GREEN_CURVE_LINUX_MULTI_GPU_RUNTIME_H

#include "linux_backend.h"
#include "linux_daemon_state.h"
#include "service_operation_tracker.h"
#include <pthread.h>

// One physical GPU owns every piece of mutable runtime and durable intent.
// Entries never move: a fan worker holds its entry for its whole lifetime.
struct LinuxDaemonGpuRuntime {
    LinuxGpuState gpu;
    bool gpuReady;
    pl_mutex lock;
    DesiredSettings activeDesired;
    bool hasActiveDesired;
    GpuAdapterInfo activeTarget;
    LinuxGpuUuid activeTargetUuid;
    GpuLocationRole activeTargetRole;
    bool stateUncertain;
    ServiceOperationTracker operationTracker;
    LinuxDaemonStartupRecord startupPolicy;
    LinuxAutoRestoreGuard autoRestoreGuard;
    ServiceAutoApplyFailure autoApplyFailure;
    unsigned int fanFailureCount;
    pthread_mutex_t fanWakeMutex;
    pthread_cond_t fanWakeCondition;
    unsigned long long fanWakeGeneration;
    bool fanOwnershipMarkerCommitted;
    bool fanOwnershipHandbackPending;
    bool fanOwnershipHandbackDeferred;
    LinuxFanOwnershipMarker fanOwnershipRecord;
    gc_u64 stateRevision;
    gc_u64 gpuGeneration;
    bool backendReadInitAttempted;
    GpuAdapterInfo binding;
    LinuxGpuUuid bindingUuid;
    char stateFile[256];
    char operationFile[256];
    char startupFile[256];
    char guardFile[256];
    char fanMarkerFile[256];
    pl_thread fanThread;
    bool fanThreadStarted;
};

// Exact live request routing. No ordinal/model fallback can move an active
// intent to another identical board. UUID is checked again by the backend.
static inline int linux_multi_gpu_find_runtime(
    const LinuxDaemonGpuRuntime* entries, unsigned int count,
    const GpuAdapterInfo* target) {
    if (!entries || !target || !target->valid || count > MAX_GPU_ADAPTERS)
        return -1;
    int found = -1;
    for (unsigned int i = 0; i < count; ++i) {
        if (!linux_gpu_identity_matches(target, &entries[i].binding)) continue;
        if (found >= 0) return -1;
        found = (int)i;
    }
    return found;
}

static inline bool linux_multi_gpu_set_paths(LinuxDaemonGpuRuntime* runtime,
    const char* directory, const GpuAdapterInfo* target,
    const LinuxGpuUuid* uuid) {
    if (!runtime || !directory || !target || !linux_gpu_bdf_valid(target))
        return false;
    char key[96] = {};
    if (linux_gpu_uuid_valid(uuid))
        gc_strlcpy(key, sizeof(key), uuid->text);
    else
        gc_snprintf(key, sizeof(key), "pci-%04x-%02x-%02x-%u",
            target->pciDomain, target->pciBus, target->pciDevice,
            target->pciFunction);
    runtime->binding = *target;
    runtime->bindingUuid = uuid ? *uuid : LinuxGpuUuid{};
    auto path = [&](char* out, size_t size, const char* leaf) {
        int written = gc_snprintf(out, size, "%s/%s-%s", directory, key, leaf);
        return written > 0 && (size_t)written < size;
    };
    return path(runtime->stateFile, sizeof(runtime->stateFile), "active.bin") &&
        path(runtime->operationFile, sizeof(runtime->operationFile), "operation.bin") &&
        path(runtime->startupFile, sizeof(runtime->startupFile), "startup.bin") &&
        path(runtime->guardFile, sizeof(runtime->guardFile), "restore-guard.bin") &&
        path(runtime->fanMarkerFile, sizeof(runtime->fanMarkerFile), "fan-owned.bin");
}

#endif
