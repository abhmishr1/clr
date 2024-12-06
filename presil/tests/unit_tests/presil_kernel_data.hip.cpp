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
    hipKernelData kernel_data;

    if (argc == 1) {
        std::cout << "No arch specified! Please specify an arch (e.g., gfx1100)" << std::endl;
        return 0;
    }

    for (int i = 1; i < argc; ++i) {

        hip_error = hipGetKernelData((void *) triad_kernel<2,1,float>, argv[i], kernel_data);

        if (hip_error != hipSuccess) {
            std::cout << "Ran with DEFAULT HIP. Pre-Sil HIP .so NOT used!" << std::endl;
            return 0;
        }
        std::cout << "\nArch: " << argv[i] << std::endl;
        printf("kernel binary size: %zu \n", kernel_data.kernelBin.size());
        printf("kernel binary:\n");
        for (int i = 0; i < kernel_data.kernelBin.size(); i++) {
            printf("%i ", kernel_data.kernelBin[i]);
        }
        printf("\nargs: %zu", kernel_data.kArgsSizes.size());
        for (size_t i = 0; i < kernel_data.kArgsSizes.size(); ++i) {
            printf("\n - .size:\t%i\n", kernel_data.kArgsSizes[i]);
            printf("   .offset:\t%i\n", kernel_data.kArgsOffsets[i]);
            if (i < kernel_data.kArgsAccQuals.size()) {
                switch (kernel_data.kArgsAccQuals[i])
                {
                // emum for acc qual type in presil_helper
                case READ_ONLY:
                    printf("   .access:\tread_only\n");
                    printf("   .value_kind:\tglobal_buffer\n");
                    break;
                case WRITE_ONLY:
                    printf("   .access:\twrite_only\n");
                    printf("   .value_kind:\tglobal_buffer\n");
                    break;
                case READ_WRITE:
                    printf("   .access:\tread_write\n");
                    printf("   .value_kind:\tglobal_buffer\n");
                    break;
                }
            }
        }
        printf("\n.kernarg_segment_size: %zu\n\n", kernel_data.kArgSegSize);
        // free the struct
        kernel_data.free();
    }

    std::cout << "Ran using Pre-Sil HIP .so!\n";
    return 0;
}

