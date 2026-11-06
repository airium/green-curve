# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Desktop cleanup contract; native Linux also executes the shell helper."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile


def run_self_tests(root):
    source = (Path(root) / "tools" / "greencurve-setup.sh").read_text(encoding="utf-8")
    match = re.search(r"^remove_user_desktop_entry\(\) \{\n.*?^\}\n", source, re.M | re.S)
    if not match or 'remove_user_desktop_entry "$user" "$home"' not in source:
        raise RuntimeError("Linux desktop cleanup helper is not wired")
    helper = match.group()
    if 'runuser -u "$user" -- rm -f --' not in helper or re.search(r"^\s*rm\b", helper, re.M):
        raise RuntimeError("Linux desktop cleanup must use the account's privileges")
    if not sys.platform.startswith("linux"):
        print("Linux desktop cleanup: source checks passed; shell execution requires Linux")
        return
    bash = shutil.which("bash")
    if not bash:
        raise RuntimeError("Bash is required to verify the shipped Linux setup helper")
    with tempfile.TemporaryDirectory(prefix="gc-desktop-cleanup-") as temporary:
        directory = Path(temporary)
        home = directory / "home with spaces"
        for scenario in ("success", "failure", "unavailable"):
            captured = directory / "arguments"
            root_rm = directory / "root-rm"
            captured.unlink(missing_ok=True)
            root_rm.unlink(missing_ok=True)
            environment = dict(os.environ, PATH="", CAPTURE=str(captured), ROOT_RM=str(root_rm))
            environment.pop("BASH_ENV", None)
            mock = '' if scenario == "unavailable" else (
                'runuser() { printf "%s\\0" "$@" > "$CAPTURE"; return ' +
                ('1' if scenario == "failure" else '0') + '; }\n')
            harness = ('warn() { printf "%s\\n" "$*"; }\n'
                       'info() { printf "%s\\n" "$*"; }\n'
                       'rm() { printf root > "$ROOT_RM"; return 1; }\n' + mock +
                       helper + '\nremove_user_desktop_entry "$1" "$2"\n')
            result = subprocess.run([bash, "--noprofile", "--norc", "-c", harness,
                                     "fixture", "fixture-user", str(home)],
                                    env=environment, capture_output=True, text=True, timeout=10)
            if result.returncode or root_rm.exists():
                raise RuntimeError(f"Linux cleanup used a root fallback: {scenario}")
            if scenario != "unavailable":
                expected = ["-u", "fixture-user", "--", "rm", "-f", "--",
                            str(home / ".local/share/applications/greencurve.desktop")]
                if captured.read_bytes().split(b"\0")[:-1] != [x.encode() for x in expected]:
                    raise RuntimeError("Linux cleanup arguments were not preserved")
            elif captured.exists() or "unavailable" not in result.stdout:
                raise RuntimeError("Linux cleanup did not report an unavailable user switch")
    print("Linux desktop cleanup shell fixtures passed")
    run_instance_setup_tests(source, bash)


def run_instance_setup_tests(source, bash):
    """Execute the shipped uninstall function with side effects intercepted."""
    match = re.search(r"^cmd_uninstall\(\) \{\n.*?^\}\n", source, re.M | re.S)
    if not match:
        raise RuntimeError("Linux instance uninstall function is missing")
    with tempfile.TemporaryDirectory(prefix="gc-instance-setup-") as temporary:
        directory = Path(temporary)
        state = directory / "state"
        state.mkdir()
        capture = directory / "calls"
        environment = dict(os.environ, CAPTURE=str(capture), INSTANCE_STATE=str(state))
        environment.pop("BASH_ENV", None)
        harness = r'''
require_root() { :; }
require_binary() { :; }
say() { :; }
info() { :; }
warn() { :; }
die() { exit 23; }
fixture_binary() { printf 'binary:%s\n' "$*" >> "$CAPTURE"; return "${BINARY_RESULT:-0}"; }
rm() { printf 'rm:%s\n' "$*" >> "$CAPTURE"; }
systemctl() { printf 'systemctl:%s\n' "$*" >> "$CAPTURE"; }
groupdel() { printf 'groupdel:%s\n' "$*" >> "$CAPTURE"; }
compgen() { return 0; }
target_user() { printf fixture-user; }
BINARY=fixture_binary
INSTANCE=GPU-01234567-89ab-cdef-0123-456789abcdef
INSTANCE_ARGS=(--instance "$INSTANCE")
STATE_DIR=$INSTANCE_STATE
PURGE=1
'''
        command = harness + match.group() + '\ncmd_uninstall\n'
        result = subprocess.run([bash, "--noprofile", "--norc", "-c", command],
                                env=environment, capture_output=True, text=True, timeout=10)
        expected = ["binary:--instance GPU-01234567-89ab-cdef-0123-456789abcdef --service-remove",
                    f"rm:-rf -- {state}"]
        if result.returncode or capture.read_text().splitlines() != expected:
            raise RuntimeError("instance uninstall touched shared service files or failed to forward its identity")
        capture.unlink()
        failed_environment = dict(environment, BINARY_RESULT="7")
        result = subprocess.run([bash, "--noprofile", "--norc", "-c", command],
                                env=failed_environment, capture_output=True, text=True, timeout=10)
        if result.returncode != 23 or capture.read_text().splitlines() != expected[:1]:
            raise RuntimeError("instance uninstall purged state or succeeded after binary removal failed")
        capture.unlink()
        command = harness + '\nINSTANCE=""\nINSTANCE_ARGS=()\n' + match.group() + '\ncmd_uninstall\n'
        result = subprocess.run([bash, "--noprofile", "--norc", "-c", command],
                                env=environment, capture_output=True, text=True, timeout=10)
        if result.returncode != 23 or capture.exists():
            raise RuntimeError("singleton uninstall did not refuse before touching named instances")
    print("Linux instance setup lifecycle shell fixtures passed")
