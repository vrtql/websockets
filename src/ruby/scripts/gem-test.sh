#!/usr/bin/env bash

. "scripts/common.sh"

rm -rf vrtql-ws-$version.gem
gem build config/vrtql-ws.gemspec
gem install --verbose --install-dir /tmp/gem vrtql-ws-$version.gem
find /tmp/gem vrtql-ws-$version.gem
