#!/bin/bash

echo
echo "Issue Build Commands"
echo

mkdir -p out
export ARCH=arm64
export SUBARCH=arm64
export CLANG_PATH=~/toolchains/linux-x86/bin
export PATH=${CLANG_PATH}:${PATH}
export DTC_EXT=~/toolchains/dtc/dtc
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=~/toolchains/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export CROSS_COMPILE_ARM32=~/toolchains/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
export LD_LIBRARY_PATH=~/toolchains/linux-x86/lib64:$LD_LIBRARY_PATH

echo
echo "Set DEFCONFIG"
echo 
make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out vendor/dragon_flash_defconfig

echo
echo "Build The Good Stuff"
echo 

make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out -j$(nproc --all)
cp -f ./out/arch/arm64/boot/Image.gz-dtb ./release/Dragon/Image.gz-dtb