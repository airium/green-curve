// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production request router, journals, rollback orchestration and
// resume dispatcher against fake driver/persistence dependencies. No GPU I/O.
#include <cerrno>
#include <sys/stat.h>
static bool failDestinationProbe;
static int fixture_migration_lstat(const char* path, struct stat* status) {
    if (failDestinationProbe) { errno = EACCES; return -1; }
    return ::lstat(path, status);
}
#define lstat fixture_migration_lstat
#define APP_VERSION "fixture"
#include "vf_backends.h"
#define GC_DAEMON_RUNTIME_FIXTURE
#include "linux_daemon.cpp"
#undef linux_backend_select_target
#undef lstat
#include <map>
#include <string>

static std::map<std::string, LinuxDaemonStateRecord> states;
static std::map<std::string, LinuxDaemonOperationRecord> operations;
static std::map<std::string, LinuxDaemonStartupRecord> startups;
static std::map<std::string, LinuxAutoRestoreGuard> guards;
static std::map<std::string, LinuxFanOwnershipMarker> fans;
static int writes[2], resets[2], duties[2], autoWrites[2];
static bool inventoryAvailable;
static LinuxGpuState inventoryTemplate;
static bool failApply, failCommit, corruptLegacyFan, bootIdUnavailable;
static std::string failFanStorePath;

bool linux_debug_log_is_enabled() { return false; }
void linux_debug_logf(const char*, ...) {}
static void daemon_release_fan_to_driver() {}

bool linux_backend_init(LinuxGpuState* gpu, const GpuAdapterInfo* target, char* err,
    size_t size, GpuLocationRole role, const LinuxGpuUuid* uuid) {
    if (!inventoryAvailable) {
        gc_strlcpy(err, size, "fake inventory unavailable");
        return false;
    }
    *gpu = inventoryTemplate;
    if (target && target->valid) {
        const auto selected = linux_gpu_restore_selection(target, role,
            gpu->adapters, gpu->adapterCount, uuid, gpu->adapterUuids, nullptr);
        if (!selected.writeResolved) return false;
        gpu->selectedAdapterIndex = (unsigned int)selected.index;
        gpu->selectedGpu = gpu->adapters[selected.index];
        gpu->writeIdentityResolved = true;
    } else {
        gpu->writeIdentityResolved = false;
    }
    return true;
}
void linux_backend_shutdown(LinuxGpuState* gpu) { *gpu = {}; }
bool linux_backend_select_target(LinuxGpuState* gpu, const GpuAdapterInfo* target,
    char*, size_t, GpuLocationRole role, const LinuxGpuUuid* uuid,
    LinuxGpuUuidVerdict*) {
    const auto selected = linux_gpu_restore_selection(target, role,
        gpu->adapters, gpu->adapterCount, uuid, gpu->adapterUuids, nullptr);
    return selected.writeResolved && selected.index >= 0 &&
        (unsigned int)selected.index == gpu->selectedAdapterIndex;
}
LinuxBackendRefreshResult linux_backend_refresh(LinuxGpuState* gpu) {
    return {true, true, gpu->health};
}
bool linux_backend_capture_snapshot(LinuxGpuState* gpu,
    LinuxHardwareSnapshot* out, char*, size_t) {
    *out = {};
    out->valid = out->gpuOffsetValid = out->memOffsetValid = true;
    out->curveValid = out->powerValid = true;
    out->availableMutationDomains = SERVICE_MUTATION_DOMAIN_ALL;
    out->gpuOffsetMHz = gpu->freqOffsets[0] / 1000;
    out->powerLimitmW = (unsigned int)gpu->powerLimitCurrentmW;
    memcpy(out->curveOffsets, gpu->freqOffsets, sizeof(out->curveOffsets));
    return true;
}
bool linux_backend_restore_snapshot(LinuxGpuState* gpu,
    const LinuxHardwareSnapshot* before, unsigned int, char*, size_t) {
    memcpy(gpu->freqOffsets, before->curveOffsets, sizeof(gpu->freqOffsets));
    return true;
}
LinuxMutationResult linux_backend_apply(LinuxGpuState* gpu,
    const DesiredSettings* desired, const DesiredSettings*,
    const DesiredSettings*, char*, size_t) {
    const unsigned int index = gpu->selectedAdapterIndex;
    ++writes[index];
    if (failApply) return {false, true, true, true, 1, 0, 1};
    if (desired->hasGpuOffset) gpu->freqOffsets[0] = desired->gpuOffsetMHz * 1000;
    return {true, true, false, false, 1, 1, 0};
}
LinuxMutationResult linux_backend_reset(LinuxGpuState* gpu, char*, size_t) {
    ++resets[gpu->selectedAdapterIndex];
    memset(gpu->freqOffsets, 0, sizeof(gpu->freqOffsets));
    return {true, true, false, false, 1, 1, 0};
}
bool linux_backend_set_curve_fan_percent(LinuxGpuState* gpu, unsigned int value) {
    duties[gpu->selectedAdapterIndex] = (int)value;
    return true;
}
bool linux_backend_set_fan_auto(LinuxGpuState* gpu) {
    ++autoWrites[gpu->selectedAdapterIndex]; return true;
}
bool linux_backend_fans_are_auto(LinuxGpuState*) { return true; }
FanFixedMaintenanceDecision linux_backend_fixed_fan_check(LinuxGpuState*, int) {
    return {true, false, "fake driver lost fixed duty"};
}

LinuxDaemonStateLoadResult linux_daemon_state_load(const char* path,
    LinuxDaemonStateRecord* out, char*, size_t, bool* migrated) {
    if (migrated) *migrated = false;
    auto found = states.find(path);
    if (found == states.end()) return LINUX_DAEMON_STATE_MISSING;
    *out = found->second;
    return LINUX_DAEMON_STATE_LOADED;
}
bool linux_daemon_state_store(const char* path,
    const LinuxDaemonStateRecord* record, char*, size_t) {
    if (failCommit && record->state == LINUX_DAEMON_RECORD_ACTIVE) return false;
    states[path] = *record;
    return true;
}
bool linux_daemon_state_remove(const char* path, char*, size_t) {
    states.erase(path); operations.erase(path); startups.erase(path);
    guards.erase(path); fans.erase(path);
    return true;
}
bool linux_daemon_operation_store(const char* path,
    const LinuxDaemonOperationRecord* record, char*, size_t) {
    operations[path] = *record; return true;
}
bool linux_daemon_operation_load(const char* path,
    LinuxDaemonOperationRecord* out, char*, size_t) {
    auto found = operations.find(path);
    if (found == operations.end()) return false;
    *out = found->second; return true;
}
bool linux_daemon_startup_store(const char* path,
    const LinuxDaemonStartupRecord* record, char*, size_t) {
    startups[path] = *record; return true;
}
bool linux_daemon_startup_load(const char* path,
    LinuxDaemonStartupRecord* out, bool* corrupt, char*, size_t, bool* migrated) {
    if (corrupt) *corrupt = false;
    if (migrated) *migrated = false;
    auto found = startups.find(path);
    if (found == startups.end()) return false;
    *out = found->second; return true;
}
bool linux_daemon_guard_store(const char* path,
    const LinuxAutoRestoreGuard* value, char*, size_t) {
    guards[path] = *value; return true;
}
bool linux_daemon_guard_load(const char* path,
    LinuxAutoRestoreGuard* out, bool* corrupt, char*, size_t) {
    if (corrupt) *corrupt = false;
    auto found = guards.find(path);
    if (found == guards.end()) return false;
    *out = found->second; return true;
}
bool linux_read_boot_id(char* out, size_t size) {
    if (bootIdUnavailable) return false;
    gc_strlcpy(out, size, "00000000-0000-0000-0000-000000000001"); return true;
}
bool linux_fan_ownership_marker_store(const char* path,
    const LinuxFanOwnershipMarker* marker, char*, size_t) {
    if (failFanStorePath == path) return false;
    fans[path] = *marker; return true;
}
int linux_fan_ownership_marker_load(const char* path,
    LinuxFanOwnershipMarker* out, char*, size_t) {
    if (corruptLegacyFan && std::string(path) == GC_DAEMON_STATE_DIR "/fan-owned.bin")
        return -1;
    auto found = fans.find(path);
    if (found == fans.end()) return 0;
    *out = found->second; return 1;
}

static ServiceRequest request(unsigned int command, int index = -1) {
    ServiceRequest result = {};
    result.magic = SERVICE_PROTOCOL_MAGIC;
    result.version = SERVICE_PROTOCOL_VERSION;
    result.command = command;
    result.callerPid = 1;
    if (index >= 0) result.targetGpu = g_gpuRuntimes[index].binding;
    return result;
}

static void initialize() {
    g_running = 1;
    daemon_initialize_discovery_runtime();
    pl_mutex_init(&g_discoveryRuntime.lock);
    LinuxGpuRuntimeScope discovery(&g_discoveryRuntime);
    init_fan_wake_condition();
    g_gpuRuntimeCount = 2;
    for (unsigned int i = 0; i < 2; ++i) {
        auto& entry = g_gpuRuntimes[i];
        GpuAdapterInfo target = {};
        target.valid = target.pciInfoValid = true;
        target.deviceId = 0x1e04;
        target.pciBus = i + 1;
        auto uuid = linux_gpu_uuid_parse(i == 0
            ? "GPU-00000000-0000-0000-0000-000000000001"
            : "GPU-00000000-0000-0000-0000-000000000002");
        linux_multi_gpu_set_paths(&entry, "/fixture", &target, &uuid);
        entry.gpuGeneration = ((gc_u64)(i + 1) << 32) | 1;
        entry.activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
        entry.gpuReady = true;
        entry.gpu.nvmlReady = true;
        entry.gpu.health.availableMutationDomains = SERVICE_MUTATION_DOMAIN_ALL;
        entry.gpu.adapterCount = 2;
        entry.gpu.selectedAdapterIndex = i;
        entry.gpu.selectedGpu = target;
        entry.gpu.writeIdentityResolved = true;
        entry.gpu.gpuHandle = &entry;
        entry.gpu.backend = &g_vfBackendTuring;
        entry.gpu.family = GPU_FAMILY_TURING;
        entry.gpu.vfSnapshotFresh = true;
        entry.gpu.numPopulated = 2;
        entry.gpu.curve[0] = {1500000, 800000};
        entry.gpu.curve[1] = {1600000, 850000};
        entry.gpu.powerLimitDefaultmW = 250000;
        entry.gpu.powerLimitCurrentmW = 250000;
        pl_mutex_init(&entry.lock);
        pthread_mutex_init(&entry.fanWakeMutex, nullptr);
        LinuxGpuRuntimeScope selected(&entry);
        init_fan_wake_condition();
        load_auto_restore_guard_at_boot();
    }
    for (auto& entry : g_gpuRuntimes) {
        for (unsigned int i = 0; i < 2; ++i) {
            entry.gpu.adapters[i] = g_gpuRuntimes[i].binding;
            entry.gpu.adapterUuids[i] = g_gpuRuntimes[i].bindingUuid;
        }
    }
}

static ServiceResponse apply(int index, int offset, gc_u64 operation) {
    auto read = request(SERVICE_CMD_GET_SNAPSHOT, index);
    ServiceResponse before = {};
    handle_request(&read, &before);
    auto write = request(SERVICE_CMD_APPLY, index);
    write.operationId = operation;
    write.applyOrigin = SERVICE_APPLY_ORIGIN_CLI;
    write.expectedServiceInstanceId = before.state.serviceInstanceId;
    write.expectedGpuGeneration = before.state.gpuGeneration;
    write.expectedTopologySignature = before.state.topologySignature;
    write.desired.hasGpuOffset = true;
    write.desired.gpuOffsetMHz = offset;
    ServiceResponse answer = {};
    handle_request(&write, &answer);
    if (answer.status) std::fprintf(stderr, "apply fixture op=%llu: %s\n",
        (unsigned long long)operation, answer.message);
    return answer;
}

int main() {
    initialize();
    // --gpu BDF --json-live can reach the daemon before PCI metadata is
    // enriched. The request constrains only the exact BDF, not missing IDs.
    auto bare = request(SERVICE_CMD_GET_SNAPSHOT, 0);
    bare.targetGpu.pciInfoValid = false;
    bare.targetGpu.deviceId = bare.targetGpu.subSystemId = 0;
    ServiceResponse bareAnswer = {};
    handle_request(&bare, &bareAnswer);
    if (bareAnswer.status != SERVICE_STATUS_OK ||
        !linux_gpu_identity_matches(&g_gpuRuntimes[0].binding,
            &bareAnswer.snapshot.adapters[bareAnswer.snapshot.selectedAdapterIndex]))
        return 23;
    if (apply(0, 100, 1).status || apply(1, 200, 2).status) return 1;
    if (!g_gpuRuntimes[0].hasActiveDesired || !g_gpuRuntimes[1].hasActiveDesired ||
        g_gpuRuntimes[0].activeDesired.gpuOffsetMHz != 100 ||
        g_gpuRuntimes[1].activeDesired.gpuOffsetMHz != 200 || states.size() != 2)
        return 2;
    if (apply(0, 100, 1).status || writes[0] != 1) return 3;
    failApply = true;
    if (apply(0, 300, 3).status == SERVICE_STATUS_OK) return 4;
    failApply = false;
    if (g_gpuRuntimes[0].activeDesired.gpuOffsetMHz != 100 ||
        g_gpuRuntimes[1].activeDesired.gpuOffsetMHz != 200 ||
        states[g_gpuRuntimes[1].stateFile].desired.gpuOffsetMHz != 200) return 5;

    auto snapshot = request(SERVICE_CMD_GET_SNAPSHOT, 0);
    ServiceResponse fromA = {};
    handle_request(&snapshot, &fromA);
    auto stale = request(SERVICE_CMD_RESET, 1);
    stale.operationId = 4;
    stale.expectedServiceInstanceId = fromA.state.serviceInstanceId;
    stale.expectedGpuGeneration = fromA.state.gpuGeneration;
    stale.expectedTopologySignature = fromA.state.topologySignature;
    ServiceResponse answer = {};
    handle_request(&stale, &answer);
    if (answer.status == SERVICE_STATUS_OK || resets[1]) return 6;

    auto policy = request(SERVICE_CMD_SET_STARTUP_POLICY, 0);
    policy.startupMode = SERVICE_STARTUP_POLICY_NONE;
    handle_request(&policy, &answer);
    if (answer.status || g_gpuRuntimes[0].startupPolicy.mode !=
            SERVICE_STARTUP_POLICY_NONE || g_gpuRuntimes[1].startupPolicy.mode !=
            SERVICE_STARTUP_POLICY_RESTORE_LAST) return 7;

    // A fan operation for B can acquire B's context while A is locked for an
    // apply. This executes the real fixed-duty maintenance path, not a mock.
    pl_mutex_lock(&g_gpuRuntimes[0].lock);
    {
        LinuxGpuRuntimeScope selected(&g_gpuRuntimes[1]);
        pl_mutex_lock(&g_lock);
        g_activeDesired.fanPercent = 71;
        const char* reason = nullptr;
        if (daemon_fixed_fan_maintain(&reason) != FAN_RUNTIME_OUTCOME_SUCCESS)
            return 8;
        pl_mutex_unlock(&g_lock);
    }
    pl_mutex_unlock(&g_gpuRuntimes[0].lock);
    if (duties[0] || duties[1] != 71) return 9;

    auto resume = request(SERVICE_CMD_RESUME_RESTORE);
    handle_request(&resume, &answer);
    if (answer.status || writes[0] != 3 || writes[1] != 2) return 10;
    // This used to self-deadlock when a no-target resume routed into the sole
    // context's already-held mutex before aggregate dispatch locked it again.
    g_gpuRuntimeCount = 1;
    handle_request(&resume, &answer);
    if (answer.status || writes[0] != 4) return 11;
    resume.targetGpu = g_gpuRuntimes[0].binding;
    handle_request(&resume, &answer);
    if (answer.status == SERVICE_STATUS_OK || writes[0] != 4) return 12;
    g_gpuRuntimeCount = 2;

    auto missing = request(SERVICE_CMD_APPLY, 0);
    missing.targetGpu.pciBus = 99;
    missing.operationId = 5;
    missing.desired.hasGpuOffset = true;
    missing.desired.gpuOffsetMHz = 999;
    handle_request(&missing, &answer);
    if (answer.status == SERVICE_STATUS_OK || writes[0] != 4 || writes[1] != 2)
        return 13;

    auto moved = g_gpuRuntimes[0].binding;
    moved.pciBus = 9;
    if (daemon_register_gpu_runtime(&moved, &g_gpuRuntimes[0].bindingUuid))
        return 14;
    if (daemon_register_gpu_runtime(&g_gpuRuntimes[0].binding,
            &g_gpuRuntimes[1].bindingUuid)) return 15;
    auto replacement = g_gpuRuntimes[0].binding;
    ++replacement.deviceId;
    if (daemon_register_gpu_runtime(&replacement,
            &g_gpuRuntimes[1].bindingUuid) || g_gpuRuntimeCount != 2)
        return 24;

    // An ACTIVE persistence failure rolls A back without disturbing B's
    // committed settings or journal. The next explicit operation can repair A.
    failCommit = true;
    if (apply(0, 400, 6).status == SERVICE_STATUS_OK) return 16;
    failCommit = false;
    if (g_gpuRuntimes[0].gpu.freqOffsets[0] != 100000 ||
        g_gpuRuntimes[1].gpu.freqOffsets[0] != 200000 ||
        g_gpuRuntimes[1].activeDesired.gpuOffsetMHz != 200) return 17;

    // Upgrade: old active intent and old boot profile may name different
    // cards. Both inherit the old crash guard; no migration writes a GPU.
    states.clear(); startups.clear(); guards.clear(); operations.clear();
    g_discoveryRuntime.gpu.adapterCount = 2;
    for (unsigned int i = 0; i < 2; ++i) {
        g_discoveryRuntime.gpu.adapters[i] = g_gpuRuntimes[i].binding;
        g_discoveryRuntime.gpu.adapterUuids[i] = g_gpuRuntimes[i].bindingUuid;
    }
    DesiredSettings saved = {};
    saved.hasGpuOffset = true; saved.gpuOffsetMHz = 111;
    LinuxDaemonStateRecord legacy = {};
    linux_daemon_record_initialize(&legacy, LINUX_DAEMON_RECORD_ACTIVE,
        &g_gpuRuntimes[0].binding, &saved, 90, SERVICE_OPERATION_SUCCEEDED,
        GPU_LOCATION_ROLE_REQUIRED, &g_gpuRuntimes[0].bindingUuid);
    states[GC_DAEMON_STATE_DIR "/active.bin"] = legacy;
    LinuxDaemonStartupRecord boot = {};
    saved.gpuOffsetMHz = 222;
    linux_daemon_startup_initialize(&boot, SERVICE_STARTUP_POLICY_PROFILE,
        1, "fixture profile", &g_gpuRuntimes[1].binding, &saved,
        GPU_LOCATION_ROLE_REQUIRED, &g_gpuRuntimes[1].bindingUuid);
    startups[GC_DAEMON_STATE_DIR "/startup.bin"] = boot;
    LinuxAutoRestoreGuard oldGuard = {};
    oldGuard.startAttempts = 2;
    linux_auto_restore_note_lockout(&oldGuard,
        SERVICE_AUTO_RESTORE_LOCKOUT_UNSTABLE_APPLY);
    guards[GC_DAEMON_STATE_DIR "/restore-guard.bin"] = oldGuard;
    const int beforeMigration = writes[0] + writes[1];
    failDestinationProbe = true;
    if (daemon_migrate_single_gpu_records() ||
        !states.count(GC_DAEMON_STATE_DIR "/active.bin") ||
        !startups.count(GC_DAEMON_STATE_DIR "/startup.bin") ||
        !guards.count(GC_DAEMON_STATE_DIR "/restore-guard.bin") ||
        writes[0] + writes[1] != beforeMigration) return 20;
    failDestinationProbe = false;
    if (!daemon_migrate_single_gpu_records() ||
        states[g_gpuRuntimes[0].stateFile].desired.gpuOffsetMHz != 111 ||
        startups[g_gpuRuntimes[1].startupFile].desired.gpuOffsetMHz != 222 ||
        !guards[g_gpuRuntimes[0].guardFile].lockedOut ||
        !guards[g_gpuRuntimes[1].guardFile].lockedOut ||
        guards[g_gpuRuntimes[0].guardFile].startAttempts != 2 ||
        states.count(GC_DAEMON_STATE_DIR "/active.bin") ||
        writes[0] + writes[1] != beforeMigration) return 18;

    legacy.targetGpu.pciBus = 99;
    legacy.targetUuid = {};
    states[GC_DAEMON_STATE_DIR "/active.bin"] = legacy;
    guards[GC_DAEMON_STATE_DIR "/restore-guard.bin"] = oldGuard;
    if (!daemon_migrate_single_gpu_records() ||
        !states.count(GC_DAEMON_STATE_DIR "/active.bin") ||
        !guards.count(GC_DAEMON_STATE_DIR "/restore-guard.bin")) return 19;
    // An ownerless same-boot marker survives every partial inventory. Existing
    // current per-GPU ownership must not be replaced by an older global blob.
    states.clear(); startups.clear(); guards.clear(); fans.clear();
    LinuxFanOwnershipMarker ownerless = {}, existing = {};
    linux_fan_ownership_marker_initialize(&ownerless,
        "00000000-0000-0000-0000-000000000001");
    linux_fan_ownership_marker_initialize(&existing, ownerless.bootId,
        &g_gpuRuntimes[1].binding, GPU_LOCATION_ROLE_REQUIRED,
        &g_gpuRuntimes[1].bindingUuid);
    fans[GC_DAEMON_STATE_DIR "/fan-owned.bin"] = ownerless;
    fans[g_gpuRuntimes[1].fanMarkerFile] = existing;
    guards[GC_DAEMON_STATE_DIR "/restore-guard.bin"] = oldGuard;
    saved.gpuOffsetMHz = 111;
    linux_daemon_record_initialize(&legacy, LINUX_DAEMON_RECORD_ACTIVE,
        &g_gpuRuntimes[0].binding, &saved, 90, SERVICE_OPERATION_SUCCEEDED,
        GPU_LOCATION_ROLE_REQUIRED, &g_gpuRuntimes[0].bindingUuid);
    states[GC_DAEMON_STATE_DIR "/active.bin"] = legacy;
    const int autoBefore = autoWrites[0] + autoWrites[1];
    for (unsigned int visible : {0u, 1u, 2u}) {
        g_gpuRuntimeCount = visible;
        if (daemon_migrate_single_gpu_records() ||
            !fans.count(GC_DAEMON_STATE_DIR "/fan-owned.bin") ||
            !states.count(GC_DAEMON_STATE_DIR "/active.bin") ||
            !guards.count(GC_DAEMON_STATE_DIR "/restore-guard.bin") ||
            memcmp(&fans[g_gpuRuntimes[1].fanMarkerFile], &existing,
                sizeof(existing)) != 0 ||
            autoWrites[0] + autoWrites[1] != autoBefore) return 25;
    }
    g_gpuRuntimeCount = 2;
    bootIdUnavailable = true;
    if (daemon_migrate_single_gpu_records() ||
        !fans.count(GC_DAEMON_STATE_DIR "/fan-owned.bin")) return 30;
    bootIdUnavailable = false;
    corruptLegacyFan = true;
    if (daemon_migrate_single_gpu_records() ||
        !fans.count(GC_DAEMON_STATE_DIR "/fan-owned.bin")) return 26;
    corruptLegacyFan = false;
    gc_strlcpy(ownerless.bootId, sizeof(ownerless.bootId),
        "00000000-0000-0000-0000-000000000002");
    fans[GC_DAEMON_STATE_DIR "/fan-owned.bin"] = ownerless;
    if (!daemon_migrate_single_gpu_records() ||
        fans.count(GC_DAEMON_STATE_DIR "/fan-owned.bin") ||
        memcmp(&fans[g_gpuRuntimes[1].fanMarkerFile], &existing,
            sizeof(existing)) != 0 ||
        autoWrites[0] + autoWrites[1] != autoBefore) return 27;

    // Late discovery loads inherited fan ownership but defers its hardware
    // handback to the worker, never to the caller's state-read deadline.
    {
        LinuxGpuRuntimeScope selected(&g_gpuRuntimes[1]);
        LinuxFanOwnershipMarker marker = {};
        linux_fan_ownership_marker_initialize(&marker,
            "00000000-0000-0000-0000-000000000001", &g_runtime->binding,
            GPU_LOCATION_ROLE_REQUIRED, &g_runtime->bindingUuid);
        fans[g_runtime->fanMarkerFile] = marker;
        g_fanOwnershipMarkerCommitted = false;
        g_fanOwnershipHandbackPending = false;
        g_fanOwnershipHandbackDeferred = false;
        int before = autoWrites[1];
        daemon_fan_ownership_handback_at_start(true);
        if (!g_fanOwnershipHandbackDeferred || autoWrites[1] != before) return 21;
    }

    // Start without a driver, then service an explicit targeted read after it
    // becomes available. Real worker threads are joined without sleeps.
    inventoryTemplate = g_gpuRuntimes[0].gpu;
    auto lateTarget = g_gpuRuntimes[0].binding;
    for (unsigned int i = 0; i < 2; ++i) {
        pl_mutex_destroy(&g_gpuRuntimes[i].lock);
        pthread_mutex_destroy(&g_gpuRuntimes[i].fanWakeMutex);
        pthread_cond_destroy(&g_gpuRuntimes[i].fanWakeCondition);
        g_gpuRuntimes[i] = {};
    }
    g_gpuRuntimeCount = 0;
    g_discoveryRuntime.gpu = {};
    g_discoveryRuntime.gpuReady = false;
    states.clear(); startups.clear(); guards.clear(); fans.clear();
    gc_strlcpy(ownerless.bootId, sizeof(ownerless.bootId),
        "00000000-0000-0000-0000-000000000001");
    fans[GC_DAEMON_STATE_DIR "/fan-owned.bin"] = ownerless;
    inventoryAvailable = true;
    auto late = request(SERVICE_CMD_GET_SNAPSHOT);
    late.targetGpu = lateTarget;
    handle_request(&late, &answer);
    if (answer.status == SERVICE_STATUS_OK || g_gpuRuntimeCount != 0 ||
        !fans.count(GC_DAEMON_STATE_DIR "/fan-owned.bin")) return 28;
    inventoryTemplate.adapterCount = 1;
    handle_request(&late, &answer);
    if (answer.status == SERVICE_STATUS_OK || g_gpuRuntimeCount != 0 ||
        !fans.count(GC_DAEMON_STATE_DIR "/fan-owned.bin")) return 29;
    inventoryTemplate.adapterCount = 2;
    gc_strlcpy(ownerless.bootId, sizeof(ownerless.bootId),
        "00000000-0000-0000-0000-000000000002");
    fans[GC_DAEMON_STATE_DIR "/fan-owned.bin"] = ownerless;
    handle_request(&late, &answer);
    const bool discovered = answer.status == SERVICE_STATUS_OK &&
        g_gpuRuntimeCount == 2 &&
        !fans.count(GC_DAEMON_STATE_DIR "/fan-owned.bin") &&
        autoWrites[0] + autoWrites[1] == autoBefore &&
        linux_gpu_identity_matches(&lateTarget, &answer.snapshot.adapters[
            answer.snapshot.selectedAdapterIndex]);
    g_running = 0;
    daemon_shutdown_gpu_runtimes();
    if (!discovered) return 22;
    std::puts("Multi-GPU production request/context regressions passed");
    return 0;
}
