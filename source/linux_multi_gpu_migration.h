// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Validated singleton records are transferred once to their physical owner.
// Existing per-GPU records always win; unresolved legacy records remain intact.

static int daemon_legacy_record_owner(const GpuAdapterInfo* target,
    GpuLocationRole role, const LinuxGpuUuid* uuid) {
    const LinuxGpuState& discovery = g_discoveryRuntime.gpu;
    auto selected = linux_gpu_restore_selection(target, role,
        discovery.adapters, discovery.adapterCount, uuid,
        discovery.adapterUuids, nullptr);
    if (!selected.writeResolved || selected.index < 0) return -1;
    return linux_multi_gpu_find_runtime(g_gpuRuntimes, g_gpuRuntimeCount,
        &discovery.adapters[selected.index]);
}

template <typename Record, typename Writer>
static bool daemon_migrate_store_if_absent(const char* path,
    const Record* record, Writer writer, char* err, size_t errSize) {
    struct stat status = {};
    if (lstat(path, &status) == 0) {
        // An existing private record wins. An invalid destination never gives
        // permission to retire the only validated legacy copy.
        if (S_ISREG(status.st_mode) && status.st_uid == 0 &&
            status.st_nlink == 1 && (status.st_mode & 0077) == 0) return true;
        gc_strlcpy(err, errSize, "migration destination is not a protected record");
        return false;
    }
    if (errno != ENOENT) {
        gc_snprintf(err, errSize, "cannot inspect migration destination: %s", strerror(errno));
        return false;
    }
    return writer(path, record, err, errSize);
}

static bool daemon_migrate_single_gpu_records() {
    const char* activePath = GC_DAEMON_STATE_DIR "/active.bin";
    const char* startupPath = GC_DAEMON_STATE_DIR "/startup.bin";
    const char* guardPath = GC_DAEMON_STATE_DIR "/restore-guard.bin";
    const char* fanPath = GC_DAEMON_STATE_DIR "/fan-owned.bin";
    const char* operationPath = GC_DAEMON_STATE_DIR "/operation.bin";
    char err[256] = {};
    LinuxFanOwnershipMarker fan = {};
    int fanLoaded = linux_fan_ownership_marker_load(fanPath, &fan, err, sizeof(err));
    if (fanLoaded < 0) {
        dlog("daemon migration: legacy fan ownership unreadable; refusing startup\n");
        return false;
    }
    if (fanLoaded > 0 && !fan.targetGpu.valid) {
        char bootId[LINUX_FAN_OWNERSHIP_BOOT_ID_MAX] = {};
        if (!linux_read_boot_id(bootId, sizeof(bootId))) {
            dlog("daemon migration: cannot verify the legacy fan boot; refusing startup\n");
            return false;
        }
        // A visible inventory, even one board, cannot prove that the owner is
        // not a temporarily missing GPU. Keep all legacy sources intact until
        // a real boot boundary proves the old manual fan state was reset.
        if (strcmp(fan.bootId, bootId) == 0) {
            dlog("daemon migration: legacy fan owner is unknown; reboot required "
                 "before multi-GPU control can start (no fan writes attempted)\n");
            return false;
        }
        if (!linux_daemon_state_remove(fanPath, err, sizeof(err))) return false;
        fanLoaded = 0;
        dlog("daemon migration: retired ownerless fan marker from another boot\n");
    }
    LinuxDaemonStateRecord active = {};
    bool widened = false;
    auto loaded = linux_daemon_state_load(activePath, &active, err,
        sizeof(err), &widened);
    int activeOwner = loaded == LINUX_DAEMON_STATE_LOADED
        ? daemon_legacy_record_owner(&active.targetGpu,
            (GpuLocationRole)active.targetLocationRole, &active.targetUuid) : -1;
    LinuxDaemonStartupRecord startup = {};
    bool corrupt = false;
    bool haveStartup = linux_daemon_startup_load(startupPath, &startup,
        &corrupt, err, sizeof(err), &widened);
    int startupOwner = haveStartup && startup.targetGpu.valid
        ? daemon_legacy_record_owner(&startup.targetGpu,
            (GpuLocationRole)startup.targetLocationRole, &startup.targetUuid)
        : activeOwner;
    if (startupOwner < 0 && g_gpuRuntimeCount == 1 &&
        (!haveStartup || !startup.targetGpu.valid)) startupOwner = 0;

    // The old guard covered all old automatic paths. Preserve its lockout and
    // spent budget for every resolved legacy owner before retiring any source.
    LinuxAutoRestoreGuard guard = {};
    bool guardCorrupt = false;
    bool haveGuard = linux_daemon_guard_load(guardPath, &guard,
        &guardCorrupt, err, sizeof(err));
    if (guardCorrupt) {
        linux_auto_restore_note_lockout(&guard,
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED);
        haveGuard = true;
    }
    int owners[] = {activeOwner, startupOwner};
    for (int index : owners) {
        if (index < 0) continue;
        auto& entry = g_gpuRuntimes[index];
        if (haveGuard && !daemon_migrate_store_if_absent(entry.guardFile,
                &guard, linux_daemon_guard_store, err, sizeof(err)))
            return false;
    }
    if (activeOwner >= 0) {
        auto& entry = g_gpuRuntimes[activeOwner];
        if (!daemon_migrate_store_if_absent(entry.stateFile,
                &active, linux_daemon_state_store, err, sizeof(err)))
            return false;
        LinuxDaemonOperationRecord operation = {};
        if (linux_daemon_operation_load(operationPath, &operation, err, sizeof(err))) {
            if (!daemon_migrate_store_if_absent(entry.operationFile,
                    &operation, linux_daemon_operation_store, err, sizeof(err))) return false;
            if (!linux_daemon_state_remove(operationPath, err, sizeof(err))) return false;
        }
        if (!linux_daemon_state_remove(activePath, err, sizeof(err))) return false;
        dlog("daemon migration: singleton active intent transferred to GPU runtime %d\n",
             activeOwner);
    }
    if (startupOwner >= 0 && (haveStartup || corrupt)) {
        if (corrupt) linux_daemon_startup_initialize(&startup,
            SERVICE_STARTUP_POLICY_NONE, 0, nullptr, nullptr, nullptr);
        auto& entry = g_gpuRuntimes[startupOwner];
        if (!daemon_migrate_store_if_absent(entry.startupFile,
                &startup, linux_daemon_startup_store, err, sizeof(err))) return false;
        if (!linux_daemon_state_remove(startupPath, err, sizeof(err))) return false;
    }
    int fanOwner = fanLoaded > 0 && fan.targetGpu.valid
        ? daemon_legacy_record_owner(&fan.targetGpu,
            (GpuLocationRole)fan.targetLocationRole, &fan.targetUuid) : -1;
    if (fanOwner >= 0) {
        auto& entry = g_gpuRuntimes[fanOwner];
        if (!daemon_migrate_store_if_absent(entry.fanMarkerFile,
                &fan, linux_fan_ownership_marker_store, err, sizeof(err))) return false;
        if (!linux_daemon_state_remove(fanPath, err, sizeof(err))) return false;
    }
    // An unresolved source keeps the old guard. A later start can resolve the
    // original board without receiving a fresh automatic-write budget.
    if ((loaded != LINUX_DAEMON_STATE_LOADED || activeOwner >= 0) &&
        (!haveStartup || startupOwner >= 0) &&
        (activeOwner >= 0 || startupOwner >= 0))
        return linux_daemon_state_remove(guardPath, err, sizeof(err));
    return true;
}
