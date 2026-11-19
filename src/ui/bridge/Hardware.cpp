// QML bridge: Hardware responsibilities.
#include "../QmlPlayerBridge_p.h"
#include "veyra/pipeline/Rdna3NrBackend.h"
#include "veyra/pipeline/AmdNrProvider.h"
#include "veyra/pipeline/VfxSrBackend.h"
#include "veyra/ngx/NvOfSession.h"
#include "veyra/Brand.h"
#include "veyra/ngx/AnrProvider.h"
#include "veyra/gfx/AmfFrcBackend.h"
#include "veyra/pushflow/PushFlowBackend.h"
#include "veyra/engine/DisplayFgBudget.h"
#include "veyra/engine/PushHardwareCapabilities.h"
#include <d3d11.h>

namespace veyra::ui {

// The first hardware adapter, the one the engine opens (gfx adapter[0]): 0x10DE NVIDIA,
// 0x1002 AMD, 0x8086 Intel, 0 unknown.
uint32_t primaryGpuVendor() {
    return effectGpu().vendor;
}
// FSR 4 ML frame generation needs an AMD RX 9000; on other GPUs the provider crashed the
// whole program (field report 2026-10-01). Known non-AMD adapters are refused up front.
bool fsr4Possible() { return currentEffectCapabilities().value("fsr4").toMap().value("available").toBool(); }
// The adapter the engine opens: the user's compute GPU when one is chosen
// (gfx::preferredAdapterLuid) and usable, else the first D3D12 hardware
// adapter in DXGI's high-performance order, as D3D12DeviceContext picks it.
// UI thread only; cached per preference.
const DXGI_ADAPTER_DESC1* engineAdapterDesc(){
    static uint64_t cachedFor=~0ull;static DXGI_ADAPTER_DESC1 cached{};static bool found=false;
    const uint64_t preferred=gfx::preferredAdapterLuid();
    if(cachedFor==preferred)return found?&cached:nullptr;
    cachedFor=preferred;found=false;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return nullptr;
    for(int pass=preferred?0:1;pass<2&&!found;++pass)
        for(UINT i=0;;++i){Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)))||!adapter)break;
            DXGI_ADAPTER_DESC1 d{};
            if(FAILED(adapter->GetDesc1(&d))||(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
            const uint64_t luid=uint64_t(uint32_t(d.AdapterLuid.HighPart))<<32|d.AdapterLuid.LowPart;
            if(pass==0&&luid!=preferred)continue;
            if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),nullptr)))continue;
            cached=d;found=true;break;
        }
    return found?&cached:nullptr;
}
// The menu checks the same adapter as the engine. The native backend then
// verifies CUDA compute capability and the exact adapter LUID.
bool vfgHardwarePossible(){
    const auto* d=engineAdapterDesc();
    if(!d)return false;
    const auto name=QString::fromWCharArray(d->Description);
    return d->VendorId==0x10DE&&(name.contains(QStringLiteral("GeForce RTX 30"),Qt::CaseInsensitive)||name.contains(QStringLiteral("GeForce RTX 40"),Qt::CaseInsensitive)||
        name.contains(QStringLiteral("GeForce RTX 50"),Qt::CaseInsensitive)||name.contains(QStringLiteral("Ada"),Qt::CaseInsensitive)||name.contains(QStringLiteral("Blackwell"),Qt::CaseInsensitive));
}
// PushFPS first-launch card: which video decode profiles the engine's adapter offers (a PS5 stream is
// H.264, or HEVC and 10-bit HEVC for HDR). Asked of the adapter itself, never looked up from a model
// name: the DXVA profiles its Direct3D 11 video device lists, on a short-lived device created the way
// FFmpegVideoDecoder creates its D3D11VA device. It says the GPU has such a decoder; a stream reports
// what it really decodes with once it runs. Asked once per chosen adapter; known=false when the adapter
// or its video device could not be asked. UI thread only. Unverified on Windows.
QVariantMap pushDecodeCapabilities(){
    static uint64_t cachedFor=~0ull;static QVariantMap cached;
    const uint64_t preferred=gfx::preferredAdapterLuid();
    if(cachedFor==preferred&&!cached.isEmpty())return cached;
    cachedFor=preferred;
    cached=QVariantMap{{"known",false},{"h264",false},{"hevc",false},{"hevc10",false}};
    const auto* desc=engineAdapterDesc();
    if(!desc)return cached;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> device;Microsoft::WRL::ComPtr<ID3D11VideoDevice> video;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||FAILED(factory->EnumAdapterByLuid(desc->AdapterLuid,IID_PPV_ARGS(&adapter)))||!adapter)return cached;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT,levels,UINT(sizeof(levels)/sizeof(levels[0])),
        D3D11_SDK_VERSION,&device,nullptr,nullptr))||!device||FAILED(device.As(&video))||!video)return cached;
    // DXVA2_ModeH264_VLD_NoFGT, DXVA_ModeHEVC_VLD_Main and _Main10 by value (their SDK symbols need dxva.h and dxguid).
    static const GUID h264{0x1b81be68,0xa0c7,0x11d3,{0xb9,0x84,0x00,0xc0,0x4f,0x2e,0x73,0xc5}};
    static const GUID hevc{0x5b11d51b,0x2f4c,0x4452,{0xbc,0xc3,0x09,0xf2,0xa1,0x16,0x0c,0xc0}};
    static const GUID hevc10{0x107af0e0,0xef1a,0x4d19,{0xab,0xa8,0x67,0xa1,0x63,0x07,0x3d,0x13}};
    bool hasH264=false,hasHevc=false,hasHevc10=false;
    const UINT count=video->GetVideoDecoderProfileCount();
    for(UINT i=0;i<count&&i<1024;++i){
        GUID profile{};
        if(FAILED(video->GetVideoDecoderProfile(i,&profile)))continue;
        hasH264=hasH264||IsEqualGUID(profile,h264);hasHevc=hasHevc||IsEqualGUID(profile,hevc);hasHevc10=hasHevc10||IsEqualGUID(profile,hevc10);
    }
    cached=QVariantMap{{"known",true},{"h264",hasH264},{"hevc",hasHevc},{"hevc10",hasHevc10}};
    return cached;
}
// PushFPS hardware capability model (veyra/engine/PushHardwareCapabilities.h): its enums carry the engine's
// numbers and its multiplier table copies displayFgMultiplierSelectable. Checked here, where both are visible.
static_assert(int(engine::PushFgMethod::Dlss)==int(engine::FrameGenerationBackend::Dlss)&&int(engine::PushFgMethod::XeSS)==int(engine::FrameGenerationBackend::XeSS)&&
    int(engine::PushFgMethod::Fsr)==int(engine::FrameGenerationBackend::Fsr)&&int(engine::PushFgMethod::Fsr4)==int(engine::FrameGenerationBackend::Fsr4)&&
    int(engine::PushFgMethod::Vfg)==int(engine::FrameGenerationBackend::Vfg)&&int(engine::PushFgMethod::Anr)==int(engine::FrameGenerationBackend::Anr)&&
    int(engine::PushFgMethod::AmdFrc)==int(engine::FrameGenerationBackend::AmdFrc)&&int(engine::PushFgMethod::PushFlow)==int(engine::FrameGenerationBackend::PushFlow),
    "PushFgMethod must keep FrameGenerationBackend's numbers");
static_assert(int(engine::PushOpticalFlow::Nvidia)==int(engine::OpticalFlowBackend::Nvidia)&&int(engine::PushOpticalFlow::AmdFidelityFx)==int(engine::OpticalFlowBackend::AmdFidelityFx)&&
    int(engine::PushOpticalFlow::GpuDis)==int(engine::OpticalFlowBackend::GpuDis),"PushOpticalFlow must keep OpticalFlowBackend's numbers");
static constexpr bool pushMultiplierTablesAgree(){
    for(unsigned b=0;b<engine::kPushFgMethodCount;++b)
        for(unsigned m=1;m<=engine::kPushFgLargestMultiplier+1;++m)
            if(engine::pushFgMultiplierSelectable(static_cast<engine::PushFgMethod>(b),m)!=engine::displayFgMultiplierSelectable(static_cast<engine::FrameGenerationBackend>(b),m))return false;
    return true;
}
static_assert(pushMultiplierTablesAgree(),"pushFgMultiplierSelectable must match displayFgMultiplierSelectable");

namespace {
// What D3D12 says about one adapter: its highest feature level (0 = could not ask) and whether it shares
// system memory (an integrated GPU).
struct PushAdapterArchitecture { uint32_t featureLevel=0; engine::PushCapability uma=engine::PushCapability::Unknown; };
PushAdapterArchitecture pushAdapterArchitecture(IDXGIAdapter1* adapter){
    PushAdapterArchitecture out;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(!adapter||FAILED(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))||!device)return out;
    // 12_2 by value: older SDK headers do not name it.
    const D3D_FEATURE_LEVEL levels[]={static_cast<D3D_FEATURE_LEVEL>(engine::kPushFeatureLevel12_2),D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS query{};query.NumFeatureLevels=UINT(sizeof(levels)/sizeof(levels[0]));query.pFeatureLevelsRequested=levels;
    if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&query,sizeof(query))))out.featureLevel=uint32_t(query.MaxSupportedFeatureLevel);
    D3D12_FEATURE_DATA_ARCHITECTURE architecture{};architecture.NodeIndex=0;
    if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE,&architecture,sizeof(architecture))))
        out.uma=architecture.UMA?engine::PushCapability::Yes:engine::PushCapability::No;
    return out;
}
}

// PushFPS: the capability model's input for the engine's adapter (PushHardwareCapabilities.h), as reported by
// D3D12, DXGI and Windows, never looked up from a model name. refreshHz and vrrState are the bridge's own
// (displayRefreshHz, vrrState). Adapter facts are asked once per chosen adapter (a short-lived D3D12 device
// per hardware adapter); HDR and the power source are asked on every call. "defaults" is what
// pushHardwareDefaults() allows at most on this PC (HEVC, HDR, PushFlow Quality 4X of a 60 fps source asked);
// it changes no setting. UI thread only. Reporting only: nothing calls it yet. Unverified on Windows.
QVariantMap pushAdapterCapabilities(double refreshHz,int vrrState){
    struct Adapter { uint64_t luid=~0ull; engine::PushHardwareCapabilities caps; };
    static Adapter cached;
    const uint64_t preferred=gfx::preferredAdapterLuid();
    engine::PushHardwareCapabilities caps;
    if(cached.luid!=preferred){
        cached=Adapter{};cached.luid=preferred;
        if(const auto* desc=engineAdapterDesc()){
            auto& c=cached.caps;
            c.vendor=engine::pushGpuVendorFromPciId(desc->VendorId);
            c.dedicatedVramBytes=uint64_t(desc->DedicatedVideoMemory);
            Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
            if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))){
                bool othersKnown=true,otherIntegrated=false;
                for(UINT i=0;i<64;++i){
                    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                    if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)))||!adapter)break;
                    DXGI_ADAPTER_DESC1 d{};
                    if(FAILED(adapter->GetDesc1(&d))||(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
                    const auto architecture=pushAdapterArchitecture(adapter.Get());
                    if(d.AdapterLuid.LowPart==desc->AdapterLuid.LowPart&&d.AdapterLuid.HighPart==desc->AdapterLuid.HighPart){
                        c.featureLevel=architecture.featureLevel;c.uma=architecture.uma;
                    }else if(architecture.uma==engine::PushCapability::Unknown)othersKnown=false;
                    else otherIntegrated=otherIntegrated||architecture.uma==engine::PushCapability::Yes;
                }
                // Hybrid: the engine runs on a discrete adapter and an integrated one exists (a laptop, or a desktop
                // with the CPU's graphics enabled); the display may then be driven by the other adapter.
                if(c.uma==engine::PushCapability::No)
                    c.hybridGpu=otherIntegrated?engine::PushCapability::Yes:othersKnown?engine::PushCapability::No:engine::PushCapability::Unknown;
                else if(c.uma==engine::PushCapability::Yes)c.hybridGpu=engine::PushCapability::No;
            }
        }
    }
    caps=cached.caps;
    const QVariantMap decode=pushDecodeCapabilities();
    const bool decodeKnown=decode.value("known").toBool();
    caps.decodeH264=engine::pushCapability(decodeKnown,decode.value("h264").toBool());
    caps.decodeHevc=engine::pushCapability(decodeKnown,decode.value("hevc").toBool());
    caps.decodeHevc10=engine::pushCapability(decodeKnown,decode.value("hevc10").toBool());
    const auto effects=currentEffectCapabilities();
    const auto has=[&](const char* id){return effects.value(QLatin1String(id)).toMap().value("available").toBool();};
    for(const auto& [id,method]:std::initializer_list<std::pair<const char*,engine::PushFgMethod>>{{"dlss",engine::PushFgMethod::Dlss},{"xess",engine::PushFgMethod::XeSS},
            {"fsr3",engine::PushFgMethod::Fsr},{"fsr4",engine::PushFgMethod::Fsr4},{"vfg",engine::PushFgMethod::Vfg},{"anr",engine::PushFgMethod::Anr},
            {"amdfrc",engine::PushFgMethod::AmdFrc},{"pushflow",engine::PushFgMethod::PushFlow}})
        if(has(id))caps.fgMethods|=engine::pushFgBit(method);
    if(has("flow0"))caps.opticalFlows|=engine::pushFlowBit(engine::PushOpticalFlow::Nvidia);
    if(has("flow1"))caps.opticalFlows|=engine::pushFlowBit(engine::PushOpticalFlow::AmdFidelityFx);
    caps.opticalFlows|=engine::pushFlowBit(engine::PushOpticalFlow::GpuDis);   // the project's own shaders
    caps.refreshHz=std::isfinite(refreshHz)&&refreshHz>0?refreshHz:0;
    caps.vrr=engine::pushVrrState(vrrState);
    // HDR: any display output in PQ / BT.2020 mode (the window's own monitor is the presenter's business).
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;bool anyOutput=false,anyHdr=false,allAnswered=true;
        if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            for(UINT a=0;a<64;++a){
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND||!adapter)break;
                for(UINT o=0;o<64;++o){
                    Microsoft::WRL::ComPtr<IDXGIOutput> output;
                    if(FAILED(adapter->EnumOutputs(o,&output))||!output)break;
                    anyOutput=true;
                    Microsoft::WRL::ComPtr<IDXGIOutput6> six;DXGI_OUTPUT_DESC1 desc{};
                    if(FAILED(output.As(&six))||!six||FAILED(six->GetDesc1(&desc))){allAnswered=false;continue;}
                    anyHdr=anyHdr||desc.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                }
            }
        caps.hdrDisplay=anyHdr?engine::PushCapability::Yes:anyOutput&&allAnswered?engine::PushCapability::No:engine::PushCapability::Unknown;
    }
    SYSTEM_POWER_STATUS power{};
    if(GetSystemPowerStatus(&power))
        caps.power=power.BatteryFlag==128||power.ACLineStatus==1?engine::PushPowerSource::Mains:power.ACLineStatus==0?engine::PushPowerSource::Battery:engine::PushPowerSource::Unknown;

    engine::PushHardwareRequest most;most.codec=engine::PushStreamCodec::Hevc;most.hdr=true;most.frameGeneration=true;
    most.fgMethod=engine::PushFgMethod::PushFlow;most.multiplier=4;most.tier=2;most.opticalFlow=engine::PushOpticalFlow::Nvidia;most.sourceFps=60;
    const auto defaults=engine::pushHardwareDefaults(caps,most);
    QStringList reasons;
    for(const auto reason:defaults.reasons)reasons<<QString::fromLatin1(reason.data(),qsizetype(reason.size()));
    const auto name=[](engine::PushCapability c){return c==engine::PushCapability::Yes?QStringLiteral("yes"):c==engine::PushCapability::No?QStringLiteral("no"):QStringLiteral("unknown");};
    const auto vendor=engine::pushGpuVendorName(caps.vendor);
    const QString power_=caps.power==engine::PushPowerSource::Mains?QStringLiteral("mains"):caps.power==engine::PushPowerSource::Battery?QStringLiteral("battery"):QStringLiteral("unknown");
    QVariantList targets30,targets60;
    for(const auto& t:engine::pushValidOutputTargets(defaults.fgMethod,30,caps.refreshHz))targets30<<QVariantMap{{"multiplier",t.multiplier},{"fps",t.outputFps},{"even",t.evenWithoutVrr}};
    for(const auto& t:engine::pushValidOutputTargets(defaults.fgMethod,60,caps.refreshHz))targets60<<QVariantMap{{"multiplier",t.multiplier},{"fps",t.outputFps},{"even",t.evenWithoutVrr}};
    QVariantMap out{{"vendor",QString::fromLatin1(vendor.data(),qsizetype(vendor.size()))},{"vramMB",qulonglong(caps.dedicatedVramBytes>>20)},
        {"featureLevel",QString::number(caps.featureLevel,16)},{"uma",name(caps.uma)},{"decodeH264",name(caps.decodeH264)},{"decodeHevc",name(caps.decodeHevc)},
        {"decodeHevc10",name(caps.decodeHevc10)},{"fgMethods",uint(caps.fgMethods)},{"opticalFlows",uint(caps.opticalFlows)},{"refreshHz",caps.refreshHz},
        {"vrrState",int(caps.vrr)},{"hdrDisplay",name(caps.hdrDisplay)},{"hybridGpu",name(caps.hybridGpu)},{"power",power_},
        {"defaults",QVariantMap{{"codec",defaults.codec==engine::PushStreamCodec::Hevc?QStringLiteral("hevc"):QStringLiteral("h264")},{"hdr",defaults.hdr},
            {"frameGeneration",defaults.frameGeneration},{"fgMethod",int(defaults.fgMethod)},{"multiplier",defaults.multiplier},{"tier",defaults.tier},
            {"opticalFlow",int(defaults.opticalFlow)},{"reasons",reasons},{"targets30",targets30},{"targets60",targets60}}}};
    veyra::log::info("push-gpu-caps",std::format("vendor={} vramMB={} featureLevel={:#x} uma={} decode h264={} hevc={} hevc10={} fgMethods={:#x} flows={:#x} refresh={:.2f} vrr={} "
        "hdr={} hybrid={} power={} ceiling codec={} hdr={} fg={} method={} multiplier={} tier={} reasons={}",vendor,caps.dedicatedVramBytes>>20,caps.featureLevel,
        name(caps.uma).toStdString(),name(caps.decodeH264).toStdString(),name(caps.decodeHevc).toStdString(),name(caps.decodeHevc10).toStdString(),caps.fgMethods,
        caps.opticalFlows,caps.refreshHz,int(caps.vrr),name(caps.hdrDisplay).toStdString(),name(caps.hybridGpu).toStdString(),power_.toStdString(),
        defaults.codec==engine::PushStreamCodec::Hevc?"hevc":"h264",defaults.hdr,defaults.frameGeneration,int(defaults.fgMethod),defaults.multiplier,defaults.tier,
        reasons.join(',').toStdString()));
    return out;
}
unsigned normalizeFgMultiplier(unsigned value,engine::FrameGenerationBackend backend,unsigned cap){
    value=std::clamp(value,2u,cap);
    return backend==engine::FrameGenerationBackend::Dlss&&value==5?4u:value;
}
bool QmlPlayerBridge::amdNrGpu() const { return primaryGpuVendor()==0x1002; }

ui::EffectGpu effectGpu() {
    ui::EffectGpu result;
    const auto* d = engineAdapterDesc();
    if (!d) return result;
    const auto name = QString::fromWCharArray(d->Description);
    result.vendor = d->VendorId;
    result.deviceId = d->DeviceId;
    result.rtx = name.contains("RTX", Qt::CaseInsensitive);
    result.blackwell = name.contains("RTX 50", Qt::CaseInsensitive) || name.contains("Blackwell", Qt::CaseInsensitive);
    result.ada = name.contains("RTX 40", Qt::CaseInsensitive) || name.contains("Ada", Qt::CaseInsensitive);
    result.ampere = name.contains("GeForce RTX 30", Qt::CaseInsensitive);
    result.rx9000 = name.contains("RX 90", Qt::CaseInsensitive);
    result.rx7000 = pipeline::amdNrProvider(d->VendorId,d->Description)==pipeline::AmdNrProvider::RedDukeRdna3;
    return result;
}

static QVariantMap scanEffectCapabilities() {
    using H = ui::HardwareEffect;
    const auto gpu = effectGpu();
    const auto nr = runtime::localRuntimeDirectory();
    const auto data = runtime::localDataDirectory();
    auto exists = [](const std::filesystem::path& path) { std::error_code ec; return std::filesystem::is_regular_file(path, ec); };
    const auto fsr = data / "amd/fidelityfx";
    const bool fsrLoader = exists(fsr / "amd_fidelityfx_loader_dx12.dll");
    QVariantMap out;
    auto add = [&](const QString& id, H hardware, bool files, const QString& requirement) {
        const bool supported = ui::hardwareSupports(hardware, gpu);
        out[id] = QVariantMap{{"available", supported && files}, {"reason", !supported ? requirement : !files ? QObject::tr("当前包缺少此功能的运行组件") : QString{}}};
    };
    add("nr2", H::NvidiaNrSf, exists(nr / "nr-ampere/nvngx_dlssnr.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    out["nr0"]=out["nr2"]; // Retired preset ID; no retired runtime is loaded.
    out["nrAuto"]=QVariantMap{{"available",gpu.vendor==0x10DE&&gpu.deviceId==0x2f04&&out.value("nr0").toMap().value("available").toBool()},
        {"reason",gpu.deviceId!=0x2f04?QObject::tr("自动 NR 当前仅验证 RTX 5070"):!out.value("nr0").toMap().value("available").toBool()?QObject::tr("当前包缺少此功能的运行组件"):QString{}}};
    add("nr3", H::NvidiaNr50, exists(nr / "nr-original/nvngx_dlssnr.dll"), QObject::tr("需要 NVIDIA RTX 50 / Blackwell"));
    const auto rdna3Directory=(nr/"../amd-nr-rdna3").lexically_normal();
    const bool amdNrFiles=gpu.rx7000?pipeline::Rdna3NrBackend::runtimeAvailable(rdna3Directory):exists(nr / "../amd-nr/LmxxfNrRuntime.dll");
    add("nr4", H::AmdNr, amdNrFiles, QObject::tr("需要 AMD RX 7000 / 9000"));
    if(gpu.rx7000&&!amdNrFiles){auto cap=out.value("nr4").toMap();cap["reason"]=QObject::tr("当前包缺少 RX 7000 NR 运行组件");out["nr4"]=cap;}
    wchar_t systemDir[MAX_PATH]{};
    const bool hipMissing=gpu.rx7000?!pipeline::Rdna3NrBackend::hipRuntimeAvailable():
        GetSystemDirectoryW(systemDir, MAX_PATH)&&!exists(std::filesystem::path(systemDir)/"amdhip64_7.dll")&&!GetModuleHandleW(L"amdhip64_7.dll");
    if (hipMissing) {
        auto cap = out.value("nr4").toMap();
        if (cap.value("available").toBool()) { cap["available"] = false; cap["reason"] = QObject::tr("需要 AMD HIP 7 驱动运行组件"); out["nr4"] = cap; }
    }
    add("sr0", H::Nvidia, exists(nr / "nvngx_dlss.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    for (int i = 1; i <= 4; ++i) add("sr" + QString::number(i), H::Nvidia, exists(nr / "nvngx_vsr.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    // RTX Video SR high bitrate / streaming: Video Effects VideoSuperRes, from the VFG runtime directory.
    const bool vfxSr = pipeline::VfxSrBackend::runtimeAvailable(nr.wstring());
    for (int q : {16, 17, 18, 19}) add("sr" + QString::number(q), H::Nvidia, vfxSr, QObject::tr("需要 NVIDIA RTX 显卡"));
    for (int q : {21, 23}) add("sr" + QString::number(q), H::NvidiaAmpere, vfxSr, QObject::tr("需要 NVIDIA RTX 30 及更新显卡"));
    add("sr5", H::CrossVendor, fsrLoader && exists(fsr / "amd_fidelityfx_upscaler_dx12.dll"), QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    // PushFPS: NVIDIA Image Scaling ships as shaders next to the program (no runtime DLL).
    add("sr6", H::CrossVendor, exists(runtime::shaderDirectory() / "NisScalerSdr.dxil") && exists(runtime::shaderDirectory() / "NisScalerPq.dxil") &&
        exists(runtime::shaderDirectory() / "NisColor.dxil"), QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    // PushFPS: the usual reason ANR is unavailable is that its files are not installed yet.
    add("anr", H::CrossVendor, brand::kAnrFrameGeneration && ngx::AnrProvider::available(nr.wstring()),
        ngx::AnrProvider::available(nr.wstring()) ? QObject::tr("需要支持 DirectX 12 的硬件显卡")
                                                  : QObject::tr("先在 设置 → 组件与许可 中安装 Apex 的 ANR"));
    add("dlss", H::Nvidia, exists(nr / "nvngx_dlssg.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    add("vfg", H::Vfg, pipeline::VfgBackend::runtimeAvailable(nr.wstring()), QObject::tr("需要 NVIDIA RTX 30 / 40 / 50（30 系为实验兼容）"));
    add("xess", H::CrossVendor, exists(data / "intel/experimental/libxess_fg.dll") && exists(data / "intel/experimental/libxell.dll"), QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    add("fsr3", H::CrossVendor, fsrLoader && exists(fsr / "amd_fidelityfx_framegeneration_dx12.dll"), QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    add("fsr4", H::Fsr4, fsrLoader && exists(fsr / "amd_fidelityfx_framegeneration_dx12.dll"), QObject::tr("需要 AMD RX 9000"));
    // AMD FRC uses the AMF runtime the AMD driver installs; nothing ships with Veyra.
    add("amdfrc", H::AmdFrc, gfx::AmfFrcBackend::runtimeInstalled(), QObject::tr("需要 AMD 显卡（驱动自带 AMF 运行库）"));
    // PushFPS: PushFlow (RIFE on DirectML) runs on any DirectX 12 GPU; its runtime ships in runtime\experimental\pushflow.
    add("pushflow", H::CrossVendor, pushflow::runtimeInstalled(nr.wstring()),
        pushflow::runtimeInstalled(nr.wstring()) ? QObject::tr("需要支持 DirectX 12 的硬件显卡")
                                                                        : QObject::tr("缺少 PushFlow 运行组件，请重新安装 PushFPS"));
    add("hdr", H::Nvidia, exists(nr / "nvngx_truehdr.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    // PushFPS: a build without the NVIDIA Optical Flow SDK reports NVOF unavailable, and the
    // sanitizer then runs every chain on AMD FidelityFX optical flow (flow1).
    if (ngx::NvOfSession::builtWithSdk())
        add("flow0", H::Nvidia, true, QObject::tr("需要 NVIDIA RTX 显卡及 NVOF 驱动"));
    else
        out["flow0"] = QVariantMap{{"available", false}, {"reason", QObject::tr("此版本未包含 NVIDIA 光流，已改用 AMD FidelityFX 光流")}};
    add("flow1", H::CrossVendor, true, QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    return out;
}
// All callers are on the bridge/UI thread. Snapshot reads must not perform
// filesystem probes or rebuild capability maps. Refresh on actions, language
// changes and a slow timer so user-replaced runtime files remain discoverable.
QVariantMap currentEffectCapabilities(bool refresh) {
    static QVariantMap cached;
    static QString language;
    const auto currentLanguage=i18n::current();
    if(refresh||cached.isEmpty()||language!=currentLanguage){
        cached=scanEffectCapabilities();language=currentLanguage;
    }
    return cached;
}
QString fgEffectId(engine::FrameGenerationBackend backend) {
    switch (backend) {
    case engine::FrameGenerationBackend::Dlss: return "dlss";
    case engine::FrameGenerationBackend::XeSS: return "xess";
    case engine::FrameGenerationBackend::Fsr: return "fsr3";
    case engine::FrameGenerationBackend::Fsr4: return "fsr4";
    case engine::FrameGenerationBackend::Vfg: return "vfg";
    case engine::FrameGenerationBackend::Anr: return "anr";
    case engine::FrameGenerationBackend::AmdFrc: return "amdfrc";
    case engine::FrameGenerationBackend::PushFlow: return "pushflow";
    }
    return {};
}
QString unavailableEffects(const engine::EnhancementSettings& settings) {
    const auto caps = currentEffectCapabilities(true);
    QStringList requested;
    if (settings.nr) requested << "nr" + QString::number(int(engine::currentNrRuntime(settings.nrRuntime)));
    if (settings.sr) requested << "sr" + QString::number(settings.videoSrQuality);
    // Double SR runs a second pass with its own backend (any SR algorithm).
    if (settings.sr && settings.dualSrEnabled) requested << "sr" + QString::number(settings.dualSrQuality);
    if (settings.multiplier > 1) requested << fgEffectId(settings.frameGenerationBackend);
    if (settings.videoHdr.enabled) requested << "hdr";
    for (const auto& id : requested) {
        const auto cap = caps.value(id).toMap();
        if (!cap.value("available").toBool()) return id + ": " + cap.value("reason").toString();
    }
    return {};
}
QString disableUnavailableEffects(engine::ChainConfiguration& configuration) {
    const auto caps = currentEffectCapabilities();
    const auto available = [&](const char* id) { return caps.value(id).toMap().value("available").toBool(); };
    const engine::FrameGenerationAvailability fgAvailable{
        available("dlss"),available("xess"),available("fsr3"),available("fsr4"),available("vfg"),available("anr"),available("amdfrc"),available("pushflow")};
    QStringList reasons;
    const auto sanitize = [&](engine::EffectChain& chain, engine::ChainGlobalSettings& globals) {
        // Also migrate disabled stages: keeping an unavailable DLSS selection
        // left the AMD dropdown at DLSS even though a fresh profile chose FSR.
        const auto savedBackend = globals.fgBackend;
        globals.fgBackend = engine::restoredFrameGenerationBackend(savedBackend,fgAvailable);
        if (globals.fgBackend != savedBackend) {
            if (engine::twoXFrameGeneration(globals.fgBackend)) chain.fgMultiplier=2;
            else if (globals.fgBackend==engine::FrameGenerationBackend::XeSS)
                chain.fgMultiplier=normalizeFgMultiplier(chain.fgMultiplier,globals.fgBackend,2);
            else chain.fgMultiplier=normalizeFgMultiplier(chain.fgMultiplier,globals.fgBackend,globals.fgBackend==engine::FrameGenerationBackend::PushFlow?4:6);
            veyra::log::warn("fg-default",std::format("unavailable saved backend={} selected={} rememberedMultiplier={} enabled flags preserved",
                engine::frameGenerationBackendName(savedBackend),engine::frameGenerationBackendName(globals.fgBackend),chain.fgMultiplier));
        }
        for (uint32_t i = 0; i < chain.nodeCount; ++i) {
            auto& node = chain.nodes[i];
            QString id;
            switch (node.type) {
            case engine::EffectType::NrEnhance: id = "nr" + QString::number(int(engine::currentNrRuntime(node.nr.runtime))); break;
            case engine::EffectType::SuperResolution: id = "sr" + QString::number(globals.videoSrQuality); break;
            case engine::EffectType::FrameGeneration: id = fgEffectId(globals.fgBackend); break;
            case engine::EffectType::VideoHdr: id = "hdr"; break;
            default: break;
            }
            if (id.isEmpty() || !node.enabled) continue;
            const auto cap = caps.value(id).toMap();
            if (!cap.value("available").toBool()) {
                node.enabled = false;
                reasons << id + ": " + cap.value("reason").toString();
            }
        }
        if (globals.opticalFlowBackend == engine::OpticalFlowBackend::Nvidia &&
            !caps.value("flow0").toMap().value("available").toBool())
            globals.opticalFlowBackend = engine::OpticalFlowBackend::AmdFidelityFx;
        // A double SR second algorithm this card cannot run (a profile moved from
        // an NVIDIA card) follows the base algorithm, or double SR turns off.
        const auto runs = [&](uint32_t quality) { return caps.value("sr" + QString::number(quality)).toMap().value("available").toBool(); };
        if (globals.dualSrEnabled && !runs(globals.dualSrQuality)) {
            const auto saved = globals.dualSrQuality;
            if (runs(globals.videoSrQuality)) globals.dualSrQuality = globals.videoSrQuality;
            else globals.dualSrEnabled = false;
            veyra::log::warn("effect-availability", std::format("double SR second quality={} unavailable; second={} enabled={}",
                saved, globals.dualSrQuality, globals.dualSrEnabled));
        }
    };
    sanitize(configuration.chain, configuration);
    if (configuration.editor) {
        auto editor = std::make_shared<engine::NodeEditorDocument>(*configuration.editor);
        auto globals = editor->globals.value_or(static_cast<const engine::ChainGlobalSettings&>(configuration));
        sanitize(editor->nodes, globals);
        editor->globals = globals;
        configuration.editor = std::move(editor);
    }
    reasons.removeDuplicates();
    return reasons.join("; ");
}
QVariantMap QmlPlayerBridge::effectCapabilities() const { return currentEffectCapabilities(); }
QVariantMap QmlPlayerBridge::effectAvailability(const QString& id) const {
    const auto& s = settings();
    const QString key = id == "nr" ? "nr" + QString::number(int(engine::currentNrRuntime(s.nrRuntime))) :
        id == "sr" ? "sr" + QString::number(s.videoSrQuality) : id == "video-hdr" ? QString("hdr") :
        id == "frame-generation" ? fgEffectId(s.frameGenerationBackend) :
        id == "dlss-fg" ? QString("dlss") : id == "xess-fg" ? QString("xess") :
        id == "fsr3-fg" ? QString("fsr3") : id == "fsr4-fg" ? QString("fsr4") : id == "vfg-fg" ? QString("vfg") : id == "anr-fg" ? QString("anr") : id == "amdfrc-fg" ? QString("amdfrc") : id == "pushflow-fg" ? QString("pushflow") : id;
    const auto caps = effectCapabilities();
    return caps.contains(key) ? caps.value(key).toMap() : QVariantMap{{"available", true}, {"reason", QString{}}};
}
QVariantList QmlPlayerBridge::nrRuntimeChoices() const {
    QVariantList choices;
    const auto caps = effectCapabilities();
    for (const auto& entry : std::initializer_list<std::pair<int, QString>>{{2, "RTX 20–50 · SF 310.9.16"}, {3, tr("RTX 50 · NVIDIA 原版")}, {4, effectGpu().rx7000?tr("RX 7000 · RedDuke（实验）"):tr("RX 9000 · lmxxf（实验）")}}) {
        const auto cap = caps.value("nr" + QString::number(entry.first)).toMap();
        choices << QVariantMap{{"id", QString::number(entry.first)}, {"label", entry.second}, {"disabled", !cap.value("available").toBool()}, {"note", cap.value("reason")}};
    }
    return choices;
}
QVariantList QmlPlayerBridge::srBackendChoices() const {
    QVariantList choices;
    const auto caps = effectCapabilities();
    // RTX Video SR families take string ids (srAlgorithmOf); the tier is chosen
    // separately. Availability follows each family's runtime.
    struct Entry { QString id; int capability; QString label; };
    for (const auto& entry : std::initializer_list<Entry>{{"0", 0, "DLSS SR"}, {"rtx", 4, tr("RTX 视频超分 · 标准")},
            {"rtx-hb", 16, tr("RTX 视频超分 · 高码率")}, {"rtx-st", 21, tr("RTX 视频超分 · 流媒体")},
            {"5", 5, effectGpu().vendor == 0x1002 ? tr("FSR 3.1 / 4 · 自动兼容") : QString("FSR 3.1")},
            {"6", 6, QStringLiteral("NVIDIA Image Scaling (NIS)")}}) {
        const auto cap = caps.value("sr" + QString::number(entry.capability)).toMap();
        choices << QVariantMap{{"id", entry.id}, {"label", entry.label}, {"disabled", !cap.value("available").toBool()}, {"note", cap.value("reason")}};
    }
    return choices;
}


} // namespace veyra::ui
