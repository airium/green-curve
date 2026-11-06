// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_instance.h"

#if !defined(_WIN32)
#include "linux_instance_ownership.h"
#include "posix_test_environment.h"
#endif

namespace {
const char* const UUID_A = "GPU-01234567-89ab-cdef-0123-456789abcdef";
const char* const UUID_B = "GPU-11234567-89ab-cdef-0123-456789abcdef";
const char* reportedUuid = nullptr;
nvmlReturn_t fake_uuid(nvmlDevice_t, char* out, unsigned int size) {
    if (!reportedUuid) return NVML_ERROR_NOT_SUPPORTED;
    snprintf(out, size, "%s", reportedUuid);
    return NVML_SUCCESS;
}

GpuAdapterInfo gpu_at(unsigned int bus) {
    GpuAdapterInfo gpu = {};
    gpu.valid = true;
    gpu.pciBus = bus;
    return gpu;
}

int selection_tests() {
    const LinuxGpuUuid a = linux_gpu_uuid_parse(UUID_A);
    const LinuxGpuUuid b = linux_gpu_uuid_parse(UUID_B);
    const LinuxGpuUuid observed[] = {b, a};
    if (linux_instance_gpu_index(&a, observed, 2) != 1) return 7800;
    if (linux_instance_gpu_index(&b, observed, 2) != 0) return 7801;
    const LinuxGpuUuid duplicate[] = {a, a};
    if (linux_instance_gpu_index(&a, duplicate, 2) != -2) return 7802;
    const LinuxGpuUuid missing[] = {b, {}};
    if (linux_instance_gpu_index(&a, missing, 2) != -1) return 7803;
    GpuAdapterInfo oldLocation = gpu_at(1), moved = gpu_at(7);
    if (linux_instance_restore_allowed(&a, &oldLocation, &moved, nullptr)) return 7804;
    if (!linux_instance_restore_allowed(&a, &oldLocation, &moved, &a)) return 7805;
    if (linux_instance_restore_allowed(&a, &moved, &moved, &b)) return 7806;
    if (!linux_instance_restore_allowed(&a, nullptr, &moved, nullptr)) return 7807;
    reportedUuid = UUID_A;
    if (!linux_instance_observed_identity(&a, fake_uuid, nullptr, true)) return 7824;
    if (linux_instance_observed_identity(&a, fake_uuid, nullptr, false)) return 7825;
    reportedUuid = UUID_B;
    if (linux_instance_observed_identity(&a, fake_uuid, nullptr, true)) return 7826;
    reportedUuid = nullptr;
    if (linux_instance_observed_identity(&a, fake_uuid, nullptr, true)) return 7827;
    if (linux_instance_observed_identity(&a, nullptr, nullptr, true)) return 7828;
    if (!linux_instance_observed_identity(nullptr, nullptr, nullptr, false)) return 7829;
    return 0;
}

int path_tests() {
    auto a = linux_gpu_uuid_parse(UUID_A), b = linux_gpu_uuid_parse(UUID_B);
    auto first = linux_instance_build_paths(&a);
    auto second = linux_instance_build_paths(&b);
    auto legacy = linux_instance_build_paths(nullptr);
    if (strcmp(legacy.socketPath, "/run/greencurve/greencurve.sock") != 0) return 7808;
    if (strcmp(legacy.active, "/var/lib/greencurve/active.bin") != 0) return 7809;
    if (strcmp(first.socketPath, second.socketPath) == 0 ||
        strcmp(first.active, second.active) == 0 ||
        strcmp(first.operation, second.operation) == 0 ||
        strcmp(first.startup, second.startup) == 0 ||
        strcmp(first.guard, second.guard) == 0 ||
        strcmp(first.fan, second.fan) == 0 ||
        strcmp(first.unitName, second.unitName) == 0 ||
        strcmp(first.resumeName, second.resumeName) == 0) return 7810;
    if (strlen(first.socketPath) >= 108 || !strstr(first.socketPath, UUID_A)) return 7811;
    if (linux_instance_configure("GPU-../../another-instance")) return 7812;
    if (!linux_instance_configure(UUID_A)) return 7813;
    if (strcmp(linux_instance().paths.active, first.active) != 0) return 7814;
    if (linux_instance_configure("GPU-01234567-89ab-cdef-0123-456789abcdef/other")) return 7815;
    if (strcmp(linux_instance().uuid.text, UUID_A) != 0) return 7816;
    return 0;
}

#if !defined(_WIN32)
struct OwnerFixture {
    char directory[256] = {};
    int dir = -1, a = -1, b = -1;
    ~OwnerFixture() {
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        if (dir >= 0) {
            unlinkat(dir, "GPU-01234567-89ab-cdef-0123-456789abcdef.lock", 0);
            unlinkat(dir, "GPU-11234567-89ab-cdef-0123-456789abcdef.lock", 0);
            unlinkat(dir, "pci-0000:01:00.0.lock", 0);
            unlinkat(dir, "created-directory", AT_REMOVEDIR);
            close(dir);
        }
        if (directory[0]) rmdir(directory);
    }
};

int ownership_tests() {
    OwnerFixture f;
    if (!gc_test_make_temp_dir(f.directory, sizeof(f.directory), "gc-instance-owner", 60)) return 7817;
    f.dir = open(f.directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    auto a = linux_gpu_uuid_parse(UUID_A), b = linux_gpu_uuid_parse(UUID_B);
    f.a = linux_instance_owner_open_at(f.dir, &a, geteuid());
    if (f.a < 0) return 7818;
    int duplicate = linux_instance_owner_open_at(f.dir, &a, geteuid());
    if (duplicate >= 0) { close(duplicate); return 7819; }
    f.b = linux_instance_owner_open_at(f.dir, &b, geteuid());
    if (f.b < 0) return 7820;
    close(f.a); f.a = -1;
    f.a = linux_instance_owner_open_at(f.dir, &a, geteuid());
    if (f.a < 0) return 7821;
    close(f.b); f.b = -1;
    if (unlinkat(f.dir, "GPU-11234567-89ab-cdef-0123-456789abcdef.lock", 0) != 0 ||
        symlinkat("GPU-01234567-89ab-cdef-0123-456789abcdef.lock", f.dir,
            "GPU-11234567-89ab-cdef-0123-456789abcdef.lock") != 0) return 7822;
    int alias = linux_instance_owner_open_at(f.dir, &b, geteuid());
    if (alias >= 0) { close(alias); return 7823; }
    int pci = linux_instance_owner_named_at(f.dir, "pci-0000:01:00.0.lock", geteuid());
    if (pci < 0) return 7845;
    int pciAlias = linux_instance_owner_named_at(f.dir, "pci-0000:01:00.0.lock", geteuid());
    close(pci);
    if (pciAlias >= 0) { close(pciAlias); return 7846; }
    if (linux_instance_owner_named_at(f.dir, "../another-GPU", geteuid()) >= 0) return 7847;
    char created[512] = {};
    snprintf(created, sizeof(created), "%s/created-directory", f.directory);
    mode_t originalMask = umask(0077);
    bool prepared = linux_instance_prepare_directory_for_owner(created, geteuid(), true);
    umask(originalMask);
    struct stat st = {};
    if (!prepared || stat(created, &st) != 0 || (st.st_mode & 0777) != 0755) return 7848;
    if (chmod(created, 0700) != 0 ||
        !linux_instance_prepare_directory_for_owner(created, geteuid(), false)) return 7849;
    if (linux_instance_prepare_directory_for_owner(created, geteuid(), true)) return 7850;
    return 0;
}
#endif
}

int run_linux_instance_tests() {
    const LinuxInstanceContext before = linux_instance();
    int result = selection_tests();
    if (!result) result = path_tests();
    linux_instance() = before;
#if !defined(_WIN32)
    if (!result) result = ownership_tests();
#endif
    return result;
}
