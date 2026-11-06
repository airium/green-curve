// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// One process owns exactly one backend, intent, fan and recovery lifetime.
#ifndef GREEN_CURVE_LINUX_DAEMON_RUNTIME_STATE_H
#define GREEN_CURVE_LINUX_DAEMON_RUNTIME_STATE_H
static LinuxGpuState g_gpu;
static bool g_gpuReady = false;
static pl_mutex g_lock;
static DesiredSettings g_activeDesired;
static bool g_hasActiveDesired = false;
static GpuAdapterInfo g_activeTarget;
static LinuxGpuUuid g_activeTargetUuid;
static GpuLocationRole g_activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
static bool g_stateUncertain = false;
static ServiceOperationTracker g_operationTracker = {};
// The committed startup-apply policy.  Zero-initialized means RESTORE_LAST,
// which is exactly what every build before protocol v13 did.
static LinuxDaemonStartupRecord g_startupPolicy = {};
// Whether an unattended write is still allowed.  See
// linux_auto_restore_policy.h; the persisted copy lives in GC_DAEMON_GUARD_FILE.
static LinuxAutoRestoreGuard g_autoRestoreGuard = {};
// The protocol-v29 cause of an AUTOMATIC_APPLY_FAILED latched by THIS daemon
// process (linux_auto_restore_runtime.h).  In memory only, like Windows: after
// a restart the persisted lockout is published without a cause.
static ServiceAutoApplyFailure g_autoApplyFailure = {};
#endif
