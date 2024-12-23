/* Copyright (c) 2008 - 2024 Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#include "hip_runtime_hooks.hpp"

hipError_t hipLaunchKernel_sim(const uint8_t* kernelBin, size_t binSize,
                            void** kArgs, size_t kArgsSize, dim3 gridDim,
                            dim3 blockDim, size_t sharedMemBytes, hipStream_t stream) {

  auto func_sptr = hipRuntimeHookFunc<decltype(hipLaunchKernel_sim)>("hipLaunchKernel_sim").get();

  if (func_sptr == nullptr) {
    return hipErrorInvalidValue;
  }

  hipError_t hip_error = func_sptr(kernelBin, binSize, kArgs, kArgsSize,
                                  gridDim, blockDim, sharedMemBytes, stream);

  return hip_error;
}


