#!/bin/bash

# Expand the response files produced by CMake/Ninja before invoking Apple's
# archiver, which does not understand the @file convention.
set -e

expanded=()
for argument in "$@"; do
	if [[ "$argument" == @* ]]; then
		response_file=${argument#@}
		while IFS= read -r object || [[ -n "$object" ]]; do
			while [[ "$object" == *[[:space:]] ]]; do object=${object%?}; done
			while [[ "$object" == [[:space:]]* ]]; do object=${object#?}; done
			[[ -z "$object" ]] && continue
			expanded+=("$object")
		done < "$response_file"
	else
		expanded+=("$argument")
	fi
done

exec /usr/bin/ar "${expanded[@]}"
