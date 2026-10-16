#include <cstddef>
#include <cstdint>

#include "kernel_operator.h"

// AIV 真正执行 GM -> UB -> GM，Host wrapper 不执行内存搬运。
extern "C" __global__ __aicore__ void fabric_copy_kernel(
    GM_ADDR dst, GM_ADDR src, uint64_t bytes)
{
    constexpr uint32_t kTileBytes = 4096;
    AscendC::TPipe pipe;
    AscendC::TBuf<AscendC::TPosition::VECCALC> buffer;
    pipe.InitBuffer(buffer, kTileBytes);
    auto local = buffer.Get<uint32_t>();
    AscendC::GlobalTensor<uint32_t> source;
    AscendC::GlobalTensor<uint32_t> target;
    source.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t*>(src));
    target.SetGlobalBuffer(reinterpret_cast<__gm__ uint32_t*>(dst));
    for (uint64_t offset = 0; offset < bytes; offset += kTileBytes) {
        uint32_t count = static_cast<uint32_t>(
            ((bytes - offset) < kTileBytes ? (bytes - offset) : kTileBytes) / sizeof(uint32_t));
        AscendC::DataCopy(local, source[offset / sizeof(uint32_t)], count);
        AscendC::SetFlag<AscendC::HardEvent::MTE2_MTE3>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::MTE2_MTE3>(EVENT_ID0);
        AscendC::DataCopy(target[offset / sizeof(uint32_t)], local, count);
        AscendC::SetFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
        AscendC::WaitFlag<AscendC::HardEvent::MTE3_MTE2>(EVENT_ID0);
    }
    AscendC::PipeBarrier<PIPE_ALL>();
}

// 返回 0 只表示已调用 launch；调用方必须检查 stream synchronize 的结果。
extern "C" int launch_fabric_copy(void* dst, const void* src, size_t bytes, void* stream)
{
    if (dst == nullptr || src == nullptr || stream == nullptr || bytes == 0 || bytes % 256 != 0) {
        return -1;
    }
    fabric_copy_kernel<<<1, nullptr, stream>>>(
        static_cast<uint8_t*>(dst), const_cast<uint8_t*>(static_cast<const uint8_t*>(src)), bytes);
    return 0;
}
