// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included after the existing per-GPU restoration helpers.
static void daemon_load_gpu_at_boot() {
    char err[256] = {};
    LinuxDaemonOperationRecord operation = {};
    if (linux_daemon_operation_load(GC_DAEMON_OPERATION_FILE, &operation,
            err, sizeof(err))) {
        gc_u32 restoredState = operation.state == SERVICE_OPERATION_IN_PROGRESS
            ? SERVICE_OPERATION_OUTCOME_UNKNOWN : operation.state;
        service_operation_restore(&g_operationTracker, operation.operationId,
            restoredState, operation.responseStatus, operation.outcomeSeverity,
            restoredState == SERVICE_OPERATION_OUTCOME_UNKNOWN
                ? "operation outcome became uncertain across daemon restart"
                : operation.message);
    }

    load_auto_restore_guard_at_boot();
    // After the guard load (a give-up latches it), before any startup write
    // and before the fan worker exists: a previous daemon in this boot that
    // died with the fan under manual control gets it handed back to the
    // driver, at most twice (linux_fan_ownership.h).
    daemon_fan_ownership_handback_at_start();
    load_startup_policy_at_boot();

    if (g_startupPolicy.mode == SERVICE_STARTUP_POLICY_PROFILE) {
        apply_startup_profile_policy();
    }

    // Startup restart-reapply is authorized only by a complete, checksummed
    // ACTIVE record for the exact physical GPU. Prepared/uncertain/legacy state
    // never causes an automatic hardware write.
    LinuxDaemonStateRecord saved = {};
    bool stateMigratedFromLegacy = false;
    LinuxDaemonStateLoadResult loadResult = linux_daemon_state_load(
        GC_DAEMON_STATE_FILE, &saved, err, sizeof(err), &stateMigratedFromLegacy);
    if (loadResult == LINUX_DAEMON_STATE_LOADED && err[0]) {
        // Informational migration note from the loader (an older on-disk
        // generation was widened into the current record); the committed record
        // itself is rewritten by the next state transition.
        dlog("daemon: %s\n", err);
    }
    if (loadResult == LINUX_DAEMON_STATE_LEGACY_REMOVED ||
        loadResult == LINUX_DAEMON_STATE_INVALID_REMOVED) {
        // The loader's reason is the only thing that separates an upgrade that
        // lost its layout from a torn or tampered file, and this failure is
        // silent by nature -- the daemon simply applies nothing.  Never drop it.
        dlog("daemon: rejected and removed %s daemon state (%s); explicit Apply/Reset required\n",
             loadResult == LINUX_DAEMON_STATE_LEGACY_REMOVED ? "legacy" : "invalid",
             err[0] ? err : "no detail");
    } else if (loadResult == LINUX_DAEMON_STATE_IO_ERROR) {
        g_stateUncertain = true;
        dlog("daemon: state load failed closed: %s\n", err);
    } else if (loadResult == LINUX_DAEMON_STATE_LOADED) {
        if (saved.operationId != 0) {
            gc_u32 restoredState = saved.operationState;
            if (saved.state != LINUX_DAEMON_RECORD_ACTIVE ||
                restoredState == SERVICE_OPERATION_IN_PROGRESS) {
                restoredState = SERVICE_OPERATION_OUTCOME_UNKNOWN;
            }
            // The committed state record carries no severity of its own; the
            // resolver derives it from the status this restore reconstructs,
            // which is exactly as much as this record actually knows.
            gc_u32 restoredStatus = restoredState == SERVICE_OPERATION_SUCCEEDED
                ? (gc_u32)SERVICE_STATUS_OK : (gc_u32)SERVICE_STATUS_ERROR;
            service_operation_restore(&g_operationTracker,
                saved.operationId, restoredState, restoredStatus,
                service_response_resolve_outcome_severity(restoredStatus,
                    SERVICE_OUTCOME_SEVERITY_SUCCESS),
                restoredState == SERVICE_OPERATION_SUCCEEDED
                    ? "operation restored from committed daemon state"
                    : "operation outcome became uncertain across daemon restart");
        }
        if (g_startupPolicy.mode != SERVICE_STARTUP_POLICY_RESTORE_LAST) {
            // The administrator asked for something other than "replay what was
            // applied last", so the committed record stays on disk untouched
            // and is simply not written to the GPU.  A later explicit Apply or
            // a policy change back to restore-last picks it up again.
            dlog("daemon: committed intent present but startup policy is %s; "
                 "not replaying it\n",
                 service_startup_policy_mode_name(g_startupPolicy.mode));
        } else if (saved.state != LINUX_DAEMON_RECORD_ACTIVE) {
            g_stateUncertain = true;
            dlog("daemon: startup state=%u is not ACTIVE; no automatic write\n", saved.state);
        } else {
            // One unattended-write path for all three automatic origins: it
            // owns the crash-loop guard, the GPU re-selection, and -- the part
            // this code used to get wrong -- building the request through
            // service_build_full_restore_request(), so the replay resets the OC
            // baseline before laying the stored curve back down instead of
            // stacking it on whatever the driver currently holds.
            LinuxAutoRestoreOutcome outcome = daemon_automatic_restore_write(
                LINUX_AUTO_RESTORE_TRIGGER_BOOT_RESTORE_LAST,
                &saved.targetGpu, &saved.desired,
                (GpuLocationRole)saved.targetLocationRole, &saved.targetUuid);
            if (!outcome.success)
                dlog("daemon: startup reapply did not commit -> %s\n",
                     outcome.message[0] ? outcome.message : "unknown reason");
        }
    }

}
