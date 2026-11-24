// M8: upload and readback buffers are ranges of the GPU address arena (its host-visible memory), so their console VAs
// are arena offsets like every other buffer's, and a V# can hold them. Through the D3D12 API on the runtime's own
// device, with a translator-output shader (test/shaders/cs_upload_read.asm):
//   - committed UPLOAD and READBACK buffers have VAs below 4 GiB (arena offsets; the shaders' 32-bit table pointers
//     and the BVH builder's 2^40 rule both need that);
//   - a compute shader reads the upload buffer through a raw SRV V# in a descriptor table and writes a default-heap
//     UAV; the result is copied into the readback buffer and read through its Map pointer: every word checked;
//   - Map hands out the same pointer each time, and what the CPU wrote is what the GPU read.
// Before M8 an upload buffer's VA was its device address minus the arena's, negative on the test GPUs, and the V#'s
// 48-bit base sent the load to a wild address (docs/d3d12_vulkan.md, "Measured (D3D-53)"). 77: no Vulkan device.
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef LEONIDA_TEST_UPLOAD_DIR
#error "LEONIDA_TEST_UPLOAD_DIR must name the folder build.py wrote (spirv/index.json, fssm/cs_upload_read.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr UINT kCount = 256;  // words, 4 groups of 64
constexpr std::uint32_t kTag = 0x00ab0000;

std::uint32_t input(UINT i) { return i * 7 + 1; }

D3D12_RESOURCE_DESC plain_big(UINT64 bytes) {
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
    const std::filesystem::path root_dir = LEONIDA_TEST_UPLOAD_DIR;
    _wputenv_s(L"LEONIDA_SHADERS", (root_dir / "spirv").wstring().c_str());
    _wputenv_s(L"LEONIDA_PSO_RECORD", L"0");
    _wputenv_s(L"LEONIDA_PIPELINE_CACHE_PERSIST", L"0");
    Ref<ID3D12Device> device;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p)))) {
        std::puts("SKIP: no Vulkan device for the runtime");
        return 77;
    }
    const auto program = read_file(root_dir / "fssm" / "cs_upload_read.fssm");
    EXPECT(!program.empty());
    const auto cs = shader_container(program);

    // [0] two constants (s4 = tag after the shift), [1] a table: an SRV then a UAV.
    const D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                              {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto blob = serialize_root_signature(root_desc);
    EXPECT(!blob.empty());
    Ref<ID3D12RootSignature> root;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.CS = {cs.data(), cs.size()};
    Ref<ID3D12PipelineState> pso;
    EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto plain = buffer_desc(kCount * 4);
    const auto uav_desc = buffer_desc(kCount * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Ref<ID3D12Resource> upload, out, readback;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &plain,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&upload.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                     IID_PPV_ARGS(&out.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &plain,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));
    // Arena offsets: non-zero (the arena's first 64 KiB are never handed out) and below 4 GiB. Readback buffers stay
    // out of the arena (no shader can address one), so only the upload buffer's VA is an arena offset.
    for (ID3D12Resource* r : {upload.p}) {
        const D3D12_GPU_VIRTUAL_ADDRESS va = r->GetGPUVirtualAddress();
        if (!va || va >= (1ull << 32)) std::fprintf(stderr, "VA 0x%llx is not an arena offset\n", (unsigned long long)va);
        EXPECT(va && va < (1ull << 32));
    }
    void* up = nullptr;
    void* up_again = nullptr;
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, &up)) && up);
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, &up_again)) && up_again == up);
    for (UINT i = 0; i < kCount; ++i) {
        const std::uint32_t v = input(i);
        std::memcpy(static_cast<std::uint8_t*>(up) + 4 * i, &v, 4);
    }
    upload->Unmap(0, nullptr);
    upload->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(step == 32);  // the shader reads slot 1 at +0x20
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER,
                                        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Buffer = {0, kCount, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    device->CreateShaderResourceView(upload.p, &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
    uav.Buffer = {0, kCount, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    device->CreateUnorderedAccessView(out.p, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + step});

    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Ref<ID3D12CommandQueue> queue;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    Ref<ID3D12CommandAllocator> allocator;
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p,
                                               IID_PPV_ARGS(&list.p))));
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.p);
    const UINT constants[2] = {kTag, 0};
    list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(kCount / 64, 1, 1);
    const D3D12_RESOURCE_BARRIER to_copy =
        transition(out.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    list->CopyBufferRegion(readback.p, 0, out.p, 0, kCount * 4);
    EXPECT(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
    const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, done)));
    EXPECT(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0);
    CloseHandle(done);

    void* mapped = nullptr;
    const D3D12_RANGE read{0, kCount * 4};
    EXPECT(SUCCEEDED(readback->Map(0, &read, &mapped)) && mapped);
    unsigned mismatches = 0;
    for (UINT i = 0; i < kCount; ++i) {
        std::uint32_t got = 0;
        std::memcpy(&got, static_cast<const std::uint8_t*>(mapped) + 4 * i, 4);
        const std::uint32_t want = 3 * input(i) + kTag;
        if (got != want && mismatches++ < 8) std::fprintf(stderr, "word %u: got %08x want %08x\n", i, got, want);
    }
    readback->Unmap(0, nullptr);
    EXPECT(mismatches == 0);

    // Arena pressure (review of #444): a large readback buffer kept alive takes no arena space, so a default buffer
    // created after it still gets its range. With the default 256 MiB arena, 160 MiB of readback and then 128 MiB of
    // default-heap buffer do not fit together in the arena; both must still succeed.
    const char* arena_env = std::getenv("LEONIDA_ARENA_MB");
    bool pressure_checked = false;
    if (!arena_env || !*arena_env) {
        Ref<ID3D12Resource> big_readback, big_default;
        const D3D12_RESOURCE_DESC readback_desc = plain_big(160ull << 20), default_desc = plain_big(128ull << 20);
        const HRESULT readback_hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&big_readback.p));
        const HRESULT default_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &default_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&big_default.p));
        std::printf("arena pressure: 160 MiB READBACK 0x%08lx, then 128 MiB DEFAULT 0x%08lx (VA 0x%llx)\n",
                    static_cast<unsigned long>(readback_hr), static_cast<unsigned long>(default_hr),
                    big_default.p ? (unsigned long long)big_default->GetGPUVirtualAddress() : 0ull);
        EXPECT(SUCCEEDED(readback_hr) && SUCCEEDED(default_hr));
        EXPECT(big_default->GetGPUVirtualAddress() < (1ull << 32));
        pressure_checked = true;
    }
    std::printf("upload in the arena, readback in its own memory: VAs 0x%llx and 0x%llx; %u words read through a V# "
                "on the upload buffer and read back through the readback buffer's Map pointer match%s\n",
                (unsigned long long)upload->GetGPUVirtualAddress(), (unsigned long long)readback->GetGPUVirtualAddress(),
                kCount, pressure_checked ? "; a live readback buffer leaves the arena to default buffers" : "");
    return 0;
}
