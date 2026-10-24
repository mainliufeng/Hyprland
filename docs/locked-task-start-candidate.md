# Direct locked task start candidate

`locked-task-start-v1` supports Cornice's existing direct Agent desktop allocation
model during a stable human lock. There is no new user permission/preauthorization
step or default-off switch. Standard full-session lock still refuses all tasks.

Internal trusted Broker commands bind the seat socket lifecycle and generation:

* `seat task-initialize NAME SOCKET GENERATION LOCK_EPOCH WORKSPACE`: initialize
  only a fresh unmanaged generation-one private seat, on its own output. It
  records idle state without granting input.
* `seat locked-task-start NAME SOCKET GENERATION LOCK_EPOCH EXPORT_GRANT`: consume
  fresh/completed idle state only under presented secure human protection with a
  connected owner, rotate devices/export authority and bind the original private
  output object and exact lock epoch.
* `seat task-idle NAME SOCKET GENERATION`: successful normal completion; rotate
  devices/grants and record idle. Pause/takeover/loss/restart are interruptions.
* `seat create-private-output NAME LOCK_EPOCH` and
  `seat create NAME OUTPUT LOCK_EPOCH`: require the same stable human epoch.
  Privacy is registered before output announcement; fresh initialization cannot
  select a workspace on a human output. Initial seat configuration is allowed
  only once under a valid new lease; locked reconfiguration remains disabled.

Human lock excludes managed private headless output objects, including newly
created ones; unknown and physical outputs are always protected. Full session
lock excludes none. Tasks started before lock keep the original continuity
policy. Lock-created leases deny access immediately on epoch/scope/owner/output
changes and cannot be revived by restoring an old binding.

These are trusted same-UID management IPC, not Agent tool credentials or an OS
sandbox. Cornice serializes ownership and exposes existing acquire/finish tools.
Seat snapshots expose `taskIdle` and `lockedTaskActive`; lease grants are never
argv/log/configuration data.

Integration tests live in the paired Cornice repository:
`test/desktop-locked-start-verify.py`, under the bounded private compositor
wrapper. No installation/current-session change or upstream PR is part of this
candidate. Preserve the previous paired artifacts for rollback.
