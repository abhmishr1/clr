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

# TESTS

# Test 1: presil_kernel_data
presil_kernel_data() {
    if [ "$1" = "-h" ]; then
        echo -e "\n\tHelp information for Test 'presil_kernel_data'"
        echo -e "\t----------------------------------------------"
        echo -e "\n\tUsage: ./run_tests.sh -t presil_kernel_data"
        echo -e "\twill compile and run on current device ($arch)"
        echo -e "\n\tCan pass multiple archs as arguments separated by spaces:"
        echo -e "\t./run_tests.sh -t presil_kernel_data gfx90a gfx940 gfx1100\n"
    else
        # Test name
        TEST_NAME=presil_kernel_data

        # Path to your test file
        test_file="$script_dir/unit_tests/$TEST_NAME.hip.cpp"

        # Check if the test executable exists
        if [ ! -f "$test_file" ]; then
            echo "Error: Test file not found at $test_file"
            exit 1
        fi
        archs=$(IFS=,; echo "$*")

        # Compile the file using hipcc
        $cc --offload-arch=$archs -I$include_path -o "${test_file%%.*}" "$test_file"

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
}

# Test 2: kargs_malloc
kargs_malloc() {
    if [ "$1" = "-h" ]; then
        echo -e "\n\tHelp information for Test 'kargs_malloc'"
        echo -e "\t----------------------------------------"
        echo -e "\n\tUsage: ./run_tests.sh -t kargs_malloc\n"
    else
        # Test name
        TEST_NAME=kargs_malloc

        # Path to your test file
        test_file="$script_dir/unit_tests/$TEST_NAME.hip.cpp"

        # Check if the test executable exists
        if [ ! -f "$test_file" ]; then
            echo "Error: Test file not found at $test_file"
            exit 1
        fi
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
}

# Test 3: ffm_sim_hook
ffm_sim_hook() {
    if [ "$1" = "-h" ]; then
        echo -e "\n\tHelp information for Test 'ffm_sim_hook'"
        echo -e "\t----------------------------------------"
        echo -e "\n\tUsage: ./run_tests.sh -t ffm_sim_hook"
        echo -e "\twill compile and run on current device ($arch)"
        echo -e "\n\tTo use a different arch for FFM, pass it as an argument separated by space:"
        echo -e "\t./run_tests.sh -t ffm_sim_hook gfx1200\n"
    else
        # Test name
        TEST_NAME=ffm_sim_hook

        # Path to your test file
        test_file="$script_dir/unit_tests/$TEST_NAME.hip.cpp"

        # Check if the test executable exists
        if [ ! -f "$test_file" ]; then
            echo "Error: Test file not found at $test_file"
            exit 1
        fi

        ffm_arch=$(IFS=,; echo "$*")

        # export FFM env variables
        export HIP_USE_SIM=0
        export HIP_SIM_ARCH=$ffm_arch

        # make so file path
        make_so_file="$script_dir/unit_tests/add_ons/make_hooks_so.cpp"

        # check to see if make so file exists
        if [ ! -f "$make_so_file" ]; then
            echo "Error: make .so file not found at $make_so_file"
            exit 1
        fi

        # hooks .so file path
        hook_so_file="$script_dir/unit_tests/add_ons/libhip_runtimehooks.so"

        # compile hooks cpp file in add_ons to generate an .so
        $cc -fPIC -shared -o $hook_so_file $make_so_file

        # Check if .so created
        if [ ! -f "$hook_so_file" ]; then
            echo "Error: .so not found at $hook_so_file"
            exit 1
        fi

        # Compile the file using hipcc
        $cc -Wno-unused-result --offload-arch=$ffm_arch -I$include_path -o "${test_file%%.*}" "$test_file"

        # Path to your test executable
        test_executable="$script_dir/unit_tests/$TEST_NAME"

        # Check if the test executable exists
        if [ ! -x "$test_executable" ]; then
            echo "Error: Test executable not found at $test_executable"
            exit 1
        fi

        # Add hook so path to ld_library_path
        export LD_LIBRARY_PATH=$script_dir/unit_tests/add_ons/:$LD_LIBRARY_PATH

        # Run the test executable
        echo -e "\n*********************************\n"
        echo "Running $(basename $test_executable)"
        echo -e "\n*********************************\n"
        "$test_executable"
        echo -e "\nTest $(basename $test_executable) complete\n"
    fi
}

# Check if test flag is passed as an argument
if [ "$1" = "-t" ]; then
  shift # shift arguments to ignore '-t'

    # Run the specified test or show help message
    case $1 in
        presil_kernel_data)
            shift
            if [ $# -eq 0 ]; then
                echo "No arch specified, current device used ($arch)"
                presil_kernel_data $arch
            else
                presil_kernel_data $@
            fi
            ;;
        kargs_malloc)
            kargs_malloc
            ;;
        ffm_sim_hook)
            shift
            if [ $# -eq 0 ]; then
                echo "No arch specified, current device used ($arch)"
                ffm_sim_hook $arch
            else
                ffm_sim_hook $@
            fi
            ;;
        *)
            echo -e "\nInvalid Test Name!"
            echo -e "Available tests: presil_kernel_data, kargs_malloc, ffm_sim_hook\n"
            exit 1
            ;;
    esac

else
  # Run all tests or show help message
  if [ "$1" = "-h" ]; then
    echo -e "\nHelp information for run_tests.sh"
    echo -e "----------------------------------\n"
    echo "Usage: ./run_tests.sh"
    echo "to run all tests"
    echo -e "\nTon run an individual test:\n ./run_tests.sh -t TEST_NAME"
    echo -e "\nAvailable tests: presil_kernel_data, kargs_malloc, ffm_sim_hook\n"
    presil_kernel_data -h
    kargs_malloc -h
    ffm_sim_hook -h
  else
    presil_kernel_data $arch
    kargs_malloc
    ffm_sim_hook $arch
  fi
fi

# Find all executable files and delete them
find "$script_dir/unit_tests" -type f -executable -delete
