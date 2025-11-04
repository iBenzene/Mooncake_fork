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

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "transfer_engine.h"
#include "transfer_metadata.h"
#include "transport/transport.h"
#include "transport/cxl_transport/cxl_transport.h"

using namespace mooncake;

// ===================== gflags =====================
DEFINE_string(cxl_path, "/dev/shm/cxl", "Path to shared-memory backed CXL file");
DEFINE_int64(cxl_size, 2LL * 1024 * 1024 * 1024, "Size of CXL region (bytes)");
DEFINE_string(local_server_name, getHostname(),
              "Local server name for segment discovery");
DEFINE_string(metadata_server, "127.0.0.1:2379", "etcd server host address");
DEFINE_int32(iters, 4, "Iterations per test");
DEFINE_int32(length, 1 * 1024 * 1024, "Payload length per transfer (bytes)");
DEFINE_uint64(offset_a, 2ULL * 1024 * 1024, "First CXL offset");
DEFINE_uint64(offset_b, 6ULL * 1024 * 1024, "Second CXL offset");

// ===================== CUDA helpers (optional) =====================
#ifdef USE_CUDA
#include <cuda.h>
#include <cuda_runtime.h>
static bool cudaOk(cudaError_t st, const char* what) {
    if (st != cudaSuccess) {
        LOG(ERROR) << what << " failed: " << cudaGetErrorString(st);
        return false;
    }
    return true;
}
static bool hasCudaDevice() {
    int cnt = 0;
    auto st = cudaGetDeviceCount(&cnt);
    return (st == cudaSuccess) && (cnt > 0);
}
#endif

// ===================== Host memory helpers =====================
static void* hostAlloc(size_t n, size_t align = 64) {
    void* p = nullptr;
    if (posix_memalign(&p, align, n) != 0) return nullptr;
    return p;
}
static void hostFree(void* p) { free(p); }

// ===================== Test Fixture =====================
class SharedCXLVramTest : public ::testing::Test {
  protected:
    std::unique_ptr<mooncake::TransferEngine> engine_;
    mooncake::Transport* xport_ = nullptr;
    mooncake::CxlTransport* cxlt_ = nullptr;
    std::pair<std::string, uint16_t> host_port_{};
    mooncake::Transport::SegmentID seg_id_{};
    std::shared_ptr<mooncake::TransferMetadata::SegmentDesc> seg_desc_;
    uint8_t* cxl_base_ = nullptr;

    void SetUp() override {
        google::InitGoogleLogging("SharedCXLVramTest");
        FLAGS_logtostderr = 1;

        // 仅设置环境变量, 交由 CxlTransport 自行创建/映射, 不做 unlink
        setenv("MC_CXL_DEV_PATH", FLAGS_cxl_path.c_str(), 1);
        setenv("MC_CXL_DEV_SIZE", std::to_string(FLAGS_cxl_size).c_str(), 1);

        engine_ = std::make_unique<mooncake::TransferEngine>(false);
        host_port_ = parseHostNameWithPort(FLAGS_local_server_name);
        // 为避免同机多进程端口冲突, 随机或偏移端口
        static std::atomic<uint16_t> base_off{0};
        engine_->init(FLAGS_metadata_server, FLAGS_local_server_name.c_str(),
                      host_port_.first.c_str(),
                      static_cast<uint16_t>(host_port_.second + base_off.fetch_add(1)));

        void** args = (void**)malloc(2 * sizeof(void*));
        args[0] = nullptr;
        xport_ = engine_->installTransport("cxl", args);
        free(args);
        ASSERT_NE(xport_, nullptr);

        cxlt_ = dynamic_cast<mooncake::CxlTransport*>(xport_);
        ASSERT_NE(cxlt_, nullptr);
        cxl_base_ = reinterpret_cast<uint8_t*>(cxlt_->getCxlBaseAddr());
        ASSERT_NE(cxl_base_, nullptr);

        // 预先注册一段 CXL 区域给 metadata (覆盖两个偏移)
        const size_t reg_len = std::max<uint64_t>(FLAGS_offset_a, FLAGS_offset_b) +
                               static_cast<uint64_t>(FLAGS_length) + 4 * 1024;
        int rc = engine_->registerLocalMemory(cxl_base_, reg_len);
        ASSERT_EQ(rc, 0);

        seg_id_   = engine_->openSegment(FLAGS_local_server_name.c_str());
        seg_desc_ = engine_->getMetadata()->getSegmentDescByID(seg_id_);
        ASSERT_TRUE(seg_desc_ != nullptr);
    }

    void TearDown() override {
        // 只做引擎内存与日志收尾, 不 unlink /dev/shm/cxl
        engine_.reset();
        google::ShutdownGoogleLogging();
    }

    // 等待单 slice 完成
    void waitOne(mooncake::Transport::BatchID bid, int idx = 0) {
        mooncake::TransferStatus st;
        while (true) {
            auto s = xport_->getTransferStatus(bid, idx, st);
            ASSERT_EQ(s, mooncake::Status::OK());
            if (st.s == mooncake::TransferStatusEnum::COMPLETED) break;
            if (st.s == mooncake::TransferStatusEnum::FAILED) {
                FAIL() << "Transfer failed";
            }
            // 如需自旋等待, 可根据需要 sleep/yield
        }
    }
};

// ===================== TEST 1: VRAM -> CXL -> VRAM =====================
TEST_F(SharedCXLVramTest, VRAM_Roundtrip_VRAM) {
#ifndef USE_CUDA
    GTEST_SKIP() << "USE_CUDA not defined; skipping GPU test.";
#else
    if (!hasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";

    const size_t N = static_cast<size_t>(FLAGS_length);
    const uint64_t off = FLAGS_offset_a;

    // Host 侧准备随机数据
    std::vector<uint8_t> h_src(N), h_dst(N, 0);
    for (size_t i = 0; i < N; ++i) h_src[i] = static_cast<uint8_t>('A' + (lrand48() % 26));

    // 设备内存
    uint8_t* d_src = nullptr;
    uint8_t* d_dst = nullptr;
    ASSERT_TRUE(cudaOk(cudaMalloc(&d_src, N), "cudaMalloc d_src"));
    ASSERT_TRUE(cudaOk(cudaMalloc(&d_dst, N), "cudaMalloc d_dst"));
    ASSERT_TRUE(cudaOk(cudaMemcpy(d_src, h_src.data(), N, cudaMemcpyHostToDevice),
                       "H2D"));

    for (int it = 0; it < FLAGS_iters; ++it) {
        // WRITE: local (VRAM) -> CXL
        auto bid_w = xport_->allocateBatchID(1);
        mooncake::TransferRequest w{};
        w.opcode        = mooncake::TransferRequest::WRITE;
        w.length        = N;
        w.source        = d_src;  // VRAM 指针
        w.target_id     = seg_id_;
        w.target_offset = off;
        auto s = engine_->submitTransfer(bid_w, {w});
        ASSERT_TRUE(s.ok());
        waitOne(bid_w, 0);
        ASSERT_EQ(xport_->freeBatchID(bid_w), mooncake::Status::OK());

        // READ: CXL -> local (VRAM d_dst)
        auto bid_r = xport_->allocateBatchID(1);
        mooncake::TransferRequest r{};
        r.opcode        = mooncake::TransferRequest::READ;
        r.length        = N;
        r.source        = d_dst;  // VRAM 指针作为 READ 的「local 目标」
        r.target_id     = seg_id_;
        r.target_offset = off;
        s = engine_->submitTransfer(bid_r, {r});
        ASSERT_TRUE(s.ok());
        waitOne(bid_r, 0);
        ASSERT_EQ(xport_->freeBatchID(bid_r), mooncake::Status::OK());

        // 回拷主机并校验
        ASSERT_TRUE(cudaOk(cudaMemcpy(h_dst.data(), d_dst, N, cudaMemcpyDeviceToHost),
                           "D2H"));
        ASSERT_EQ(0, ::memcmp(h_src.data(), h_dst.data(), N));
    }

    cudaFree(d_src);
    cudaFree(d_dst);
#endif
}

// ===================== TEST 2: VRAM -> CXL -> DRAM =====================
TEST_F(SharedCXLVramTest, VRAM_to_DRAM) {
#ifndef USE_CUDA
    GTEST_SKIP() << "USE_CUDA not defined; skipping GPU test.";
#else
    if (!hasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";

    const size_t N = static_cast<size_t>(FLAGS_length);
    const uint64_t off = FLAGS_offset_b;

    std::vector<uint8_t> h_src(N), h_dst(N, 0);
    for (size_t i = 0; i < N; ++i)
        h_src[i] = static_cast<uint8_t>((i * 2654435761u) & 0xFF);

    uint8_t* d_src = nullptr;
    ASSERT_TRUE(cudaOk(cudaMalloc(&d_src, N), "cudaMalloc d_src"));
    ASSERT_TRUE(cudaOk(cudaMemcpy(d_src, h_src.data(), N, cudaMemcpyHostToDevice),
                       "H2D"));

    // DRAM 目标
    uint8_t* host_buf = reinterpret_cast<uint8_t*>(hostAlloc(N));
    ASSERT_NE(host_buf, nullptr);

    for (int it = 0; it < FLAGS_iters; ++it) {
        // VRAM -> CXL
        auto bid_w = xport_->allocateBatchID(1);
        mooncake::TransferRequest w{};
        w.opcode        = mooncake::TransferRequest::WRITE;
        w.length        = N;
        w.source        = d_src;   // VRAM
        w.target_id     = seg_id_;
        w.target_offset = off;
        auto s = engine_->submitTransfer(bid_w, {w});
        ASSERT_TRUE(s.ok());
        waitOne(bid_w, 0);
        ASSERT_EQ(xport_->freeBatchID(bid_w), mooncake::Status::OK());

        // CXL -> DRAM
        auto bid_r = xport_->allocateBatchID(1);
        mooncake::TransferRequest r{};
        r.opcode        = mooncake::TransferRequest::READ;
        r.length        = N;
        r.source        = host_buf;   // DRAM 作为 READ 的「local 目标」
        r.target_id     = seg_id_;
        r.target_offset = off;
        s = engine_->submitTransfer(bid_r, {r});
        ASSERT_TRUE(s.ok());
        waitOne(bid_r, 0);
        ASSERT_EQ(xport_->freeBatchID(bid_r), mooncake::Status::OK());

        ASSERT_EQ(0, ::memcmp(h_src.data(), host_buf, N));
    }

    hostFree(host_buf);
    cudaFree(d_src);
#endif
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, false);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
