# Linux multi-GPU runtime

One daemon/socket manages independent physical GPU runtimes. Selecting a card
selects its runtime; it never repurposes another card's active backend.

## Source anchors

- `source/linux_multi_gpu_runtime.h`: complete per-GPU state and exact routing.
- `source/linux_multi_gpu_context.h`: thread-local runtime scope and immutable
  owner validation shared by user applies, automatic restore and fan handback.
- `source/linux_multi_gpu_registry.h`: discovery, lifetime, fan workers, resume
  aggregation and exclusive service ownership.
- `source/linux_multi_gpu_migration.h`, `source/linux_multi_gpu_boot.h`:
  validated singleton migration and existing per-GPU startup orchestration.
- `source/linux_gpu_profile_scope.h`, `source/linux_port_profiles.cpp`:
  per-card user banks and shared-bank upgrade templates.

## Invariants

- Backend, desired state, revisions/generation, operations, uncertainty,
  startup/restore guards and fan ownership belong to one fixed GPU context.
- TUI automatic adoption and explicit switching do not depend on another
  card's active intent. Dirty drafts and genuine target refusals retain their
  existing guards; switching never resets a card merely to select another.
- Requests route before generation/topology validation. Each context has a
  distinct generation range; a frame from another card cannot authorize it.
- Runtime paths use a UUID prefix when available and an exact PCI prefix
  otherwise. No two contexts may own the same known UUID, including after a
  location move. A running-session move requires a restart.
- Each fan worker has a fixed thread-local context and mutex. An apply holds
  only its target's mutex, so another GPU's fan worker can continue. Backend
  transition-ceiling scratch is thread-local rather than shared across calls.
- Resume visits every context, records individual failures and continues to
  the others. It is a global notification and never acquires the selected
  context's mutex before aggregate dispatch.
- Driver initialization references are balanced even when initialization or
  later enumeration fails. One failed context cannot release another's NVML
  initialization reference.
- Singleton records migrate only to their verified owner. Spent automatic
  restore budgets and lockouts follow both resolved old restore targets before
  any automatic write. Existing per-GPU records win over old records; missing
  owners leave legacy sources intact. An ownerless same-boot fan marker is a
  reboot-required migration barrier regardless of visible GPU count (including
  zero or one); it is never guessed onto the first visible board. Corrupt
  ownership or unreadable boot identity fails closed. A verified different boot
  retires only that old marker, preserving newer per-GPU ownership.
- Late discovery prepares all new contexts, completes ownership migration,
  then starts their fan workers. Failed migration removes the unroutable new
  contexts and retains its source records; missing driver readiness remains
  retryable. A worker creation failure stops partial service.
- Read-time discovery does not replay stored tuning. Inherited intent on a
  late-discovered GPU remains uncertain until explicit Apply/Reset or restart.
- Default user banks are UUID-scoped, with PCI fallback. Shared legacy banks
  are read templates; saves/clears only write the selected bank. Explicit
  `--config` retains shared-bank behavior. Failed reads do not change the
  client's policy/profile target; changed service/generation invalidates its
  cached physical identity.

## Verification and diagnostics

`tests/linux_multi_gpu_regression.cpp` executes the production request handler
against fake driver and journal dependencies: independent applies, rollback,
operation deduplication, stale frames, policy routing, fan-context isolation,
single/multi-GPU resume, unavailable targets and duplicate physical owners.
`tests/linux_gpu_profile_scope_regression.cpp` exercises real profile INI
reads/writes/clears and shared template preservation. The production backend
lifetime fixture is `tests/linux_nvml_lifetime_tests.cpp`.

Journal prefixes `daemon GPU runtime`, `daemon discovery` and `daemon migration`
identify runtime creation, late discovery and legacy-record transfer. UUID
values are not included in these diagnostics.

## Open questions / limits

Hardware VF writes and sustained multi-GPU fan maintenance have not been
exercised by these fixtures. The service still handles IPC requests serially;
a long apply can delay another client's reply, but not its GPU's independent
fan worker. Driver-wide failures can still affect multiple boards. No atomic
all-card apply is promised. PCI fallback cannot distinguish identical boards
exchanged at the same location when UUID readback is unavailable.

Last verified: 2026-11-06; executing fake-driver and filesystem fixtures. Full
build/test results belong in the task's verification report.
