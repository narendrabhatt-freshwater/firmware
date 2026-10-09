# Channel release preparation

Channel Card and its matching Berry compiler form one source release. The SVN
layout has firmware at `trunk/` and the compiler at `trunk/berry_compiler/`.
Both come from the same Git revision. The compiler remains independently
buildable in its existing working-repository directory.

## Local preview

From the firmware repository root:

```sh
python3 scripts/channel_release.py --stage /tmp/channel-preview --working-tree
```

The destination and its adjacent manifest must not already exist. This creates
a package from current files, validates required inputs and documentation links,
and records paths, sizes, hashes, and executable permissions in
`/tmp/channel-preview.manifest.json`. The manifest labels it as an uncommitted
preview; it is not a reproducible release tied solely to the recorded Git SHA.
Nothing accesses or changes SVN.

## Committed release snapshot

Once the reviewed cleanup has been committed to Git:

```sh
python3 scripts/channel_release.py --stage /tmp/channel-release
```

The tool requires clean Channel, compiler, and export-tool inputs, then reads
both product trees from a single `git archive HEAD`. Unrelated work in
`voice_bd/` or Effect Card does not block preparation. Ignored build output is
never copied. The manifest records the exact Git commit and every shipped file.

The package contains firmware and compiler sources, required generated Berry
tables, licenses/provenance, build inputs, Berry source examples, the Linux
ARM64 compiler, two first-party READMEs, and essential protocol/filter references.
Upstream dependency READMEs remain with their snapshots. Tests, fixtures,
images, development scripts, editor/agent metadata, and generated firmware
binaries are excluded. The manifest stays beside the package, outside `trunk/`.

## Read-only SVN comparison

When explicitly requested, compare a committed package with a clean local SVN
working copy containing `trunk/`:

```sh
./fw svn-publish channel_card /path/to/channel-working-copy --dry-run
```

Only local `svn info`, `svn status`, and `rsync --dry-run` are used. The tool
rejects modified, unversioned, ignored, conflicted, switched, or external content
in the destination. It reports additions, changes, and deletions, including
previously shipped tests, images, and helper scripts. It never changes the
working copy, registers files, commits, or creates tags. Working-tree previews
cannot be compared to SVN through this command.

Channel preparation without `--dry-run` and separate Berry publication are
rejected. Actual SVN publication is a separate, explicitly requested step.

## Verification before release

Build firmware Debug and Release and the compiler from the staged package.
Run retained local tests against those staged sources using a harness outside
the package. Compile the included Berry examples and check the bundled Linux
ARM64 executable against a source-built compiler. Review the manifest and
comparison output, record actual results, and retain hardware qualification
limitations. No tests or verification reports enter the source package.

## Other products

Effect Card and `voice_bd` retain their existing separate release workflows;
this Channel cleanup does not publish them. Their legacy commands in
`scripts/svn_publish.sh` can commit, so they must not be used for Channel
preparation. Shared documentation outside `channel_card/` remains available
for those products and is not copied into the Channel package.
