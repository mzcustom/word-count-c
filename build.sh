#!bin/bash

clang -g -Wall q3.c -o q3
clang -mavx2 -march=native -O3 -g -Wall q3.c -o q3_simd

