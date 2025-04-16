@echo off

where /q clang && (
  call clang -DDEBUG -g -Wall -fuse-ld=lld q3.c -o q3.exe
  call clang -DDEBUG -mavx2 -mavx512f -march=native -O3 -g -Wall -fuse-ld=lld q3.c -o q3_simd.exe
)
