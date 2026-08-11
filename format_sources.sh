#!/usr/bin/env bash

for source in  `find projects -type f -name "*.cpp" -o -name "*.hpp"`;do
	clang-format --style=file -i $source
done
