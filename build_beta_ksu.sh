#!/bin/bash

echo
echo "Clean Build Directory"
echo 

make clean && make mrproper

echo
echo "Issue Build Commands"
echo

mkdir -p out
export ARCH=arm64
export SUBARCH=arm64
export CLANG_PATH=~/toolchains/neutron-clang/bin
export PATH=${CLANG_PATH}:${PATH}
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-
export CLANG_TRIPLE=aarch64-linux-gnu-

echo
echo "Set DEFCONFIG"
echo 
make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out vendor/dragon_ksu_beta_defconfig

echo
echo "Build The Good Stuff"
echo 

make CC=clang AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out -j24
# Copy the current Image.gz-dtb to history with incremented name
history_dir=./release/Dragon/history-beta
mkdir -p $history_dir
current_file=./release/Dragon/Image-beta
if [ -f "$current_file" ]; then
    n=$(ls $history_dir | grep -oP '^Image-beta\K\d+$' | sort -nr | head -n1)
    n=$((n + 1))
    cp -f "$current_file" "$history_dir/Image-beta${n}"
fi

# Copy the new build to the release directory
cp -f ./out/arch/arm64/boot/Image ./release/Dragon/Image-beta