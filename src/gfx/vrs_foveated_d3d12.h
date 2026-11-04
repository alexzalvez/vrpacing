#pragma once
#ifdef _WIN32

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vrpacing {

class VrsFoveatedD3D12 {
public:
    static VrsFoveatedD3D12& Instance();

    void OnSessionCreated(ID3D12Device* dev, const std::string& extraFormatsCsv = "");
    void UpdateTarget(ID3D12Device* dev,
                      bool enabled,
                      uint32_t eyeWidth,
                      uint32_t eyeHeight,
                      int debugMode,
                      bool debugLogging = false);
    void RegisterSwapchainTexture(ID3D12Resource* tex);
    void ClearSwapchainTextures();
    void RecordVisibilityMaskCall();
    void EndFrame();
    void Shutdown(const char* reason = "DestroySession");
    void ShutdownOnProcessDetach();

    int32_t GetVrsStatus() const noexcept {
        if (!tier2Supported_ || !enabled_.load(std::memory_order_relaxed)) {
            return 0;
        }
        if (bindsCount_.load(std::memory_order_relaxed) > 0 || vrs12Ready_.load(std::memory_order_relaxed)) {
            return 1;
        }
        return 2;
    }

    void OnCreateRenderTargetView(ID3D12Device* dev,
                                  ID3D12Resource* pResource,
                                  const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor);
    void OnCreateDepthStencilView(ID3D12Device* dev,
                                  ID3D12Resource* pResource,
                                  const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor);
    void OnPostOMSetRenderTargets(ID3D12GraphicsCommandList* cmdList,
                                  UINT numRenderTargetDescriptors,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                  BOOL RTsSingleHandleToDescriptorRange,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
    void OnBeginRenderPass();

private:
    struct DescriptorPod {
        uint32_t w = 0;
        uint32_t h = 0;
        DXGI_FORMAT resourceFmt = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
        uint32_t samples = 1;
        uint32_t dim = 0;
        uint32_t arraySize = 1;
        bool isSwapchain = false;
    };

    struct HistKey12 {
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t fRt = 0;
        uint32_t fView = 0;
        uint32_t ms = 0;
        uint8_t dsv = 0;
        uint8_t sc = 0;

        bool operator==(const HistKey12& o) const {
            return w == o.w && h == o.h && fRt == o.fRt && fView == o.fView &&
                   ms == o.ms && dsv == o.dsv && sc == o.sc;
        }
    };

    struct HistKey12Hash {
        size_t operator()(const HistKey12& k) const {
            size_t h = 14695981039346656037ULL;
            auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
            mix(k.w); mix(k.h); mix(k.fRt); mix(k.fView); mix(k.ms); mix(k.dsv); mix(k.sc);
            return h;
        }
    };

    struct CmdList5VtableEntry {
        bool checked = false;
        bool supported = false;
        bool pointerEqualsThis = false;
    };

    static constexpr size_t kMaxDistinctRtLogs = 32;
    static constexpr size_t kMaxHistEntries = 16;

    VrsFoveatedD3D12() = default;
    ~VrsFoveatedD3D12() = default;

    void InstallHooksLocked(ID3D12Device* rawDevice);
    bool EnsureUploadObjectsLocked();
    bool AllocateAndUploadVrsTextureLocked(uint32_t eyeW, uint32_t eyeH, int mode, bool reuploadInPlace);
    void ParseExtraFormatsLocked(const std::string& csv);
    bool IsExtraFormatAllowed(DXGI_FORMAT fmt) const noexcept;

    std::mutex mutex_;
    std::shared_mutex rtvMapMutex_;
    std::shared_mutex dsvMapMutex_;
    std::shared_mutex vtblCacheMutex_;

    std::unordered_map<size_t, DescriptorPod> rtvMap_;
    std::unordered_map<size_t, DescriptorPod> dsvMap_;
    std::unordered_set<ID3D12Resource*> swapchainTextures_;
    std::unordered_map<void**, CmdList5VtableEntry> cmdList5VtblCache_;

    DXGI_FORMAT extraFormats_[16]{};
    std::atomic<uint32_t> extraFormatsCount_{ 0 };

    ID3D12Device* device_ = nullptr;
    uint32_t rtvDescriptorSize_ = 0;
    bool capabilityChecked_ = false;
    bool tier2Supported_ = false;
    bool hooksInstalled_ = false;
    uint32_t tileSize_ = 16;
    bool additionalShadingRates_ = false;

    std::atomic<bool> debugLogging_{ false };
    std::atomic<bool> enabled_{ false };
    std::atomic<bool> vrs12Ready_{ false };
    std::atomic<bool> shutdownLogged_{ false };
    std::atomic<int> debugMode_{ 1 };
    std::atomic<uint32_t> targetEyeW_{ 0 };
    std::atomic<uint32_t> targetEyeH_{ 0 };
    std::atomic<uint32_t> capPixelsW_{ 0 };
    std::atomic<uint32_t> capPixelsH_{ 0 };
    std::atomic<uint32_t> texTilesW_{ 0 };
    std::atomic<uint32_t> texTilesH_{ 0 };
    std::atomic<uint32_t> capDwPixelsW_{ 0 };
    std::atomic<uint32_t> capDwPixelsH_{ 0 };
    std::atomic<uint32_t> texDwTilesW_{ 0 };
    std::atomic<uint32_t> texDwTilesH_{ 0 };
    std::atomic<size_t> distinctRtLogCount_{ 0 };
    std::atomic<bool> deviceRemovedLogged_{ false };

    uint32_t builtEyeW_ = 0;
    uint32_t builtEyeH_ = 0;
    int builtMode_ = -1;

    ID3D12CommandQueue* uploadQueue_ = nullptr;
    ID3D12CommandAllocator* uploadAlloc_ = nullptr;
    ID3D12GraphicsCommandList* uploadList_ = nullptr;
    ID3D12Fence* uploadFence_ = nullptr;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;
    ID3D12Resource* currentVrsTexture_ = nullptr;
    ID3D12Resource* currentVrsTextureDw_ = nullptr;
    std::vector<ID3D12Resource*> leakedResources_;

    std::unordered_set<size_t> loggedDistinctRtHandles_;
    std::unordered_map<HistKey12, uint64_t, HistKey12Hash> histogram_;

    std::atomic<uint64_t> framesCount_{ 0 };
    std::atomic<uint64_t> omsrtCount_{ 0 };
    std::atomic<uint64_t> directCount_{ 0 };
    std::atomic<uint64_t> candidatesCount_{ 0 };
    std::atomic<uint64_t> dwCount_{ 0 };
    std::atomic<uint64_t> bindsCount_{ 0 };
    std::atomic<uint64_t> resetsCount_{ 0 };
    std::atomic<uint64_t> rtvUnknownCount_{ 0 };
    std::atomic<uint64_t> beginRpCount_{ 0 };
    std::atomic<uint64_t> visMaskCallsCount_{ 0 };
};

} // namespace vrpacing

#endif // _WIN32
