// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_GPU_PROFILE_SCOPE_H
#define GREEN_CURVE_LINUX_GPU_PROFILE_SCOPE_H

#include "linux_gpu_uuid.h"
#include "platform.h"
#include <string>

struct LinuxGpuProfileScope {
    bool enabled = false;
    GpuAdapterInfo target = {};
    LinuxGpuUuid uuid = {};
};

inline LinuxGpuProfileScope& linux_gpu_profile_scope() {
    static LinuxGpuProfileScope scope;
    return scope;
}

// The default bank is per physical card. An explicitly supplied --config
// retains the existing shared-bank semantics, including offline editing.
inline void linux_gpu_profile_scope_enable(bool enabled) {
    linux_gpu_profile_scope().enabled = enabled;
}

inline void linux_gpu_profile_scope_select(const GpuAdapterInfo* target,
    const LinuxGpuUuid* knownUuid = nullptr) {
    auto& scope = linux_gpu_profile_scope();
    if (!knownUuid && target && linux_gpu_uuid_valid(&scope.uuid) &&
        linux_gpu_identity_matches(target, &scope.target)) return;
    scope.target = target ? *target : GpuAdapterInfo{};
    scope.uuid = knownUuid ? *knownUuid : LinuxGpuUuid{};
    if (!scope.enabled || !target || !linux_gpu_bdf_valid(target) ||
        linux_gpu_uuid_valid(&scope.uuid)) return;
    PlLib library = pl_open_driver_library(PL_DRIVER_NVML);
    if (!library) return;
    using Init = nvmlReturn_t (*)();
    using Handle = nvmlReturn_t (*)(const char*, nvmlDevice_t*);
    using Uuid = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
    auto init = reinterpret_cast<Init>(pl_lib_sym(library, "nvmlInit_v2"));
    auto shutdown = reinterpret_cast<Init>(pl_lib_sym(library, "nvmlShutdown"));
    auto handle = reinterpret_cast<Handle>(pl_lib_sym(library,
        "nvmlDeviceGetHandleByPciBusId_v2"));
    auto uuid = reinterpret_cast<Uuid>(pl_lib_sym(library, "nvmlDeviceGetUUID"));
    if (init && shutdown && handle && uuid && init() == NVML_SUCCESS) {
        char bus[32] = {}, text[96] = {};
        gc_snprintf(bus, sizeof(bus), "%04x:%02x:%02x.%u", target->pciDomain,
            target->pciBus, target->pciDevice, target->pciFunction);
        nvmlDevice_t device = nullptr;
        if (handle(bus, &device) == NVML_SUCCESS &&
            uuid(device, text, sizeof(text)) == NVML_SUCCESS)
            scope.uuid = linux_gpu_uuid_parse(text);
        shutdown();
    }
    pl_lib_close(library);
}

inline std::string linux_gpu_profile_path(const char* path) {
    if (!path) return {};
    const auto& scope = linux_gpu_profile_scope();
    if (!scope.enabled || !linux_gpu_bdf_valid(&scope.target)) return path;
    char key[96] = {};
    if (linux_gpu_uuid_valid(&scope.uuid))
        gc_strlcpy(key, sizeof(key), scope.uuid.text);
    else
        gc_snprintf(key, sizeof(key), "pci-%04x-%02x-%02x-%u",
            scope.target.pciDomain, scope.target.pciBus,
            scope.target.pciDevice, scope.target.pciFunction);
    std::string result(path), suffix = std::string(".gpu-") + key + ".ini";
    if (result.size() < suffix.size() ||
        result.compare(result.size() - suffix.size(), suffix.size(), suffix) != 0)
        result += suffix;
    return result;
}

#endif
