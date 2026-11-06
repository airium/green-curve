# GPU-bound Linux daemon instances

## Summary

`--instance GPU-UUID` selects one immutable physical GPU and independent socket,
state and user-profile namespaces. No selector retains the existing singleton.
This branch is the per-process alternative to a multi-GPU runtime in one daemon.

## Source anchors

- `source/linux_instance.h`: strict UUID selection, path construction and restore identity policy.
- `source/linux_instance_cli.h`: process context and default per-instance user configuration.
- `source/linux_instance_ownership.h`: protected directories and shared physical-card ownership locks.
- `source/linux_backend_discovery.cpp`, `source/linux_backend.cpp`: immutable selection and fresh mutation identity.
- `source/linux_backend_mutation.cpp`, `source/linux_backend_rollback.h`: instance checks at write and recovery boundaries.
- `source/linux_daemon_snapshot_runtime.cpp`: request target admission.
- `source/linux_instance_units.h`, `source/linux_service_install.cpp`: instance-aware main/resume rendering and activation.
- `source/linux_assets.cpp`, `tools/greencurve-setup.sh`: client launcher and lifecycle forwarding.
- `tests/linux_instance_tests.cpp`, `tests/linux_instance_cli_regression.cpp`, `tools/linux_setup_tests.py`: executing regression coverage.

## Invariants

- The daemon binds by UUID before any automatic restore or fan handback. A missing,
  unreadable, ambiguous or different identity never becomes another GPU.
- Trusted saved UUIDs may follow their physical card across PCI bus renumbering;
  explicit requests for another live location are refused.
- A fresh physical identity check precedes every apply/reset phase, rollback and
  runtime fan write. Failed identity reads never weaken an instance binding.
- Active/startup/operation/guard/fan records retain their existing checksummed
  formats but live under the UUID-specific state directory. Logs and crash reports
  use that same namespace. User configuration defaults to an `instances/UUID` child.
- Ownership locks use a common root-owned `owners` directory, not instance state.
  UUID and PCI aliases prevent updated singleton and instance daemons from owning
  the same card even when an older singleton GPU cannot report a UUID.
- Main and resume units carry the same immutable selector. Runtime directories
  are preserved so stopping a singleton cannot remove another instance's socket
  or ownership namespace.
- Instance removal does not remove the shared executable, command link or group.
  Both main and resume writers must stop successfully before unit files are
  removed; removal failure aborts wrapper cleanup and preserves saved settings.
  Default removal refuses while named instance unit files remain installed.
- The TUI remains bound to its instance; its selector is disabled. Use separate
  terminals for different GPUs. Explicit config paths are administrator/user
  overrides and should not be shared between cards.

## Diagnostics and verification

Instance/target mismatches and ownership refusal are explicit startup or request
errors. Read-only self-tests remain available with an instance selector. Tests
exercise two namespaces, UUID renumbering/refusal, fresh identity failures, real
duplicate-owner locks and symlink refusal, restrictive-umask directory creation,
real CLI parsing, generated launchers and unit bodies, intercepted native removal
commands and unlink ordering, and setup cleanup refusal after failed removal.
A production-backend fake-driver fixture also executes init/select and all
apply/reset phases, rollback and fan writes against two independently counted cards;
wrong or unreadable physical identity refuses before every setter.

## Limits

No automatic conversion of singleton active state into per-GPU records is attempted.
Stop the previous owner before installing the corresponding instance. A previously
installed, unmodified daemon cannot participate in the new ownership locking and
must be stopped during migration. Process separation does not isolate kernel-driver
failures. Real multi-GPU tuning, service installation and resume writes remain
hardware-unverified; software tests do not establish undervolt stability.

Last verified: 2026-11-06. Full hardware-free `python build.py --test` and plain
`python build.py` passed. The Linux-host matrix produced eight fresh release
packages with matching SHA-256 sidecars; both Linux archives contain exactly the
newly built executables. Service installation, resume and physical writes remain
unverified.
