#!/bin/bash
set -e
cd "$(dirname "$0")" || exit
#export cjHeapSize=16MB
cjpm update
exec cjpm build
