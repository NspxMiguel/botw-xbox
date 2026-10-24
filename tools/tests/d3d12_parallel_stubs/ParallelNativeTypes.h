#pragma once
#include <map>
#include <stdexcept>
struct D3D12_VERTEX_BUFFER_VIEW {
  uint64 BufferLocation{};
  uint32 SizeInBytes{}, StrideInBytes{};
  bool operator==(const D3D12_VERTEX_BUFFER_VIEW &) const = default;
};
struct D3D12_INDEX_BUFFER_VIEW {
  uint64 BufferLocation{};
  uint32 SizeInBytes{}, Format{};
};
struct D3D12_CONSTANT_BUFFER_VIEW_DESC {
  uint64 BufferLocation;
  uint32 SizeInBytes;
};
struct D3D12_RESOURCE_BARRIER {
  uint32 resource, before, after;
  bool operator==(const D3D12_RESOURCE_BARRIER &) const = default;
};
struct ID3D12PipelineState {
  uint32 id;
};
struct ID3D12RootSignature {};
struct ID3D12CommandAllocator {
  HRESULT Reset() { return 0; }
};
struct ID3D12CommandList {
  virtual ~ID3D12CommandList() = default;
};
constexpr bool FALSE = false;
constexpr uint32 D3D12_COMMAND_LIST_TYPE_DIRECT = 0;
using HANDLE = void *;
constexpr uint32 EVENT_ALL_ACCESS = 0, INFINITE = 0, WAIT_OBJECT_0 = 0;
inline HANDLE CreateEventEx(void *, void *, int, int) { return new int(0); }
inline void CloseHandle(HANDLE event) { delete static_cast<int *>(event); }
inline int WaitForSingleObjectEx(HANDLE, int, bool) { return WAIT_OBJECT_0; }
struct ID3D12Fence {
  std::atomic<uint64> completed{0};
  uint64 GetCompletedValue() { return completed.load(); }
  HRESULT SetEventOnCompletion(uint64 value, HANDLE) {
    completed.store(value);
    return 0;
  }
};
struct NativeDrawState {
  uint32 pipeline = 0, topology = 0;
  std::array<D3D12_VERTEX_BUFFER_VIEW, 16> vertices;
  D3D12_INDEX_BUFFER_VIEW indices;
  D3D12_VIEWPORT viewport;
  D3D12_RECT scissor;
  std::array<float, 4> blend{};
  uint32 stencil = 0, targetCount = 0;
  std::array<size_t, 8> targets{};
  size_t depth = 0;
  std::array<uint32, 16> runtime{};
  std::array<uint64, 12> cbvs{};
  std::array<uint64, 6> tables{};
  uint32 count = 0, instances = 0, baseInstance = 0;
  int32_t baseVertex = 0;
  bool indexed = false;
  std::vector<D3D12_RESOURCE_BARRIER> barriers;
};
struct ID3D12GraphicsCommandList : ID3D12CommandList {
  NativeDrawState state{};
  std::vector<NativeDrawState> draws;
  bool closed = false, rootBound = false, heapsBound = false;
  unsigned heapChanges = 0;
  HRESULT Close() {
    closed = true;
    return 0;
  }
  HRESULT Reset(ID3D12CommandAllocator *, void *) {
    assert(closed);
    closed = rootBound = heapsBound = false;
    state = {};
    draws.clear();
    heapChanges = 0;
    return 0;
  }
  void SetDescriptorHeaps(uint32 count, ID3D12DescriptorHeap **) {
    assert(count == 2);
    heapsBound = true;
    ++heapChanges;
    state.tables.fill(0);
  }
  void SetGraphicsRootSignature(ID3D12RootSignature *) { rootBound = true; }
  void SetPipelineState(ID3D12PipelineState *p) { state.pipeline = p->id; }
  void IASetPrimitiveTopology(uint32 t) { state.topology = t; }
  void OMSetRenderTargets(uint32 count,
                          const D3D12_CPU_DESCRIPTOR_HANDLE *targets, bool,
                          const D3D12_CPU_DESCRIPTOR_HANDLE *depth) {
    state.targetCount = count;
    for (uint32 i = 0; i < count; ++i)
      state.targets[i] = targets[i].ptr;
    state.depth = depth ? depth->ptr : 0;
  }
  void IASetVertexBuffers(uint32 first, uint32 count,
                          const D3D12_VERTEX_BUFFER_VIEW *views) {
    std::copy_n(views, count, state.vertices.begin() + first);
  }
  void RSSetViewports(uint32 count, const D3D12_VIEWPORT *v) {
    assert(count == 1);
    state.viewport = *v;
  }
  void RSSetScissorRects(uint32 count, const D3D12_RECT *r) {
    assert(count == 1);
    state.scissor = *r;
  }
  void OMSetBlendFactor(const float *b) {
    std::copy_n(b, 4, state.blend.begin());
  }
  void OMSetStencilRef(uint32 value) { state.stencil = value; }
  void SetGraphicsRootDescriptorTable(uint32 index,
                                      D3D12_GPU_DESCRIPTOR_HANDLE t) {
    state.tables.at(index) = t.ptr;
  }
  void SetGraphicsRootConstantBufferView(uint32 index, uint64 value) {
    state.cbvs.at(index - 6) = value;
  }
  void SetGraphicsRoot32BitConstants(uint32 index, uint32 count,
                                     const void *bytes, uint32 offset) {
    assert(index == 18 && count == 16 && offset == 0);
    memcpy(state.runtime.data(), bytes, 64);
  }
  void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view) {
    state.indices = *view;
  }
  void ResourceBarrier(uint32 count, const D3D12_RESOURCE_BARRIER *barriers) {
    state.barriers.insert(state.barriers.end(), barriers, barriers + count);
  }
  void DrawInstanced(uint32 count, uint32 instances, uint32 baseVertex,
                     uint32 baseInstance) {
    state.indexed = false;
    Emit(count, instances, static_cast<int32_t>(baseVertex), baseInstance);
  }
  void DrawIndexedInstanced(uint32 count, uint32 instances, uint32 first,
                            INT baseVertex, uint32 baseInstance) {
    assert(first == 0);
    state.indexed = true;
    Emit(count, instances, baseVertex, baseInstance);
  }
  void Emit(uint32 count, uint32 instances, int32_t baseVertex,
            uint32 baseInstance) {
    assert(!closed && heapsBound && rootBound);
    state.count = count;
    state.instances = instances;
    state.baseVertex = baseVertex;
    state.baseInstance = baseInstance;
    draws.push_back(state);
    state.barriers.clear();
  }
};
struct ID3D12Device {
  std::mutex mutex;
  size_t next = 0x100000;
  std::map<size_t, uint64> descriptors;
  std::atomic<uint32> copies{0};
  uint32 GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) {
    return 32;
  }
  HRESULT CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                               ID3D12DescriptorHeap **heap) {
    std::lock_guard lock(mutex);
    *heap = new ID3D12DescriptorHeap{next, next + 0x100000000};
    next += static_cast<size_t>(desc->NumDescriptors) * 32 + 0x1000;
    return 0;
  }
  void CopyDescriptorsSimple(uint32 count, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                             D3D12_CPU_DESCRIPTOR_HANDLE src,
                             D3D12_DESCRIPTOR_HEAP_TYPE) {
    std::lock_guard lock(mutex);
    for (uint32 i = 0; i < count; ++i)
      descriptors[dst.ptr + i * 32] = descriptors[src.ptr + i * 32];
    copies += count;
  }
  void CopyDescriptors(uint32 count, const D3D12_CPU_DESCRIPTOR_HANDLE *dst,
                       const UINT *size, uint32 sources,
                       const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *,
                       D3D12_DESCRIPTOR_HEAP_TYPE) {
    assert(count == 1 && *size == sources);
    std::lock_guard lock(mutex);
    for (uint32 i = 0; i < sources; ++i)
      descriptors[dst->ptr + i * 32] = descriptors[src[i].ptr];
    copies += sources;
  }
  void CreateSampler(const D3D12_SAMPLER_DESC *desc,
                     D3D12_CPU_DESCRIPTOR_HANDLE h) {
    std::lock_guard lock(mutex);
    descriptors[h.ptr] = desc->words[0];
  }
  void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                D3D12_CPU_DESCRIPTOR_HANDLE h) {
    std::lock_guard lock(mutex);
    descriptors[h.ptr] = desc->BufferLocation;
  }
  HRESULT CreateCommandAllocator(uint32, ID3D12CommandAllocator **out) {
    *out = new ID3D12CommandAllocator;
    return 0;
  }
  HRESULT CreateCommandList(uint32, uint32, ID3D12CommandAllocator *, void *,
                            ID3D12GraphicsCommandList **out) {
    *out = new ID3D12GraphicsCommandList;
    return 0;
  }
};
