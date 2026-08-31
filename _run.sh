#!/bin/bash
cd "$(dirname "$0")" || exit
export cjStackSize=64MB
export cjHeapSize=64MB
exec cjpm run --skip-build --run-args="$@"
#exec ./target/release/bin/main $@
