# Branching and tags

Two repositories matter: the host (`engrave-mcp`) and the engine checkout
(`vendor/engrave`). Both use one long-lived branch and tags for checkpoints.

## Host repository

- `main` is the only long-lived branch and the accepted integration line.
- Work happens on short-lived topic branches (`m4/...`, `cleanup/...`). Merge to
  `main` when the step is verified, then delete the branch. A phase or step
  branch is not kept after merge; its commits are reachable from `main`.
- Accepted checkpoints are annotated tags, not branches:
  - `m4-accepted` — the M4 acceptance record.
  - `checkpoint/YYYY-MM-DD` — a consolidated state worth returning to.
  - `v<version>` — a real release, once there is one.
- Do not create a tag for every merge; the history already records those.
- `origin` is the private GitHub repository
  `https://github.com/Locietta/engrave-mcp` (published 2026-11-06); `main`
  tracks `origin/main`. Push `main` and checkpoint tags as they are made.

## Moving work between machines

Windows and linux-host exchange commits only through GitHub; git bundles and copied
worktrees are retired.

- A topic branch that linux-host must build or validate is pushed to `origin`, fetched
  on linux-host, and deleted on GitHub once merged or abandoned. Work done on linux-host is
  pushed the same way.
- Push the engine commit and its `engrave-<n>` tag before pushing a host commit
  that pins it, so the submodule pin always resolves.
- linux-host's checkout is `~/OSS/engrave-mcp`. Port 22 to GitHub is blocked there;
  its `~/.ssh/config` routes `github.com` through `ssh.github.com:443`. The host
  remote is `git@github.com:Locietta/engrave-mcp.git`; the engine fetches over
  HTTPS and pushes over SSH.

## Engine repository (`vendor/engrave`)

- `main` is the only long-lived branch. It moves forward only: no force-push and
  no rebase of published history.
- The checkout has `origin` configured
  (`https://github.com/Locietta/engrave.git`); `main` tracks `origin/main`.
  Pull remote `main` instead of creating a local line.
- Every state the host pins carries an `engrave-<n>` tag. The host pins only
  tagged SHAs; the tag ledger is in [fork.md](fork.md).
- Feature branches (`feat/...`, `fix/...`) are merged to `main` and then
  deleted. Abandoned work is not kept as a branch.
- Upstream fixes are cherry-picked onto `main` with provenance. The frozen
  `upstream/*` reference refs are history, not development branches.

The engine submodule is intentionally shallow, so a fresh checkout can show
only its pinned commit in `git log` even though the remote branch has earlier
history. To inspect more history without moving the checkout or changing the
pin, run `git -C vendor/engrave fetch --deepen=256 origin main`; repeat as
needed. If the full work clone exists at `.upstream/engrave`, its `main` also
provides the complete local history.

## Checkpoints

| Tag                     | Meaning                                                      |
| ----------------------- | ------------------------------------------------------------ |
| `engrave-35`            | Engine pin used by the host.                                 |
| `m4-accepted`           | Host M4 acceptance (2026-11-03).                             |
| `checkpoint/2026-11-03` | Post-M4 cleanup: docs, Qt6 removal, submodule rename.        |
| `archive/engrave-a5`    | Superseded 2026-10-31 v2 registry work, kept for provenance. |
| `pre-fork-baseline`     | Last executable pre-fork host state.                         |

## Cleanup performed (2026-11-03)

- Host: 44 local branches reduced to `main`; every deleted branch was an
  ancestor of `main` except `engrave/a5`, whose tip is the
  `archive/engrave-a5` tag.
- Engine submodule checkout: `main` tracks `origin/main` at `engrave-31`; two
  merged local feature branches and 56 stale `origin/*` remote-tracking refs
  removed. On GitHub, 50 merged remote branches were deleted, leaving `main`
  plus four frozen branches: `mcp/v4.7.5`, `mcp/v4.7.6`, `upstream/musescore`,
  `upstream/muse-framework`. The 37 engine tags are unchanged.
- The separate `.upstream/engrave` work clone was not included in that local
  cleanup. It still has topic refs and worktree records, including four branch
  tips not merged into `main`: `a3/old-main`, `a5/api-gaps`, `scratch/base`,
  and `t/main`. Review those tips before removing them.

Remote cleanup rule: only delete a branch whose commits are fully contained in
`main` (`compare` reports `ahead_by: 0`). Keep unmerged work, and keep the
frozen `mcp/*` and `upstream/*` reference branches regardless of merge state.
