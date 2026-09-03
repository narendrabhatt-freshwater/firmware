# SVN publishing

The git monorepo is the working repository. The company Apache
Subversion server holds one repository per product, each with the
conventional `trunk/` + `tags/` layout. Publishing is **snapshot
based**: SVN receives release states, not git history.

## Mapping

Each product maps to one SVN repository's trunk content:

| Monorepo directory | SVN repository | Status |
| ------------------ | -------------- | ------ |
| `channel_card/` | `channel_card` | exists |
| `effect_card/`  | `effect_card`  | exists |
| `cmi_core/` | `cmi_core` | create when needed |
| `cmi_control/`  | `cmi_control`  | create when needed |

## Release flow

1. Merge `develop` → `main` (release line).
2. Tag the release in git per product: `git tag channel_card-v1.2`.
3. Check out the SVN repo root (must contain `trunk/`; `tags/` for
   tagging) and run:

```bash
fw svn-publish channel_card ~/svn/channel_card --tag v1.2
```

The script (`scripts/svn_publish.sh`):

- refuses to run with uncommitted changes under the product directory,
  `docs/` or the root `README.md`;
- rsyncs the product directory over `trunk/` (`--delete`, so removals
  propagate), excluding build output, tool caches, IDE/agent metadata
  (`.vscode/`, `.settings/`, `.github/`, Eclipse project files),
  `.gitignore` files and — for `cmi_control` — the generated `waves/`
  banks;
- copies the shared docs the export needs to stand alone into
  `trunk/docs/`: `protocol.md` (must match `$PRODUCT/docs/protocol.md`
  when that file exists), the root README as `firmware_handbook.md`
  for the cards, the filter reference and diagrams for the Channel
  card, `rs485_console_architecture.md` for the host products;
- registers adds/removals with `svn add` / `svn rm` and commits with
  the git SHA and branch in the message;
- with `--tag vX.Y`, copies `^/trunk` to `^/tags/vX.Y`.

`--dry-run` prints the file changes without touching the SVN state.

## Conventions

- Git tags and SVN tags correspond 1:1: git `channel_card-v1.2` ↔ SVN
  `channel_card` repo `tags/v1.2`.
- Every SVN commit message names the git SHA it was exported from, so
  an SVN revision can always be traced back to the exact monorepo
  state.
- The binary documents in `docs/` (`.docx`/`.pdf`/`.drawio`) are never
  exported — see `docs/README.md`.
- Do not edit in the SVN working copies; changes flow one way,
  git → SVN. Anything committed directly to SVN trunk will be
  overwritten by the next publish.

## First-time repository setup

For a new product repository, create the conventional layout before
the first publish:

```bash
svn mkdir <repo-url>/trunk <repo-url>/tags -m "Repository layout."
svn checkout <repo-url> ~/svn/<product>
fw svn-publish <product> ~/svn/<product>
```
