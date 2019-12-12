//
// Copyright (c) 2010 Advanced Micro Devices, Inc. All rights reserved.
//

#include "platform/commandqueue.hpp"
#include "device/gpu/gpudevice.hpp"
#include "device/gpu/gpublit.hpp"
#include "device/gpu/gpumemory.hpp"
#include "device/gpu/gpuvirtual.hpp"
#include "utils/debug.hpp"
#include <algorithm>

namespace gpu {

DmaBlitManager::DmaBlitManager(VirtualGPU& gpu, Setup setup)
    : HostBlitManager(gpu, setup),
      MinSizeForPinnedTransfer(dev().settings().pinnedMinXferSize_),
      completeOperation_(false),
      context_(NULL) {}

inline void DmaBlitManager::synchronize() const {
  if (syncOperation_) {
    gpu().waitAllEngines();
    gpu().releaseMemObjects();
  }
}

inline Memory& DmaBlitManager::gpuMem(device::Memory& mem) const {
  return static_cast<Memory&>(mem);
}

bool DmaBlitManager::readMemoryStaged(Memory& srcMemory, void* dstHost, Memory** xferBuf,
                                      size_t origin, size_t& offset, size_t& totalSize,
                                      size_t xferSize) const {
  amd::Coord3D dst(0, 0, 0);
  size_t tmpSize;
  uint idxWrite = 0;
  uint idxRead = 0;
  size_t chunkSize;
  static const bool CopyRect = false;
  // Flush DMA for ASYNC copy
  static const bool FlushDMA = true;

  if (dev().xferRead().bufSize() < 128 * Ki) {
    chunkSize = dev().xferRead().bufSize();
  } else {
    chunkSize = std::min(amd::alignUp(xferSize / 4, 256), dev().xferRead().bufSize());
    chunkSize = std::max(chunkSize, 128 * Ki);
  }

  // Find the partial transfer size
  tmpSize = std::min(chunkSize, xferSize);

  amd::Coord3D srcLast(origin + offset, 0, 0);
  amd::Coord3D copySizeLast(tmpSize, 0, 0);

  // Copy data into the temporary surface
  if (!srcMemory.partialMemCopyTo(gpu(), srcLast, dst, copySizeLast, *xferBuf[idxWrite], CopyRect,
                                  FlushDMA)) {
    return false;
  }

  totalSize -= tmpSize;
  xferSize -= tmpSize;
  offset += tmpSize;

  while (xferSize != 0) {
    // Find the partial transfer size
    tmpSize = std::min(chunkSize, xferSize);

    amd::Coord3D src(origin + offset, 0, 0);
    amd::Coord3D copySize(tmpSize, 0, 0);

    idxWrite = (idxWrite + 1) % 2;
    // Copy data into the temporary surface
    if (!srcMemory.partialMemCopyTo(gpu(), src, dst, copySize, *xferBuf[idxWrite], CopyRect,
                                    FlushDMA)) {
      return false;
    }

    // Read previous buffer
    if (!xferBuf[idxRead]->hostRead(&gpu(),
                                    reinterpret_cast<char*>(dstHost) + offset - copySizeLast[0],
                                    dst, copySizeLast)) {
      return false;
    }
    idxRead = (idxRead + 1) % 2;
    copySizeLast = copySize;

    totalSize -= tmpSize;
    xferSize -= tmpSize;
    offset += tmpSize;
  }

  // Last read
  if (!xferBuf[idxRead]->hostRead(
          &gpu(), reinterpret_cast<char*>(dstHost) + offset - copySizeLast[0], dst, copySizeLast)) {
    return false;
  }

  return true;
}

bool DmaBlitManager::readBuffer(device::Memory& srcMemory, void* dstHost,
                                const amd::Coord3D& origin, const amd::Coord3D& size,
                                bool entire) const {
  // Use host copy if memory has direct access
  if (setup_.disableReadBuffer_ ||
      (gpuMem(srcMemory).isHostMemDirectAccess() && gpuMem(srcMemory).isCacheable())) {
    return HostBlitManager::readBuffer(srcMemory, dstHost, origin, size, entire);
  } else {
    size_t srcSize = size[0];
    size_t offset = 0;
    size_t pinSize = dev().settings().pinnedXferSize_;
    pinSize = std::min(pinSize, srcSize);

    // Check if a pinned transfer can be executed
    if (pinSize && (srcSize > MinSizeForPinnedTransfer)) {
      // Allign offset to 4K boundary (Vista/Win7 limitation)
      char* tmpHost = const_cast<char*>(
          amd::alignDown(reinterpret_cast<const char*>(dstHost), PinnedMemoryAlignment));

      // Find the partial size for unaligned copy
      size_t partial = reinterpret_cast<const char*>(dstHost) - tmpHost;

      amd::Memory* pinned = NULL;
      bool first = true;
      size_t tmpSize;
      size_t pinAllocSize;

      // Copy memory, using pinning
      while (srcSize > 0) {
        // If it's the first iterarion, then readjust the copy size
        // to include alignment
        if (first) {
          pinAllocSize = amd::alignUp(pinSize + partial, PinnedMemoryAlignment);
          tmpSize = std::min(pinAllocSize - partial, srcSize);
          first = false;
        } else {
          tmpSize = std::min(pinSize, srcSize);
          pinAllocSize = amd::alignUp(tmpSize, PinnedMemoryAlignment);
          partial = 0;
        }
        amd::Coord3D dst(partial, 0, 0);
        amd::Coord3D srcPin(origin[0] + offset, 0, 0);
        amd::Coord3D copySizePin(tmpSize, 0, 0);
        size_t partial2;

        // Allocate a GPU resource for pinning
        pinned = pinHostMemory(tmpHost, pinAllocSize, partial2);

        if (pinned != NULL) {
          // Get device memory for this virtual device
          Memory* dstMemory = dev().getGpuMemory(pinned);

          if (!gpuMem(srcMemory).partialMemCopyTo(gpu(), srcPin, dst, copySizePin, *dstMemory)) {
            LogWarning("DmaBlitManager::readBuffer failed a pinned copy!");
            gpu().addPinnedMem(pinned);
            break;
          }
          gpu().addPinnedMem(pinned);
        } else {
          LogWarning("DmaBlitManager::readBuffer failed to pin a resource!");
          break;
        }
        srcSize -= tmpSize;
        offset += tmpSize;
        tmpHost = reinterpret_cast<char*>(tmpHost) + tmpSize + partial;
      }
    }

    if (0 != srcSize) {
      Memory& xferBuf0 = dev().xferRead().acquire();
      Memory& xferBuf1 = dev().xferRead().acquire();
      Memory* xferBuf[2] = {&xferBuf0, &xferBuf1};

      // Read memory using a staged resource
      if (!readMemoryStaged(gpuMem(srcMemory), dstHost, xferBuf, origin[0], offset, srcSize,
                            srcSize)) {
        LogError("DmaBlitManager::readBuffer failed!");
        return false;
      }

      dev().xferRead().release(gpu(), xferBuf1);
      dev().xferRead().release(gpu(), xferBuf0);
    }
  }

  return true;
}

bool DmaBlitManager::readBufferRect(device::Memory& srcMemory, void* dstHost,
                                    const amd::BufferRect& bufRect, const amd::BufferRect& hostRect,
                                    const amd::Coord3D& size, bool entire) const {
  // Use host copy if memory has direct access
  if (setup_.disableReadBufferRect_ ||
      (gpuMem(srcMemory).isHostMemDirectAccess() && gpuMem(srcMemory).isCacheable())) {
    return HostBlitManager::readBufferRect(srcMemory, dstHost, bufRect, hostRect, size, entire);
  } else {
    Memory& xferBuf = dev().xferRead().acquire();

    amd::Coord3D dst(0, 0, 0);
    size_t tmpSize = 0;
    size_t bufOffset;
    size_t hostOffset;
    size_t srcSize;

    for (size_t z = 0; z < size[2]; ++z) {
      for (size_t y = 0; y < size[1]; ++y) {
        srcSize = size[0];
        bufOffset = bufRect.offset(0, y, z);
        hostOffset = hostRect.offset(0, y, z);

        while (srcSize != 0) {
          // Find the partial transfer size
          tmpSize = std::min(dev().xferRead().bufSize(), srcSize);

          amd::Coord3D src(bufOffset, 0, 0);
          amd::Coord3D copySize(tmpSize, 0, 0);

          // Copy data into the temporary surface
          if (!gpuMem(srcMemory).partialMemCopyTo(gpu(), src, dst, copySize, xferBuf, true)) {
            LogError("DmaBlitManager::readBufferRect failed!");
            return false;
          }

          if (!xferBuf.hostRead(&gpu(), reinterpret_cast<char*>(dstHost) + hostOffset, dst,
                                copySize)) {
            LogError("DmaBlitManager::readBufferRect failed!");
            return false;
          }

          srcSize -= tmpSize;
          bufOffset += tmpSize;
          hostOffset += tmpSize;
        }
      }
    }
    dev().xferRead().release(gpu(), xferBuf);
  }

  return true;
}

bool DmaBlitManager::readImage(device::Memory& srcMemory, void* dstHost, const amd::Coord3D& origin,
                               const amd::Coord3D& size, size_t rowPitch, size_t slicePitch,
                               bool entire) const {
  if (setup_.disableReadImage_) {
    return HostBlitManager::readImage(srcMemory, dstHost, origin, size, rowPitch, slicePitch,
                                      entire);
  } else {
    //! @todo Add HW accelerated path
    return HostBlitManager::readImage(srcMemory, dstHost, origin, size, rowPitch, slicePitch,
                                      entire);
  }

  return true;
}

bool DmaBlitManager::writeMemoryStaged(const void* srcHost, Memory& dstMemory, Memory& xferBuf,
                                       size_t origin, size_t& offset, size_t& totalSize,
                                       size_t xferSize) const {
  amd::Coord3D src(0, 0, 0);
  size_t chunkSize;
  static const bool CopyRect = false;
  // Flush DMA for ASYNC copy
  // @todo Blocking write requires a flush to start earlier,
  // but currently VDI doesn't provide that info
  static const bool FlushDMA = false;

  if (dev().xferRead().bufSize() < 128 * Ki) {
    chunkSize = dev().xferWrite().bufSize();
  } else {
    chunkSize = std::min(amd::alignUp(xferSize / 4, 256), dev().xferWrite().bufSize());
    chunkSize = std::max(chunkSize, 128 * Ki);
  }

  while (xferSize != 0) {
    // Find the partial transfer size
    size_t tmpSize = std::min(chunkSize, xferSize);
    amd::Coord3D dst(origin + offset, 0, 0);
    amd::Coord3D copySize(tmpSize, 0, 0);

    // Copy data into the temporary buffer, using CPU
    if (!xferBuf.hostWrite(&gpu(), reinterpret_cast<const char*>(srcHost) + offset, src, copySize,
                           Resource::Discard)) {
      return false;
    }

    // Copy data into the original destination memory
    if (!xferBuf.partialMemCopyTo(gpu(), src, dst, copySize, dstMemory, CopyRect, FlushDMA)) {
      return false;
    }

    totalSize -= tmpSize;
    offset += tmpSize;
    xferSize -= tmpSize;
  }

  return true;
}

bool DmaBlitManager::writeBuffer(const void* srcHost, device::Memory& dstMemory,
                                 const amd::Coord3D& origin, const amd::Coord3D& size,
                                 bool entire) const {
  // Use host copy if memory has direct access or it's persistent
  if (setup_.disableWriteBuffer_ ||
      (gpuMem(dstMemory).isHostMemDirectAccess() &&
      (gpuMem(dstMemory).memoryType() != Resource::ExternalPhysical)) ||
      gpuMem(dstMemory).isPersistentDirectMap()) {
    return HostBlitManager::writeBuffer(srcHost, dstMemory, origin, size, entire);
  } else {
    size_t dstSize = size[0];
    size_t tmpSize = 0;
    size_t offset = 0;
    size_t pinSize = dev().settings().pinnedXferSize_;
    pinSize = std::min(pinSize, dstSize);

    // Check if a pinned transfer can be executed
    if (pinSize && (dstSize > MinSizeForPinnedTransfer)) {
      // Allign offset to 4K boundary (Vista/Win7 limitation)
      char* tmpHost = const_cast<char*>(
          amd::alignDown(reinterpret_cast<const char*>(srcHost), PinnedMemoryAlignment));

      // Find the partial size for unaligned copy
      size_t partial = reinterpret_cast<const char*>(srcHost) - tmpHost;

      amd::Memory* pinned = NULL;
      bool first = true;
      size_t tmpSize;
      size_t pinAllocSize;

      // Copy memory, using pinning
      while (dstSize > 0) {
        // If it's the first iterarion, then readjust the copy size
        // to include alignment
        if (first) {
          pinAllocSize = amd::alignUp(pinSize + partial, PinnedMemoryAlignment);
          tmpSize = std::min(pinAllocSize - partial, dstSize);
          first = false;
        } else {
          tmpSize = std::min(pinSize, dstSize);
          pinAllocSize = amd::alignUp(tmpSize, PinnedMemoryAlignment);
          partial = 0;
        }
        amd::Coord3D src(partial, 0, 0);
        amd::Coord3D dstPin(origin[0] + offset, 0, 0);
        amd::Coord3D copySizePin(tmpSize, 0, 0);
        size_t partial2;

        // Allocate a GPU resource for pinning
        pinned = pinHostMemory(tmpHost, pinAllocSize, partial2);

        if (pinned != NULL) {
          // Get device memory for this virtual device
          Memory* srcMemory = dev().getGpuMemory(pinned);

          if (!srcMemory->partialMemCopyTo(gpu(), src, dstPin, copySizePin, gpuMem(dstMemory))) {
            LogWarning("DmaBlitManager::writeBuffer failed a pinned copy!");
            gpu().addPinnedMem(pinned);
            break;
          }
          gpu().addPinnedMem(pinned);
        } else {
          LogWarning("DmaBlitManager::writeBuffer failed to pin a resource!");
          break;
        }
        dstSize -= tmpSize;
        offset += tmpSize;
        tmpHost = reinterpret_cast<char*>(tmpHost) + tmpSize + partial;
      }
    }


    if (dstSize != 0) {
      Memory& xferBuf = dev().xferWrite().acquire();

      // Write memory using a staged resource
      if (!writeMemoryStaged(srcHost, gpuMem(dstMemory), xferBuf, origin[0], offset, dstSize,
                             dstSize)) {
        LogError("DmaBlitManager::writeBuffer failed!");
        return false;
      }

      gpu().addXferWrite(xferBuf);
    }
  }

  return true;
}

bool DmaBlitManager::writeBufferRect(const void* srcHost, device::Memory& dstMemory,
                                     const amd::BufferRect& hostRect,
                                     const amd::BufferRect& bufRect, const amd::Coord3D& size,
                                     bool entire) const {
  // Use host copy if memory has direct access or it's persistent
  if (setup_.disableWriteBufferRect_ ||
      (dstMemory.isHostMemDirectAccess() &&
      (gpuMem(dstMemory).memoryType() != Resource::ExternalPhysical)) ||
      gpuMem(dstMemory).isPersistentDirectMap()) {
    return HostBlitManager::writeBufferRect(srcHost, dstMemory, hostRect, bufRect, size, entire);
  } else {
    Memory& xferBuf = dev().xferWrite().acquire();

    amd::Coord3D src(0, 0, 0);
    size_t tmpSize = 0;
    size_t bufOffset;
    size_t hostOffset;
    size_t dstSize;

    for (size_t z = 0; z < size[2]; ++z) {
      for (size_t y = 0; y < size[1]; ++y) {
        dstSize = size[0];
        bufOffset = bufRect.offset(0, y, z);
        hostOffset = hostRect.offset(0, y, z);

        while (dstSize != 0) {
          // Find the partial transfer size
          tmpSize = std::min(dev().xferWrite().bufSize(), dstSize);

          amd::Coord3D dst(bufOffset, 0, 0);
          amd::Coord3D copySize(tmpSize, 0, 0);

          // Copy data into the temporary buffer, using CPU
          if (!xferBuf.hostWrite(&gpu(), reinterpret_cast<const char*>(srcHost) + hostOffset, src,
                                 copySize, Resource::Discard)) {
            LogError("DmaBlitManager::writeBufferRect failed!");
            return false;
          }

          // Copy data into the original destination memory
          if (!xferBuf.partialMemCopyTo(gpu(), src, dst, copySize, gpuMem(dstMemory))) {
            LogError("DmaBlitManager::writeBufferRect failed!");
            return false;
          }

          dstSize -= tmpSize;
          bufOffset += tmpSize;
          hostOffset += tmpSize;
        }
      }
    }
    gpu().addXferWrite(xferBuf);
  }

  return true;
}

bool DmaBlitManager::writeImage(const void* srcHost, device::Memory& dstMemory,
                                const amd::Coord3D& origin, const amd::Coord3D& size,
                                size_t rowPitch, size_t slicePitch, bool entire) const {
  if (setup_.disableWriteImage_) {
    return HostBlitManager::writeImage(srcHost, dstMemory, origin, size, rowPitch, slicePitch,
                                       entire);
  } else {
    //! @todo Add HW accelerated path
    return HostBlitManager::writeImage(srcHost, dstMemory, origin, size, rowPitch, slicePitch,
                                       entire);
  }

  return true;
}

bool DmaBlitManager::copyBuffer(device::Memory& srcMemory, device::Memory& dstMemory,
                                const amd::Coord3D& srcOrigin, const amd::Coord3D& dstOrigin,
                                const amd::Coord3D& size, bool entire) const {
  if (setup_.disableCopyBuffer_ ||
      (gpuMem(srcMemory).isHostMemDirectAccess() && gpuMem(srcMemory).isCacheable() &&
       !dev().settings().apuSystem_ && gpuMem(dstMemory).isHostMemDirectAccess())) {
    return HostBlitManager::copyBuffer(srcMemory, dstMemory, srcOrigin, dstOrigin, size);
  } else {
    return gpuMem(srcMemory).partialMemCopyTo(gpu(), srcOrigin, dstOrigin, size, gpuMem(dstMemory));
  }

  return true;
}

bool DmaBlitManager::copyBufferRect(device::Memory& srcMemory, device::Memory& dstMemory,
                                    const amd::BufferRect& srcRect, const amd::BufferRect& dstRect,
                                    const amd::Coord3D& size, bool entire) const {
  if (setup_.disableCopyBufferRect_ ||
      (gpuMem(srcMemory).isHostMemDirectAccess() && gpuMem(srcMemory).isCacheable() &&
       gpuMem(dstMemory).isHostMemDirectAccess() &&
       (gpuMem(dstMemory).memoryType() != Resource::ExternalPhysical))) {
    return HostBlitManager::copyBufferRect(srcMemory, dstMemory, srcRect, dstRect, size, entire);
  } else {
    size_t srcOffset;
    size_t dstOffset;

    uint bytesPerElement = 16;
    bool optimalElementSize = false;
    bool subWindowRectCopy = true;

    srcOffset = srcRect.offset(0, 0, 0);
    dstOffset = dstRect.offset(0, 0, 0);

    while (bytesPerElement >= 1) {
      if (((srcOffset % 4) == 0) && ((dstOffset % 4) == 0) && ((size[0] % bytesPerElement) == 0) &&
          ((srcRect.rowPitch_ % bytesPerElement) == 0) &&
          ((srcRect.slicePitch_ % bytesPerElement) == 0) &&
          ((dstRect.rowPitch_ % bytesPerElement) == 0) &&
          ((dstRect.slicePitch_ % bytesPerElement) == 0)) {
        optimalElementSize = true;
        break;
      }
      bytesPerElement = bytesPerElement >> 1;
    }

    // 19 bit limit in HW in SI and 16 bit limit in CI+(we adjust the ElementSize to 4bytes but the
    // packet still has 14bits)
    size_t pitchLimit = dev().settings().ciPlus_ ? (0x3FFF * bytesPerElement) | 0xF : 0x7FFFF;
    size_t sizeLimit = dev().settings().ciPlus_ ? (0x3FFF * bytesPerElement) | 0xF : 0x3FFF;

    if (!optimalElementSize || (srcRect.rowPitch_ > pitchLimit) ||
        (dstRect.rowPitch_ > pitchLimit) || (size[0] > sizeLimit) ||  // See above
        (size[1] > 0x3fff) ||                                         // 14 bits limit in HW
        (size[2] > 0x7ff)) {                                          // 11 bits limit in HW
      // Restriction with rectLinearDRMDMA packet
      subWindowRectCopy = false;
    }

    if (subWindowRectCopy) {
      // Copy data with subwindow copy packet
      if (!gpuMem(srcMemory).partialMemCopyTo(
              gpu(), amd::Coord3D(srcOffset, srcRect.rowPitch_, srcRect.slicePitch_),
              amd::Coord3D(dstOffset, dstRect.rowPitch_, dstRect.slicePitch_), size,
              gpuMem(dstMemory), true, false, bytesPerElement)) {
        LogError("copyBufferRect failed!");
        return false;
      }
    } else {
      for (size_t z = 0; z < size[2]; ++z) {
        for (size_t y = 0; y < size[1]; ++y) {
          srcOffset = srcRect.offset(0, y, z);
          dstOffset = dstRect.offset(0, y, z);

          amd::Coord3D src(srcOffset, 0, 0);
          amd::Coord3D dst(dstOffset, 0, 0);
          amd::Coord3D copySize(size[0], 0, 0);

          // Copy data
          if (!gpuMem(srcMemory).partialMemCopyTo(gpu(), src, dst, copySize, gpuMem(dstMemory))) {
            LogError("copyBufferRect failed!");
            return false;
          }
        }
      }
    }
  }
  return true;
}

bool DmaBlitManager::copyImageToBuffer(device::Memory& srcMemory, device::Memory& dstMemory,
                                       const amd::Coord3D& srcOrigin, const amd::Coord3D& dstOrigin,
                                       const amd::Coord3D& size, bool entire, size_t rowPitch,
                                       size_t slicePitch) const {
  bool result = false;

  if (setup_.disableCopyImageToBuffer_) {
    result = HostBlitManager::copyImageToBuffer(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                                entire, rowPitch, slicePitch);
  } else {
    // Use CAL path for a transfer
    result =
        gpuMem(srcMemory).partialMemCopyTo(gpu(), srcOrigin, dstOrigin, size, gpuMem(dstMemory));

    // Check if a HostBlit transfer is required
    if (completeOperation_ && !result) {
      result = HostBlitManager::copyImageToBuffer(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                                  entire, rowPitch, slicePitch);
    }
  }

  return result;
}

bool DmaBlitManager::copyBufferToImage(device::Memory& srcMemory, device::Memory& dstMemory,
                                       const amd::Coord3D& srcOrigin, const amd::Coord3D& dstOrigin,
                                       const amd::Coord3D& size, bool entire, size_t rowPitch,
                                       size_t slicePitch) const {
  bool result = false;

  if (setup_.disableCopyBufferToImage_) {
    result = HostBlitManager::copyBufferToImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                                entire, rowPitch, slicePitch);
  } else {
    // Use CAL path for a transfer
    result =
        gpuMem(srcMemory).partialMemCopyTo(gpu(), srcOrigin, dstOrigin, size, gpuMem(dstMemory));

    // Check if a HostBlit transfer is required
    if (completeOperation_ && !result) {
      result = HostBlitManager::copyBufferToImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                                  entire, rowPitch, slicePitch);
    }
  }

  return result;
}

bool DmaBlitManager::copyImage(device::Memory& srcMemory, device::Memory& dstMemory,
                               const amd::Coord3D& srcOrigin, const amd::Coord3D& dstOrigin,
                               const amd::Coord3D& size, bool entire) const {
  bool result = false;

  if (setup_.disableCopyImage_) {
    return HostBlitManager::copyImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size, entire);
  } else {
    //! @todo Add HW accelerated path
    return HostBlitManager::copyImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size, entire);
  }

  return result;
}

KernelBlitManager::KernelBlitManager(VirtualGPU& gpu, Setup setup)
    : DmaBlitManager(gpu, setup),
      program_(NULL),
      constantBuffer_(NULL),
      xferBufferSize_(0),
      lockXferOps_(NULL) {
  for (uint i = 0; i < BlitTotal; ++i) {
    kernels_[i] = NULL;
  }

  for (uint i = 0; i < MaxXferBuffers; ++i) {
    xferBuffers_[i] = NULL;
  }

  completeOperation_ = false;
}

KernelBlitManager::~KernelBlitManager() {
  for (uint i = 0; i < BlitTotal; ++i) {
    if (NULL != kernels_[i]) {
      kernels_[i]->release();
    }
  }
  if (NULL != program_) {
    program_->release();
  }

  if (NULL != context_) {
    // Release a dummy context
    context_->release();
  }

  if (NULL != constantBuffer_) {
    constantBuffer_->release();
  }

  for (uint i = 0; i < MaxXferBuffers; ++i) {
    if (NULL != xferBuffers_[i]) {
      xferBuffers_[i]->release();
    }
  }

  delete lockXferOps_;
}

bool KernelBlitManager::create(amd::Device& device) {
  if (!createProgram(static_cast<Device&>(device))) {
    return false;
  }
  return true;
}

bool KernelBlitManager::createProgram(Device& device) {
  std::vector<amd::Device*> devices;
  devices.push_back(&device);

  // Save context and program for this device
  context_ = device.blitProgram()->context_;
  context_->retain();
  program_ = device.blitProgram()->program_;
  program_->retain();

  bool result = false;
  do {
    // Create kernel objects for all blits
    for (uint i = 0; i < BlitTotal; ++i) {
      const amd::Symbol* symbol = program_->findSymbol(BlitName[i]);
      if (symbol == NULL) {
        break;
      }
      kernels_[i] = new amd::Kernel(*program_, *symbol, BlitName[i]);
      if (kernels_[i] == NULL) {
        break;
      }
      // Validate blit kernels for the scratch memory usage (pre SI)
      if (!device.validateKernel(*kernels_[i], &gpu())) {
        break;
      }
    }

    result = true;
  } while (!result);

  // Create an internal constant buffer
  constantBuffer_ = new (*context_) amd::Buffer(*context_, CL_MEM_ALLOC_HOST_PTR, 4 * Ki);
  // Assign the constant buffer to the current virtual GPU
  constantBuffer_->setVirtualDevice(&gpu());
  if ((constantBuffer_ != NULL) && !constantBuffer_->create(NULL)) {
    constantBuffer_->release();
    constantBuffer_ = NULL;
    return false;
  } else if (constantBuffer_ == NULL) {
    return false;
  }

  if (dev().settings().xferBufSize_ > 0) {
    xferBufferSize_ = dev().settings().xferBufSize_;
    for (uint i = 0; i < MaxXferBuffers; ++i) {
      // Create internal xfer buffers for image copy optimization
      xferBuffers_[i] = new (*context_) amd::Buffer(*context_, 0, xferBufferSize_);
      // Assign the xfer buffer to the current virtual GPU
      xferBuffers_[i]->setVirtualDevice(&gpu());
      if ((xferBuffers_[i] != NULL) && !xferBuffers_[i]->create(NULL)) {
        xferBuffers_[i]->release();
        xferBuffers_[i] = NULL;
        return false;
      } else if (xferBuffers_[i] == NULL) {
        return false;
      }

      //! @note Workaround for conformance allocation test.
      //! Force GPU mem alloc.
      //! Unaligned images require xfer optimization,
      //! but deferred memory allocation can cause
      //! virtual heap fragmentation for big allocations and
      //! then fail the following test with 32 bit ISA, because
      //! runtime runs out of 4GB space.
      dev().getGpuMemory(xferBuffers_[i]);
    }
  }

  lockXferOps_ = new amd::Monitor("Transfer Ops Lock", true);
  if (NULL == lockXferOps_) {
    return false;
  }

  return result;
}

// The following data structures will be used for the view creations.
// Some formats has to be converted before a kernel blit operation
struct FormatConvertion {
  cl_uint clOldType_;
  cl_uint clNewType_;
};

// The list of rejected data formats and corresponding conversion
static const FormatConvertion RejectedData[] = {
    {CL_UNORM_INT8, CL_UNSIGNED_INT8},       {CL_UNORM_INT16, CL_UNSIGNED_INT16},
    {CL_SNORM_INT8, CL_UNSIGNED_INT8},       {CL_SNORM_INT16, CL_UNSIGNED_INT16},
    {CL_HALF_FLOAT, CL_UNSIGNED_INT16},      {CL_FLOAT, CL_UNSIGNED_INT32},
    {CL_SIGNED_INT8, CL_UNSIGNED_INT8},      {CL_SIGNED_INT16, CL_UNSIGNED_INT16},
    {CL_UNORM_INT_101010, CL_UNSIGNED_INT8}, {CL_SIGNED_INT32, CL_UNSIGNED_INT32}};

// The list of rejected channel's order and corresponding conversion
static const FormatConvertion RejectedOrder[] = {
    {CL_A, CL_R},        {CL_RA, CL_RG},      {CL_LUMINANCE, CL_R}, {CL_INTENSITY, CL_R},
    {CL_RGB, CL_RGBA},   {CL_BGRA, CL_RGBA},  {CL_ARGB, CL_RGBA},   {CL_sRGB, CL_RGBA},
    {CL_sRGBx, CL_RGBA}, {CL_sRGBA, CL_RGBA}, {CL_sBGRA, CL_RGBA}};

const uint RejectedFormatDataTotal = sizeof(RejectedData) / sizeof(FormatConvertion);
const uint RejectedFormatChannelTotal = sizeof(RejectedOrder) / sizeof(FormatConvertion);

bool KernelBlitManager::copyBufferToImage(device::Memory& srcMemory, device::Memory& dstMemory,
                                          const amd::Coord3D& srcOrigin,
                                          const amd::Coord3D& dstOrigin, const amd::Coord3D& size,
                                          bool entire, size_t rowPitch, size_t slicePitch) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;
  static const bool CopyRect = false;
  // Flush DMA for ASYNC copy
  static const bool FlushDMA = true;
  size_t imgRowPitch = size[0] * gpuMem(dstMemory).elementSize();
  size_t imgSlicePitch = imgRowPitch * size[1];

  if (setup_.disableCopyBufferToImage_) {
    result = DmaBlitManager::copyBufferToImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                               entire, rowPitch, slicePitch);
    synchronize();
    return result;
  }
  // Check if buffer is in system memory with direct access
  else if (gpuMem(srcMemory).isHostMemDirectAccess() &&
           (((rowPitch == 0) && (slicePitch == 0)) ||
            ((rowPitch == imgRowPitch) && ((slicePitch == 0) || (slicePitch == imgSlicePitch))))) {
    // First attempt to do this all with DMA,
    // but there are restriciton with older hardware
    if (dev().settings().imageDMA_) {
      result = DmaBlitManager::copyBufferToImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                                 entire, rowPitch, slicePitch);
      if (result) {
        synchronize();
        return result;
      }
    }

    if (!setup_.disableCopyBufferToImageOpt_) {
      // Find the overall copy size
      size_t copySize = size[0] * size[1] * size[2] * gpuMem(dstMemory).elementSize();

      // Check if double copy was requested
      if (xferBufferSize_ != 0) {
        amd::Coord3D src(srcOrigin);
        amd::Coord3D xferSrc(0, 0, 0);
        amd::Coord3D dst(dstOrigin);
        amd::Coord3D xferRect(size);
        // Find transfer size in pixels
        size_t xferSizePix = xferBufferSize_ / gpuMem(dstMemory).elementSize();
        bool transfer = true;

        // Find transfer rectangle
        if (xferRect[0] > xferSizePix) {
          // The algorithm can't break a line.
          // It requires multiple rectangles tracking
          transfer = false;
        } else {
          xferRect.c[1] = xferSizePix / xferRect[0];
        }
        // Check if we exceeded the original size boundary in Y
        if (xferRect[1] > size[1]) {
          xferRect.c[1] = size[1];
          xferRect.c[2] = xferSizePix / (xferRect[0] * xferRect[1]);
        } else {
          xferRect.c[2] = 1;
        }
        // Check if we exceeded the original size boundary in Z
        if (xferRect[2] > size[2]) {
          xferRect.c[2] = size[2];
        }
        // Make sure size in Y dimension is divided by the rectangle size
        if (size[2] > 1) {
          while ((size[1] % xferRect[1]) != 0) {
            xferRect.c[1]--;
          }
        }

        // Find one step copy size, based on the copy rectange
        amd::Coord3D oneStepSize(xferRect[0] * xferRect[1] * xferRect[2] *
                                 gpuMem(dstMemory).elementSize());

        // Initialize transfer buffer array
        Memory* xferBuf[MaxXferBuffers];
        for (uint i = 0; i < MaxXferBuffers; ++i) {
          xferBuf[i] = dev().getGpuMemory(xferBuffers_[i]);
          if (xferBuf[i] == NULL) {
            transfer = false;
            break;
          }
        }

        // Loop until we transfer all data
        while (transfer && (copySize > 0)) {
          size_t copySizeTmp = copySize;
          amd::Coord3D srcTmp(src);
          amd::Coord3D oneStepSizeTmp(oneStepSize);
          // Step 1. Initiate DRM transfer with all staging buffers
          for (uint i = 0; i < MaxXferBuffers; ++i) {
            // Make sure we don't transfer more than copy size
            if (copySizeTmp > 0) {
              if (!gpuMem(srcMemory).partialMemCopyTo(gpu(), srcTmp, xferSrc, oneStepSizeTmp,
                                                      *xferBuf[i], CopyRect, FlushDMA)) {
                transfer = false;
                break;
              }

              copySizeTmp -= oneStepSizeTmp[0];
              // Change buffer offset
              srcTmp.c[0] += oneStepSizeTmp[0];

              if (copySizeTmp < oneStepSizeTmp[0]) {
                oneStepSizeTmp.c[0] = copySizeTmp;
              }
            } else {
              break;
            }
          }

          // Step 2. Initiate compute transfer with all staging buffers
          for (uint i = 0; i < MaxXferBuffers; ++i) {
            if (copySize > 0) {
              if (!copyBufferToImageKernel(*xferBuf[i], dstMemory, xferSrc, dst, xferRect, false)) {
                transfer = false;
                break;
              }
              gpu().flushDMA(MainEngine);

              copySize -= oneStepSize[0];
              // Change buffer offset
              src.c[0] += oneStepSize[0];
              // Change image offset, ignore X offset
              for (uint j = 1; j < 3; ++j) {
                dst.c[j] += xferRect[j];
                if ((dst[j] - dstOrigin[j]) >= size[j]) {
                  dst.c[j] = dstOrigin[j];
                } else {
                  break;
                }
              }
              // Recalculate rectangle size if the remain data is smaller
              if (copySize < oneStepSize[0]) {
                for (uint j = 0; j < 3; ++j) {
                  xferRect.c[j] = size[j] - (dst[j] - dstOrigin[j]);
                }
                oneStepSize.c[0] = copySize;
              }
            } else {
              break;
            }
          }
        }

        if (copySize == 0) {
          result = true;
        } else {
          LogWarning("2 step transfer in copyBufferToImage failed");
        }
      }
    }
  }

  if (!result) {
    result = copyBufferToImageKernel(srcMemory, dstMemory, srcOrigin, dstOrigin, size, entire,
                                     rowPitch, slicePitch);
  }

  synchronize();

  return result;
}

void CalcRowSlicePitches(cl_ulong* pitch, const cl_int* copySize, size_t rowPitch,
                         size_t slicePitch, const Memory& mem) {
  size_t memFmtSize = memoryFormatSize(mem.cal()->format_).size_;
  bool img1Darray = (mem.cal()->dimension_ == GSL_MOA_TEXTURE_1D_ARRAY) ? true : false;

  if (rowPitch == 0) {
    pitch[0] = copySize[0];
  } else {
    pitch[0] = rowPitch / memFmtSize;
  }
  if (slicePitch == 0) {
    pitch[1] = pitch[0] * (img1Darray ? 1 : copySize[1]);
  } else {
    pitch[1] = slicePitch / memFmtSize;
  }
  assert((pitch[0] <= pitch[1]) && "rowPitch must be <= slicePitch");

  if (img1Darray) {
    // For 1D array rowRitch = slicePitch
    pitch[0] = pitch[1];
  }
}

static void setArgument(amd::Kernel* kernel, size_t index, size_t size, const void* value) {
  const amd::KernelParameterDescriptor& desc = kernel->signature().at(index);

  void* param = kernel->parameters().values() + desc.offset_;
  assert((desc.type_ == T_POINTER || value != NULL ||
    (desc.addressQualifier_ == CL_KERNEL_ARG_ADDRESS_LOCAL)) &&
    "not a valid local mem arg");

  uint32_t uint32_value = 0;
  uint64_t uint64_value = 0;

  if (desc.type_ == T_POINTER && (desc.addressQualifier_ != CL_KERNEL_ARG_ADDRESS_LOCAL)) {
    if ((value == NULL) || (static_cast<const cl_mem*>(value) == NULL)) {
      LP64_SWITCH(uint32_value, uint64_value) = 0;
      reinterpret_cast<Memory**>(kernel->parameters().values() +
        kernel->parameters().memoryObjOffset())[desc.info_.arrayIndex_] = nullptr;
    } else {
      // convert cl_mem to amd::Memory*, return false if invalid.
      LP64_SWITCH(uint32_value, uint64_value) = static_cast<uintptr_t>((
        *static_cast<Memory* const*>(value))->virtualAddress());
      reinterpret_cast<Memory**>(kernel->parameters().values() +
        kernel->parameters().memoryObjOffset())[desc.info_.arrayIndex_] =
        *static_cast<Memory* const*>(value);
    }
  } else if (desc.type_ == T_SAMPLER) {
    assert(false && "No sampler support in blit manager! Use internal samplers!");
  } else
    switch (desc.size_) {
      case 4:
        if (desc.addressQualifier_ == CL_KERNEL_ARG_ADDRESS_LOCAL) {
          uint32_value = size;
        } else {
          uint32_value = *static_cast<const uint32_t*>(value);
        }
        break;
      case 8:
        if (desc.addressQualifier_ == CL_KERNEL_ARG_ADDRESS_LOCAL) {
          uint64_value = size;
        } else {
          uint64_value = *static_cast<const uint64_t*>(value);
        }
        break;
      default:
        break;
    }
  switch (desc.size_) {
    case sizeof(uint32_t):
      *static_cast<uint32_t*>(param) = uint32_value;
      break;
    case sizeof(uint64_t):
      *static_cast<uint64_t*>(param) = uint64_value;
      break;
    default:
      ::memcpy(param, value, size);
      break;
  }
}

bool KernelBlitManager::copyBufferToImageKernel(device::Memory& srcMemory,
                                                device::Memory& dstMemory,
                                                const amd::Coord3D& srcOrigin,
                                                const amd::Coord3D& dstOrigin,
                                                const amd::Coord3D& size, bool entire,
                                                size_t rowPitch, size_t slicePitch) const {
  bool rejected = false;
  Memory* dstView = &gpuMem(dstMemory);
  bool releaseView = false;
  bool result = false;
  CalFormat imgFormat;
  imgFormat.channelOrder_ = gpuMem(dstMemory).cal()->channelOrder_;
  imgFormat.type_ = gpuMem(dstMemory).cal()->format_;
  amd::Image::Format newFormat(dev().getOclFormat(imgFormat));

  // Find unsupported formats
  for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
    if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
      newFormat.image_channel_data_type = RejectedData[i].clNewType_;
      rejected = true;
      break;
    }
  }

  // Find unsupported channel's order
  for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
    if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
      newFormat.image_channel_order = RejectedOrder[i].clNewType_;
      rejected = true;
      break;
    }
  }

  // If the image format was rejected, then attempt to create a view
  if (rejected) {
    dstView = createView(gpuMem(dstMemory), dev().getCalFormat(newFormat));
    if (dstView != NULL) {
      rejected = false;
      releaseView = true;
    }
  }

  // Fall into the host path if the image format was rejected
  if (rejected) {
    return HostBlitManager::copyBufferToImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                              entire);
  }

  // Use a common blit type with three dimensions by default
  uint blitType = BlitCopyBufferToImage;
  size_t dim = 0;
  size_t globalWorkOffset[3] = {0, 0, 0};
  size_t globalWorkSize[3];
  size_t localWorkSize[3];

  // Program the kernels workload depending on the blit dimensions
  dim = 3;
  if (gpuMem(dstMemory).cal()->dimSize_ == 1) {
    globalWorkSize[0] = amd::alignUp(size[0], 256);
    globalWorkSize[1] = amd::alignUp(size[1], 1);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = 256;
    localWorkSize[1] = localWorkSize[2] = 1;
  } else if (gpuMem(dstMemory).cal()->dimSize_ == 2) {
    globalWorkSize[0] = amd::alignUp(size[0], 16);
    globalWorkSize[1] = amd::alignUp(size[1], 16);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = localWorkSize[1] = 16;
    localWorkSize[2] = 1;
  } else {
    globalWorkSize[0] = amd::alignUp(size[0], 8);
    globalWorkSize[1] = amd::alignUp(size[1], 8);
    globalWorkSize[2] = amd::alignUp(size[2], 4);
    localWorkSize[0] = localWorkSize[1] = 8;
    localWorkSize[2] = 4;
  }

  // Program kernels arguments for the blit operation
  Memory* mem = &gpuMem(srcMemory);
  setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
  mem = dstView;
  setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);
  const MemFormatStruct& memFmt = memoryFormatSize(gpuMem(dstMemory).cal()->format_);

  // 1 element granularity for writes by default
  cl_int granularity = 1;
  if (memFmt.size_ == 2) {
    granularity = 2;
  } else if (memFmt.size_ >= 4) {
    granularity = 4;
  }
  CondLog(((srcOrigin[0] % granularity) != 0), "Unaligned offset in blit!");
  cl_ulong srcOrg[4] = {srcOrigin[0] / granularity, srcOrigin[1], srcOrigin[2], 0};
  setArgument(kernels_[blitType], 2, sizeof(srcOrg), srcOrg);

  cl_int dstOrg[4] = {(cl_int)dstOrigin[0], (cl_int)dstOrigin[1], (cl_int)dstOrigin[2], 0};
  cl_int copySize[4] = {(cl_int)size[0], (cl_int)size[1], (cl_int)size[2], 0};

  setArgument(kernels_[blitType], 3, sizeof(dstOrg), dstOrg);
  setArgument(kernels_[blitType], 4, sizeof(copySize), copySize);

  // Program memory format
  uint multiplier = memFmt.size_ / sizeof(uint32_t);
  multiplier = (multiplier == 0) ? 1 : multiplier;
  cl_uint format[4] = {memFmt.components_, memFmt.size_ / memFmt.components_, multiplier, 0};
  setArgument(kernels_[blitType], 5, sizeof(format), format);

  // Program row and slice pitches
  cl_ulong pitch[4] = {0};
  CalcRowSlicePitches(pitch, copySize, rowPitch, slicePitch, gpuMem(dstMemory));
  setArgument(kernels_[blitType], 6, sizeof(pitch), pitch);

  // Create ND range object for the kernel's execution
  amd::NDRangeContainer ndrange(dim, globalWorkOffset, globalWorkSize, localWorkSize);

  // Execute the blit
  address parameters = kernels_[blitType]->parameters().values();
  result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters);
  if (releaseView) {
    delete dstView;
  }

  return result;
}

bool KernelBlitManager::copyImageToBuffer(device::Memory& srcMemory, device::Memory& dstMemory,
                                          const amd::Coord3D& srcOrigin,
                                          const amd::Coord3D& dstOrigin, const amd::Coord3D& size,
                                          bool entire, size_t rowPitch, size_t slicePitch) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;
  static const bool CopyRect = false;
  // Flush DMA for ASYNC copy
  static const bool FlushDMA = true;
  size_t imgRowPitch = size[0] * gpuMem(srcMemory).elementSize();
  size_t imgSlicePitch = imgRowPitch * size[1];

  if (setup_.disableCopyImageToBuffer_) {
    result = HostBlitManager::copyImageToBuffer(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                                entire, rowPitch, slicePitch);
    synchronize();
    return result;
  }
  // Check if buffer is in system memory with direct access
  else if (gpuMem(dstMemory).isHostMemDirectAccess() &&
           (((rowPitch == 0) && (slicePitch == 0)) ||
            ((rowPitch == imgRowPitch) && ((slicePitch == 0) || (slicePitch == imgSlicePitch))))) {
    // First attempt to do this all with DMA,
    // but there are restriciton with older hardware
    // If the dest buffer is external physical(SDI), copy two step as
    // single step SDMA is causing corruption and the cause is under investigation
    if (dev().settings().imageDMA_ &&
        gpuMem(dstMemory).memoryType() != Resource::ExternalPhysical) {
      result = DmaBlitManager::copyImageToBuffer(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                                 entire, rowPitch, slicePitch);
      if (result) {
        synchronize();
        return result;
      }
    }

    // Find the overall copy size
    size_t copySize = size[0] * size[1] * size[2] * gpuMem(srcMemory).elementSize();

    // Check if double copy was requested
    if (xferBufferSize_ != 0) {
      amd::Coord3D src(srcOrigin);
      amd::Coord3D dst(dstOrigin);
      amd::Coord3D xferDst(0, 0, 0);
      amd::Coord3D xferRect(size);
      // Find transfer size in pixels
      size_t xferSizePix = xferBufferSize_ / gpuMem(srcMemory).elementSize();
      bool transfer = true;

      // Find transfer rectangle
      if (xferRect[0] > xferSizePix) {
        // The algorithm can't break a line.
        // It requires multiple rectangles tracking
        transfer = false;
      } else {
        xferRect.c[1] = xferSizePix / xferRect[0];
      }
      // Check if we exceeded the original size boundary in Y
      if (xferRect[1] > size[1]) {
        xferRect.c[1] = size[1];
        xferRect.c[2] = xferSizePix / (xferRect[0] * xferRect[1]);
      } else {
        xferRect.c[2] = 1;
      }
      // Check if we exceeded the original size boundary in Z
      if (xferRect[2] > size[2]) {
        xferRect.c[2] = size[2];
      }
      // Make sure size in Y dimension is divided by the rectangle size
      if (size[2] > 1) {
        while ((size[1] % xferRect[1]) != 0) {
          xferRect.c[1]--;
        }
      }

      // Find one step copy size, based on the copy rectange
      amd::Coord3D oneStepSize(xferRect[0] * xferRect[1] * xferRect[2] *
                               gpuMem(srcMemory).elementSize());

      // Initialize transfer buffer array
      Memory* xferBuf[MaxXferBuffers];
      for (uint i = 0; i < MaxXferBuffers; ++i) {
        xferBuf[i] = dev().getGpuMemory(xferBuffers_[i]);
        if (xferBuf[i] == NULL) {
          transfer = false;
          break;
        }
      }

      // Loop until we transfer all data
      while (transfer && (copySize > 0)) {
        size_t copySizeTmp = copySize;
        amd::Coord3D srcTmp(src);
        amd::Coord3D oneStepSizeTmp(oneStepSize);
        amd::Coord3D xferRectTmp(xferRect);

        // Step 1. Initiate compute transfer with all staging buffers
        for (uint i = 0; i < MaxXferBuffers; ++i) {
          if (copySizeTmp > 0) {
            if (!copyImageToBufferKernel(srcMemory, *xferBuf[i], srcTmp, xferDst, xferRectTmp,
                                         false)) {
              transfer = false;
              break;
            }
            gpu().flushDMA(MainEngine);

            copySizeTmp -= oneStepSizeTmp[0];
            // Change image offset, ignore X offset
            for (uint j = 1; j < 3; ++j) {
              srcTmp.c[j] += xferRectTmp[j];
              if ((srcTmp[j] - srcOrigin[j]) >= size[j]) {
                srcTmp.c[j] = srcOrigin[j];
              } else {
                break;
              }
            }
            // Recalculate rectangle size if the remain data is smaller
            if (copySizeTmp < oneStepSizeTmp[0]) {
              for (uint j = 0; j < 3; ++j) {
                xferRectTmp.c[j] = size[j] - (srcTmp[j] - srcOrigin[j]);
              }
            }
          } else {
            break;
          }
        }

        // Step 2. Initiate DRM transfer with all staging buffers
        for (uint i = 0; i < MaxXferBuffers; ++i) {
          // Make sure we don't transfer more than copy size
          if (copySize > 0) {
            if (!xferBuf[i]->partialMemCopyTo(gpu(), xferDst, dst, oneStepSize, gpuMem(dstMemory),
                                              CopyRect, FlushDMA)) {
              transfer = false;
              break;
            }

            copySize -= oneStepSize[0];
            // Change buffer offset
            dst.c[0] += oneStepSize[0];
            // Change image offset, ignore X offset
            for (uint j = 1; j < 3; ++j) {
              src.c[j] += xferRect[j];
              if ((src[j] - srcOrigin[j]) >= size[j]) {
                src.c[j] = srcOrigin[j];
              } else {
                break;
              }
            }
            // Recalculate rectangle size if the remain data is smaller
            if (copySize < oneStepSize[0]) {
              for (uint j = 0; j < 3; ++j) {
                xferRect.c[j] = size[j] - (src[j] - srcOrigin[j]);
              }
              oneStepSize.c[0] = copySize;
            }
          } else {
            break;
          }
        }
      }

      if (copySize == 0) {
        result = true;
      } else {
        LogWarning("2 step transfer in copyBufferToImage failed");
      }
    }
  }

  if (!result) {
    result = copyImageToBufferKernel(srcMemory, dstMemory, srcOrigin, dstOrigin, size, entire,
                                     rowPitch, slicePitch);
  }

  synchronize();

  return result;
}

bool KernelBlitManager::copyImageToBufferKernel(device::Memory& srcMemory,
                                                device::Memory& dstMemory,
                                                const amd::Coord3D& srcOrigin,
                                                const amd::Coord3D& dstOrigin,
                                                const amd::Coord3D& size, bool entire,
                                                size_t rowPitch, size_t slicePitch) const {
  bool rejected = false;
  Memory* srcView = &gpuMem(srcMemory);
  bool releaseView = false;
  bool result = false;
  CalFormat imgFormat;
  imgFormat.channelOrder_ = gpuMem(srcMemory).cal()->channelOrder_;
  imgFormat.type_ = gpuMem(srcMemory).cal()->format_;
  amd::Image::Format newFormat(dev().getOclFormat(imgFormat));

  // Find unsupported formats
  for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
    if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
      newFormat.image_channel_data_type = RejectedData[i].clNewType_;
      rejected = true;
      break;
    }
  }

  // Find unsupported channel's order
  for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
    if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
      newFormat.image_channel_order = RejectedOrder[i].clNewType_;
      rejected = true;
      break;
    }
  }

  // If the image format was rejected, then attempt to create a view
  if (rejected) {
    srcView = createView(gpuMem(srcMemory), dev().getCalFormat(newFormat));
    if (srcView != NULL) {
      rejected = false;
      releaseView = true;
    }
  }

  // Fall into the host path if the image format was rejected
  if (rejected) {
    return HostBlitManager::copyImageToBuffer(srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                                              entire);
  }

  uint blitType = BlitCopyImageToBuffer;
  size_t dim = 0;
  size_t globalWorkOffset[3] = {0, 0, 0};
  size_t globalWorkSize[3];
  size_t localWorkSize[3];

  // Program the kernels workload depending on the blit dimensions
  dim = 3;
  // Find the current blit type
  if (gpuMem(srcMemory).cal()->dimSize_ == 1) {
    globalWorkSize[0] = amd::alignUp(size[0], 256);
    globalWorkSize[1] = amd::alignUp(size[1], 1);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = 256;
    localWorkSize[1] = localWorkSize[2] = 1;
  } else if (gpuMem(srcMemory).cal()->dimSize_ == 2) {
    globalWorkSize[0] = amd::alignUp(size[0], 16);
    globalWorkSize[1] = amd::alignUp(size[1], 16);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = localWorkSize[1] = 16;
    localWorkSize[2] = 1;
  } else {
    globalWorkSize[0] = amd::alignUp(size[0], 8);
    globalWorkSize[1] = amd::alignUp(size[1], 8);
    globalWorkSize[2] = amd::alignUp(size[2], 4);
    localWorkSize[0] = localWorkSize[1] = 8;
    localWorkSize[2] = 4;
  }

  // Program kernels arguments for the blit operation
  Memory* mem = srcView;
  setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
  mem = &gpuMem(dstMemory);
  setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);

  // Update extra paramters for USHORT and UBYTE pointers.
  // Only then compiler can optimize the kernel to use
  // UAV Raw for other writes
  setArgument(kernels_[blitType], 2, sizeof(cl_mem), &mem);
  setArgument(kernels_[blitType], 3, sizeof(cl_mem), &mem);

  cl_int srcOrg[4] = {(cl_int)srcOrigin[0], (cl_int)srcOrigin[1], (cl_int)srcOrigin[2], 0};
  cl_int copySize[4] = {(cl_int)size[0], (cl_int)size[1], (cl_int)size[2], 0};
  setArgument(kernels_[blitType], 4, sizeof(srcOrg), srcOrg);
  const MemFormatStruct& memFmt = memoryFormatSize(gpuMem(srcMemory).cal()->format_);

  // 1 element granularity for writes by default
  cl_int granularity = 1;
  if (memFmt.size_ == 2) {
    granularity = 2;
  } else if (memFmt.size_ >= 4) {
    granularity = 4;
  }
  CondLog(((dstOrigin[0] % granularity) != 0), "Unaligned offset in blit!");
  cl_ulong dstOrg[4] = {dstOrigin[0] / granularity, dstOrigin[1], dstOrigin[2], 0};
  setArgument(kernels_[blitType], 5, sizeof(dstOrg), dstOrg);
  setArgument(kernels_[blitType], 6, sizeof(copySize), copySize);

  // Program memory format
  uint multiplier = memFmt.size_ / sizeof(uint32_t);
  multiplier = (multiplier == 0) ? 1 : multiplier;
  cl_uint format[4] = {memFmt.components_, memFmt.size_ / memFmt.components_, multiplier, 0};
  setArgument(kernels_[blitType], 7, sizeof(format), format);

  // Program row and slice pitches
  cl_ulong pitch[4] = {0};
  CalcRowSlicePitches(pitch, copySize, rowPitch, slicePitch, gpuMem(srcMemory));
  setArgument(kernels_[blitType], 8, sizeof(pitch), pitch);

  // Create ND range object for the kernel's execution
  amd::NDRangeContainer ndrange(dim, globalWorkOffset, globalWorkSize, localWorkSize);

  // Execute the blit
  address parameters = kernels_[blitType]->parameters().values();
  result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters);
  if (releaseView) {
    delete srcView;
  }

  return result;
}

bool KernelBlitManager::copyImage(device::Memory& srcMemory, device::Memory& dstMemory,
                                  const amd::Coord3D& srcOrigin, const amd::Coord3D& dstOrigin,
                                  const amd::Coord3D& size, bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool rejected = false;
  Memory* srcView = &gpuMem(srcMemory);
  Memory* dstView = &gpuMem(dstMemory);
  bool releaseView = false;
  bool result = false;
  CalFormat imgFormat;
  imgFormat.channelOrder_ = gpuMem(srcMemory).cal()->channelOrder_;
  imgFormat.type_ = gpuMem(srcMemory).cal()->format_;
  amd::Image::Format newFormat(dev().getOclFormat(imgFormat));

  // Find unsupported formats
  for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
    if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
      newFormat.image_channel_data_type = RejectedData[i].clNewType_;
      rejected = true;
      break;
    }
  }

  // Search for the rejected channel's order only if the format was rejected
  // Note: Image blit is independent from the channel order
  if (rejected) {
    for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
      if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
        newFormat.image_channel_order = RejectedOrder[i].clNewType_;
        rejected = true;
        break;
      }
    }
  }

  // Attempt to create a view if the format was rejected
  if (rejected) {
    srcView = createView(gpuMem(srcMemory), dev().getCalFormat(newFormat));
    if (srcView != NULL) {
      dstView = createView(gpuMem(dstMemory), dev().getCalFormat(newFormat));
      if (dstView != NULL) {
        rejected = false;
        releaseView = true;
      } else {
        delete srcView;
      }
    }
  }

  // Fall into the host path for the entire 2D copy or
  // if the image format was rejected
  if (rejected) {
    result = HostBlitManager::copyImage(srcMemory, dstMemory, srcOrigin, dstOrigin, size, entire);
    synchronize();
    return result;
  }

  uint blitType = BlitCopyImage;
  size_t dim = 0;
  size_t globalWorkOffset[3] = {0, 0, 0};
  size_t globalWorkSize[3];
  size_t localWorkSize[3];

  // Program the kernels workload depending on the blit dimensions
  dim = 3;
  // Find the current blit type
  if ((gpuMem(srcMemory).cal()->dimSize_ == 1) || (gpuMem(dstMemory).cal()->dimSize_ == 1)) {
    globalWorkSize[0] = amd::alignUp(size[0], 256);
    globalWorkSize[1] = amd::alignUp(size[1], 1);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = 256;
    localWorkSize[1] = localWorkSize[2] = 1;
  } else if ((gpuMem(srcMemory).cal()->dimSize_ == 2) || (gpuMem(dstMemory).cal()->dimSize_ == 2)) {
    globalWorkSize[0] = amd::alignUp(size[0], 16);
    globalWorkSize[1] = amd::alignUp(size[1], 16);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = localWorkSize[1] = 16;
    localWorkSize[2] = 1;
  } else {
    globalWorkSize[0] = amd::alignUp(size[0], 8);
    globalWorkSize[1] = amd::alignUp(size[1], 8);
    globalWorkSize[2] = amd::alignUp(size[2], 4);
    localWorkSize[0] = localWorkSize[1] = 8;
    localWorkSize[2] = 4;
  }

  // The current OpenCL spec allows "copy images from a 1D image
  // array object to a 1D image array object" only.
  if ((gpuMem(srcMemory).cal()->dimension_ == GSL_MOA_TEXTURE_1D_ARRAY) ||
      (gpuMem(dstMemory).cal()->dimension_ == GSL_MOA_TEXTURE_1D_ARRAY)) {
    blitType = BlitCopyImage1DA;
  }

  // Program kernels arguments for the blit operation
  Memory* mem = srcView;
  setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
  mem = dstView;
  setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);

  // Program source origin
  cl_int srcOrg[4] = {(cl_int)srcOrigin[0], (cl_int)srcOrigin[1], (cl_int)srcOrigin[2], 0};
  setArgument(kernels_[blitType], 2, sizeof(srcOrg), srcOrg);

  // Program destinaiton origin
  cl_int dstOrg[4] = {(cl_int)dstOrigin[0], (cl_int)dstOrigin[1], (cl_int)dstOrigin[2], 0};
  setArgument(kernels_[blitType], 3, sizeof(dstOrg), dstOrg);

  cl_int copySize[4] = {(cl_int)size[0], (cl_int)size[1], (cl_int)size[2], 0};
  setArgument(kernels_[blitType], 4, sizeof(copySize), copySize);

  // Create ND range object for the kernel's execution
  amd::NDRangeContainer ndrange(dim, globalWorkOffset, globalWorkSize, localWorkSize);

  // Execute the blit
  address parameters = kernels_[blitType]->parameters().values();
  result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters);
  if (releaseView) {
    delete srcView;
    delete dstView;
  }

  synchronize();

  return result;
}

void FindPinSize(size_t& pinSize, const amd::Coord3D& size, size_t& rowPitch, size_t& slicePitch,
                 const Memory& mem) {
  pinSize = size[0] * mem.elementSize();
  if ((rowPitch == 0) || (rowPitch == pinSize)) {
    rowPitch = 0;
  } else {
    pinSize = rowPitch;
  }

  // Calculate the pin size, which should be equal to the copy size
  for (uint i = 1; i < mem.cal()->dimSize_; ++i) {
    pinSize *= size[i];
    if (i == 1) {
      if ((slicePitch == 0) || (slicePitch == pinSize)) {
        slicePitch = 0;
      } else {
        if (mem.cal()->dimension_ != GSL_MOA_TEXTURE_1D_ARRAY) {
          pinSize = slicePitch;
        } else {
          pinSize = slicePitch * size[i];
        }
      }
    }
  }
}

bool KernelBlitManager::readImage(device::Memory& srcMemory, void* dstHost,
                                  const amd::Coord3D& origin, const amd::Coord3D& size,
                                  size_t rowPitch, size_t slicePitch, bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  // Use host copy if memory has direct access or it's persistent
  if (setup_.disableReadImage_ ||
      (gpuMem(srcMemory).isHostMemDirectAccess() && gpuMem(srcMemory).isCacheable())) {
    result =
        HostBlitManager::readImage(srcMemory, dstHost, origin, size, rowPitch, slicePitch, entire);
    synchronize();
    return result;
  } else {
    size_t pinSize;
    FindPinSize(pinSize, size, rowPitch, slicePitch, gpuMem(srcMemory));

    size_t partial;
    amd::Memory* amdMemory = pinHostMemory(dstHost, pinSize, partial);

    if (amdMemory == NULL) {
      // Force SW copy
      result = HostBlitManager::readImage(srcMemory, dstHost, origin, size, rowPitch, slicePitch,
                                          entire);
      synchronize();
      return result;
    }

    // Readjust destination offset
    const amd::Coord3D dstOrigin(partial);

    // Get device memory for this virtual device
    Memory* dstMemory = dev().getGpuMemory(amdMemory);

    // Copy image to buffer
    result = copyImageToBuffer(srcMemory, *dstMemory, origin, dstOrigin, size, entire, rowPitch,
                               slicePitch);

    // Add pinned memory for a later release
    gpu().addPinnedMem(amdMemory);
  }

  synchronize();

  return result;
}

bool KernelBlitManager::writeImage(const void* srcHost, device::Memory& dstMemory,
                                   const amd::Coord3D& origin, const amd::Coord3D& size,
                                   size_t rowPitch, size_t slicePitch, bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  // Use host copy if memory has direct access or it's persistent
  if (setup_.disableWriteImage_ || gpuMem(dstMemory).isHostMemDirectAccess() ||
      gpuMem(dstMemory).isPersistentDirectMap()) {
    result =
        HostBlitManager::writeImage(srcHost, dstMemory, origin, size, rowPitch, slicePitch, entire);
    synchronize();
    return result;
  } else {
    size_t pinSize;
    FindPinSize(pinSize, size, rowPitch, slicePitch, gpuMem(dstMemory));

    size_t partial;
    amd::Memory* amdMemory = pinHostMemory(srcHost, pinSize, partial);

    if (amdMemory == NULL) {
      // Force SW copy
      result = HostBlitManager::writeImage(srcHost, dstMemory, origin, size, rowPitch, slicePitch,
                                           entire);
      synchronize();
      return result;
    }

    // Readjust destination offset
    const amd::Coord3D srcOrigin(partial);

    // Get device memory for this virtual device
    Memory* srcMemory = dev().getGpuMemory(amdMemory);

    // Copy image to buffer
    result = copyBufferToImage(*srcMemory, dstMemory, srcOrigin, origin, size, entire, rowPitch,
                               slicePitch);

    // Add pinned memory for a later release
    gpu().addPinnedMem(amdMemory);
  }

  synchronize();

  return result;
}

bool KernelBlitManager::copyBufferRect(device::Memory& srcMemory, device::Memory& dstMemory,
                                       const amd::BufferRect& srcRectIn,
                                       const amd::BufferRect& dstRectIn, const amd::Coord3D& sizeIn,
                                       bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;
  bool rejected = false;

  // Fall into the CAL path for rejected transfers
  if (setup_.disableCopyBufferRect_ || gpuMem(srcMemory).isHostMemDirectAccess() ||
      gpuMem(dstMemory).isHostMemDirectAccess()) {
    result =
        DmaBlitManager::copyBufferRect(srcMemory, dstMemory, srcRectIn, dstRectIn, sizeIn, entire);

    if (result) {
      synchronize();
      return result;
    }
  }

  uint blitType = BlitCopyBufferRect;
  size_t dim = 3;
  size_t globalWorkOffset[3] = {0, 0, 0};
  size_t globalWorkSize[3];
  size_t localWorkSize[3];

  const static uint CopyRectAlignment[3] = {16, 4, 1};

  uint i;
  for (i = 0; i < sizeof(CopyRectAlignment) / sizeof(uint); i++) {
    // Check source alignments
    bool aligned = ((srcRectIn.rowPitch_ % CopyRectAlignment[i]) == 0);
    aligned &= ((srcRectIn.slicePitch_ % CopyRectAlignment[i]) == 0);
    aligned &= ((srcRectIn.start_ % CopyRectAlignment[i]) == 0);

    // Check destination alignments
    aligned &= ((dstRectIn.rowPitch_ % CopyRectAlignment[i]) == 0);
    aligned &= ((dstRectIn.slicePitch_ % CopyRectAlignment[i]) == 0);
    aligned &= ((dstRectIn.start_ % CopyRectAlignment[i]) == 0);

    // Check copy size alignment in the first dimension
    aligned &= ((sizeIn[0] % CopyRectAlignment[i]) == 0);

    if (aligned) {
      if (CopyRectAlignment[i] != 1) {
        blitType = BlitCopyBufferRectAligned;
      }
      break;
    }
  }

  amd::BufferRect srcRect;
  amd::BufferRect dstRect;
  amd::Coord3D size(sizeIn[0], sizeIn[1], sizeIn[2]);

  srcRect.rowPitch_ = srcRectIn.rowPitch_ / CopyRectAlignment[i];
  srcRect.slicePitch_ = srcRectIn.slicePitch_ / CopyRectAlignment[i];
  srcRect.start_ = srcRectIn.start_ / CopyRectAlignment[i];
  srcRect.end_ = srcRectIn.end_ / CopyRectAlignment[i];

  dstRect.rowPitch_ = dstRectIn.rowPitch_ / CopyRectAlignment[i];
  dstRect.slicePitch_ = dstRectIn.slicePitch_ / CopyRectAlignment[i];
  dstRect.start_ = dstRectIn.start_ / CopyRectAlignment[i];
  dstRect.end_ = dstRectIn.end_ / CopyRectAlignment[i];

  size.c[0] /= CopyRectAlignment[i];

  // Program the kernel's workload depending on the transfer dimensions
  if ((size[1] == 1) && (size[2] == 1)) {
    globalWorkSize[0] = amd::alignUp(size[0], 256);
    globalWorkSize[1] = 1;
    globalWorkSize[2] = 1;
    localWorkSize[0] = 256;
    localWorkSize[1] = 1;
    localWorkSize[2] = 1;
  } else if (size[2] == 1) {
    globalWorkSize[0] = amd::alignUp(size[0], 16);
    globalWorkSize[1] = amd::alignUp(size[1], 16);
    globalWorkSize[2] = 1;
    localWorkSize[0] = localWorkSize[1] = 16;
    localWorkSize[2] = 1;
  } else {
    globalWorkSize[0] = amd::alignUp(size[0], 8);
    globalWorkSize[1] = amd::alignUp(size[1], 8);
    globalWorkSize[2] = amd::alignUp(size[2], 4);
    localWorkSize[0] = localWorkSize[1] = 8;
    localWorkSize[2] = 4;
  }


  // Program kernels arguments for the blit operation
  Memory* mem = &gpuMem(srcMemory);
  setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
  mem = &gpuMem(dstMemory);
  setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);
  cl_ulong src[4] = {srcRect.rowPitch_, srcRect.slicePitch_, srcRect.start_, 0};
  setArgument(kernels_[blitType], 2, sizeof(src), src);
  cl_ulong dst[4] = {dstRect.rowPitch_, dstRect.slicePitch_, dstRect.start_, 0};
  setArgument(kernels_[blitType], 3, sizeof(dst), dst);
  cl_ulong copySize[4] = {size[0], size[1], size[2], CopyRectAlignment[i]};
  setArgument(kernels_[blitType], 4, sizeof(copySize), copySize);

  // Create ND range object for the kernel's execution
  amd::NDRangeContainer ndrange(dim, globalWorkOffset, globalWorkSize, localWorkSize);

  // Execute the blit
  address parameters = kernels_[blitType]->parameters().values();
  result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters);

  synchronize();

  return result;
}

bool KernelBlitManager::readBuffer(device::Memory& srcMemory, void* dstHost,
                                   const amd::Coord3D& origin, const amd::Coord3D& size,
                                   bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;
  // Use host copy if memory has direct access
  if (setup_.disableReadBuffer_ ||
      (gpuMem(srcMemory).isHostMemDirectAccess() && gpuMem(srcMemory).isCacheable())) {
    result = HostBlitManager::readBuffer(srcMemory, dstHost, origin, size, entire);
    synchronize();
    return result;
  } else {
    size_t pinSize = size[0];
    // Check if a pinned transfer can be executed with a single pin
    if ((pinSize <= dev().settings().pinnedXferSize_) && (pinSize > MinSizeForPinnedTransfer)) {
      size_t partial;
      amd::Memory* amdMemory = pinHostMemory(dstHost, pinSize, partial);

      if (amdMemory == NULL) {
        // Force SW copy
        result = HostBlitManager::readBuffer(srcMemory, dstHost, origin, size, entire);
        synchronize();
        return result;
      }

      // Readjust host mem offset
      amd::Coord3D dstOrigin(partial);

      // Get device memory for this virtual device
      Memory* dstMemory = dev().getGpuMemory(amdMemory);

      // Copy image to buffer
      result = copyBuffer(srcMemory, *dstMemory, origin, dstOrigin, size, entire);

      // Add pinned memory for a later release
      gpu().addPinnedMem(amdMemory);
    } else {
      result = DmaBlitManager::readBuffer(srcMemory, dstHost, origin, size, entire);
    }
  }

  synchronize();

  return result;
}

bool KernelBlitManager::readBufferRect(device::Memory& srcMemory, void* dstHost,
                                       const amd::BufferRect& bufRect,
                                       const amd::BufferRect& hostRect, const amd::Coord3D& size,
                                       bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  // Use host copy if memory has direct access
  if (setup_.disableReadBufferRect_ ||
      (gpuMem(srcMemory).isHostMemDirectAccess() && gpuMem(srcMemory).isCacheable())) {
    result = HostBlitManager::readBufferRect(srcMemory, dstHost, bufRect, hostRect, size, entire);
    synchronize();
    return result;
  } else {
    size_t pinSize = hostRect.start_ + hostRect.end_;
    size_t partial;
    amd::Memory* amdMemory = pinHostMemory(dstHost, pinSize, partial);

    if (amdMemory == NULL) {
      // Force SW copy
      result = HostBlitManager::readBufferRect(srcMemory, dstHost, bufRect, hostRect, size, entire);
      synchronize();
      return result;
    }

    // Readjust host mem offset
    amd::BufferRect rect;
    rect.rowPitch_ = hostRect.rowPitch_;
    rect.slicePitch_ = hostRect.slicePitch_;
    rect.start_ = hostRect.start_ + partial;
    rect.end_ = hostRect.end_;

    // Get device memory for this virtual device
    Memory* dstMemory = dev().getGpuMemory(amdMemory);

    // Copy image to buffer
    result = copyBufferRect(srcMemory, *dstMemory, bufRect, rect, size, entire);

    // Add pinned memory for a later release
    gpu().addPinnedMem(amdMemory);
  }

  synchronize();

  return result;
}

bool KernelBlitManager::writeBuffer(const void* srcHost, device::Memory& dstMemory,
                                    const amd::Coord3D& origin, const amd::Coord3D& size,
                                    bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  // Use host copy if memory has direct access or it's persistent
  if (setup_.disableWriteBuffer_ ||
      (gpuMem(dstMemory).isHostMemDirectAccess() &&
      (gpuMem(dstMemory).memoryType() != Resource::ExternalPhysical)) ||
      (gpuMem(dstMemory).memoryType() == Resource::Persistent)) {
    result = HostBlitManager::writeBuffer(srcHost, dstMemory, origin, size, entire);
    synchronize();
    return result;
  } else {
    size_t pinSize = size[0];

    // Check if a pinned transfer can be executed with a single pin
    if ((pinSize <= dev().settings().pinnedXferSize_) && (pinSize > MinSizeForPinnedTransfer)) {
      size_t partial;
      amd::Memory* amdMemory = pinHostMemory(srcHost, pinSize, partial);

      if (amdMemory == NULL) {
        // Force SW copy
        result = HostBlitManager::writeBuffer(srcHost, dstMemory, origin, size, entire);
        synchronize();
        return result;
      }

      // Readjust destination offset
      const amd::Coord3D srcOrigin(partial);

      // Get device memory for this virtual device
      Memory* srcMemory = dev().getGpuMemory(amdMemory);

      // Copy buffer rect
      result = copyBuffer(*srcMemory, dstMemory, srcOrigin, origin, size, entire);

      // Add pinned memory for a later release
      gpu().addPinnedMem(amdMemory);
    } else {
      result = DmaBlitManager::writeBuffer(srcHost, dstMemory, origin, size, entire);
    }
  }

  synchronize();


  return result;
}

bool KernelBlitManager::writeBufferRect(const void* srcHost, device::Memory& dstMemory,
                                        const amd::BufferRect& hostRect,
                                        const amd::BufferRect& bufRect, const amd::Coord3D& size,
                                        bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  // Use host copy if memory has direct access or it's persistent
  if (setup_.disableWriteBufferRect_ ||
      (gpuMem(dstMemory).isHostMemDirectAccess() &&
      (gpuMem(dstMemory).memoryType() != Resource::ExternalPhysical)) ||
      gpuMem(dstMemory).isPersistentDirectMap()) {
    result = HostBlitManager::writeBufferRect(srcHost, dstMemory, hostRect, bufRect, size, entire);
    synchronize();
    return result;
  } else {
    size_t pinSize = hostRect.start_ + hostRect.end_;
    size_t partial;
    amd::Memory* amdMemory = pinHostMemory(srcHost, pinSize, partial);

    if (amdMemory == NULL) {
      // Force SW copy
      result =
          HostBlitManager::writeBufferRect(srcHost, dstMemory, hostRect, bufRect, size, entire);
      synchronize();
      return result;
    }

    // Readjust destination offset
    const amd::Coord3D srcOrigin(partial);

    // Get device memory for this virtual device
    Memory* srcMemory = dev().getGpuMemory(amdMemory);

    // Readjust host mem offset
    amd::BufferRect rect;
    rect.rowPitch_ = hostRect.rowPitch_;
    rect.slicePitch_ = hostRect.slicePitch_;
    rect.start_ = hostRect.start_ + partial;
    rect.end_ = hostRect.end_;

    // Copy buffer rect
    result = copyBufferRect(*srcMemory, dstMemory, rect, bufRect, size, entire);

    // Add pinned memory for a later release
    gpu().addPinnedMem(amdMemory);
  }

  synchronize();

  return result;
}

bool KernelBlitManager::fillBuffer(device::Memory& memory, const void* pattern, size_t patternSize,
                                   const amd::Coord3D& origin, const amd::Coord3D& size,
                                   bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  // Use host fill if memory has direct access
  if (setup_.disableFillBuffer_ || gpuMem(memory).isHostMemDirectAccess()) {
    result = HostBlitManager::fillBuffer(memory, pattern, patternSize, origin, size, entire);
    synchronize();
    return result;
  } else {
    uint fillType = FillBuffer;
    size_t globalWorkOffset[3] = {0, 0, 0};
    cl_ulong fillSize = size[0] / patternSize;
    size_t globalWorkSize = amd::alignUp(fillSize, 256);
    size_t localWorkSize = 256;
    bool dwordAligned = ((patternSize % sizeof(uint32_t)) == 0) ? true : false;

    // Program kernels arguments for the fill operation
    Memory* mem = &gpuMem(memory);
    if (dwordAligned) {
      setArgument(kernels_[fillType], 0, sizeof(cl_mem), NULL);
      setArgument(kernels_[fillType], 1, sizeof(cl_mem), &mem);
    } else {
      setArgument(kernels_[fillType], 0, sizeof(cl_mem), &mem);
      setArgument(kernels_[fillType], 1, sizeof(cl_mem), NULL);
    }
    Memory* gpuCB = dev().getGpuMemory(constantBuffer_);
    if (gpuCB == NULL) {
      return false;
    }
    void* constBuf = gpuCB->map(&gpu(), Resource::WriteOnly);
    memcpy(constBuf, pattern, patternSize);
    gpuCB->unmap(&gpu());
    setArgument(kernels_[fillType], 2, sizeof(cl_mem), &gpuCB);
    cl_ulong offset = origin[0];
    if (dwordAligned) {
      patternSize /= sizeof(uint32_t);
      offset /= sizeof(uint32_t);
    }
    setArgument(kernels_[fillType], 3, sizeof(cl_uint), &patternSize);
    setArgument(kernels_[fillType], 4, sizeof(offset), &offset);
    setArgument(kernels_[fillType], 5, sizeof(fillSize), &fillSize);

    // Create ND range object for the kernel's execution
    amd::NDRangeContainer ndrange(1, globalWorkOffset, &globalWorkSize, &localWorkSize);

    // Execute the blit
    address parameters = kernels_[fillType]->parameters().values();
    result = gpu().submitKernelInternal(ndrange, *kernels_[fillType], parameters);
  }

  synchronize();

  return result;
}

bool KernelBlitManager::copyBuffer(device::Memory& srcMemory, device::Memory& dstMemory,
                                   const amd::Coord3D& srcOrigin, const amd::Coord3D& dstOrigin,
                                   const amd::Coord3D& sizeIn, bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  if (!gpuMem(srcMemory).isHostMemDirectAccess() && !gpuMem(dstMemory).isHostMemDirectAccess()) {
    uint blitType = BlitCopyBuffer;
    size_t dim = 1;
    size_t globalWorkOffset[3] = {0, 0, 0};
    size_t globalWorkSize = 0;
    size_t localWorkSize = 0;

    const static uint CopyBuffAlignment[3] = {16, 4, 1};
    amd::Coord3D size(sizeIn[0], sizeIn[1], sizeIn[2]);

    bool aligned;
    uint i;
    for (i = 0; i < sizeof(CopyBuffAlignment) / sizeof(uint); i++) {
      // Check source alignments
      aligned = ((srcOrigin[0] % CopyBuffAlignment[i]) == 0);
      // Check destination alignments
      aligned &= ((dstOrigin[0] % CopyBuffAlignment[i]) == 0);
      // Check copy size alignment in the first dimension
      aligned &= ((sizeIn[0] % CopyBuffAlignment[i]) == 0);

      if (aligned) {
        if (CopyBuffAlignment[i] != 1) {
          blitType = BlitCopyBufferAligned;
        }
        break;
      }
    }

    cl_uint remain;
    if (blitType == BlitCopyBufferAligned) {
      size.c[0] /= CopyBuffAlignment[i];
    } else {
      if (dev().settings().ciPlus_) {
        remain = size[0] % 4;
        size.c[0] /= 4;
        size.c[0] += 1;
      } else {
        // Check if offsets are aligned
        aligned = ((srcOrigin[0] % sizeof(uint32_t)) == 0);
        aligned &= ((dstOrigin[0] % sizeof(uint32_t)) == 0);
        if (aligned) {
          remain = size[0] % 4;
          size.c[0] /= 4;
          size.c[0] += 1;
        } else {
          remain = 8;
        }
      }
    }

    // Program the dispatch dimensions
    localWorkSize = 256;
    globalWorkSize = amd::alignUp(size[0], 256);

    // Program kernels arguments for the blit operation
    Memory* mem = &gpuMem(srcMemory);
    setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
    mem = &gpuMem(dstMemory);
    setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);
    // Program source origin
    cl_ulong srcOffset = srcOrigin[0] / CopyBuffAlignment[i];
    ;
    setArgument(kernels_[blitType], 2, sizeof(srcOffset), &srcOffset);

    // Program destinaiton origin
    cl_ulong dstOffset = dstOrigin[0] / CopyBuffAlignment[i];
    ;
    setArgument(kernels_[blitType], 3, sizeof(dstOffset), &dstOffset);

    cl_ulong copySize = size[0];
    setArgument(kernels_[blitType], 4, sizeof(copySize), &copySize);

    if (blitType == BlitCopyBufferAligned) {
      cl_int alignment = CopyBuffAlignment[i];
      setArgument(kernels_[blitType], 5, sizeof(alignment), &alignment);
    } else {
      setArgument(kernels_[blitType], 5, sizeof(remain), &remain);
    }

    // Create ND range object for the kernel's execution
    amd::NDRangeContainer ndrange(1, globalWorkOffset, &globalWorkSize, &localWorkSize);

    // Execute the blit
    address parameters = kernels_[blitType]->parameters().values();
    result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters);
  } else {
    result = DmaBlitManager::copyBuffer(srcMemory, dstMemory, srcOrigin, dstOrigin, sizeIn, entire);
  }

  synchronize();

  return result;
}

bool KernelBlitManager::fillImage(device::Memory& memory, const void* pattern,
                                  const amd::Coord3D& origin, const amd::Coord3D& size,
                                  bool entire) const {
  amd::ScopedLock k(lockXferOps_);
  bool result = false;

  // Use host fill if memory has direct access
  if (setup_.disableFillImage_ || gpuMem(memory).isHostMemDirectAccess()) {
    result = HostBlitManager::fillImage(memory, pattern, origin, size, entire);
    synchronize();
    return result;
  }

  uint fillType;
  size_t dim = 0;
  size_t globalWorkOffset[3] = {0, 0, 0};
  size_t globalWorkSize[3];
  size_t localWorkSize[3];
  Memory* memView = &gpuMem(memory);
  amd::Image::Format newFormat(gpuMem(memory).owner()->asImage()->getImageFormat());

  // Program the kernels workload depending on the fill dimensions
  fillType = FillImage;
  dim = 3;

  void* newpattern = const_cast<void*>(pattern);
  cl_uint4 iFillColor;

  bool rejected = false;
  bool releaseView = false;
  // For depth, we need to create a view
  if ((memView->cal()->format_ == CM_SURF_FMT_DEPTH32F) ||
      (memView->cal()->format_ == CM_SURF_FMT_RGBA8_SRGB) ||
      (memView->cal()->format_ == CM_SURF_FMT_DEPTH16)) {
    // Find unsupported data type
    for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
      if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
        newFormat.image_channel_data_type = RejectedData[i].clNewType_;
        rejected = true;
        break;
      }
    }

    // Below may not be correct. We need to find why unsigned int view doesn't work for DEPTH16.
    if (gpuMem(memory).cal()->format_ == CM_SURF_FMT_DEPTH16) {
      newFormat.image_channel_data_type = CL_UNORM_INT16;
    }

    if (gpuMem(memory).cal()->format_ == CM_SURF_FMT_RGBA8_SRGB) {
      // Converting a linear RGB floating-point color value to a 8-bit unsigned integer sRGB value
      // because hw is not support write_imagef for sRGB.
      float* fColor = static_cast<float*>(newpattern);
      iFillColor.s[0] = sRGBmap(fColor[0]);
      iFillColor.s[1] = sRGBmap(fColor[1]);
      iFillColor.s[2] = sRGBmap(fColor[2]);
      iFillColor.s[3] = (cl_uint)(fColor[3] * 255.0f);
      newpattern = static_cast<void*>(&iFillColor);
      for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
        if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
          newFormat.image_channel_order = RejectedOrder[i].clNewType_;
          rejected = true;
          break;
        }
      }
    }
  }
  // If the image format was rejected, then attempt to create a view
  if (rejected) {
    memView = createView(gpuMem(memory), dev().getCalFormat(newFormat));
    if (memView != NULL) {
      rejected = false;
      releaseView = true;
    }
  }

  // Perform workload split to allow multiple operations in a single thread
  globalWorkSize[0] = (size[0] + TransferSplitSize - 1) / TransferSplitSize;
  // Find the current blit type
  if (memView->cal()->dimSize_ == 1) {
    globalWorkSize[0] = amd::alignUp(globalWorkSize[0], 256);
    globalWorkSize[1] = amd::alignUp(size[1], 1);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = 256;
    localWorkSize[1] = localWorkSize[2] = 1;
  } else if (memView->cal()->dimSize_ == 2) {
    globalWorkSize[0] = amd::alignUp(globalWorkSize[0], 16);
    globalWorkSize[1] = amd::alignUp(size[1], 16);
    globalWorkSize[2] = amd::alignUp(size[2], 1);
    localWorkSize[0] = localWorkSize[1] = 16;
    localWorkSize[2] = 1;
  } else {
    globalWorkSize[0] = amd::alignUp(globalWorkSize[0], 8);
    globalWorkSize[1] = amd::alignUp(size[1], 8);
    globalWorkSize[2] = amd::alignUp(size[2], 4);
    localWorkSize[0] = localWorkSize[1] = 8;
    localWorkSize[2] = 4;
  }

  // Program kernels arguments for the blit operation
  Memory* mem = memView;
  setArgument(kernels_[fillType], 0, sizeof(cl_mem), &mem);
  setArgument(kernels_[fillType], 1, sizeof(cl_float4), newpattern);
  setArgument(kernels_[fillType], 2, sizeof(cl_int4), newpattern);
  setArgument(kernels_[fillType], 3, sizeof(cl_uint4), newpattern);

  cl_int fillOrigin[4] = {(cl_int)origin[0], (cl_int)origin[1], (cl_int)origin[2], 0};
  cl_int fillSize[4] = {(cl_int)size[0], (cl_int)size[1], (cl_int)size[2], 0};
  setArgument(kernels_[fillType], 4, sizeof(fillOrigin), fillOrigin);
  setArgument(kernels_[fillType], 5, sizeof(fillSize), fillSize);

  // Find the type of image
  uint32_t type = 0;
  switch (newFormat.image_channel_data_type) {
    case CL_SNORM_INT8:
    case CL_SNORM_INT16:
    case CL_UNORM_INT8:
    case CL_UNORM_INT16:
    case CL_UNORM_SHORT_565:
    case CL_UNORM_SHORT_555:
    case CL_UNORM_INT_101010:
    case CL_HALF_FLOAT:
    case CL_FLOAT:
      type = 0;
      break;
    case CL_SIGNED_INT8:
    case CL_SIGNED_INT16:
    case CL_SIGNED_INT32:
      type = 1;
      break;
    case CL_UNSIGNED_INT8:
    case CL_UNSIGNED_INT16:
    case CL_UNSIGNED_INT32:
      type = 2;
      break;
  }
  setArgument(kernels_[fillType], 6, sizeof(type), &type);

  // Create ND range object for the kernel's execution
  amd::NDRangeContainer ndrange(dim, globalWorkOffset, globalWorkSize, localWorkSize);

  // Execute the blit
  address parameters = kernels_[fillType]->parameters().values();
  result = gpu().submitKernelInternal(ndrange, *kernels_[fillType], parameters);
  if (releaseView) {
    delete memView;
  }

  synchronize();

  return result;
}

bool KernelBlitManager::runScheduler(device::Memory& vqueue, device::Memory& params, uint paramIdx,
                                     uint threads) const {
  amd::ScopedLock k(lockXferOps_);

  size_t globalWorkOffset[1] = {0};
  size_t globalWorkSize[1] = {threads};
  size_t localWorkSize[1] = {1};

  // Program kernels arguments
  Memory* q = &gpuMem(vqueue);
  Memory* p = &gpuMem(params);
  setArgument(kernels_[Scheduler], 0, sizeof(cl_mem), &q);
  setArgument(kernels_[Scheduler], 1, sizeof(cl_mem), &p);
  setArgument(kernels_[Scheduler], 2, sizeof(uint), &paramIdx);

  // Create ND range object for the kernel's execution
  amd::NDRangeContainer ndrange(1, globalWorkOffset, globalWorkSize, localWorkSize);

  // Execute the blit
  address parameters = kernels_[Scheduler]->parameters().values();
  bool result = gpu().submitKernelInternal(ndrange, *kernels_[Scheduler], parameters);

  synchronize();

  return result;
}

amd::Memory* DmaBlitManager::pinHostMemory(const void* hostMem, size_t pinSize,
                                           size_t& partial) const {
  size_t pinAllocSize;
  const static bool SysMem = true;
  amd::Memory* amdMemory;

  // Allign offset to 4K boundary (Vista/Win7 limitation)
  char* tmpHost = const_cast<char*>(
      amd::alignDown(reinterpret_cast<const char*>(hostMem), PinnedMemoryAlignment));

  // Find the partial size for unaligned copy
  partial = reinterpret_cast<const char*>(hostMem) - tmpHost;

  // Recalculate pin memory size
  pinAllocSize = amd::alignUp(pinSize + partial, PinnedMemoryAlignment);

  amdMemory = gpu().findPinnedMem(tmpHost, pinAllocSize);

  if (NULL != amdMemory) {
    return amdMemory;
  }

  amdMemory = new (*context_) amd::Buffer(*context_, CL_MEM_USE_HOST_PTR, pinAllocSize);
  amdMemory->setVirtualDevice(&gpu());
  if ((amdMemory != NULL) && !amdMemory->create(tmpHost, SysMem)) {
    amdMemory->release();
    return NULL;
  }

  // Get device memory for this virtual device
  // @note: This will force real memory pinning
  Memory* srcMemory = dev().getGpuMemory(amdMemory);

  if (srcMemory == NULL) {
    // Release all pinned memory and attempt pinning again
    gpu().releasePinnedMem();
    srcMemory = dev().getGpuMemory(amdMemory);
    if (srcMemory == NULL) {
      // Release memory
      amdMemory->release();
      amdMemory = NULL;
    }
  }

  return amdMemory;
}

Memory* KernelBlitManager::createView(const Memory& parent, const CalFormat& format) const {
  assert(!parent.cal()->buffer_ && "View supports images only");
  gpu::Memory* gpuImage = new gpu::Image(
    dev(), parent.size(), parent.cal()->width_, parent.cal()->height_,
    parent.cal()->depth_, format.type_, format.channelOrder_,
    parent.cal()->imageType_, 1);

  // Create resource
  if (NULL != gpuImage) {
    Resource::ImageViewParams params;
    const Memory& gpuMem = static_cast<const Memory&>(parent);

    params.owner_ = parent.owner();
    params.level_ = 0;
    params.layer_ = 0;
    params.resource_ = &gpuMem;
    params.memory_ = &gpuMem;
    params.gpu_ = &gpu();

    // Create memory object
    bool result = gpuImage->create(Resource::ImageView, &params);
    if (!result) {
      delete gpuImage;
      return NULL;
    }
  }

  return gpuImage;
}

}  // namespace gpu