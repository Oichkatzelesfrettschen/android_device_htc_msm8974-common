#!/bin/sh
set -eu

test_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_directory=$(CDPATH= cd -- "$test_directory/.." && pwd)
build_directory=$(mktemp -d)
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

cxx=${CXX:-c++}
"$cxx" -std=c++11 -Wall -Wextra -Werror -fPIC -shared \
    -I"$source_directory/../loc_api_v02" \
    "$test_directory/fake_loc_api.cpp" -o "$build_directory/libloc_api_v02.so"
"$cxx" -std=c++11 -Wall -Wextra -Werror \
    -I"$test_directory" -I"$source_directory/../loc_api_v02" \
    "$source_directory/XtraQmiInjector.cpp" \
    "$test_directory/xtra_qmi_injector_test.cpp" \
    -L"$build_directory" -lloc_api_v02 -ldl -pthread \
    -Wl,-rpath,"$build_directory" -o "$build_directory/xtra_qmi_injector_test"
LD_LIBRARY_PATH="$build_directory${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$build_directory/xtra_qmi_injector_test"
