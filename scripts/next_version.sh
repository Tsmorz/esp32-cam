#!/usr/bin/env bash
# What this CI run releases, for $GITHUB_OUTPUT (.github/workflows/ci.yml):
#   tag=       the release to publish (empty: none)
#   version=   what the firmware reports (empty: git describe)
#   previous=  the release before it, where its notes start (the rolling
#              "dev" release would otherwise count as the previous one)
#
# - a pushed v* tag: that tag
# - a push to main whose commit isn't tagged yet: the next version after the
#   highest vX.Y.Z tag — patch by default; minor or major if "#minor" or
#   "#major" appears in a commit message since then (a PR's title lands in
#   its merge commit)
# - a push to development: the rolling pre-release "dev", reporting the next
#   version as a development build: v1.4.1-dev.3+3da6968 (3 commits since
#   the last release)
# - anything else: none
#
# Try it locally: GITHUB_REF=refs/heads/main GITHUB_EVENT_NAME=push \
#                 scripts/next_version.sh [commit]   (default HEAD)
set -euo pipefail
rev="${1:-HEAD}"
ref="${GITHUB_REF:-}"
event="${GITHUB_EVENT_NAME:-}"

out() {
  echo "tag=$1"
  echo "version=$2"
  echo "previous=$(git tag --list 'v*' --sort=-v:refname |
    grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | grep -vxF "$1" | head -n 1 || true)"
  exit 0
}

if [[ "$ref" == refs/tags/v* ]]; then
  out "${ref#refs/tags/}" "${ref#refs/tags/}"
fi
if [[ "$event" != push || ("$ref" != refs/heads/main && "$ref" != refs/heads/development) ]]; then
  out "" ""
fi

own=$(git tag --points-at "$rev" | grep -E '^v' | head -n 1 || true)
last=$(git tag --list 'v*' --sort=-v:refname |
       grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1 || true)
next=v0.1.0
if [[ -n "$last" ]]; then
  IFS=. read -r major minor patch <<<"${last#v}"
  log=$(git log "$last..$rev" --format=%B)
  if grep -q '#major' <<<"$log"; then
    major=$((major + 1)) minor=0 patch=0
  elif grep -q '#minor' <<<"$log"; then
    minor=$((minor + 1)) patch=0
  else
    patch=$((patch + 1))
  fi
  next="v$major.$minor.$patch"
fi

if [[ "$ref" == refs/heads/development ]]; then
  if [[ -n "$own" ]]; then # a released commit: it is that version
    out dev "$own"
  fi
  count=$(git rev-list --count "${last:+$last..}$rev")
  out dev "$next-dev.$count+$(git rev-parse --short=7 "$rev")"
fi
if [[ -n "$own" ]]; then # main, already tagged (by hand)
  out "" ""
fi
out "$next" "$next"
