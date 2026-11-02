# Filesystem support scope

M4 exports target **ordinary, single-user, unprivileged local filesystem use**.
This file is the triage rule for reviews and for future changes. A finding
outside this scope is documented as a non-goal; it is not a release blocker and
does not get a fix merely because it is real.

## Supported

- Local volumes on Windows and POSIX, with the host running as an ordinary user.
- Ordinary directories and files, including missing parents that the host may
  create recursively.
- Ordinary inherited permissions: the published file is usable by the account
  that ran the export. Existing ordinary DACLs are preserved on Windows as a
  best effort; a new file gets the permissions a normally created file in that
  directory would get.
- POSIX owner-only staging (`0700`) and Windows private staging.
- The documented destination syntax: normal drive/UNC paths, POSIX paths,
  non-ASCII UTF-8 paths, and the Windows filename rules in
  [m4-live-default.md](m4-live-default.md#destination-conditions).
- Per-file publication: a completed file is visible; a later file may fail and
  the error reports exactly the files already published. Multi-file publication
  is not a transaction.
- The host may leave parents created before a later failure and may clean
  staging only on a best-effort basis; both are documented outcomes.

## Non-goals

Do not add fixes or open review rounds for these unless the user re-scopes them:

- Complex, custom, protected or deny-ACE-heavy DACLs; ACL merging or exact
  security-descriptor reproduction beyond the current best-effort path.
- SACLs/auditing, ownership, creation-time, alternate data streams, encryption,
  compression, or resource-attribute preservation.
- Remote-share authentication, SMB server semantics, IPv6-literal UNC hosts, and
  hostnames that only exist in a particular network.
- Windows device namespaces (`\\.\`, `\\?\`), extended-length paths, drive-
  relative edge forms, and exotic reserved-name corpora beyond the documented
  table.
- Cross-volume publication, multi-file atomicity, crash-consistent directory
  metadata, and rollback of created parents.
- ACL behavior under a second account, elevated tokens, service accounts, or
  domain policy. The normal-privilege single-user case is the contract.

If one of these is encountered, the host may return `Io` or `InvalidArgument`,
may leave created parents, and must not publish partial content. That is an
acceptable failure, not a defect to chase.

## Review rule

A reviewer may report anything, but the orchestrator triages it:

1. Does it affect ordinary single-user local export? If yes, fix it.
2. If no, add or update a non-goal note and move on.
3. Do not reopen an accepted surface for a non-goal; do not add tests that only
   exercise it.

The destination preflight, staging and publication code is frozen at this
scope. Phase acceptance depends on the normal-use tests, not on exhaustive
coverage of ACL or namespace corners.
