@echo off

where /q clang && (
  call clang -g -Wall -fuse-ld=lld q3.c -o q3.exe
  call clang -mavx2 -march=native -O3 -g -Wall -fuse-ld=lld q3.c -o q3_simd.exe
)
