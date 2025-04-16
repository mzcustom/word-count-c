#!bin/bash

clang -DDEBUG -g -Wall q3.c -o q3
clang -DDEBUG -mavx2 -march=native -O3 -g -Wall q3.c -o q3_simd

