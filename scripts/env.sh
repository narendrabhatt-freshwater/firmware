# Freshwater shell env — adds repo root to PATH so `fw` works without ./.
#
# Auto (Cursor/VS Code):  .vscode/settings.json already sets PATH for new terminals
# Auto (direnv):          brew install direnv && direnv allow
# Manual:                 source scripts/env.sh

# Resolve this file's directory when sourced (bash or zsh)
if [ -n "${ZSH_VERSION:-}" ]; then
  _FW_SCRIPTS="$(cd "$(dirname "${(%):-%x}")" && pwd)"
elif [ -n "${BASH_VERSION:-}" ]; then
  _FW_SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
  _FW_SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
fi

_FW_ROOT="$(cd "${_FW_SCRIPTS}/.." && pwd)"

case ":${PATH}:" in
  *":${_FW_ROOT}:"*) ;;
  *) export PATH="${_FW_ROOT}:${PATH}" ;;
esac

export FW_ROOT="${_FW_ROOT}"

# Tab completion
if [ -n "${ZSH_VERSION:-}" ]; then
  # Ensure completion system is up (no-op if already initialized)
  if ! typeset -f compdef >/dev/null 2>&1; then
    autoload -Uz compinit
    compinit -C 2>/dev/null || compinit 2>/dev/null || true
  fi
  fpath=("${_FW_SCRIPTS}/completions" "${fpath[@]}")
  # Source directly so it works even before a fresh compinit rebuild
  # shellcheck disable=SC1091
  source "${_FW_SCRIPTS}/completions/_fw" 2>/dev/null || true
elif [ -n "${BASH_VERSION:-}" ] && [ -f "${_FW_SCRIPTS}/completions/fw.bash" ]; then
  # shellcheck disable=SC1091
  source "${_FW_SCRIPTS}/completions/fw.bash"
fi

unset _FW_SCRIPTS
unset _FW_ROOT
