// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Real INI load/save/clear isolation; UUIDs are fixtures, no driver is loaded.
#include "linux_port.h"
#include "linux_gpu_profile_scope.h"
#include "posix_test_environment.h"
#include <cstdio>
#include <string>
#include <unistd.h>

void linux_debug_logf(const char*, ...) {}

int main() {
    char storage[256] = {};
    char* directory = gc_test_make_temp_dir(storage, sizeof(storage),
        "greencurve-profile-scope", 0);
    if (!directory) return 1;
    std::string base = std::string(directory) + "/profiles.ini";
    char err[256] = {};
    DesiredSettings desired = {};
    initialize_desired_settings_defaults(&desired);
    desired.hasGpuOffset = true;
    desired.gpuOffsetMHz = 50;
    if (!save_profile_to_config_path(base.c_str(), 1, &desired, err, sizeof(err)))
        return 2;
    GpuAdapterInfo a = {}, b = {};
    a.valid = a.pciInfoValid = b.valid = b.pciInfoValid = true;
    a.pciBus = 1; b.pciBus = 2;
    a.deviceId = b.deviceId = 0x1e04;
    auto uuidA = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000001");
    auto uuidB = linux_gpu_uuid_parse("GPU-00000000-0000-0000-0000-000000000002");
    linux_gpu_profile_scope_enable(true);
    linux_gpu_profile_scope_select(&a, &uuidA);
    const auto pathA = linux_gpu_profile_path(base.c_str());
    DesiredSettings loaded = {};
    if (!load_profile_from_config_path(base.c_str(), 1, &loaded, err, sizeof(err)) ||
        loaded.gpuOffsetMHz != 50) return 3;
    desired.gpuOffsetMHz = 100;
    if (!save_profile_to_config_path(base.c_str(), 1, &desired, err, sizeof(err)))
        return 4;
    linux_gpu_profile_scope_select(&b, &uuidB);
    const auto pathB = linux_gpu_profile_path(base.c_str());
    if (pathA == pathB ||
        !load_profile_from_config_path(base.c_str(), 1, &loaded, err, sizeof(err)) ||
        loaded.gpuOffsetMHz != 50) return 5;
    desired.gpuOffsetMHz = 200;
    if (!save_profile_to_config_path(base.c_str(), 1, &desired, err, sizeof(err)))
        return 6;
    linux_gpu_profile_scope_select(&a, &uuidA);
    if (!load_profile_from_config_path(base.c_str(), 1, &loaded, err, sizeof(err)) ||
        loaded.gpuOffsetMHz != 100) return 7;
    if (!clear_profile_from_config_path(base.c_str(), 1, err, sizeof(err))) return 8;
    if (load_profile_from_config_path(base.c_str(), 1, &loaded, err, sizeof(err)))
        return 9;
    linux_gpu_profile_scope_select(&b, &uuidB);
    if (!load_profile_from_config_path(base.c_str(), 1, &loaded, err, sizeof(err)) ||
        loaded.gpuOffsetMHz != 200) return 10;
    b.pciBus = 9;
    linux_gpu_profile_scope_select(&b, &uuidB);
    if (linux_gpu_profile_path(base.c_str()) != pathB) return 11;
    linux_gpu_profile_scope_enable(false);
    if (!load_profile_from_config_path(base.c_str(), 1, &loaded, err, sizeof(err)) ||
        loaded.gpuOffsetMHz != 50) return 12;
    unlink(pathA.c_str()); unlink(pathB.c_str()); unlink(base.c_str());
    rmdir(directory);
    std::puts("Per-GPU user profile banks preserve shared templates and isolate writes");
    return 0;
}
