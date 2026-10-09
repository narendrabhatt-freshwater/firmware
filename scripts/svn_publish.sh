#!/usr/bin/env bash
#
# svn_publish.sh — compare the bundled Channel release, or run the legacy
# publisher for Effect Card / voice_bd. Channel mode never modifies SVN.
#
# Usage:
#   svn_publish.sh <product> <svn-working-copy> [--tag vX.Y] [--dry-run]
#
#   <product>            channel_card | effect_card | berry_compiler | voice_bd
#   <svn-working-copy>   checkout of the SVN repo ROOT (must contain trunk/;
#                        tags/ is required only when --tag is used)
#   --tag vX.Y           after committing trunk, svn copy trunk -> tags/vX.Y
#   --dry-run            show what would change; no svn add/rm/commit
#
# The publish flow is documented in docs/reference/svn_publishing.md. Summary:
# release snapshots only — merge develop -> main, git tag
# <product>-vX.Y, then run this against the company SVN working copy.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

err()  { echo "error: $*" >&2; exit 1; }
info() { echo "== $*"; }

[ $# -ge 2 ] || err "usage: svn_publish.sh <product> <svn-working-copy> [--tag vX.Y] [--dry-run]"

PRODUCT="$1"; shift
WC="$1"; shift

# Channel releases contain the matching Berry compiler. Preparation and SVN
# comparison are read-only; publication is an explicit, separate release step.
if [ "$PRODUCT" = channel_card ]; then
  [ "$#" = 1 ] && [ "$1" = --dry-run ] || \
    err "Channel preparation requires --dry-run; this tool does not modify or commit SVN. Use channel_release.py --stage DIR to prepare a local package."
  exec python3 "$ROOT/scripts/channel_release.py" --svn-working-copy "$WC"
fi
if [ "$PRODUCT" = berry_compiler ]; then
  err "Berry is bundled with channel_card; prepare the combined Channel release instead"
fi

TAG=""
DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --tag)     [ $# -ge 2 ] || err "--tag needs a value (e.g. v1.0)"; TAG="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    *)         err "unknown argument: $1" ;;
  esac
done

case "$PRODUCT" in
  channel_card|effect_card|berry_compiler|voice_bd) ;;
  *) err "unknown product '$PRODUCT' (channel_card | effect_card | berry_compiler | voice_bd)" ;;
esac

command -v svn   >/dev/null || err "svn not found on PATH"
command -v rsync >/dev/null || err "rsync not found on PATH"

SOURCE_REL="$PRODUCT"
SRC="$ROOT/$SOURCE_REL"
[ -d "$SRC" ] || err "product directory not found: $SRC"
TRUNK="$WC/trunk"
[ -d "$TRUNK" ] || err "no trunk/ in working copy: $WC (check out the SVN repo root)"
svn info "$WC" >/dev/null 2>&1 || err "$WC is not an SVN working copy"

# Refuse to publish a dirty product tree: the SVN revision must
# correspond to a reproducible git state.
if [ -n "$(git -C "$ROOT" status --porcelain -- "$SOURCE_REL" docs README.md)" ]; then
  err "uncommitted changes under $PRODUCT/, docs/ or README.md — commit first"
fi

GIT_SHA="$(git -C "$ROOT" rev-parse --short HEAD)"
GIT_BRANCH="$(git -C "$ROOT" branch --show-current)"

# Not exported: build output, tool caches, IDE and agent metadata, git
# metadata (SVN uses svn:ignore) and local photo dumps. Binary docs (.docx/.pdf/.drawio)
# never ship — see docs/README.md.
EXCLUDES=(
  --exclude 'build/'
  --exclude '.build/'
  --exclude 'voicebd'
  --exclude 'channel.bec'
  --exclude '.cache/'
  --exclude '.vscode/'
  --exclude '.settings/'
  --exclude '.github/'
  --exclude '.cproject'
  --exclude '.project'
  --exclude '.osx.project'
  --exclude '.gitignore'
  --exclude '.DS_Store'
  --exclude '.clangd'
  --exclude 'compile_commands.json'
  --exclude 'imgui.ini'
  --exclude 'docs/img/'
)

RSYNC_FLAGS=(-a --delete --exclude '.svn/')
[ "$DRY" = 1 ] && RSYNC_FLAGS+=(-n -v)

info "sync $PRODUCT/ -> $TRUNK (git $GIT_SHA on $GIT_BRANCH)"
rsync "${RSYNC_FLAGS[@]}" "${EXCLUDES[@]}" "$SRC/" "$TRUNK/"

# Shared docs each export needs to stand alone (card READMEs point at
# docs/; the protocol repo README references the wire spec).
DOCS_DIR="$TRUNK/docs"
copy_doc() { # copy_doc <src-rel-to-root> <dst-rel-to-trunk-docs>
  if [ "$DRY" = 1 ]; then
    echo "would copy $1 -> docs/$2"
  else
    mkdir -p "$DOCS_DIR/$(dirname "$2")"
    cp "$ROOT/$1" "$DOCS_DIR/$2"
  fi
}
# Card trees carry their own docs/protocol.md; content must match the
# shared copy, allowing only relative scripting-link differences.
if [ -f "$SRC/docs/protocol.md" ]; then
  if ! cmp -s <(sed -E 's@\[[^]]*\]\([^)]*(SCRIPTING.md|berry_compiler/README.md)\)@[Berry scripting](SCRIPTING.md)@g' "$ROOT/docs/protocol.md") \
              <(sed -E 's@\[[^]]*\]\([^)]*(SCRIPTING.md|berry_compiler/README.md)\)@[Berry scripting](SCRIPTING.md)@g' "$SRC/docs/protocol.md"); then
    err "docs/protocol.md differs from $PRODUCT/docs/protocol.md — keep them identical"
  fi
fi
if [ ! -f "$SRC/docs/protocol.md" ]; then
  copy_doc docs/protocol.md protocol.md
fi
case "$PRODUCT" in
  effect_card)
    copy_doc README.md firmware_handbook.md
    copy_doc docs/diagrams/card_data_flow.md diagrams/card_data_flow.md
    ;;
  voice_bd)
    copy_doc docs/reference/rs485_console_architecture.md reference/rs485_console_architecture.md
    ;;
esac

if [ "$DRY" = 1 ]; then
  info "dry run — no svn add/rm/commit performed"
  exit 0
fi

# Register adds and deletes with SVN (rsync only changed the files).
(
  cd "$TRUNK"
  svn status | awk '$1 == "?" { $1=""; sub(/^ +/,""); print }' | while IFS= read -r p; do
    svn add -q --parents "$p"
  done
  svn status | awk '$1 == "!" { $1=""; sub(/^ +/,""); print }' | while IFS= read -r p; do
    svn rm -q "$p"
  done
)

if [ -z "$(svn status "$TRUNK")" ]; then
  info "trunk already matches git $GIT_SHA — nothing to commit"
else
  MSG="Sync $PRODUCT from git $GIT_SHA ($GIT_BRANCH)."
  info "svn commit: $MSG"
  svn commit -q "$TRUNK" -m "$MSG"
fi

if [ -n "$TAG" ]; then
  info "svn tag: tags/$TAG"
  ( cd "$WC" && svn copy -q "^/trunk" "^/tags/$TAG" \
      -m "Tag $PRODUCT $TAG (git $GIT_SHA)." )
fi

info "done"
