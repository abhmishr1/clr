/* Copyright (c) 2024 Advanced Micro Devices, Inc.  All rights reserved. */
/* Author: Abhishek Mishra (Abhishek.Mishra2@amd.com) */

#include <hip/hip_presil_helper.h>

#define startScalar (0.4)

template<unsigned int elements_per_lane, unsigned int chunks_per_block, typename T>
  __global__ static void triad_kernel(T * __restrict a, const T * __restrict b, const T * __restrict c) {
    const auto dx = blockDim.x * gridDim.x * elements_per_lane;
    const auto gidx = (threadIdx.x + blockIdx.x * blockDim.x) * elements_per_lane;

    for (auto i = 0u; i != chunks_per_block; ++i)
    {
      for (auto j = 0u; j != elements_per_lane; ++j)
      {
        a[gidx + i * dx + j] = b[gidx + i * dx + j] + (static_cast<T>(startScalar) * c[gidx + i * dx + j]);
      }
    }
  }

int main(int argc, char* argv[]) {

    hipError_t    hip_error;
    hipKernelInfo kernel_data;

    for (int i = 1; i < argc; ++i) {
        std::cout << "\nArch: " << argv[i] << std::endl;

        hip_error = hipGetKernelData((void *) triad_kernel<2,1,float>, &kernel_data, argv[i]);

        if (hip_error != hipSuccess) {
            std::cout << "Ran with DEFAULT HIP. Pre-Sil HIP .so NOT used!" << std::endl;
            return 0;
        }
        printf("kernel binary size: %zu \n", kernel_data.binary.size);
        printf("kernel binary:\n");
        for (int i = 0; i < kernel_data.binary.size; i++) {
            printf("%i ", kernel_data.binary.data[i]);
        }
        printf("\nargs: %zu", kernel_data.kernArgsSizes.size);
        for (size_t i = 0; i < kernel_data.kernArgsSizes.size; ++i) {
            printf("\n - .size:\t%i\n", kernel_data.kernArgsSizes.data[i]);
            printf("   .offset:\t%i\n", kernel_data.kernArgsOffsets.data[i]);
            if (i < kernel_data.kernArgsAccQualifiers.size) {
                switch (kernel_data.kernArgsAccQualifiers.data[i])
                {
                case hipArgReadOnly:
                    printf("   .access:\tread_only\n");
                    printf("   .value_kind:\tglobal_buffer\n");
                    break;
                case hipArgWriteOnly:
                    printf("   .access:\twrite_only\n");
                    printf("   .value_kind:\tglobal_buffer\n");
                    break;
                case hipArgReadWrite:
                    printf("   .access:\tread_write\n");
                    printf("   .value_kind:\tglobal_buffer\n");
                    break;
                }
            }
        }

        hip_error = hipFreeKernelData(&kernel_data);
    }

    std::cout << "Hello World from CPU!\n";
    std::cout << "Ran using Pre-Sil HIP .so!\n";
    return 0;
}

