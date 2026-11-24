// Readback buffers and the GPU address arena (review of #444). Upload buffers live in the arena so a V# can reach them
// (d3d12_upload_arena_test); readback buffers do not: D3D12 binds no READBACK resource to a shader, so an arena range
// would only take space default-heap buffers need, and the arena's memory is uncached for CPU reads. Through the D3D12
// API on the runtime's own device, no shaders:
//   - with the default 256 MiB arena, a live 160 MiB READBACK buffer and then a 128 MiB DEFAULT buffer both succeed,
//     the default buffer at an arena offset (below 4 GiB); in the arena together they would not fit;
//   - the readback buffer is mapped, zeroed, and a copy from an upload buffer lands in it.
// 77: no Vulkan device, or LEONIDA_ARENA_MB is set (the sizes assume the default arena).
#include "../shims/d3d12_com.h"

#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using Microsoft::WRL::ComPtr;

namespace {

D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

}  // namespace

int main() {
    if (const char* arena = std::getenv("LEONIDA_ARENA_MB"); arena && *arena) {
        std::puts("SKIP: LEONIDA_ARENA_MB is set; the sizes assume the default 256 MiB arena");
        return 77;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device)))) {
        std::puts("SKIP: no Vulkan device for the runtime");
        return 77;
    }
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};

    const D3D12_RESOURCE_DESC readback_desc = buffer(160ull << 20), default_desc = buffer(128ull << 20);
    ComPtr<ID3D12Resource> readback, big_default;
    const HRESULT readback_hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&readback));
    const HRESULT default_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &default_desc,
                                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                               IID_PPV_ARGS(&big_default));
    std::printf("160 MiB READBACK: 0x%08lx; then 128 MiB DEFAULT: 0x%08lx\n", static_cast<unsigned long>(readback_hr),
                static_cast<unsigned long>(default_hr));
    EXPECT(SUCCEEDED(readback_hr) && SUCCEEDED(default_hr));
    const D3D12_GPU_VIRTUAL_ADDRESS default_va = big_default->GetGPUVirtualAddress();
    EXPECT(default_va && default_va < (1ull << 32));  // an arena offset

    // The readback buffer still works as one: zeroed, and a copy lands in it.
    constexpr UINT kWords = 1024;
    const D3D12_RESOURCE_DESC staging_desc = buffer(kWords * 4);
    ComPtr<ID3D12Resource> upload;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &staging_desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&upload))));
    void* up = nullptr;
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, &up)) && up);
    for (UINT i = 0; i < kWords; ++i) {
        const std::uint32_t v = i * 2654435761u;
        std::memcpy(static_cast<std::uint8_t*>(up) + 4 * i, &v, 4);
    }
    upload->Unmap(0, nullptr);
    void* back = nullptr;
    const D3D12_RANGE whole{0, kWords * 4};
    EXPECT(SUCCEEDED(readback->Map(0, &whole, &back)) && back);
    bool zeroed = true;
    for (UINT i = 0; i < kWords * 4; ++i) zeroed = zeroed && static_cast<const std::uint8_t*>(back)[i] == 0;
    readback->Unmap(0, nullptr);
    EXPECT(zeroed);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&list))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    list->CopyBufferRegion(readback.Get(), (160ull << 20) - kWords * 4, upload.Get(), 0, kWords * 4);
    EXPECT(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(SUCCEEDED(queue->Signal(fence.Get(), 1)));
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, nullptr)));
    const D3D12_RANGE tail{(160ull << 20) - kWords * 4, 160ull << 20};
    EXPECT(SUCCEEDED(readback->Map(0, &tail, &back)) && back);
    unsigned mismatches = 0;
    for (UINT i = 0; i < kWords; ++i) {
        std::uint32_t got = 0;
        std::memcpy(&got, static_cast<const std::uint8_t*>(back) + tail.Begin + 4 * i, 4);
        mismatches += got != i * 2654435761u;
    }
    readback->Unmap(0, nullptr);
    EXPECT(mismatches == 0);
    std::printf("d3d12_readback_arena: a live 160 MiB readback buffer leaves the 256 MiB arena to a 128 MiB default "
                "buffer (VA 0x%llx); %u words copied into the readback buffer's tail read back\n",
                static_cast<unsigned long long>(default_va), kWords);
    return 0;
}
