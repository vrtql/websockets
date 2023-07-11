#!/usr/bin/env bash

. "scripts/common.sh"

rm -rf vrtql-ws-$version
rm -rf vrtql-ws-$version.gem
gem build config/vrtql-ws.gemspec
