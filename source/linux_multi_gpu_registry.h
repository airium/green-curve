// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// One service, one socket, independent physical GPU runtimes. Included by
// linux_daemon.cpp after its transaction, fan and restoration helpers.

#include <sys/file.h>

static pl_thread_ret multi_gpu_fan_thread(void* argument) {
    LinuxGpuRuntimeScope selected(static_cast<LinuxDaemonGpuRuntime*>(argument));
    return fan_reassert_thread(nullptr);
}

static bool daemon_acquire_runtime_owner() {
    if (mkdir(GC_DAEMON_STATE_DIR, 0700) != 0 && errno != EEXIST) return false;
    int directory = open(GC_DAEMON_STATE_DIR,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat status = {};
    if (directory < 0) return false;
    const bool protectedDirectory = fstat(directory, &status) == 0 &&
        S_ISDIR(status.st_mode) && status.st_uid == 0 &&
        (status.st_mode & 0022) == 0;
    int owner = protectedDirectory ? openat(directory, "daemon.lock",
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    close(directory);
    if (owner < 0) return false;
    const bool protectedFile = fstat(owner, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == 0 && status.st_nlink == 1 &&
        (status.st_mode & 0077) == 0;
    if (!protectedFile || flock(owner, LOCK_EX | LOCK_NB) != 0) {
        close(owner);
        return false;
    }
    g_daemonOwnerFd = owner;
    return true;
}

static void daemon_initialize_discovery_runtime() {
    gc_strlcpy(g_discoveryRuntime.stateFile, sizeof(g_discoveryRuntime.stateFile),
        GC_DAEMON_STATE_DIR "/active.bin");
    gc_strlcpy(g_discoveryRuntime.operationFile, sizeof(g_discoveryRuntime.operationFile),
        GC_DAEMON_STATE_DIR "/operation.bin");
    gc_strlcpy(g_discoveryRuntime.startupFile, sizeof(g_discoveryRuntime.startupFile),
        GC_DAEMON_STATE_DIR "/startup.bin");
    gc_strlcpy(g_discoveryRuntime.guardFile, sizeof(g_discoveryRuntime.guardFile),
        GC_DAEMON_STATE_DIR "/restore-guard.bin");
    gc_strlcpy(g_discoveryRuntime.fanMarkerFile, sizeof(g_discoveryRuntime.fanMarkerFile),
        GC_DAEMON_STATE_DIR "/fan-owned.bin");
    g_discoveryRuntime.activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
    g_discoveryRuntime.gpuGeneration = 1;
    pthread_mutex_init(&g_discoveryRuntime.fanWakeMutex, nullptr);
}

static LinuxDaemonGpuRuntime* daemon_runtime_for_request(
    const ServiceRequest* req, char* err, size_t errSize) {
    if (req->command == SERVICE_CMD_RESUME_RESTORE) return &g_discoveryRuntime;
    if (req->targetGpu.valid) {
        if (linux_multi_gpu_find_runtime(g_gpuRuntimes, g_gpuRuntimeCount,
                &req->targetGpu) < 0 && !daemon_discover_gpu_runtimes(true)) {
            gc_strlcpy(err, errSize, "GPU discovery or ownership recovery failed");
            return nullptr;
        }
        int index = linux_multi_gpu_find_runtime(g_gpuRuntimes,
            g_gpuRuntimeCount, &req->targetGpu);
        if (index >= 0) return &g_gpuRuntimes[index];
        gc_strlcpy(err, errSize,
            "target GPU is not available in this daemon; refresh the GPU list");
        return nullptr;
    }
    if (req->command == SERVICE_CMD_GET_OPERATION_RESULT) {
        LinuxDaemonGpuRuntime* found = nullptr;
        for (unsigned int i = 0; i < g_gpuRuntimeCount; ++i) {
            if (!service_operation_find(&g_gpuRuntimes[i].operationTracker,
                                         req->operationId)) continue;
            if (found) {
                gc_strlcpy(err, errSize, "operation query needs an exact GPU");
                return nullptr;
            }
            found = &g_gpuRuntimes[i];
        }
        if (found) return found;
    }
    if (g_gpuRuntimeCount == 1) return &g_gpuRuntimes[0];
    if (req->command == SERVICE_CMD_PING ||
        req->command == SERVICE_CMD_RESUME_RESTORE ||
        req->command == SERVICE_CMD_GET_SNAPSHOT ||
        req->command == SERVICE_CMD_GET_TELEMETRY ||
        req->command == SERVICE_CMD_GET_OPERATION_RESULT)
        return &g_discoveryRuntime;
    gc_strlcpy(err, errSize,
        "multiple GPUs detected; select an exact PCI BDF with --gpu");
    return nullptr;
}

static void daemon_handle_resume_all(ServiceResponse* resp) {
    unsigned int restored = 0, failed = 0;
    for (unsigned int i = 0; i < g_gpuRuntimeCount; ++i) {
        LinuxGpuRuntimeScope selected(&g_gpuRuntimes[i]);
        pl_mutex_lock(&g_lock);
        ServiceResponse answer = {};
        const bool hadIntent = g_hasActiveDesired;
        daemon_handle_resume_restore(&answer);
        if (hadIntent) {
            if (answer.status == SERVICE_STATUS_OK) ++restored;
            else ++failed;
        }
        pl_mutex_unlock(&g_lock);
    }
    resp->status = failed ? SERVICE_STATUS_ERROR : SERVICE_STATUS_OK;
    gc_snprintf(resp->message, sizeof(resp->message),
        "resume restore: %u GPU(s) restored, %u failed", restored, failed);
}

static void daemon_shutdown_gpu_runtimes(bool cleanStop = false) {
    for (unsigned int i = 0; i < g_gpuRuntimeCount; ++i) {
        LinuxGpuRuntimeScope selected(&g_gpuRuntimes[i]);
        wake_fan_runtime();
    }
    for (unsigned int i = 0; i < g_gpuRuntimeCount; ++i) {
        LinuxGpuRuntimeScope selected(&g_gpuRuntimes[i]);
        if (g_runtime->fanThreadStarted) {
            pl_thread_join(g_runtime->fanThread);
            g_runtime->fanThreadStarted = false;
        }
        if (cleanStop) clear_auto_restore_attempts_on_clean_stop();
        daemon_release_fan_to_driver();
        if (g_gpu.nvmlLib || g_gpu.nvapiLib) linux_backend_shutdown(&g_gpu);
    }
    if (g_discoveryRuntime.gpu.nvmlLib || g_discoveryRuntime.gpu.nvapiLib)
        linux_backend_shutdown(&g_discoveryRuntime.gpu);
    if (g_daemonOwnerFd >= 0) {
        close(g_daemonOwnerFd);
        g_daemonOwnerFd = -1;
    }
}

#include "linux_multi_gpu_migration.h"

static bool daemon_register_gpu_runtime(const GpuAdapterInfo* target,
    const LinuxGpuUuid* uuid) {
    for (unsigned int i = 0; i < g_gpuRuntimeCount; ++i) {
        if (!linux_gpu_bdf_equal(target, &g_gpuRuntimes[i].binding)) continue;
        if (!linux_gpu_identity_matches(target, &g_gpuRuntimes[i].binding) ||
            !linux_gpu_uuid_compatible(&g_gpuRuntimes[i].bindingUuid, uuid)) {
            dlog("daemon GPU runtime: physical replacement requires a service restart\n");
            return false;
        }
        return true;
    }
    if (linux_gpu_uuid_valid(uuid)) {
        for (unsigned int i = 0; i < g_gpuRuntimeCount; ++i) {
            if (linux_gpu_uuid_valid(&g_gpuRuntimes[i].bindingUuid) &&
                linux_gpu_uuid_compatible(&g_gpuRuntimes[i].bindingUuid, uuid)) {
                dlog("daemon GPU runtime: a moved physical GPU requires a service restart\n");
                return false;
            }
        }
    }
    if (g_gpuRuntimeCount >= MAX_GPU_ADAPTERS) return false;
    const unsigned int index = g_gpuRuntimeCount;
    LinuxDaemonGpuRuntime* entry = &g_gpuRuntimes[index];
    if (!linux_multi_gpu_set_paths(entry, GC_DAEMON_STATE_DIR, target, uuid))
        return false;
    entry->activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
    entry->gpuGeneration = ((gc_u64)(index + 1) << 32) | 1;
    pl_mutex_init(&entry->lock);
    pthread_mutex_init(&entry->fanWakeMutex, nullptr);
    LinuxGpuRuntimeScope selected(entry);
    if (!init_fan_wake_condition()) {
        pthread_mutex_destroy(&entry->fanWakeMutex);
        pl_mutex_destroy(&entry->lock);
        *entry = {};
        return false;
    }
    char err[256] = {};
    entry->gpuReady = linux_backend_init(&entry->gpu, &entry->binding,
        err, sizeof(err), GPU_LOCATION_ROLE_REQUIRED, &entry->bindingUuid);
    ++g_gpuRuntimeCount;
    dlog("daemon GPU runtime: index=%u backend=%s uuid=%s%s%s\n", index,
        entry->gpuReady ? "online" : "degraded",
        linux_gpu_uuid_valid(&entry->bindingUuid) ? "bound" : "unavailable",
        err[0] ? " reason=" : "", err);
    return true;
}

static bool daemon_start_late_gpu_worker(LinuxDaemonGpuRuntime* entry) {
    LinuxGpuRuntimeScope selected(entry);
    // Discovery may migrate persistent state, but never replays stored clocks.
    LinuxDaemonStateRecord saved = {};
    bool widened = false;
    char err[256] = {};
    const auto state = linux_daemon_state_load(entry->stateFile, &saved,
        err, sizeof(err), &widened);
    entry->stateUncertain = state != LINUX_DAEMON_STATE_MISSING;
    load_auto_restore_guard_at_boot();
    daemon_fan_ownership_handback_at_start(true);
    load_startup_policy_at_boot();
    if (!pl_thread_start(&entry->fanThread, multi_gpu_fan_thread, entry))
        return false;
    entry->fanThreadStarted = true;
    return true;
}

static void daemon_discard_new_runtimes(unsigned int first) {
    for (unsigned int i = first; i < g_gpuRuntimeCount; ++i) {
        auto& entry = g_gpuRuntimes[i];
        linux_backend_shutdown(&entry.gpu);
        pl_mutex_destroy(&entry.lock);
        pthread_mutex_destroy(&entry.fanWakeMutex);
        pthread_cond_destroy(&entry.fanWakeCondition);
        entry = {};
    }
    g_gpuRuntimeCount = first;
}

static bool daemon_discover_gpu_runtimes(bool explicitRefresh) {
    LinuxGpuRuntimeScope discoveryScope(&g_discoveryRuntime);
    if (!g_gpuReady || explicitRefresh) {
        LinuxGpuState replacement = {};
        char err[256] = {};
        if (!linux_backend_init(&replacement, nullptr, err, sizeof(err))) {
            linux_backend_shutdown(&replacement);
            dlog("daemon discovery: GPU inventory unavailable: %s\n", err);
            return false;
        }
        linux_backend_shutdown(&g_gpu);
        g_gpu = replacement;
        g_gpuReady = true;
    }
    const unsigned int count = g_gpu.adapterCount;
    if (count > MAX_GPU_ADAPTERS) return false;
    const unsigned int firstNew = g_gpuRuntimeCount;
    for (unsigned int i = 0; i < count; ++i) {
        if (!daemon_register_gpu_runtime(&g_gpu.adapters[i], &g_gpu.adapterUuids[i])) {
            daemon_discard_new_runtimes(firstNew);
            return false;
        }
    }
    if (g_gpuRuntimeCount == firstNew) return true;
    // Inherited ownership must reach every new context before any fan worker
    // starts. A failed migration leaves its sources and no routable new entry.
    if (!daemon_migrate_single_gpu_records()) {
        daemon_discard_new_runtimes(firstNew);
        return false;
    }
    for (unsigned int i = firstNew; i < g_gpuRuntimeCount; ++i) {
        if (!daemon_start_late_gpu_worker(&g_gpuRuntimes[i])) {
            dlog("daemon discovery: fan worker unavailable; stopping partial service\n");
            on_daemon_stop_signal(0);
            return false;
        }
    }
    return true;
}

static bool daemon_initialize_gpu_runtimes() {
    const LinuxGpuState& discovery = g_discoveryRuntime.gpu;
    unsigned int count = discovery.adapterCount;
    if (count > MAX_GPU_ADAPTERS) return false;
    for (unsigned int i = 0; i < count; ++i)
        if (!daemon_register_gpu_runtime(&discovery.adapters[i],
                &discovery.adapterUuids[i])) return false;
    if (!daemon_migrate_single_gpu_records()) return false;
    for (unsigned int i = 0; i < g_gpuRuntimeCount; ++i) {
        LinuxGpuRuntimeScope selected(&g_gpuRuntimes[i]);
        daemon_load_gpu_at_boot();
    }
    return true;
}
