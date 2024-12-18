#!/usr/bin/env python3
# Copyright (c) 2024 Advanced Micro Devices, Inc.  All rights reserved.
# Author: Abhishek Mishra (Abhishek.Mishra2@amd.com)

import subprocess
import os
import sys
import shutil

def pip_install(package):
    subprocess.check_call([sys.executable, "-m", "pip", "install", package])

pip_install("gitpython")
from git import Repo

# setting the directories
current_dir = os.getcwd()
presil_dir = os.path.dirname(__file__)

clr_build_dir = os.path.join(presil_dir, "build")
if os.path.isdir(clr_build_dir):
    shutil.rmtree(clr_build_dir)
os.mkdir(clr_build_dir)

# build hip clr
hip_dir = os.path.join(presil_dir, "hip")

if not os.path.isdir(hip_dir):
    os.mkdir(hip_dir)
    print("Cloning HIP for CLR...")
    Repo.clone_from("https://github.com/abhmishr1/HIP.git", hip_dir)
else:
    os.chdir(hip_dir)
    repo = Repo('.')
    origin = repo.remotes.origin
    print("HIP exists: git pull ...")
    origin.pull()

pip_install("CppHeaderParser")

print("using cmake to build CLR using HIP...")
clr_install_dir = os.path.join(clr_build_dir, "install")
os.chdir(clr_build_dir)
cmd = ["cmake", f"-DHIP_COMMON_DIR={hip_dir}", "-DHIP_PLATFORM=amd", "-DCLR_BUILD_HIP=ON", "-DCLR_BUILD_OCL=OFF", "-DCMAKE_PREFIX_PATH='/opt/rocm/;/opt/rocm/llvm/'", f"-DCMAKE_INSTALL_PREFIX={clr_install_dir}", "-DCMAKE_CXX_FLAGS='-std=c++20'", "../.."]
subprocess.run(cmd, text=True)
subprocess.run(["make -j$(nproc)"], shell=True, text=True)

os.chdir(current_dir)