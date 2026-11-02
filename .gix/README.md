# Git Flow

This repository uses [gix](https://github.com/m-mdy-m/gix) for branch creation,
finishing and synchronization. The branch rules live in `config.default` so the
repository has one reviewable template and does not commit machine-generated
flow state.

## First-time setup

1. Initialize Git and create the repository's first commit on `main`.
2. Install `gix` and make sure the `gix` executable is available on `PATH`.
3. Run the initializer:

   ```sh
   bash scripts/init-git-flow.sh
   ```

The script copies `.gix/config.default` to `.gix/config`, adjusts the primary
branch name when the repository uses `master` or `trunk`, and runs `gix flow
init`. The generated `.gix/config` is local repository state and is ignored by
Git.

## Branches

| Type | Parent | Purpose |
|---|---|---|
| `feature/<name>` | `develop` | New behavior or a new module. |
| `bugfix/<name>` | `develop` | A defect fix that does not need a release hotfix. |
| `release/<version>` | `develop` | Stabilization before merging to `main` and `develop`. |
| `hotfix/<name>` | `main` | An urgent fix merged into both `main` and `develop`. |

`main` contains released code. `develop` is the integration branch. Topic
branches are removed after successful completion according to the template.

## Common commands

```sh
gix doctor
gix feature start backend-library-link
gix feature finish backend-library-link
```

Use `gix --help` for the command syntax supported by the installed gix version.
