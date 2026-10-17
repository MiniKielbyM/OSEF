#!/bin/sh
# Prints a space-free path for an OS directory whose real path contains spaces:
# a symlink build/links/<key>/<name> -> <real dir>. GNU make cannot handle
# spaces in file names, so the Makefile builds such an OS through this link.
# usage: link-os-dir.sh <build dir> <os dir>
set -e
build=$1
real=$(cd "$2" 2>/dev/null && pwd) || exit 1
base=$(basename "$real")
key=$(printf '%s' "${real#/}" | tr -c 'A-Za-z0-9._-' '_')
name=$(printf '%s' "$base" | tr -c 'A-Za-z0-9._-' '_')
mkdir -p "$build/links/$key"
ln -sfn "$real" "$build/links/$key/$name"
printf '%s' "$build/links/$key/$name"
