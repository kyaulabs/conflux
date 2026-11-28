#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail

if (( $# == 0 )); then
    printf 'Specify at least one Ubuntu package.\n' >&2
    exit 2
fi

work=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/conflux-apt.XXXXXXXX")
trap 'sudo rm -rf -- "$work"' EXIT
chmod 0755 "$work" # APT fetches as its unprivileged download user.

archives=(https://archive.ubuntu.com/ubuntu https://us.archive.ubuntu.com/ubuntu)
for i in "${!archives[@]}"; do
    archive=${archives[i]}
    source="$work/ubuntu-${i}.sources"
    lists="$work/lists-$i"
    mkdir "$lists"
    cat > "$source" <<EOF
Types: deb
URIs: $archive
Suites: noble noble-updates noble-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
    options=(
        -o "Dir::Etc::sourcelist=$source"
        -o Dir::Etc::sourceparts=-
        -o "Dir::State::lists=$lists"
        -o Acquire::Retries=2
        -o Acquire::https::Timeout=15
    )
    # Never start package installation with an inherited or partially refreshed index.
    if sudo apt-get "${options[@]}" update --error-on=any &&
       sudo apt-get "${options[@]}" install -y "$@"; then
        exit 0
    fi
    printf 'Dependency setup failed using %s.\n' "$archive" >&2
done
exit 1
