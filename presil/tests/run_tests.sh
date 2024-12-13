#!/bin/bash
# /* Copyright (c) 2024 Advanced Micro Devices, Inc.  All rights reserved. */
# Author: Abhishek Mishra (Abhishek.Mishra2@amd.com)

# Get the directory of the script
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

# Path to clr dir
clr_dir="$( dirname $script_dir )"

# Get current arch
arch="$(rocminfo | grep -m1 "gfx" | awk '{print $2}')"

# Add presil hip to ld_library_path
export LD_LIBRARY_PATH=$clr_dir/build/hipamd/lib/:$LD_LIBRARY_PATH

# Path to your compiler
cc="/opt/rocm/bin/hipcc"

# Check if compiler exists
if [ ! -x "$cc" ]; then
    echo "Error: hipcc compiler not found in /opt/rocm/bin"
    exit 1
fi

# Include path
include_path="$clr_dir/build/hipamd/include"

# Check if include path exists
if [ ! -d "$include_path" ]; then
  echo "include directory does not exist: $include_path"
  echo "build presil clr using ../build_presil_hip_clr.py"
  echo -e "\nif you have custom paths to lib/ and include/, move it to $clr_dir/build/hipamd/lib/ and $clr_dir/build/hipamd/include/ respectively.\n"
  exit 1
fi

# Find any existing executable files and delete them
find "$script_dir/unit_tests" -type f -executable -delete

# Find and run all executables if no test specified
if [ $# -eq 0 ]; then

    # Loop through all files in the unit_tests directory
    for file in "$script_dir/unit_tests"/*
    do
    # Check if the file is a C++ source file
    if [[ "$file" == *.cpp ]]; then
        # Compile the file using hipcc
        $cc -Wno-unused-result --offload-arch=$arch -I$include_path -o "${file%%.*}" "$file"
    fi
    done

    find "$script_dir/unit_tests" -type f -executable -print0 | while IFS= read -r -d '' file; do
        echo -e "\n*********************************\n"
        echo "Running $(basename $file)"
        echo -e "\n*********************************\n"
        "$file" $arch
        echo -e "\nTest $(basename $file) complete\n"
    done
else
    # Get test name
    while getopts "t:h" opt; do
        case $opt in
            h)
            echo "Usage: ./run_tests.sh -t TEST_NAME"
            echo "Available tests: presil_kernel_data, kargs_malloc"
            exit 1
            ;;
            t)
            TEST_NAME=$OPTARG
            ;;
            \?)
            echo "Invalid option; Use -t to run a particular test" >&2
            echo "Available tests: presil_kernel_data, kargs_malloc"
            exit 1
            ;;
        esac
    done

    # Path to your test file
    test_file="$script_dir/unit_tests/$TEST_NAME.hip.cpp"

    # Check if the test executable exists
    if [ ! -f "$test_file" ]; then
        echo "Error: Test file not found at $test_file"
        echo "Available tests: presil_kernel_data, kargs_malloc"
        exit 1
    fi

    # Run an individual test
    if [ $TEST_NAME = "presil_kernel_data" ]; then

        shift $((OPTIND-1))
        if [ $# -eq 0 ]; then
            echo "No arch specified! Please specify an arch (e.g., gfx1100)"
            exit 1
        fi
        remaining_args=$(IFS=,; echo "$*")

        # Compile the file using hipcc
        $cc --offload-arch=$remaining_args -I$include_path -o "${test_file%%.*}" "$test_file"

        # Path to your test executable
        test_executable="$script_dir/unit_tests/$TEST_NAME"

        # Check if the test executable exists
        if [ ! -x "$test_executable" ]; then
            echo "Error: Test executable not found at $test_executable"
            exit 1
        fi

        # Run the test executable
        echo -e "\n******************************************************\n"
        echo "Running $(basename $test_executable) $@"
        echo -e "\n******************************************************\n"
        "$test_executable" $@
        echo -e "\nTest $(basename $test_executable) complete\n"
    fi

    if [ $TEST_NAME = "kargs_malloc" ]; then

        # Compile the file using hipcc
        $cc -Wno-unused-result -I$include_path -o "${test_file%%.*}" "$test_file"

        # Path to your test executable
        test_executable="$script_dir/unit_tests/$TEST_NAME"

        # Check if the test executable exists
        if [ ! -x "$test_executable" ]; then
            echo "Error: Test executable not found at $test_executable"
            exit 1
        fi

        # Run the test executable
        echo -e "\n*********************************\n"
        echo "Running $(basename $test_executable)"
        echo -e "\n*********************************\n"
        "$test_executable"
        echo -e "\nTest $(basename $test_executable) complete\n"
    fi
fi

# Find all executable files and delete them
find "$script_dir/unit_tests" -type f -executable -delete