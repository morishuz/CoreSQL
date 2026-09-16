#!/bin/sh
# Build isolated copies of three revisions into a single comparison executable.
set -eu
work=${1:?"Pass a scratch build directory"}
repo=$(git rev-parse --show-toplevel)
mkdir -p "$work/pre-source" "$work/before-source"
git -C "$repo" archive 4d039c2fae cpp | tar -x -C "$work/pre-source"
git -C "$repo" archive 84efa5763d cpp | tar -x -C "$work/before-source"
for variant in pre before after; do
    if [ "$variant" = after ]; then source="$repo"; else source="$work/$variant-source/cpp"; fi
    cmake -S "$source" -B "$work/$variant-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCORESQL_BENCHMARKS=OFF "-DCMAKE_CXX_FLAGS=-Dcoresql=coresql_$variant" > "$work/$variant-build.log"
    cmake --build "$work/$variant-build" --target coresql -j4 >> "$work/$variant-build.log"
    c++ -std=c++20 -O3 "-Dcoresql=coresql_$variant" "-DPROFILE_PREFIX=$variant" -I "$source/include" -c "$repo/benchmarks/update_engine.cpp" -o "$work/$variant.o"
done
c++ -std=c++20 -O3 "$repo/benchmarks/update_matched.cpp" "$work/pre.o" "$work/before.o" "$work/after.o" "$work/pre-build/libcoresql.a" "$work/before-build/libcoresql.a" "$work/after-build/libcoresql.a" -o "$work/update_matched"
