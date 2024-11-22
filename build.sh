#!/bin/bash

if [ -e /usr/src/pico-sdk ]; then
    export PICO_SDK_PATH=/usr/src/pico-sdk
fi

mkdir -p build
cd build
cmake ..
make
