#!/bin/bash
cd "$(dirname "$0")" || exit
export cjHeapSize=16MB
exec cjpm build
