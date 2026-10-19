#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
export MESA_GPU_TRACES=print
export MESA_GPU_TRACEFILE=/tmp/conflux-mesa-balanced.txt
exec /tmp/conflux-stem-m0/conflux-stem-runtime-blocking "$@"
