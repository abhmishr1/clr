/* Copyright (c) 2024 Advanced Micro Devices, Inc.  All rights reserved. */
/* Author: Abhishek Mishra (Abhishek.Mishra2@amd.com) */

#include <hip/hip_runtime.h>
#include <iostream>
#include <cstring>

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

extern "C" hipError_t hipMalloc_sim(void** ptr, size_t sizeBytes, unsigned int flags) {

    std::cout << "FFM implementation of hipMalloc_sim" << std::endl;
    *ptr = malloc(sizeBytes);

    return hipSuccess;
}

extern "C" hipError_t hipFree_sim(void* ptr) {

    std::cout << "FFM implementation of hipFree_sim" << std::endl;
    free(ptr);

    return hipSuccess;
}

extern "C" hipError_t hipMemcpy_sim(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind) {

    std::cout << "FFM implementation of hipMemcpy_sim" << std::endl;
    std::memcpy(dst, src, sizeBytes);

    return hipSuccess;
}

extern "C" hipError_t hipMemset_sim(void* dst, int64_t value, size_t valueSize, size_t sizeBytes,
                        hipStream_t stream) {

    std::cout << "FFM implementation of hipMemset_sim" << std::endl;
    std::memset(dst, value, sizeBytes);

    return hipSuccess;
}

