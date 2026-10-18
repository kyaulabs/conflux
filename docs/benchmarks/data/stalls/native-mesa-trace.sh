#!/bin/sh
export MESA_GPU_TRACES=print
export MESA_GPU_TRACEFILE='/tmp/conflux-mesa-stall-%p-%i.txt'
exec /tmp/conflux-stem-m0/conflux-stem-runtime-blocking "$@"
