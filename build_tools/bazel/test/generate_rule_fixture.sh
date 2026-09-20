#!/usr/bin/env bash

set -euo pipefail

outputs=()
assets=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --header=*|--first=*|--second=*)
      outputs+=("${1#*=}")
      shift
      ;;
    --source)
      outputs+=("$2")
      shift 2
      ;;
    --assets=*)
      assets="${1#--assets=}"
      shift
      ;;
    *)
      shift
      ;;
  esac
done

for output in "${outputs[@]}"; do
  mkdir -p "$(dirname "${output}")"
  printf "generated file\n" > "${output}"
done
if [[ -n "${assets}" ]]; then
  mkdir -p "${assets}"
  printf "generated asset\n" > "${assets}/fixture.txt"
fi
