// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Production discovery, transaction phases and fan/rollback entry points run
// against two counted fake cards. No driver library is loaded.
#if !defined(_WIN32)
#include "platform.h"
#include <cstdio>
#include <cstring>

static PlLib instance_fixture_open(int);
static void* instance_fixture_symbol(PlLib, const char*);
static void instance_fixture_close(PlLib);
#define linux_backend_init instance_fixture_init
#define linux_backend_shutdown instance_fixture_shutdown
#define linux_backend_select_target instance_fixture_select_target
#define linux_backend_capture_snapshot instance_fixture_capture_snapshot
#define linux_backend_restore_snapshot instance_fixture_restore_snapshot
#define linux_backend_refresh instance_fixture_refresh
#define linux_backend_curve_plausible instance_fixture_curve_plausible
#define linux_backend_self_test instance_fixture_self_test
#define linux_backend_clk_domain_dump instance_fixture_clk_domain_dump
#define linux_backend_apply instance_fixture_apply
#define linux_backend_reset instance_fixture_reset
#define linux_backend_set_curve_fan_percent instance_fixture_set_curve_fan_percent
#define linux_backend_set_fan_auto instance_fixture_set_fan_auto
#define linux_backend_fixed_fan_check instance_fixture_fixed_fan_check
#define linux_backend_fans_are_auto instance_fixture_fans_are_auto
#define linux_platform_is_integrated_soc instance_fixture_integrated_soc
#include "linux_backend.h"
#define pl_open_driver_library instance_fixture_open
#define pl_lib_sym instance_fixture_symbol
#define pl_lib_close instance_fixture_close
#include "../source/linux_backend.cpp"
#undef pl_lib_close
#undef pl_lib_sym
#undef pl_open_driver_library
#undef linux_platform_is_integrated_soc
#undef linux_backend_fans_are_auto
#undef linux_backend_fixed_fan_check
#undef linux_backend_set_fan_auto
#undef linux_backend_set_curve_fan_percent
#undef linux_backend_reset
#undef linux_backend_apply
#undef linux_backend_clk_domain_dump
#undef linux_backend_self_test
#undef linux_backend_curve_plausible
#undef linux_backend_refresh
#undef linux_backend_restore_snapshot
#undef linux_backend_capture_snapshot
#undef linux_backend_select_target
#undef linux_backend_shutdown
#undef linux_backend_init

namespace {
const char* const IDS[] = {"GPU-01234567-89ab-cdef-0123-456789abcdef",
                         "GPU-11234567-89ab-cdef-0123-456789abcdef"};
struct Board {
    const char* uuid;
    unsigned int bus, setters, power, fan, policy;
    int offset;
    bool readable;
};
static Board boards[2];
static Board* board(nvmlDevice_t device) {
    uintptr_t index = reinterpret_cast<uintptr_t>(device);
    return index >= 1 && index <= 2 ? &boards[index - 1] : nullptr;
}
static unsigned int setter_count() { return boards[0].setters + boards[1].setters; }
static nvmlReturn_t init_library() { return NVML_SUCCESS; }
static nvmlReturn_t shutdown_library() { return NVML_SUCCESS; }
static nvmlReturn_t count_devices(unsigned int* n) { *n = 2; return NVML_SUCCESS; }
static nvmlReturn_t get_handle(unsigned int index, nvmlDevice_t* handle) {
    if (index >= 2) return NVML_ERROR_NOT_FOUND;
    *handle = reinterpret_cast<nvmlDevice_t>((uintptr_t)index + 1);
    return NVML_SUCCESS;
}
static nvmlReturn_t get_uuid(nvmlDevice_t handle, char* value, unsigned int size) {
    Board* b = board(handle);
    if (!b || !b->readable) return NVML_ERROR_UNKNOWN;
    snprintf(value, size, "%s", b->uuid);
    return NVML_SUCCESS;
}
static nvmlReturn_t get_pci(nvmlDevice_t handle, nvmlPciInfo_t* pci) {
    Board* b = board(handle);
    if (!b) return NVML_ERROR_NOT_FOUND;
    *pci = {};
    pci->bus = b->bus;
    pci->pciDeviceId = 0x1e0410deu;
    pci->pciSubSystemId = 0x123410deu;
    snprintf(pci->busId, sizeof(pci->busId), "0000:%02x:00.0", b->bus);
    snprintf(pci->busIdLegacy, sizeof(pci->busIdLegacy), "0000:%02x:00.0", b->bus);
    return NVML_SUCCESS;
}
static nvmlReturn_t get_offset(nvmlDevice_t handle, int* value) {
    *value = board(handle)->offset; return NVML_SUCCESS;
}
static nvmlReturn_t set_offset(nvmlDevice_t handle, int value) {
    Board* b = board(handle); ++b->setters; b->offset = value; return NVML_SUCCESS;
}
static nvmlReturn_t get_power(nvmlDevice_t handle, unsigned int* value) {
    *value = board(handle)->power; return NVML_SUCCESS;
}
static nvmlReturn_t get_default_power(nvmlDevice_t, unsigned int* value) {
    *value = 250000; return NVML_SUCCESS;
}
static nvmlReturn_t set_power(nvmlDevice_t handle, unsigned int value) {
    Board* b = board(handle); ++b->setters; b->power = value; return NVML_SUCCESS;
}
static nvmlReturn_t get_num_fans(nvmlDevice_t, unsigned int* value) {
    *value = 1; return NVML_SUCCESS;
}
static nvmlReturn_t get_fan(nvmlDevice_t handle, unsigned int, unsigned int* value) {
    *value = board(handle)->fan; return NVML_SUCCESS;
}
static nvmlReturn_t get_policy(nvmlDevice_t handle, unsigned int, unsigned int* value) {
    *value = board(handle)->policy; return NVML_SUCCESS;
}
static nvmlReturn_t set_policy(nvmlDevice_t handle, unsigned int, unsigned int value) {
    Board* b = board(handle); ++b->setters; b->policy = value; return NVML_SUCCESS;
}
static nvmlReturn_t set_fan(nvmlDevice_t handle, unsigned int, unsigned int value) {
    Board* b = board(handle); ++b->setters; b->fan = value; return NVML_SUCCESS;
}
static nvmlReturn_t set_fan_auto(nvmlDevice_t handle, unsigned int) {
    Board* b = board(handle); ++b->setters;
    b->policy = NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW; return NVML_SUCCESS;
}

struct ContextFixture {
    LinuxInstanceContext previous;
    LinuxGpuState a = {}, b = {}, refused = {};
    ContextFixture() : previous(linux_instance()) { linux_instance().daemon = false; }
    ~ContextFixture() {
        instance_fixture_shutdown(&a);
        instance_fixture_shutdown(&b);
        instance_fixture_shutdown(&refused);
        linux_instance() = previous;
    }
};
}

static PlLib instance_fixture_open(int kind) {
    return kind == PL_DRIVER_NVML ? reinterpret_cast<PlLib>(1) : nullptr;
}
static void* instance_fixture_symbol(PlLib, const char* name) {
#define SYMBOL(text, function) if (strcmp(name, text) == 0) return reinterpret_cast<void*>(function)
    SYMBOL("nvmlInit_v2", init_library);
    SYMBOL("nvmlShutdown", shutdown_library);
    SYMBOL("nvmlDeviceGetCount_v2", count_devices);
    SYMBOL("nvmlDeviceGetHandleByIndex_v2", get_handle);
    SYMBOL("nvmlDeviceGetUUID", get_uuid);
    SYMBOL("nvmlDeviceGetPciInfo_v3", get_pci);
    SYMBOL("nvmlDeviceGetGpcClkVfOffset", get_offset);
    SYMBOL("nvmlDeviceSetGpcClkVfOffset", set_offset);
    SYMBOL("nvmlDeviceGetPowerManagementLimit", get_power);
    SYMBOL("nvmlDeviceGetPowerManagementDefaultLimit", get_default_power);
    SYMBOL("nvmlDeviceSetPowerManagementLimit", set_power);
    SYMBOL("nvmlDeviceGetNumFans", get_num_fans);
    SYMBOL("nvmlDeviceGetFanSpeed_v2", get_fan);
    SYMBOL("nvmlDeviceGetTargetFanSpeed", get_fan);
    SYMBOL("nvmlDeviceGetFanControlPolicy_v2", get_policy);
    SYMBOL("nvmlDeviceSetFanControlPolicy", set_policy);
    SYMBOL("nvmlDeviceSetFanSpeed_v2", set_fan);
    SYMBOL("nvmlDeviceSetDefaultFanSpeed_v2", set_fan_auto);
#undef SYMBOL
    return nullptr;
}
static void instance_fixture_close(PlLib) {}
bool instance_fixture_integrated_soc() { return false; }

int run_linux_instance_backend_tests() {
    g_lbDebug = false;
    boards[0] = {IDS[0], 1, 0, 250000, 40, NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW, 0, true};
    boards[1] = {IDS[1], 0x41, 0, 250000, 40, NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW, 0, true};
    ContextFixture f;
    char err[256] = {};
    if (!linux_instance_configure(IDS[0]) ||
        !instance_fixture_init(&f.a, nullptr, err, sizeof(err)) ||
        f.a.nvmlDevice != reinterpret_cast<nvmlDevice_t>(1) || !f.a.writeIdentityResolved) return 7830;
    const GpuAdapterInfo original = f.a.selectedGpu;
    boards[0].bus = 7;
    const LinuxGpuUuid idA = linux_gpu_uuid_parse(IDS[0]);
    if (!instance_fixture_init(&f.refused, &original, err, sizeof(err),
            GPU_LOCATION_ROLE_REQUIRED, &idA) || f.refused.selectedGpu.pciBus != 7) return 7831;
    instance_fixture_shutdown(&f.refused);
    if (!linux_instance_configure(IDS[1]) ||
        !instance_fixture_init(&f.b, nullptr, err, sizeof(err)) ||
        f.b.nvmlDevice != reinterpret_cast<nvmlDevice_t>(2) || !f.b.writeIdentityResolved) return 7832;
    if (instance_fixture_init(&f.refused, &f.a.selectedGpu, err, sizeof(err))) return 7833;
    instance_fixture_shutdown(&f.refused);
    if (!linux_instance_configure(IDS[0])) return 7834;

    DesiredSettings desired = {};
    desired.hasGpuOffset = true; desired.gpuOffsetMHz = 120;
    LinuxHardwareSnapshot snapshot = {};
    snapshot.valid = true; snapshot.powerValid = true; snapshot.powerLimitmW = 200000;
    LinuxApplyTransactionContext apply = {};
    apply.gpu = &f.a; apply.desired = &desired; apply.snapshot = &snapshot;
    LinuxResetTransactionContext reset = {};
    reset.gpu = &f.a;
    if (!linux_apply_transaction_step(&apply, LINUX_MUTATION_GPU_OFFSET) ||
        boards[0].offset != 120 || boards[1].setters != 0) return 7835;
    if (!linux_reset_transaction_step(&reset, LINUX_MUTATION_GPU_OFFSET) ||
        boards[0].offset != 0 || boards[1].setters != 0) return 7836;
    if (!instance_fixture_restore_snapshot(&f.a, &snapshot, LINUX_MUTATION_POWER, err, sizeof(err)) ||
        boards[0].power != 200000 || boards[1].setters != 0) return 7837;
    if (!instance_fixture_set_curve_fan_percent(&f.a, 55) ||
        !instance_fixture_set_fan_auto(&f.a) || boards[1].setters != 0) return 7838;

    const unsigned int phases[] = {LINUX_MUTATION_LOCK_CEILING, LINUX_MUTATION_RESET_BASELINE,
        LINUX_MUTATION_GPU_OFFSET, LINUX_MUTATION_MEM_OFFSET, LINUX_MUTATION_POWER,
        LINUX_MUTATION_CURVE, LINUX_MUTATION_LOCK, LINUX_MUTATION_FAN,
        LINUX_MUTATION_XBAR, LINUX_MUTATION_SYS_CLK, LINUX_MUTATION_VIDEO_CLK};
    for (unsigned int failure = 0; failure < 2; ++failure) {
        boards[0].uuid = failure == 0 ? IDS[1] : IDS[0];
        boards[0].readable = failure == 0;
        const unsigned int before = setter_count();
        for (unsigned int phase : phases) {
            if (linux_apply_transaction_step(&apply, phase)) return 7839;
            if (linux_reset_transaction_step(&reset, phase)) return 7840;
        }
        if (instance_fixture_restore_snapshot(&f.a, &snapshot, LINUX_MUTATION_POWER, err, sizeof(err)) ||
            instance_fixture_set_curve_fan_percent(&f.a, 60) ||
            instance_fixture_set_fan_auto(&f.a)) return 7841;
        if (setter_count() != before) return 7842;
    }
    boards[0].uuid = IDS[0]; boards[0].readable = true;
    if (!linux_instance_configure(IDS[1])) return 7843;
    apply.gpu = &f.b; desired.gpuOffsetMHz = 90;
    const unsigned int aWrites = boards[0].setters;
    if (!linux_apply_transaction_step(&apply, LINUX_MUTATION_GPU_OFFSET) ||
        boards[1].offset != 90 || boards[0].setters != aWrites) return 7844;
    return 0;
}
#else
int run_linux_instance_backend_tests() { return 0; }
#endif
