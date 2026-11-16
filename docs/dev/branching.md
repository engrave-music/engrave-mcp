# Branching and tags

Two repositories are involved in development: the host (`engrave-mcp`) and the engine submodule (`vendor/engrave`). Both use `main` as the single long-lived integration branch. Tags are reserved for real version releases.

## Host repository (`engrave-mcp`)

- `main` is the only long-lived branch and integration line.
- Development proceeds on short-lived topic branches (`feat/...`, `fix/...`, `cleanup/...`). Merge to `main` when changes are verified, then delete the topic branch.
- Tags are reserved for formal version releases (`v<version>`).
- Remote tracking: `origin` is `https://github.com/Locietta/engrave-mcp`; `main` tracks `origin/main`. Push `main` as changes land.

## Moving work between machines

Windows and physical Linux (`ssh user@<linux-host>`) environments exchange commits through GitHub or the lab host's bare remotes (`lab`):

- Topic branches needing cross-platform validation are pushed to `origin` (or `lab`), fetched on the other machine, and deleted on the remote once merged. Before deleting a remote branch, verify that its commits are fully contained in `main` (for example, GitHub compare reports `ahead_by: 0`). Preserve unmerged work, and preserve the frozen `mcp/*` and `upstream/*` reference branches regardless of merge state.
- When an engine change is required, push the engine commit to `origin main` on GitHub before pushing a host commit that pins it to `origin main`, ensuring public submodule references resolve; unaccepted cross-machine checks between Windows and Linux (`user@<linux-host>`) may exchange engine and host commits via `lab` first.
- Linux (`user@<linux-host>`) checkout path is `~/engrave/engrave-mcp` (environment loaded with `source ~/engrave/env.sh`); bare mirrors live at `user@<linux-host>:engrave/engrave-mcp.git` (`lab`) and `user@<linux-host>:engrave/engrave.git` (`lab`). The GitHub host remote is `Locietta/engrave-mcp.git`.

## Engine repository (`vendor/engrave`)

- `main` is the only long-lived branch. History is strictly forward-only: force-pushes and rebases of published history are prohibited.
- Submodule remote `origin` is configured to `https://github.com/Locietta/engrave.git`; `main` tracks `origin/main`.
- The host pins commits on fork `main` by commit SHA. Submodule pins are not tagged.
- Topic branches (`feat/...`, `fix/...`) merge into `main` and are deleted.
- Upstream changes are cherry-picked onto `main` with commit provenance and rationale. Upstream tracking branches (`upstream/musescore`, `upstream/muse-framework`) are frozen reference points.

The engine submodule is checked out shallowly. To inspect deeper history without altering the pin:

```bash
git -C vendor/engrave fetch --deepen=256 origin main
```
