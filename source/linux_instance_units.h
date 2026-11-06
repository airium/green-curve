// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_INSTANCE_UNITS_H
#define GREEN_CURVE_LINUX_INSTANCE_UNITS_H
#include "linux_instance.h"
#include "linux_service_sandbox.h"

static inline int linux_instance_render_daemon_unit(FILE* f, const char* executable) {
    return fprintf(f,
        "[Unit]\n"
        "Description=Green Curve NVIDIA GPU control daemon\n"
        // Deliberately NOT `After=multi-user.target`.  That ordering was
        // self-defeating: the unit is WantedBy that same target, so ordering
        // after it scheduled Green Curve dead last in boot, behind every other
        // service that might touch the GPU.  Ordering against the driver's own
        // units is the real dependency, and it is ordering only -- no Wants=,
        // so a machine without nvidia-persistenced is unaffected rather than
        // having a service started for it.
        "After=nvidia-persistenced.service\n"
        // Windows' SCM resets its failure count after ten minutes and re-arms
        // the restart actions. systemd's start limit is a rolling window with a
        // 5-in-10s default, which a service that crashes on a two-second
        // RestartSec= trips almost immediately -- and once tripped the unit
        // stays failed until someone runs `systemctl reset-failed`. A ten
        // minute window gives the same shape as the SCM: spaced-out driver
        // events each get a fresh restart, a tight crash loop still stops.
        "StartLimitIntervalSec=600\n"
        "StartLimitBurst=5\n\n"
        "[Service]\n"
        "Type=notify\n"
        "NotifyAccess=main\n"
        "ExecStart=%s --daemon%s%s\n"
        // `always`, not `on-failure`. The daemon exits 0 for any stop signal,
        // so an out-of-band `kill -TERM` left the machine with no GPU control
        // and systemd declining to act -- while the same thing on Windows
        // (TerminateProcess) is exactly what the SCM restart actions cover.
        // `systemctl stop` still wins: systemd never restarts a unit it stopped.
        "Restart=always\n"
        "RestartSec=2\n"
        // Liveness, not a poll: the daemon pings from its accept loop, so a
        // wedged serve loop is restarted instead of sitting there `active`
        // while nothing re-asserts the fan. Generous on purpose -- a VF apply
        // with per-point readback verification is the longest thing that runs
        // between two pings.
        "WatchdogSec=120\n"
        "StateDirectory=%s\n"
        "RuntimeDirectory=%s\n"
        "RuntimeDirectoryPreserve=yes\n"
        "RuntimeDirectoryMode=0755\n"
        GC_LINUX_SERVICE_SANDBOX
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        executable,
        linux_instance_enabled() ? " --instance " : "",
        linux_instance_enabled() ? linux_instance().uuid.text : "",
        linux_instance().paths.directoryName,
        linux_instance().paths.directoryName);
}

static inline int linux_instance_render_resume_unit(FILE* stream, const char* executable) {
    return fprintf(stream,
        "[Unit]\n"
        "Description=Green Curve settings restore after suspend\n"
        "Documentation=https://github.com/aufkrawall/greencurve\n"
        "After=suspend.target hibernate.target hybrid-sleep.target"
        " suspend-then-hibernate.target\n"
        "After=nvidia-resume.service\n"
        "After=%s\n"
        "ConditionPathExists=%s\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=%s --resume-restore%s%s\n"
        "StandardOutput=journal\n"
        "StandardError=journal\n"
        GC_LINUX_SERVICE_SANDBOX
        "\n"
        "[Install]\n"
        "WantedBy=suspend.target hibernate.target hybrid-sleep.target"
        " suspend-then-hibernate.target\n",
        linux_instance().paths.unitName, linux_instance().paths.socketPath, executable,
        linux_instance_enabled() ? " --instance " : "",
        linux_instance_enabled() ? linux_instance().uuid.text : "");
}
#endif
