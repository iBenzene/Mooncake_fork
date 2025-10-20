// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "transport/cxl_transport/cxl_transport.h"

#include <bits/stdint-uintn.h>
#include <glog/logging.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <regex>

#include "common.h"
#include "transfer_engine.h"
#include "transfer_metadata.h"
#include "transport/transport.h"
#include <cstring>
#include <fcntl.h>     // For O_RDWR, O_CREAT, etc.
#include <unistd.h>    // For open(), close(), read(), write()
#include <sys/mman.h>  // For mmap, munmap

#include <sys/types.h>
#include <sys/stat.h>
#include <cerrno>

#ifdef USE_CUDA
#include <cuda.h>
#include <cuda_runtime.h>
#endif

namespace mooncake {

#ifdef USE_CUDA
static bool isCudaMemory(void *addr) {
    cudaPointerAttributes attributes;
    auto status = cudaPointerGetAttributes(&attributes, addr);
    if (status != cudaSuccess) return false;
    if (attributes.type == cudaMemoryTypeDevice) return true;
    return false;
}
#endif

CxlTransport::CxlTransport() {
    // cxl_dev_path = "/dev/dax0.0" or "/dev/shm/cxl";
    // cxl_dev_size = 1024 * 1024 * 1024;
    // get from env
    const char *env_cxl_dev_path = std::getenv("MC_CXL_DEV_PATH");

    if (env_cxl_dev_path) {
        LOG(INFO) << "MC_CXL_DEV_PATH: " << env_cxl_dev_path;
        cxl_dev_path = (char *)env_cxl_dev_path;
        using_shm_ = (strncmp(cxl_dev_path, "/dev/shm/", 9) == 0);
        cxl_dev_size = cxlGetDeviceSize();
    }
}

CxlTransport::~CxlTransport() {
#ifdef USE_CUDA
    // 先 unpin 再 munmap
    if (shm_pinned_) {
        cudaError_t st = cudaHostUnregister(cxl_base_addr);
        if (st != cudaSuccess) {
            LOG(WARNING) << "cudaHostUnregister failed: "
                         << cudaGetErrorString(st);
        }
        shm_pinned_ = false;
    }
#endif

    if (cxl_base_addr != nullptr && cxl_base_addr != MAP_FAILED &&
        cxl_dev_size != 0) {
        munmap(cxl_base_addr, cxl_dev_size);
    }
    metadata_->removeSegmentDesc(local_server_name_);
}

size_t CxlTransport::cxlGetDeviceSize() {
    // for now, get cxl_shm size from env
    const char *env_cxl_dev_size = std::getenv("MC_CXL_DEV_SIZE");

    if (env_cxl_dev_size) {
        LOG(INFO) << "MC_CXL_DEV_SIZE: " << env_cxl_dev_size;
        char *end = nullptr;
        unsigned long long val = strtoull(env_cxl_dev_size, &end, 10);
        if (end != env_cxl_dev_size && *end == '\0')
            return static_cast<size_t>(val);
    } else {
        // try to read dev size from sys

        if (using_shm_) {
            struct stat st{};
            if (::stat(cxl_dev_path, &st) == 0 && S_ISREG(st.st_mode)) {
                if (st.st_size > 0) {
                    LOG(INFO) << "Use existing shm file size: " << st.st_size;
                    return static_cast<size_t>(st.st_size);
                }
            } else {
                LOG(WARNING) << "MC_CXL_DEV_SIZE unset. Using default 1GiB.";
                return (size_t)1ULL << 30;
            }
        }

        // find "dax*.*" in path
        std::regex dax_pattern(R"(dax\d+\.\d+)");
        std::smatch match;
        std::string dev_name;
        std::string str_cxl_dev_path = std::string(cxl_dev_path);
        if (std::regex_search(str_cxl_dev_path, match, dax_pattern)) {
            dev_name = match.str();
        } else {
            LOG(ERROR) << "Can not find CXL device name in path: "
                       << cxl_dev_path;
            return 0;
        }

        std::string size_path = "/sys/bus/dax/devices/" + dev_name + "/size";
        LOG(INFO) << "Try to get CXL device size from: " << size_path;
        std::ifstream file(size_path);
        if (!file.is_open()) {
            LOG(ERROR) << "CXL size file does not exist";
            return 0;
        }

        std::string content;
        if (!std::getline(file, content)) {
            LOG(ERROR) << "Failed to read from: " << size_path;
            return 0;
        }

        unsigned long long val = strtoull(content.c_str(), nullptr, 10);
        // the content is written by kernel, so it should be a valid ull
        LOG(INFO) << "CXL device size is: " << val;
        return static_cast<size_t>(val);
    }
    return 0;
}

int CxlTransport::cxlMemcpy(void *dest, void *src, size_t size) {
    // Input validation
    if (!src || !dest) {
        LOG(ERROR) << "CxlTransport::cxlMemcpy invalid arguments: null pointer "
                      "provided.";
        return -1;  // null pointer
    }

    // Validate memory bounds using the helper function
    if (!validateMemoryBounds(dest, src, size)) {
        return -1;  // validation failed
    }

    // Perform the memory copy
#ifdef USE_CUDA
    bool is_dest_vram = isCudaMemory(dest);
    bool is_src_vram  = isCudaMemory(src);

    if (is_dest_vram || is_src_vram) {
        cudaError_t st;
        if (is_dest_vram) {
            // Host(CXL/DRAM) -> Device(VRAM)
            st = cudaMemcpy(dest, src, size, cudaMemcpyHostToDevice);
        } else {
            // Device(VRAM) -> Host(CXL/DRAM)
            st = cudaMemcpy(dest, src, size, cudaMemcpyDeviceToHost);
        }
        if (st != cudaSuccess) {
            LOG(ERROR) << "CxlTransport::cxlMemcpy cudaMemcpy failed: "
                       << cudaGetErrorString(st);
            return -1;
        }
    } else {
#endif
    std::memcpy(dest, src, size);
#ifdef USE_CUDA
    }
#endif

    // Memory barriers and cache operations
    if (isAddressInCxlRange(dest) || isAddressInCxlRange(src)) {
        // Ensure memory ordering for CXL operations
        __sync_synchronize();
    }

    return 0;  // success
}

bool CxlTransport::validateMemoryBounds(void *dest, void *src, size_t size) {
    uintptr_t base = reinterpret_cast<uintptr_t>(cxl_base_addr);
    uintptr_t end = base + cxl_dev_size;
    uintptr_t dest_ptr = reinterpret_cast<uintptr_t>(dest);
    uintptr_t src_ptr = reinterpret_cast<uintptr_t>(src);

    bool dest_in_cxl = isAddressInCxlRange(dest);
    bool src_in_cxl = isAddressInCxlRange(src);

    // 至少有一端落在 CXL 的范围内
    if (!dest_in_cxl && !src_in_cxl) {
        LOG(ERROR) << "CxlTransport::validateMemoryBounds: neither src nor dest "
                   << "is in CXL range; CXL transport requires at least one "
                   << "operand in CXL.";
        errno = EINVAL;
        return false;
    }

    // 凡是落在 CXL 的一端, 必须边界合法
    if (dest_in_cxl) {
        uintptr_t dest_end = dest_ptr + size;
        if (dest_end > end || dest_end < dest_ptr) {
            LOG(ERROR) << "CxlTransport::cxlMemcpy destination out of bounds.";
            return false;
        }
    }

    if (src_in_cxl) {
        uintptr_t src_end = src_ptr + size;
        if (src_end > end || src_end < src_ptr) {
            LOG(ERROR) << "CxlTransport::cxlMemcpy source out of bounds.";
            return false;
        }
    }

    // if (isAddressInCxlRange(dest)) {
    //     uintptr_t dest_end = dest_ptr + size;
    //     if (dest_end > end || dest_end < dest_ptr) {
    //         LOG(ERROR) << "CxlTransport::cxlMemcpy destination out of bounds.";
    //         return false;
    //     }
    // }

    // if (isAddressInCxlRange(src)) {
    //     uintptr_t src_end = src_ptr + size;
    //     if (src_end > end || src_end < src_ptr) {
    //         LOG(ERROR) << "CxlTransport::cxlMemcpy source out of bounds.";
    //         return false;
    //     }
    // }

    return true;
}

bool CxlTransport::isAddressInCxlRange(void *addr) {
    if (!addr || !cxl_base_addr) return false;

    uintptr_t base = reinterpret_cast<uintptr_t>(cxl_base_addr);
    uintptr_t end = base + cxl_dev_size;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(addr);

    return (ptr >= base && ptr < end);
}

int CxlTransport::cxlDevInit() {
    if (!cxl_dev_path || !cxl_dev_size) {
        LOG(ERROR) << "CxlTransport: cxl_dev_path or cxl_dev_size is null.";
        return -1;
    }

    if (using_shm_) {
        int fd = ::open(cxl_dev_path, O_RDWR | O_CREAT, 0660);
        if (fd == -1) {
            LOG(ERROR) << "CxlTransport: Cannot open shm file: " << cxl_dev_path
                       << " err=" << strerror(errno);
            return -1;
        }

        struct stat st{};
        if (::fstat(fd, &st) == 0) {
            if ((size_t)st.st_size != cxl_dev_size) {
                if (::ftruncate(fd, (off_t)cxl_dev_size) != 0) {
                    LOG(ERROR) << "CxlTransport: ftruncate failed: "
                               << strerror(errno);
                    return -1;
                }
            }
        }
        void *ptr = ::mmap(nullptr, cxl_dev_size,
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            LOG(ERROR) << "CxlTransport: mmap shm failed: " << strerror(errno);
            
            close(fd);
            return ERR_MEMORY;
        }
        cxl_base_addr = ptr;
        close(fd);

#ifdef USE_CUDA
        // Try to pin the CXL memory for better performance
        cudaError_t err = cudaHostRegister(cxl_base_addr, cxl_dev_size, 0);
        if (err != cudaSuccess) {
            // 回退为 pageable memory
            LOG(WARNING) << "cudaHostRegister failed (fallback to pageable): "
                         << cudaGetErrorString(err)
                         << ". You may increase 'ulimit -l' or reduce size.";
            cudaGetLastError(); // clear the error
        } else {
            shm_pinned_ = true;
            LOG(INFO) << "Pinned /dev/shm CXL mapping for CUDA DMA: size="
                      << cxl_dev_size;
        }
#endif
        return 0;
    }

    int fd = open(cxl_dev_path, O_RDWR);
    if (fd == -1) {
        LOG(ERROR) << "CxlTransport: Cannot open cxl device."
                   << strerror(errno);
        return -1;
    }

    void *ptr =
        mmap(NULL, cxl_dev_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return ERR_MEMORY;
    }
    cxl_base_addr = ptr;
    close(fd);
    return 0;
}

int CxlTransport::install(std::string &local_server_name,
                          std::shared_ptr<TransferMetadata> meta,
                          std::shared_ptr<Topology> topo) {
    metadata_ = meta;
    local_server_name_ = local_server_name;

    int ret = cxlDevInit();
    if (ret) {
        LOG(ERROR) << "CxlTransport: Mmap cxl device failed.";
        return -1;
    }

    ret = allocateLocalSegmentID();
    if (ret) {
        LOG(ERROR) << "CxlTransport: cannot allocate local segment";
        return -1;
    }

    ret = metadata_->updateLocalSegmentDesc();
    if (ret) {
        LOG(ERROR) << "CxlTransport: cannot publish segments, "
                      "check the availability of metadata storage";
        return -1;
    }

    return 0;
}

int CxlTransport::allocateLocalSegmentID() {
    auto desc = std::make_shared<SegmentDesc>();
    if (!desc) return ERR_MEMORY;
    desc->name = local_server_name_;
    desc->protocol = "cxl";
    desc->cxl_base_addr = (uint64_t)cxl_base_addr;
    desc->cxl_name = cxl_dev_path;
    metadata_->addLocalSegment(LOCAL_SEGMENT_ID, local_server_name_,
                               std::move(desc));
    return 0;
}

int CxlTransport::registerLocalMemory(void *addr, size_t length,
                                      const std::string &location,
                                      bool remote_accessible,
                                      bool update_metadata) {
    (void)remote_accessible;
    BufferDesc cxl_buffer_desc;
    cxl_buffer_desc.name = local_server_name_;

    uintptr_t base = reinterpret_cast<uintptr_t>(cxl_base_addr);
    uintptr_t end = base + cxl_dev_size;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(addr);
    uintptr_t ptr_end = ptr + length;
    // check addr legal
    if (ptr < base || ptr >= end) {
        errno = EFAULT;
        return -1;
    }
    // check overflow
    if (ptr_end > end || ptr_end < ptr) {
        errno = EOVERFLOW;
        return -1;
    }

    cxl_buffer_desc.offset = (uint64_t)addr - (uint64_t)cxl_base_addr;
    cxl_buffer_desc.length = length;
    return metadata_->addLocalMemoryBuffer(cxl_buffer_desc, update_metadata);
}

int CxlTransport::unregisterLocalMemory(void *addr, bool update_metadata) {
    return metadata_->removeLocalMemoryBuffer(addr, update_metadata);
}

int CxlTransport::registerLocalMemoryBatch(
    const std::vector<Transport::BufferEntry> &buffer_list,
    const std::string &location) {
    for (auto &buffer : buffer_list)
        registerLocalMemory(buffer.addr, buffer.length, location, true, false);
    return metadata_->updateLocalSegmentDesc();
}

int CxlTransport::unregisterLocalMemoryBatch(
    const std::vector<void *> &addr_list) {
    for (auto &addr : addr_list) unregisterLocalMemory(addr, false);
    return metadata_->updateLocalSegmentDesc();
}

Status CxlTransport::getTransferStatus(BatchID batch_id, size_t task_id,
                                       TransferStatus &status) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    const size_t task_count = batch_desc.task_list.size();
    if (task_id >= task_count) {
        return Status::InvalidArgument(
            "CxlTransport::getTransportStatus invalid argument, batch id: " +
            std::to_string(batch_id));
    }
    auto &task = batch_desc.task_list[task_id];
    status.transferred_bytes = task.transferred_bytes;
    uint64_t success_slice_count = task.success_slice_count;
    uint64_t failed_slice_count = task.failed_slice_count;
    if (success_slice_count + failed_slice_count == task.slice_count) {
        if (failed_slice_count) {
            status.s = TransferStatusEnum::FAILED;
        } else {
            status.s = TransferStatusEnum::COMPLETED;
        }
        task.is_finished = true;
    } else {
        status.s = TransferStatusEnum::WAITING;
    }
    return Status::OK();
}

Status CxlTransport::submitTransfer(
    BatchID batch_id, const std::vector<TransferRequest> &entries) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    if (batch_desc.task_list.size() + entries.size() > batch_desc.batch_size) {
        LOG(ERROR) << "CxlTransport: Exceed the limitation of current batch's "
                      "capacity";
        return Status::InvalidArgument(
            "CxlTransport: Exceed the limitation of capacity, batch id: " +
            std::to_string(batch_id));
    }

    size_t task_id = batch_desc.task_list.size();
    batch_desc.task_list.resize(task_id + entries.size());

    for (auto &request : entries) {
        TransferTask &task = batch_desc.task_list[task_id];
        ++task_id;
        uint64_t dest_cxl_offset = request.target_offset;
        task.total_bytes = request.length;
        Slice *slice = getSliceCache().allocate();
        slice->source_addr = (char *)request.source;
        slice->cxl.dest_addr = (char *)cxl_base_addr + dest_cxl_offset;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->task = &task;
        slice->target_id = request.target_id;
        slice->status = Slice::PENDING;
        __sync_fetch_and_add(&task.slice_count, 1);
        int err;
        // Source is in local memory, Destination is on CXL
        if (slice->opcode == TransferRequest::READ)
            // READ: CXL -> local、dest -> source
            err = cxlMemcpy(slice->source_addr, (void *)slice->cxl.dest_addr,
                            slice->length);
        else
            // WRITE: local -> CXL、source -> dest
            err = cxlMemcpy((void *)slice->cxl.dest_addr, slice->source_addr,
                            slice->length);
        if (err != 0)
            slice->markFailed();
        else
            slice->markSuccess();
    }

    return Status::OK();
}

Status CxlTransport::submitTransferTask(
    const std::vector<TransferTask *> &task_list) {
    for (size_t index = 0; index < task_list.size(); ++index) {
        assert(task_list[index]);
        auto &task = *task_list[index];
        assert(task.request);
        auto &request = *task.request;
        uint64_t dest_cxl_offset = request.target_offset;
        task.total_bytes = request.length;

        Slice *slice = getSliceCache().allocate();
        slice->source_addr = (char *)request.source;
        slice->cxl.dest_addr = (char *)cxl_base_addr + dest_cxl_offset;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->task = &task;
        slice->target_id = request.target_id;
        slice->status = Slice::PENDING;
        task.slice_list.push_back(slice);
        __sync_fetch_and_add(&task.slice_count, 1);
        int err;
        // Source is in local memory, Destination is on CXL
        if (slice->opcode == TransferRequest::READ)
            // READ: CXL -> local、dest -> source
            err = cxlMemcpy(slice->source_addr, (void *)slice->cxl.dest_addr,
                            slice->length);
        else
            // WRITE: local -> CXL、source -> dest
            err = cxlMemcpy((void *)slice->cxl.dest_addr, slice->source_addr,
                            slice->length);
        if (err != 0)
            slice->markFailed();
        else
            slice->markSuccess();
    }
    return Status::OK();
}

}  // namespace mooncake
