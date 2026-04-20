#!/bin/bash
cd "$(dirname "$0")" || exit
export cjStackSize=10000kb
exec cjpm run
