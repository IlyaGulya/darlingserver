#!/usr/bin/env bash

contract_test_init() {
	CONTRACT_TEST_NAME="$1"
}

fail() {
	echo "${CONTRACT_TEST_NAME:-contract_test}: $*" >&2
	exit 1
}

require_file() {
	[ -f "$1" ] || fail "$2 not found at $1"
}

require_grep() {
	local pattern="$1" file="$2" message="$3"
	grep -q "$pattern" "$file" || fail "$message"
}

require_not_grep() {
	local pattern="$1" file="$2" message="$3"
	if grep -Eq "$pattern" "$file"; then
		fail "$message"
	fi
}

require_text() {
	local pattern="$1" text="$2" message="$3"
	grep -q "$pattern" <<<"$text" || fail "$message"
}
