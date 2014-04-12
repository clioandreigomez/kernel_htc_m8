#!/bin/bash

#
# kernelhackx
#
echo "Anonymous kernel"
clear
make clean
git checkout master
export ARCH=arm
export SUBARCH=arm
export CROSS_COMPILE=~/tmp/arm-eabi-4.9/bin/arm-eabi-
export ENABLE_GRAPHITE=true
make "kernelhackx_defconfig"
"time" make -j4 2>&1 | tee kernel.log
echo "kernelhackx"
