# Filesystem support scope

Exports target **ordinary, single-user, unprivileged local filesystem use**. This document defines the boundary and triage policy for code changes and reviews. Findings outside this scope are documented as non-goals.

## Supported

- Local storage volumes on Windows and POSIX systems running as an unprivileged user.
- Standard directory trees and files, creating missing parent directories recursively as needed.
- Standard inherited permissions: published files are accessible to the account running the server. Existing Windows DACLs are preserved on a best-effort basis; newly created files inherit default directory permissions.
- POSIX owner-only staging (`0700`) and Windows restricted staging directories.
- Documented path syntax: drive letters, UNC paths, POSIX paths, UTF-8 encoded paths, and documented Windows filename constraints.
- Atomic per-file publication: completed files are made visible; subsequent file failures report all previously published files in the returned error payload.
- Cleanup of temporary staging folders and creation of parent directories are best-effort.

## Non-goals

Do not add complexity or open defect reports for items outside this defined scope:

- Complex, custom, protected, or deny-ACE-heavy DACLs, explicit security descriptor reconstruction, or ACL merging beyond the best-effort path.
- SACLs, auditing, file ownership changes, creation timestamps, NTFS alternate data streams, filesystem compression, or encryption flags.
- Network authentication for remote shares, SMB-specific semantics, IPv6-literal UNC hosts, and network-dependent domain hostnames.
- Windows device namespaces (`\\.\`, `\\?\`), extended-length paths, and drive-relative edge forms.
- Cross-volume publication transactions, multi-file atomic transactions, crash-consistent directory journaling, or automatic rollback of created parent folders.
- Elevated token execution, service accounts, or domain policy corner cases.
- In-server filesystem sandboxing, path allowlists, or deprecated MCP `roots` enforcement: the server runs as an ordinary local user process, and workspace confinement is owned by host-side OS sandboxing and client tool approvals.

If these conditions are encountered, the host may return `Io` or `InvalidArgument` without publishing corrupt files.

## Review and triage policy

When evaluating bug reports and proposed changes:

1. Does the issue affect ordinary single-user local export on supported filesystems? If yes, address it.
2. If no, document it as a non-goal.
3. Do not expand the accepted filesystem surface for edge-case non-goals.
