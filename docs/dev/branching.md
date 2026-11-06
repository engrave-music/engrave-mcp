# Branching and tags

Two repositories matter: the host (`engrave-mcp`) and the engine checkout
(`vendor/engrave`). Both use one long-lived branch. Tags are reserved for real
version releases; checkpoints and pins are referred to by commit SHA.

## Host repository

- `main` is the only long-lived branch and the accepted integration line.
- Work happens on short-lived topic branches (`m4/...`, `cleanup/...`). Merge to
  `main` when the step is verified, then delete the branch. A phase or step
  branch is not kept after merge; its commits are reachable from `main`.
- The only tags are `v<version>` for real releases, once there is one. Notable
  checkpoints are listed by commit SHA under [Checkpoints](#checkpoints).
- `origin` is the private GitHub repository
  `https://github.com/Locietta/engrave-mcp` (published 2026-11-06); `main`
  tracks `origin/main`. Push `main` as it moves.

## Moving work between machines

Windows and linux-host exchange commits only through GitHub; git bundles and copied
worktrees are retired.

- A topic branch that linux-host must build or validate is pushed to `origin`, fetched
  on linux-host, and deleted on GitHub once merged or abandoned. Work done on linux-host is
  pushed the same way.
- Push the engine commit to `main` before pushing a host commit that pins it, so
  the submodule pin always resolves.
- linux-host's checkout is `~/OSS/engrave-mcp`. The host remote is
  `git@github.com:Locietta/engrave-mcp.git`; the engine fetches over HTTPS and
  pushes over SSH.

## Engine repository (`vendor/engrave`)

- `main` is the only long-lived branch. It moves forward only: no force-push and
  no rebase of published history.
- The checkout has `origin` configured
  (`https://github.com/Locietta/engrave.git`); `main` tracks `origin/main`.
  Pull remote `main` instead of creating a local line.
- The host pins commits on `main` by SHA; each pin has a ledger row in
  [fork.md](fork.md). Pins are not tagged: tags are for real version releases.
  The former `engrave-<n>` pin tags (40) and `upstream-final` (`cbffaa078d`),
  all on `main`, were deleted on 2026-11-06. The `mcp-4.7.*` tags on the frozen
  `mcp/v4.7.*` branches remain.
- Feature branches (`feat/...`, `fix/...`) are merged to `main` and then
  deleted. Abandoned work is not kept as a branch.
- Upstream fixes are cherry-picked onto `main` with provenance. The frozen
  `upstream/*` reference refs are history, not development branches.

The engine submodule is intentionally shallow, so a fresh checkout can show
only its pinned commit in `git log` even though the remote branch has earlier
history. To inspect more history without moving the checkout or changing the
pin, run `git -C vendor/engrave fetch --deepen=256 origin main`; repeat as
needed.

## Checkpoints

Host commits on `main` worth returning to:

| Commit    | Meaning                                               |
| --------- | ----------------------------------------------------- |
| `d07e624` | Host M4 acceptance (2026-11-03).                      |
| `e439e9a` | Post-M4 cleanup: docs, Qt6 removal, submodule rename. |
| `f8e7d26` | Last executable pre-fork host state.                  |

These were the tags `m4-accepted`, `checkpoint/2026-11-03` and
`pre-fork-baseline` until 2026-11-06, when all host tags were deleted. The
fourth, `archive/engrave-a5` (`dd1b7de`), kept three unmerged 2026-10-31
commits: a first v2 registry draft and the worker-process export-job design
later dropped for live export (see [history.md](history.md)). Main redid that
work, so the tag and its commits were dropped.

## Cleanup performed (2026-11-03)

- Host: 44 local branches reduced to `main`; every deleted branch was an
  ancestor of `main` except `engrave/a5`, whose tip was kept as the
  `archive/engrave-a5` tag (dropped 2026-11-06).
- Engine submodule checkout: `main` tracks `origin/main` at `engrave-31`; two
  merged local feature branches and 56 stale `origin/*` remote-tracking refs
  removed. On GitHub, 50 merged remote branches were deleted, leaving `main`
  plus four frozen branches: `mcp/v4.7.5`, `mcp/v4.7.6`, `upstream/musescore`,
  `upstream/muse-framework`. The 37 engine tags are unchanged.
- The separate `.upstream/engrave` work clone was not included in that local
  cleanup. It was deleted on 2026-11-06 after review: its four tips not merged
  into `main` were superseded (`a3/old-main` by the redone A3 trim in
  `engrave-2`, `a5/api-gaps` by `a5/api-gaps-14` in `engrave-15`, and
  `scratch/base`/`t/main` were sync-tool scratch).

Remote cleanup rule: only delete a branch whose commits are fully contained in
`main` (`compare` reports `ahead_by: 0`). Keep unmerged work, and keep the
frozen `mcp/*` and `upstream/*` reference branches regardless of merge state.
