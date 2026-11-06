// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_instance_cli.h"
#include "linux_instance_units.h"
#include "linux_instance_remove.h"
#include "posix_test_environment.h"
#include <stdio.h>
#include <string>
#include <vector>

void linux_debug_logf(const char*, ...) {}

static std::string read_text(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return {};
    char buffer[16384] = {};
    size_t size = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    return std::string(buffer, size);
}

static bool parse(const char* id, LinuxCliOptions* options, bool conflict = false) {
    const char* args[] = {"greencurve", "--instance", id, "--tui", "--gpu", "0000:01:00.0"};
    return parse_linux_cli_options(conflict ? 6 : 4, const_cast<char**>(args), options);
}

static std::string render_unit(bool resume) {
    FILE* file = tmpfile();
    if (!file) return {};
    int written = resume ? linux_instance_render_resume_unit(file, "/usr/local/libexec/greencurve/greencurve")
        : linux_instance_render_daemon_unit(file, "/usr/local/libexec/greencurve/greencurve");
    rewind(file);
    char text[8192] = {};
    size_t length = fread(text, 1, sizeof(text), file);
    fclose(file);
    return written > 0 ? std::string(text, length) : std::string();
}

static std::vector<std::string> removalCalls;
static int commandCount, fileCount, failCommand, failFile;

static int remove_command(char* const* argv) {
    std::string call = "command:";
    for (unsigned int i = 0; argv[i]; ++i) {
        if (i) call += " ";
        call += argv[i];
    }
    removalCalls.push_back(call);
    return commandCount++ == failCommand ? 17 : 0;
}

static int remove_file(const char* path) {
    removalCalls.push_back(std::string("unlink:") + path);
    if (fileCount++ == failFile) { errno = EACCES; return -1; }
    return 0;
}

static void reset_removal(int commandFailure = -1, int fileFailure = -1) {
    removalCalls.clear(); commandCount = 0; fileCount = 0;
    failCommand = commandFailure; failFile = fileFailure;
}

static int removal_tests(const char* id) {
    auto uuid = linux_gpu_uuid_parse(id);
    auto paths = linux_instance_build_paths(&uuid);
    char error[256] = {};
    for (int failed = 0; failed < 2; ++failed) {
        reset_removal(failed);
        if (linux_instance_remove_units(paths, remove_command, remove_file, error, sizeof(error)) == 0 ||
            fileCount != 0 || commandCount != failed + 1 || !strstr(error, "saved settings kept")) return 12;
    }
    reset_removal();
    if (linux_instance_remove_units(paths, remove_command, remove_file, error, sizeof(error)) != 0 ||
        removalCalls.size() != 6 ||
        removalCalls[0] != std::string("command:systemctl disable --now ") + paths.resumeName ||
        removalCalls[1] != std::string("command:systemctl disable --now ") + paths.unitName ||
        removalCalls[2] != std::string("unlink:") + paths.resumePath ||
        removalCalls[3] != std::string("unlink:") + paths.unitPath ||
        removalCalls[4] != "command:systemctl daemon-reload") return 13;
    for (int failed = 0; failed < 2; ++failed) {
        reset_removal(-1, failed);
        if (linux_instance_remove_units(paths, remove_command, remove_file, error, sizeof(error)) == 0 ||
            commandCount != 2 || fileCount != failed + 1 || !strstr(error, "saved settings kept")) return 14;
    }
    reset_removal(2);
    if (linux_instance_remove_units(paths, remove_command, remove_file, error, sizeof(error)) == 0 ||
        commandCount != 3 || !strstr(error, "saved settings kept")) return 15;
    return 0;
}

int main() {
    const char* ids[] = {"GPU-01234567-89ab-cdef-0123-456789abcdef",
                         "GPU-11234567-89ab-cdef-0123-456789abcdef"};
    LinuxCliOptions options = {};
    if (int removal = removal_tests(ids[0])) return removal;
    std::string legacy = render_unit(false);
    if (legacy.find("RuntimeDirectory=greencurve\n") == std::string::npos ||
        legacy.find(" --instance ") != std::string::npos) return 10;
    if (parse("GPU-../../other", &options) || parse(ids[0], &options, true)) return 1;
    char temporary[256] = {};
    if (!gc_test_make_temp_dir(temporary, sizeof(temporary), "gc-instance-cli", 80)) return 2;
    int result = 0;
    std::string firstConfig;
    for (unsigned int i = 0; i < 2 && !result; ++i) {
        if (!parse(ids[i], &options)) { result = 3; break; }
        char config[4096] = {};
        if (!linux_instance_prepare_options(options, config, sizeof(config)) ||
            !strstr(config, ids[i])) { result = 4; break; }
        if (i == 0) firstConfig = config;
        else if (firstConfig == config) { result = 5; break; }
        std::string daemon = render_unit(false), resume = render_unit(true);
        if (daemon.find(std::string("--daemon --instance ") + ids[i]) == std::string::npos ||
            daemon.find(std::string("RuntimeDirectory=greencurve/") + ids[i]) == std::string::npos ||
            daemon.find(std::string("StateDirectory=greencurve/") + ids[i]) == std::string::npos ||
            daemon.find("RuntimeDirectoryPreserve=yes") == std::string::npos ||
            resume.find(std::string("--resume-restore --instance ") + ids[i]) == std::string::npos ||
            resume.find(linux_instance().paths.socketPath) == std::string::npos ||
            resume.find(linux_instance().paths.unitName) == std::string::npos ||
            daemon.find(ids[1 - i]) != std::string::npos || resume.find(ids[1 - i]) != std::string::npos) {
            result = 11; break;
        }
        std::string dir = std::string(temporary) + "/" + (i == 0 ? "first" : "second");
        char error[256] = {};
        if (!write_linux_assets(dir.c_str(), "/opt/green curve/greencurve", config, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error); result = 6; break;
        }
        const char* names[] = {"greencurve.desktop", "greencurve-autostart.desktop",
                               "greencurve-apply.service", "README.md"};
        for (const char* name : names) {
            std::string file = dir + "/" + name;
            std::string text = read_text(file);
            if (text.find(std::string("--instance ") + ids[i]) == std::string::npos) result = 7;
            if (text.find(ids[1 - i]) != std::string::npos) result = 8;
            if (strstr(name, ".service") && text.find(linux_instance().paths.unitName) == std::string::npos)
                result = 9;
            unlink(file.c_str());
        }
        rmdir(dir.c_str());
    }
    rmdir(temporary);
    if (result) fprintf(stderr, "instance CLI/asset regression failed: %d\n", result);
    else puts("Instance CLI routing and asset fixtures passed");
    return result;
}
