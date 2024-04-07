#!/bin/bash
set -e
clang++ -std=c++20 -O3 -g -I./include benchmark_executor.cpp src/executor_work_stealing.cpp
./a.out "$@"
