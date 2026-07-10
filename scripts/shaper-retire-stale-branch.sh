#!/usr/bin/env bash

set -o nounset
set -o pipefail
set -o errexit
#set -o xtrace

tag_and_purge() {
	args="-n"
	if [[ "${1:-}" = "for_realz" ]]; then
		args=""
	fi

	for branch in ${branches[@]}; do
		tag="stale/${tag_prefix}-$(echo ${branch} | tr '/' '-')"
		echo "tagging ${branch} with ${tag}"
		# we want to tag the upstream state before we obliterate it
		git tag -f ${tag} origin/${branch}
		echo "pushing tag ${tag}"
		git push ${args} origin ${tag}
		echo "purging original branch ${branch}"
		git push ${args} origin :${branch}
		tags+=("${tag}")
	done
}

git fetch -p

tags=()
branches=()
tag_prefix=$(date +%Y-%m-%d)

for branch in $@; do
	branch=$(echo ${branch} | sed -e "s,origin/,,g")

	qualified_path=refs/heads/${branch}
	echo "considering ${qualified_path}"
	if [[ -z $(git ls-remote --heads origin ${qualified_path}) ]]; then
		echo "remote ref ${qualified_path} does not exist; bailing from deletion path"
		exit 1
	fi
	branches+=("${branch}")
done

tag_and_purge

echo "Looking good to push tag: ${tags[@]} capturing (and deleting) ${branches[@]}"
read -p "Do you wish to continue? " -n 1 -r
echo    # (optional) move to a new line
if [[ ! $REPLY =~ ^[Yy]$ ]]
then
    echo "Bailing on tag push"
    exit 1
fi

tag_and_purge for_realz
