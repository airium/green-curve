// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_daemon.cpp. Mutable aliases always name this thread's
// fixed GPU context; the fan workers never share a selected-context pointer.
#include "linux_multi_gpu_runtime.h"

static LinuxDaemonGpuRuntime g_discoveryRuntime = {};
static LinuxDaemonGpuRuntime g_gpuRuntimes[MAX_GPU_ADAPTERS] = {};
static unsigned int g_gpuRuntimeCount = 0;
static int g_daemonOwnerFd = -1;
static thread_local LinuxDaemonGpuRuntime* g_runtime = &g_discoveryRuntime;

class LinuxGpuRuntimeScope {
    LinuxDaemonGpuRuntime* previous_;
public:
    explicit LinuxGpuRuntimeScope(LinuxDaemonGpuRuntime* next)
        : previous_(g_runtime) { g_runtime = next; }
    ~LinuxGpuRuntimeScope() { g_runtime = previous_; }
    LinuxGpuRuntimeScope(const LinuxGpuRuntimeScope&) = delete;
    LinuxGpuRuntimeScope& operator=(const LinuxGpuRuntimeScope&) = delete;
};

#define g_gpu (g_runtime->gpu)
#define g_gpuReady (g_runtime->gpuReady)
#define g_lock (g_runtime->lock)
#define g_activeDesired (g_runtime->activeDesired)
#define g_hasActiveDesired (g_runtime->hasActiveDesired)
#define g_activeTarget (g_runtime->activeTarget)
#define g_activeTargetUuid (g_runtime->activeTargetUuid)
#define g_activeTargetRole (g_runtime->activeTargetRole)
#define g_stateUncertain (g_runtime->stateUncertain)
#define g_operationTracker (g_runtime->operationTracker)
#define g_startupPolicy (g_runtime->startupPolicy)
#define g_autoRestoreGuard (g_runtime->autoRestoreGuard)
#define g_autoApplyFailure (g_runtime->autoApplyFailure)
#define g_fanFailureCount (g_runtime->fanFailureCount)
#define g_fanWakeMutex (g_runtime->fanWakeMutex)
#define g_fanWakeCondition (g_runtime->fanWakeCondition)
#define g_fanWakeGeneration (g_runtime->fanWakeGeneration)
#define g_fanOwnershipMarkerCommitted (g_runtime->fanOwnershipMarkerCommitted)
#define g_fanOwnershipHandbackPending (g_runtime->fanOwnershipHandbackPending)
#define g_fanOwnershipHandbackDeferred (g_runtime->fanOwnershipHandbackDeferred)
#define g_fanOwnershipRecord (g_runtime->fanOwnershipRecord)
#define g_daemonStateRevision (g_runtime->stateRevision)
#define g_daemonGpuGeneration (g_runtime->gpuGeneration)
#define g_backendReadInitAttempted (g_runtime->backendReadInitAttempted)

#undef GC_DAEMON_STATE_FILE
#undef GC_DAEMON_OPERATION_FILE
#undef GC_DAEMON_STARTUP_FILE
#undef GC_DAEMON_GUARD_FILE
#undef GC_DAEMON_FAN_MARKER_FILE
#define GC_DAEMON_STATE_FILE (g_runtime->stateFile)
#define GC_DAEMON_OPERATION_FILE (g_runtime->operationFile)
#define GC_DAEMON_STARTUP_FILE (g_runtime->startupFile)
#define GC_DAEMON_GUARD_FILE (g_runtime->guardFile)
#define GC_DAEMON_FAN_MARKER_FILE (g_runtime->fanMarkerFile)

static LinuxDaemonGpuRuntime* daemon_runtime_for_request(
    const ServiceRequest*, char*, size_t);
static void daemon_handle_resume_all(ServiceResponse*);

static void daemon_release_fan_to_driver();

static bool daemon_discover_gpu_runtimes(bool explicitRefresh);

// All restoration and fan-handback paths share this immutable owner check.
// A record's target is data, never authority to repurpose another context.
static bool daemon_backend_select_target(LinuxGpuState* gpu,
    const GpuAdapterInfo* target, char* err, size_t errSize,
    GpuLocationRole role = GPU_LOCATION_ROLE_REQUIRED,
    const LinuxGpuUuid* uuid = nullptr,
    LinuxGpuUuidVerdict* verdict = nullptr) {
    if (g_runtime->binding.valid) {
        const auto selected = linux_gpu_restore_selection(target, role,
            gpu->adapters, gpu->adapterCount, uuid, gpu->adapterUuids, nullptr);
        const bool uuidBound = linux_gpu_uuid_valid(&g_runtime->bindingUuid);
        const bool matches = selected.writeResolved && selected.index >= 0 &&
            (uuidBound
                ? linux_gpu_uuid_valid(&gpu->adapterUuids[selected.index]) &&
                  linux_gpu_uuid_compatible(&g_runtime->bindingUuid,
                      &gpu->adapterUuids[selected.index])
                : linux_gpu_identity_matches(&g_runtime->binding,
                      &gpu->adapters[selected.index]));
        if (!matches) {
            gc_strlcpy(err, errSize, "stored target does not belong to this GPU runtime");
            if (verdict) *verdict = LINUX_GPU_UUID_MISMATCH;
            return false;
        }
        uuid = &g_runtime->bindingUuid;
    }
    return linux_backend_select_target(gpu, target, err, errSize, role, uuid, verdict);
}
