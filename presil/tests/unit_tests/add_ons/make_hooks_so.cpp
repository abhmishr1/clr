/* Copyright (c) 2024 Advanced Micro Devices, Inc.  All rights reserved. */
/* Author: Abhishek Mishra (Abhishek.Mishra2@amd.com) */

#include <hip/hip_runtime.h>
#include <iostream>

extern "C" hipError_t hipLaunchKernel_sim(const uint8_t* kernelBin,
                        size_t binSize, void** kArgs, size_t kArgsSize, dim3 gridDim, dim3 blockDim, size_t sharedMemBytes, hipStream_t stream) {

    std::cout << "FFM implementation of hipLaunchKernel_sim" << std::endl;
    printf("kernel binary size: %zu \n", binSize);
    printf("kernel binary:\n");
    for (int i = 0; i < binSize; i++) {
        printf("%i ", kernelBin[i]);
    }
    printf("\nkArgs Size: %zu\n", kArgsSize);

    std::cout << "\nRan using Pre-Sil HIP .so!\n";

    return hipSuccess;
}

