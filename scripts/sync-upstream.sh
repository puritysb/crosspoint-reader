#!/usr/bin/env bash
#
# sync-upstream.sh — pull the upstream stable line into our fork's master.
#
# This repo is a personal fork (origin) carrying the private "AgentDeck Decision
# Card" product stack on top of upstream crosspoint-reader. We follow upstream's
# STABLE line (upstream/master, where releases land), not develop.
#
# Why merge (not rebase): our master has ~20 AgentDeck commits layered on top.
# A rebase would replay all of them and force re-resolving the same conflicts
# every sync. A merge resolves once and preserves the AgentDeck history.
#
# This script only fetches + merges on a local branch. It does NOT push — review
# the merge and build first, then `git push origin master` yourself.
#
# Usage:
#   ./scripts/sync-upstream.sh            # sync master with upstream/master
#   ./scripts/sync-upstream.sh --check    # only report pending upstream commits
#
set -euo pipefail

UPSTREAM_REMOTE="upstream"
UPSTREAM_BRANCH="master"   # upstream's stable/default line (releases land here)
LOCAL_BRANCH="master"      # our personal main line

CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

# --- preconditions ---------------------------------------------------------
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "ERROR: not inside a git repository." >&2; exit 1; }

git remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1 || {
  echo "ERROR: no '$UPSTREAM_REMOTE' remote. Add it with:" >&2
  echo "  git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git" >&2
  exit 1; }

echo "==> Fetching $UPSTREAM_REMOTE ..."
git fetch "$UPSTREAM_REMOTE" --prune

PENDING=$(git rev-list --count "$LOCAL_BRANCH..$UPSTREAM_REMOTE/$UPSTREAM_BRANCH")
if [[ "$PENDING" -eq 0 ]]; then
  echo "==> Up to date: $LOCAL_BRANCH already contains $UPSTREAM_REMOTE/$UPSTREAM_BRANCH."
  exit 0
fi

echo "==> $PENDING new upstream commit(s) on $UPSTREAM_REMOTE/$UPSTREAM_BRANCH:"
git log --oneline "$LOCAL_BRANCH..$UPSTREAM_REMOTE/$UPSTREAM_BRANCH"

if [[ "$CHECK_ONLY" -eq 1 ]]; then
  echo "==> --check only; not merging. Run without --check to merge."
  exit 0
fi

# --- clean tree + branch switch -------------------------------------------
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "ERROR: working tree has uncommitted changes. Commit or stash first." >&2
  exit 1
fi

CURRENT=$(git branch --show-current)
if [[ "$CURRENT" != "$LOCAL_BRANCH" ]]; then
  echo "==> Switching to $LOCAL_BRANCH (was $CURRENT) ..."
  git checkout "$LOCAL_BRANCH"
fi

# --- merge -----------------------------------------------------------------
echo "==> Merging $UPSTREAM_REMOTE/$UPSTREAM_BRANCH into $LOCAL_BRANCH ..."
if git merge --no-edit "$UPSTREAM_REMOTE/$UPSTREAM_BRANCH"; then
  echo
  echo "==> Merge complete. Next steps:"
  echo "    1. pio run                 # verify the firmware still builds"
  echo "    2. git push origin $LOCAL_BRANCH"
  echo
  echo "    Conflicts usually only hit AgentDeck-touched shared files"
  echo "    (ActivityManager, HomeActivity, themes). New src/agentdeck/* files don't conflict."
else
  echo
  echo "==> MERGE CONFLICT. Resolve preserving BOTH sides:" >&2
  echo "    - keep upstream's fixes AND our AgentDeck changes in the shared files" >&2
  echo "    - git add <resolved>; git commit   (then: pio run; git push origin $LOCAL_BRANCH)" >&2
  echo "    - to bail out: git merge --abort" >&2
  exit 1
fi
