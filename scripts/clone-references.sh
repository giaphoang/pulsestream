#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
clone_at() {
  local repo_url="$1" target_path="$2" commit_id="$3"
  if [[ ! -d "$target_path/.git" ]]; then git clone "$repo_url" "$target_path"; fi
  if [[ "$(git -C "$target_path" rev-parse HEAD)" != "$commit_id" ]]; then
    if [[ -n "$(git -C "$target_path" status --porcelain)" ]]; then
      echo "Refusing to change modified reference: $target_path" >&2
      exit 1
    fi
    git -C "$target_path" checkout --detach "$commit_id"
  fi
}
clone_at https://github.com/Vikas9892/KV_STORE.git references/KV_STORE 708b3e1fb2baeab2106cb7d3ab052932c06c5e10
clone_at https://github.com/rohangarim/embedded-lsm-kv.git references/embedded-lsm-kv 9141ce41bcd9a8829f8e44b046d5fce0dc21f101
clone_at https://github.com/nlohmann/json.git third_party/json 55f93686c01528224f448c19128836e7df245f72
