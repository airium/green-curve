// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_INSTANCE_CLI_H
#define GREEN_CURVE_LINUX_INSTANCE_CLI_H
#include "linux_instance.h"
#include "linux_port.h"
#include <string>

static inline std::string linux_instance_cli_arguments() {
    return linux_instance_enabled() ? std::string(" --instance ") + linux_instance().uuid.text : "";
}

static inline bool linux_instance_prepare_options(const LinuxCliOptions& opts,
                                                   char* path, size_t size) {
    if (opts.hasInstance && !linux_instance_configure(opts.instanceUuid.text)) return false;
    linux_instance().daemon = opts.daemon;
    if (opts.hasConfigPath) snprintf(path, size, "%s", opts.configPath);
    else if (!default_linux_config_path(path, size)) snprintf(path, size, "%s", CONFIG_FILE_NAME);
    if (opts.hasInstance && !opts.hasConfigPath) {
        std::string base(path);
        size_t slash = base.find_last_of('/');
        base = slash == std::string::npos ? "." : base.substr(0, slash);
        int n = snprintf(path, size, "%s/instances/%s/config.ini", base.c_str(), opts.instanceUuid.text);
        if (n < 0 || (size_t)n >= size) return false;
    }
    return true;
}
#endif
