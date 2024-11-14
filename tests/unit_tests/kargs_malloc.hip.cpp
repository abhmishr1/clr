/* Copyright (c) 2024 Advanced Micro Devices, Inc.  All rights reserved. */
/* Author: Abhishek Mishra (Abhishek.Mishra2@amd.com) */

#include <hip/hip_presil_helper.h>
#include <hip/hip_ext.h>

#define startScalar (0.4)

#ifndef DWORDS_PER_LANE
#define DWORDS_PER_LANE 4
#endif
#ifndef CHUNKS_PER_BLOCK
#define CHUNKS_PER_BLOCK 1
#endif
#ifndef TBSIZE
#define TBSIZE 1024
#endif

#define check_error(status)                                                    \
  do {                                                                         \
    hipError_t err = status;                                                   \
    if (err != hipSuccess) {                                                   \
      std::cerr << "Error: " << hipGetErrorString(err) << std::endl;           \
      exit(err);                                                               \
    }                                                                          \
  } while(0)

template <typename... Args, typename F = void (*)(Args...)>
static void hipLaunchKernelWithEvents(F kernel, const dim3& numBlocks,
                           const dim3& dimBlocks, hipStream_t stream,
                           hipEvent_t startEvent, hipEvent_t stopEvent,
                           Args... args)
{
    hipExtLaunchKernelGGL(kernel, numBlocks, dimBlocks,
                        0, stream, startEvent, stopEvent, 0, args...);
    check_error(hipGetLastError());
}

std::string getDeviceName(const int device)
{
    hipDeviceProp_t props;
    check_error(hipGetDeviceProperties(&props, device));
    return std::string(props.name);
}

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

    static constexpr unsigned int chunks_per_block{CHUNKS_PER_BLOCK};
    static constexpr unsigned int elements_per_lane{
    (DWORDS_PER_LANE * sizeof(unsigned int)) < sizeof(float) ? 1 : (
    DWORDS_PER_LANE * sizeof(unsigned int) / sizeof(float))};

    const unsigned int device_index = 0;
    const unsigned int array_size = 4096;

    if (array_size % (TBSIZE * elements_per_lane * chunks_per_block) != 0)
    {
        std::stringstream ss;
        ss << "Array size must be a multiple of elements operated on per block (" <<
            TBSIZE * elements_per_lane * chunks_per_block << ").";
        throw std::runtime_error(ss.str());
    }

    const unsigned int block_cnt = array_size / (TBSIZE * elements_per_lane * chunks_per_block);
    hipEvent_t start_ev;
    hipEvent_t stop_ev;

    float *d_a, *d_b, *d_c;

    // Set device
    int count;
    check_error(hipGetDeviceCount(&count));
    if (device_index >= count)
        throw std::runtime_error("Invalid device index");
    check_error(hipSetDevice(device_index));

    // Print out device information
    std::cout << "Using HIP device " << getDeviceName(device_index) << std::endl;

    // Create device buffers
    check_error(hipMalloc(&d_a, array_size * sizeof(float)));
    check_error(hipMalloc(&d_b, array_size * sizeof(float)));
    check_error(hipMalloc(&d_c, array_size * sizeof(float)));
    check_error(hipEventCreate(&start_ev));
    check_error(hipEventCreate(&stop_ev));

    float kernel_time = 0.;

    hipLaunchKernelWithEvents(triad_kernel<elements_per_lane, chunks_per_block, float>,
                              dim3(block_cnt), dim3(TBSIZE), nullptr, start_ev,
                              stop_ev, d_a, d_b, d_c);
    check_error(hipEventSynchronize(stop_ev));
    check_error(hipEventElapsedTime(&kernel_time, start_ev, stop_ev));

    std::cout << "Stream triad kernel ran succesfully... elapsed time: " << kernel_time << " seconds" << std::endl;

    size_t szArgs = sizeof(d_a) + sizeof(d_b) + sizeof(d_c);
    std::vector<void*> kArgsVec;
    kArgsVec.push_back(d_a);
    kArgsVec.push_back(d_b);
    kArgsVec.push_back(d_c);
    void** kArgs = kArgsVec.data();

    std::vector<void*> mallocsList;

    check_error(hipGetKernelArgsMallocs(kArgs, szArgs, device_index, mallocsList));

    bool allArgsExist = true;

    for (auto arg : kArgsVec) {
      if (std::find(mallocsList.begin(), mallocsList.end(), arg) == mallocsList.end()) {
        allArgsExist = false;
        break;
      }
    }

    if (allArgsExist) {
      std::cout << "All memory allocations found for this kernel!" << std::endl;
      std::cout << "TEST SUCCESSFUL" << std::endl;
    } else {
      std::cout << "All memory allocations were not found for this kernel!" << std::endl;
      std::cout << "TEST FAILED" << std::endl;
    }

    check_error(hipFree(d_a));
    check_error(hipFree(d_b));
    check_error(hipFree(d_c));
    check_error(hipEventDestroy(start_ev));
    check_error(hipEventDestroy(stop_ev));
    return 0;
}

