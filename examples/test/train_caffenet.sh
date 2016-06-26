#!/usr/bin/env sh

./build/tools/caffe train \
    --solver=examples/test/solver.prototxt --gpu=0
