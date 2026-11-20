// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge: the optional, experimental neural layer ("AI picture enhancement": NVIDIA's DLSSNR model applied
// to a decoded video stream) with NVIDIA's signed original runtime only. docs/NEURAL_RENDERING_RUNTIME.md.
//   - What is installed in runtime\experimental\nr-original is checked before it can be selected: Windows'
//     Authenticode verdict (WinVerifyTrust), the verified signature's own leaf certificate (subject fields compared
//     whole), the version resource, the size and the SHA-256. The decision is engine/PushNrRuntime.h (pure, tested).
//     An accepted file is checked through a handle that stays open without write or delete sharing, so the file
//     NGX loads later is the file that was checked.
//   - The untouched default runtime (the community-modified slot this installer leaves out) resolves to the
//     verified original; a runtime the user picked is never changed (preference pushNrRuntimeChosen).
//   - Install from a file the user has / Remove (Settings -> Components and licenses). Nothing is downloaded and
//     the origin path is neither stored nor logged.
//   - [nr-runtime-status] once per start; the capability reasons name the precise blocker.
//   - The layer's named looks (pushNeuralLooks, pushSetNeuralLook).
//   - VEYRA_TEST_RP_PIPELINE=<key=value;...> (test-only, environment only, like VEYRA_TEST_RP_AUTORUN): applied
//     once when the stream's first picture is shown, through the setters the interface uses; one line
//     [slipstream-pipeline] applied= refused= detail= (engine/PushNrRuntime.h pushPipeParse / pushPipeLine).
// Unverified on Windows: written without a Windows compiler; the first hardware run must show the lines above.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushNrRuntime.h"
#include "veyra/Log.h"
#include "veyra/RuntimePaths.h"

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace veyra::ui {
namespace {

// What Windows said about one file; owns the strings engine::PushNrFileFacts points into.
struct NeuralFile {
    bool exists = false;
    bool signerNvidia = false;
    engine::PushNrTrust trust = engine::PushNrTrust::Unchecked;
    std::uint64_t bytes = 0;
    std::string originalFilename, internalName, productName, fileDescription, companyName, sha256;
    engine::PushNrVersion version{};
    engine::PushNrVerdict verdict;
};

std::string utf8(const wchar_t* text) { return QString::fromWCharArray(text).toStdString(); }

engine::PushNrTrust trustOf(LONG result) {
    using T = engine::PushNrTrust;
    if (result == ERROR_SUCCESS) return T::Valid;
    if (result == TRUST_E_NOSIGNATURE || result == TRUST_E_SUBJECT_FORM_UNKNOWN || result == TRUST_E_PROVIDER_UNKNOWN) return T::NotSigned;
    if (result == TRUST_E_BAD_DIGEST) return T::HashMismatch;
    if (result == CERT_E_REVOKED) return T::Revoked;
    if (result == CERT_E_EXPIRED) return T::Expired;
    if (result == CERT_E_REVOCATION_FAILURE || result == CRYPT_E_REVOCATION_OFFLINE || result == CRYPT_E_NO_REVOCATION_CHECK) return T::RevocationUnchecked;
    if (result == CERT_E_UNTRUSTEDROOT || result == CERT_E_CHAINING || result == TRUST_E_EXPLICIT_DISTRUST ||
        result == TRUST_E_SUBJECT_NOT_TRUSTED || result == CERT_E_UNTRUSTEDTESTROOT) return T::Untrusted;
    return T::OtherError;
}

// The leaf certificate of the signature Windows has just verified (its own state, not a second parse of the
// file): common name and organisation, each compared whole in engine::pushNrSignerIsNvidia.
bool verifiedSignerIsNvidia(HANDLE stateData) {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(stateData);
    CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    CRYPT_PROVIDER_CERT* leaf = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
    if (!leaf || !leaf->pCert) return false;
    char commonNameOid[] = szOID_COMMON_NAME;
    char organizationOid[] = szOID_ORGANIZATION_NAME;
    wchar_t commonName[256]{};
    wchar_t organization[256]{};
    CertGetNameStringW(leaf->pCert, CERT_NAME_ATTR_TYPE, 0, commonNameOid, commonName, 256);
    CertGetNameStringW(leaf->pCert, CERT_NAME_ATTR_TYPE, 0, organizationOid, organization, 256);
    return engine::pushNrSignerIsNvidia(utf8(commonName), utf8(organization));
}

void readVersionResource(const std::wstring& path, NeuralFile& f) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return;
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return;
    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoLength = 0;
    if (VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&fixedInfo), &fixedInfoLength) && fixedInfo && fixedInfoLength >= sizeof(VS_FIXEDFILEINFO))
        f.version = {static_cast<std::uint16_t>(HIWORD(fixedInfo->dwFileVersionMS)), static_cast<std::uint16_t>(LOWORD(fixedInfo->dwFileVersionMS)),
                     static_cast<std::uint16_t>(HIWORD(fixedInfo->dwFileVersionLS)), static_cast<std::uint16_t>(LOWORD(fixedInfo->dwFileVersionLS))};
    struct Translation { WORD language; WORD codePage; };
    Translation* translation = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation), &translationBytes) ||
        !translation || translationBytes < sizeof(Translation)) return;
    const std::wstring block = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\", static_cast<unsigned>(translation->language), static_cast<unsigned>(translation->codePage));
    const auto text = [&](const wchar_t* name) -> std::string {
        wchar_t* value = nullptr;
        UINT characters = 0;
        const std::wstring key = block + name;
        if (!VerQueryValueW(buffer.data(), key.c_str(), reinterpret_cast<void**>(&value), &characters) || !value || characters == 0) return {};
        return utf8(value);
    };
    f.originalFilename = text(L"OriginalFilename");
    f.internalName = text(L"InternalName");
    f.productName = text(L"ProductName");
    f.fileDescription = text(L"FileDescription");
    f.companyName = text(L"CompanyName");
}

engine::PushNrVerdict judge(const NeuralFile& f) {
    engine::PushNrFileFacts facts;
    facts.exists = f.exists;
    facts.bytes = f.bytes;
    facts.trust = f.trust;
    facts.signerNvidia = f.signerNvidia;
    facts.originalFilename = f.originalFilename;
    facts.internalName = f.internalName;
    facts.productName = f.productName;
    facts.fileDescription = f.fileDescription;
    facts.companyName = f.companyName;
    facts.version = f.version;
    facts.sha256 = f.sha256;
    return engine::pushNrJudgeFile(facts);
}

// Read access; others may read, nobody may write, rename or delete while the handle is open.
HANDLE openForCheck(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Everything about the open file. online: revocation is checked as Windows does for a file being installed
// (the whole chain but the root, over the network when needed); otherwise from what Windows has cached, so that
// starting the program never waits for the network.
NeuralFile inspect(HANDLE file, const std::wstring& path, bool online) {
    NeuralFile f;
    f.exists = true;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) { f.verdict = judge(f); return f; }
    f.bytes = static_cast<std::uint64_t>(size.QuadPart);

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();
    fileInfo.hFile = file;
    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    if (!online) trustData.dwProvFlags |= WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID verifyAction = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &verifyAction, &trustData);
    f.trust = trustOf(result);
    if (result == ERROR_SUCCESS && trustData.hWVTStateData) f.signerNvidia = verifiedSignerIsNvidia(trustData.hWVTStateData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &verifyAction, &trustData);

    LARGE_INTEGER origin{};
    if (SetFilePointerEx(file, origin, nullptr, FILE_BEGIN)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        std::vector<char> buffer(std::size_t{1} << 20);
        std::uint64_t total = 0;
        bool readAll = true;
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) { readAll = false; break; }
            if (count == 0) break;
            hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(count)));
            total += count;
        }
        if (readAll && total == f.bytes) f.sha256 = hash.result().toHex().toStdString();
    }
    readVersionResource(path, f);
    f.verdict = judge(f);
    return f;
}

// Bridge thread only (one bridge per process).
struct NeuralState {
    bool scanned = false;
    bool chosen = false;            // preference pushNrRuntimeChosen: the user picked a runtime
    bool applyingDefault = false;   // a runtime change made by this file, not by the user
    bool resolved = false;          // the untouched default moved to the original in this run
    bool statusLogged = false;
    HANDLE pin = INVALID_HANDLE_VALUE;   // the accepted installed original, open since it was checked
    NeuralFile original;
    std::uintmax_t seenBytes = 0;        // a refused file is checked again only when it changes
    std::filesystem::file_time_type seenTime{};
};
NeuralState& neuralState() { static NeuralState state; return state; }

std::filesystem::path originalDirectory() { return runtime::localRuntimeDirectory() / L"nr-original"; }

void logCheck(const char* what, const NeuralFile& f) {
    const auto trust = engine::pushNrTrustName(f.trust);
    const auto refusal = engine::pushNrRefusalName(f.verdict.refusal);
    veyra::log::info("nr-runtime-check", std::format("file={} trust={} signerNvidia={} version={} bytes={} sha256={} accepted={} tested={} refusal={}",
        what, std::string(trust), f.signerNvidia ? 1 : 0, engine::pushNrVersionText(f.version), f.bytes, f.sha256.empty() ? std::string("none") : f.sha256,
        f.verdict.accepted ? 1 : 0, f.verdict.tested ? 1 : 0, std::string(refusal)));
}

void releasePin(NeuralState& s) {
    if (s.pin != INVALID_HANDLE_VALUE) CloseHandle(s.pin);
    s.pin = INVALID_HANDLE_VALUE;
    s.scanned = false;
    s.original = NeuralFile{};
}

// The installed original: checked once, and again only when a refused file changes or a file appears.
void scanOriginal(NeuralState& s) {
    if (s.pin != INVALID_HANDLE_VALUE) return;   // accepted and held open: it cannot have changed
    const auto path = originalDirectory() / L"nvngx_dlssnr.dll";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) { s.original = NeuralFile{}; s.scanned = true; return; }
    const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (s.scanned && s.original.exists && bytes == s.seenBytes && writeTime == s.seenTime) return;
    s.scanned = true;
    s.seenBytes = bytes;
    s.seenTime = writeTime;
    const std::wstring native = path.wstring();
    HANDLE file = openForCheck(native);
    if (file == INVALID_HANDLE_VALUE) {
        s.original = NeuralFile{};
        s.original.exists = true;
        s.original.verdict = judge(s.original);
        logCheck("installed", s.original);
        return;
    }
    s.original = inspect(file, native, false);
    if (s.original.verdict.accepted) s.pin = file;
    else CloseHandle(file);
    logCheck("installed", s.original);
}

engine::PushNrInstalled installedFacts(NeuralState& s) {
    scanOriginal(s);
    engine::PushNrInstalled in;
    std::error_code ec;
    in.sf.present = std::filesystem::is_regular_file(runtime::localRuntimeDirectory() / L"nr-ampere" / L"nvngx_dlssnr.dll", ec);
    in.original.present = s.original.exists;
    in.original.checked = s.original.exists && s.original.trust != engine::PushNrTrust::Unchecked;
    in.original.signedOriginal = s.original.verdict.accepted;
    in.original.tested = s.original.verdict.tested;
    in.original.version = s.original.version;
    return in;
}

// Capability answers from the app's existing gates (ui/EffectAvailability.h); no card is named here.
engine::PushNrGpu gpuFacts() {
    const auto gpu = effectGpu();
    engine::PushNrGpu g;
    g.supportsSf = ui::hardwareSupports(ui::HardwareEffect::NvidiaNrSf, gpu);
    g.supportsOriginal = ui::hardwareSupports(ui::HardwareEffect::NvidiaNr50, gpu);
    g.supportsAmd = ui::hardwareSupports(ui::HardwareEffect::AmdNr, gpu);
    return g;
}

QString blockerText(engine::PushNrBlocker blocker) {
    switch (blocker) {
    case engine::PushNrBlocker::RuntimeMissing: return QObject::tr("未安装 NVIDIA 原版运行库：在 设置 → 组件与许可 中从你已有的文件安装");
    case engine::PushNrBlocker::NotSignedOriginal: return QObject::tr("nr-original 中的文件不是 NVIDIA 签名的原版，已拒绝使用");
    case engine::PushNrBlocker::SignatureUnchecked: return QObject::tr("无法验证运行库的签名");
    case engine::PushNrBlocker::GpuNotSupported: return QObject::tr("此显卡不支持");
    case engine::PushNrBlocker::DriverTooOld: return QObject::tr("显卡驱动过旧，无法运行此运行库");
    case engine::PushNrBlocker::RuntimeVersionUntested:
    case engine::PushNrBlocker::None: break;
    }
    return {};
}

QString refusalText(engine::PushNrRefusal refusal) {
    switch (refusal) {
    case engine::PushNrRefusal::NotSigned: return QObject::tr("文件没有数字签名");
    case engine::PushNrRefusal::HashMismatch: return QObject::tr("文件在签名后被改动过");
    case engine::PushNrRefusal::Untrusted: return QObject::tr("签名证书不受 Windows 信任");
    case engine::PushNrRefusal::Expired: return QObject::tr("签名证书已过期");
    case engine::PushNrRefusal::Revoked: return QObject::tr("签名证书已被吊销");
    case engine::PushNrRefusal::RevocationUnchecked: return QObject::tr("无法检查证书是否被吊销");
    case engine::PushNrRefusal::OtherSigner: return QObject::tr("签名者不是 NVIDIA Corporation");
    case engine::PushNrRefusal::WrongFile: return QObject::tr("这不是 NVIDIA 的 nvngx_dlssnr.dll");
    case engine::PushNrRefusal::SizeOutOfRange: return QObject::tr("文件大小不对");
    case engine::PushNrRefusal::KnownVersionChanged: return QObject::tr("内容与已记录的 NVIDIA 原版文件不一致");
    case engine::PushNrRefusal::Missing:
    case engine::PushNrRefusal::Unchecked:
    case engine::PushNrRefusal::SignatureError:
    case engine::PushNrRefusal::None: break;
    }
    return QObject::tr("无法验证文件的签名");
}

// --- VEYRA_TEST_RP_PIPELINE -----------------------------------------------------------------------------------------
// The text as ASCII; nullopt when unset. Too long for the parser: kept too long, so that it is refused as too-long.
std::optional<std::string> pipelineText() {
    static const std::optional<std::string> text = []() -> std::optional<std::string> {
        wchar_t value[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_TEST_RP_PIPELINE", value, 1024);
        if (length == 0) return std::nullopt;
        if (length >= 1024) return std::string(engine::kPushPipeMaxBytes + 1, '?');
        std::string out;
        for (DWORD i = 0; i < length; ++i) out.push_back(value[i] > 0 && value[i] < 128 ? static_cast<char>(value[i]) : '?');
        return out;
    }();
    return text;
}
struct PipelineHook {
    bool parsed = false, profileApplied = false, done = false;
    std::vector<engine::PushPipeItem> items;
    std::chrono::steady_clock::time_point profileAt{};
};
PipelineHook& pipelineHook() { static PipelineHook hook; return hook; }

// One key through the bridge's own setters; the reason when it could not be applied, empty when it was.
std::string applyPipelineKey(QmlPlayerBridge& b, const std::string& key, const std::string& value) {
    const auto available = [&](const QString& id) {
        return b.effectCapabilities().value(id).toMap().value(QStringLiteral("available")).toBool();
    };
    const auto layerIndices = [&] {
        std::vector<int> indices;
        for (const QVariant& layer : b.nrLayers()) indices.push_back(layer.toMap().value(QStringLiteral("index")).toInt());
        return indices;
    };
    const QString word = QString::fromStdString(value);
    if (key == "profile") return b.setPreference(QStringLiteral("pushProfile"), word) ? std::string() : std::string("not-supported");
    if (key == "clean") return b.setPreference(QStringLiteral("pushClean"), engine::pushPipeNumber(value, 0, 100)) ? std::string() : std::string("not-supported");
    if (key == "sr") {
        if (value == "off") {
            if (b.srEnabled()) b.setSrEnabled(false);
            return b.srEnabled() ? "not-applied" : "";
        }
        const int quality = value == "nis" ? int(engine::kVideoSrNis)
                          : value == "vsr-stream" ? b.srQualityFor(QStringLiteral("rtx-st"), b.srTierOf(b.videoSrQuality())) : 0;
        if (quality < 0 || !available(QStringLiteral("sr") + QString::number(quality))) return "unavailable";
        if (b.dualSrEnabled()) b.setDualSrEnabled(false);
        if (b.videoSrQuality() != quality) b.setVideoSrQuality(quality);
        if (!b.srEnabled()) b.setSrEnabled(true);
        return b.srEnabled() && b.videoSrQuality() == quality ? "" : "not-applied";
    }
    if (key == "srModel") {
        const int model = engine::pushPipeSrModel(value);
        if (b.dlssSrModel() != model) b.setDlssSrModel(model);
        return b.dlssSrModel() == model ? "" : "not-applied";
    }
    if (key == "srTarget") {
        const int index = value == "qhd" ? 1 : 2;
        if (b.srTargetIndex() != index) b.setSrTargetIndex(index);
        return b.srTargetIndex() == index ? "" : "not-applied";
    }
    if (key == "nr") {
        const bool on = value == "1";
        if (on && !b.effectAvailability(QStringLiteral("nr")).value(QStringLiteral("available")).toBool()) {
            // The precise blocker where the runtime in force names one (the NVIDIA original), else "unavailable".
            const QString blocker = b.effectAvailability(QStringLiteral("nr")).value(QStringLiteral("blocker")).toString();
            return blocker.isEmpty() || blocker == QLatin1String("none") ? std::string("unavailable") : blocker.toStdString();
        }
        if (b.nrEnabled() != on) b.setNrEnabled(on);
        return b.nrEnabled() == on ? "" : "not-applied";
    }
    if (key == "nrRuntime") {
        const auto indices = layerIndices();
        if (indices.empty()) return "no-layer";
        if (!available(QStringLiteral("nr") + word)) return "unavailable";
        // A test's runtime is not the user's pick: the default keeps resolving after the test.
        auto& s = neuralState();
        s.applyingDefault = true;
        const bool applied = b.setNrLayerParameter(indices.front(), QStringLiteral("runtime"), double(engine::pushPipeNumber(value, 2, 4)));
        s.applyingDefault = false;
        return applied ? "" : "not-applied";
    }
    if (key == "nrSize") {
        const auto policy = value == "480" ? pipeline::NrSizePolicy::P480 : value == "720" ? pipeline::NrSizePolicy::P720
                          : value == "900" ? pipeline::NrSizePolicy::P900 : value == "1440" ? pipeline::NrSizePolicy::P1440
                          : value == "native" ? pipeline::NrSizePolicy::Native : pipeline::NrSizePolicy::Realtime;
        const auto indices = layerIndices();
        if (indices.empty()) return "no-layer";
        bool applied = true;
        for (const int index : indices) applied = b.setNrLayerParameter(index, QStringLiteral("sizePolicy"), double(int(policy))) && applied;
        return applied ? "" : "not-applied";
    }
    if (key == "nrLook") return b.pushSetNeuralLook(word) ? std::string() : std::string(b.nrLayers().isEmpty() ? "no-layer" : "not-applied");
    if (key == "fg") {
        if (value == "off") {
            if (b.fgEnabled()) b.setFgEnabled(false);
            return b.fgEnabled() ? "not-applied" : "";
        }
        const QString backend = value == "fsr" ? QStringLiteral("fsr3") : word;
        if (!available(backend)) return "unavailable";
        if (b.fgBackendName() != backend) b.setFgBackendName(backend);
        if (!b.fgEnabled()) b.setFgEnabled(true);
        return b.fgEnabled() && b.fgBackendName() == backend ? "" : "not-applied";
    }
    if (key == "fgMultiplier") {
        const int multiplier = engine::pushPipeNumber(value, 2, 8);
        if (b.fgMultiplier() != multiplier) b.setFgMultiplier(multiplier);
        return b.fgMultiplier() == multiplier ? "" : "not-applied";
    }
    if (key == "hdr") {
        const bool on = value == "1";
        if (on && !available(QStringLiteral("hdr"))) return "unavailable";
        if (b.videoHdr() != on) b.setVideoHdr(on);
        return b.videoHdr() == on ? "" : "not-applied";
    }
    return "unknown-key";
}

} // namespace

// Hardware.cpp, after its own nr2 / nr3 / nr4 entries: nr3 is available only for a verified NVIDIA-signed
// original, and both NVIDIA slots name the precise blocker. The card's own answer (its existing text) stays.
void pushNeuralCapabilities(QVariantMap& out) {
    auto& s = neuralState();
    const auto in = installedFacts(s);
    const auto gpu = gpuFacts();
    const auto original = engine::pushNrSlotStatus(in, gpu, engine::PushNrSlot::Original);
    const auto blockerName = engine::pushNrBlockerName(original.blocker);
    auto cap = out.value(QStringLiteral("nr3")).toMap();
    cap[QStringLiteral("available")] = original.usable;
    if (original.usable) cap[QStringLiteral("reason")] = QString{};
    else if (original.blocker != engine::PushNrBlocker::GpuNotSupported) cap[QStringLiteral("reason")] = blockerText(original.blocker);
    cap[QStringLiteral("blocker")] = QString::fromLatin1(blockerName.data(), qsizetype(blockerName.size()));
    cap[QStringLiteral("untested")] = original.usable && original.blocker == engine::PushNrBlocker::RuntimeVersionUntested;
    out[QStringLiteral("nr3")] = cap;
    // The default slot's file is left out by this installer: say what can be used instead.
    const auto sf = engine::pushNrSlotStatus(in, gpu, engine::PushNrSlot::Sf);
    if (!sf.usable && sf.blocker == engine::PushNrBlocker::RuntimeMissing && gpu.supportsOriginal) {
        auto own = out.value(QStringLiteral("nr2")).toMap();
        own[QStringLiteral("reason")] = original.usable ? QObject::tr("此安装不含这个运行库；请改选 NVIDIA 原版运行库") : blockerText(original.blocker);
        out[QStringLiteral("nr2")] = own;
        out[QStringLiteral("nr0")] = own;
    }
}

// The runtime a layer uses: the untouched default moves to the verified original when its own file is absent
// (engine::pushNrDecide); anything the user picked comes back unchanged.
engine::NrRuntime pushNeuralDefaultRuntime(engine::NrRuntime runtime) {
    auto& s = neuralState();
    const auto in = installedFacts(s);
    engine::PushNrChoice choice;
    choice.requested = int(runtime);
    choice.explicitChoice = s.chosen;
    const auto decision = engine::pushNrDecide(in, gpuFacts(), choice);
    if (!decision.resolvedDefault) return runtime;
    if (!s.resolved) veyra::log::info("nr-default", "untouched default resolved to the verified NVIDIA original (its own slot's file is not installed)");
    s.resolved = true;
    return engine::NrRuntime::NvidiaOriginal;
}

void QmlPlayerBridge::pushNeuralLoadChoice() { neuralState().chosen = impl_->prefBool("pushNrRuntimeChosen", false); }

void QmlPlayerBridge::pushNeuralRuntimePicked() {
    auto& s = neuralState();
    if (s.applyingDefault || s.chosen) return;
    s.chosen = true;
    impl_->prefs[QStringLiteral("pushNrRuntimeChosen")] = true;
    if (!impl_->savePrefs()) veyra::log::warn("ui-prefs", "neural runtime choice not saved");
}

// After an installation: layers still on the untouched default move to the original now, as at the next start.
bool QmlPlayerBridge::pushNeuralApplyDefault() {
    auto& s = neuralState();
    for (uint32_t i = 0; i < impl_->chain.nodeCount; ++i) {
        const auto& node = impl_->chain.nodes[i];
        if (node.type != engine::EffectType::NrEnhance) continue;
        if (pushNeuralDefaultRuntime(node.nr.runtime) == node.nr.runtime) return false;
        s.applyingDefault = true;
        const bool applied = setNrLayerParameter(int(i), QStringLiteral("runtime"), double(int(engine::NrRuntime::NvidiaOriginal)));
        s.applyingDefault = false;
        return applied;
    }
    return false;
}

// VEYRA_TEST_RP_PIPELINE, from the bridge's poll timer: nothing until a stream's picture is being presented; then
// the profile (when the text has one), and the other keys in the order written. A profile is applied by the
// interface after its preference changed, so the other keys follow one second later and override it.
void QmlPlayerBridge::pushPipelineHookTick() {
    auto& hook = pipelineHook();
    if (hook.done) return;
    const auto text = pipelineText();
    if (!text) { hook.done = true; return; }
    const auto& snapshot = impl_->snapshot;
    const auto& flow = snapshot.metrics.flow;
    if (!(snapshot.running && flow.rateWindowReady && flow.realPresentFps > 0)) return;
    const auto now = std::chrono::steady_clock::now();
    if (!hook.parsed) {
        hook.parsed = true;
        hook.items = engine::pushPipeParse(*text);
    }
    std::size_t first = 0;
    if (!hook.items.empty() && hook.items.front().key == "profile" && hook.items.front().refusal.empty()) {
        first = 1;
        if (!hook.profileApplied) {
            hook.profileApplied = true;
            hook.profileAt = now;
            auto& profile = hook.items.front();
            profile.refusal = applyPipelineKey(*this, profile.key, profile.value);
            profile.applied = profile.refusal.empty();
            return;
        }
        if (now - hook.profileAt < std::chrono::seconds(1)) return;
    }
    for (std::size_t i = first; i < hook.items.size(); ++i) {
        auto& item = hook.items[i];
        if (!item.refusal.empty()) continue;
        item.refusal = applyPipelineKey(*this, item.key, item.value);
        item.applied = item.refusal.empty();
    }
    hook.done = true;
    // streamHdr / videoHdr: an HDR run can be told from an SDR one (the layer's HDR accuracy is unverified).
    veyra::log::info("slipstream-pipeline", engine::pushPipeLine(hook.items) +
        std::format(" streamHdr={} videoHdr={}", snapshot.sourceHdr ? 1 : 0, snapshot.videoHdrActive ? 1 : 0));
}

// Once per start, from the bridge's poll timer (PushAutorun.cpp): [nr-runtime-status]; then the test hook.
void QmlPlayerBridge::pushNeuralTick() {
    pushPipelineHookTick();
    auto& s = neuralState();
    if (s.statusLogged) return;
    s.statusLogged = true;
    auto in = installedFacts(s);
    in.amd.present = effectCapabilities().value(QStringLiteral("nr4")).toMap().value(QStringLiteral("available")).toBool();
    const auto gpu = gpuFacts();
    engine::PushNrChoice choice;
    choice.requested = int(settings().nrRuntime);
    for (uint32_t i = 0; i < impl_->chain.nodeCount; ++i)
        if (impl_->chain.nodes[i].type == engine::EffectType::NrEnhance) { choice.requested = int(impl_->chain.nodes[i].nr.runtime); break; }
    choice.explicitChoice = s.chosen;
    auto decision = engine::pushNrDecide(in, gpu, choice);
    decision.resolvedDefault = decision.resolvedDefault || (s.resolved && decision.slot == engine::PushNrSlot::Original);
    veyra::log::info("nr-runtime-status", engine::pushNrStatusLine(in, gpu, decision));
}

QVariantMap QmlPlayerBridge::pushNeuralRuntimeState() const {
    auto& s = neuralState();
    const auto in = installedFacts(s);
    const auto gpu = gpuFacts();
    const auto status = engine::pushNrSlotStatus(in, gpu, engine::PushNrSlot::Original);
    const auto blockerName = engine::pushNrBlockerName(status.blocker);
    const QString versionText = QString::fromStdString(engine::pushNrVersionText(in.original.version));
    QString text;
    if (!in.original.present) text = tr("未安装");
    else if (in.original.signedOriginal)
        text = (in.original.tested ? tr("已安装：%1 · NVIDIA 签名原版 · 已随本应用测试") : tr("已安装：%1 · NVIDIA 签名原版 · 此版本未随本应用测试")).arg(versionText);
    else text = tr("nr-original 中的文件未通过验证（%1），不会被使用").arg(refusalText(s.original.verdict.refusal));
    if (!gpu.supportsOriginal) text += QStringLiteral(" · ") + tr("此显卡不支持");
    return {{"installed", in.original.present}, {"signedOriginal", in.original.signedOriginal}, {"tested", in.original.tested},
            {"version", versionText}, {"gpuSupported", gpu.supportsOriginal}, {"usable", status.usable},
            {"blocker", QString::fromLatin1(blockerName.data(), qsizetype(blockerName.size()))}, {"text", text}};
}

bool QmlPlayerBridge::pushInstallNeuralRuntime() {
    const QString picked = QFileDialog::getOpenFileName(nullptr, tr("选择你已有的 NVIDIA 原版 nvngx_dlssnr.dll"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tr("NVIDIA 运行库 (nvngx_dlssnr.dll);;所有文件 (*)"));
    if (picked.isEmpty()) return false;
    auto& s = neuralState();
    // The offered file first, with Windows' own revocation check: a refused file is never copied.
    const std::wstring source = QDir::toNativeSeparators(picked).toStdWString();
    HANDLE offered = openForCheck(source);
    if (offered == INVALID_HANDLE_VALUE) { emit notice(tr("无法打开所选文件"), true); return false; }
    const NeuralFile sourceFacts = inspect(offered, source, true);
    logCheck("offered", sourceFacts);
    if (!sourceFacts.verdict.accepted) {
        CloseHandle(offered);
        emit notice(tr("没有安装：%1").arg(refusalText(sourceFacts.verdict.refusal)), true);
        return false;
    }
    // Into a new folder next to the installed one; the COPY is checked (it is the file that will be loaded) and
    // must be the same bytes; then the folders are swapped, as the ANR installer does.
    const QString target = QString::fromStdWString(originalDirectory().wstring());
    const QString fresh = target + QStringLiteral(".new"), previous = target + QStringLiteral(".old");
    QDir(fresh).removeRecursively();
    QDir(previous).removeRecursively();
    const QString freshFile = QDir(fresh).filePath(QStringLiteral("nvngx_dlssnr.dll"));
    const bool copied = QDir().mkpath(fresh) && QFile::copy(picked, freshFile);
    CloseHandle(offered);
    bool sameBytes = false;
    if (copied) {
        const std::wstring freshNative = QDir::toNativeSeparators(freshFile).toStdWString();
        HANDLE copy = openForCheck(freshNative);
        if (copy != INVALID_HANDLE_VALUE) {
            const NeuralFile copyFacts = inspect(copy, freshNative, false);
            sameBytes = copyFacts.verdict.accepted && !copyFacts.sha256.empty() && copyFacts.sha256 == sourceFacts.sha256;
            CloseHandle(copy);   // the folder cannot be renamed with the file open; it is checked again after the swap
        }
    }
    if (!sameBytes) {
        QDir(fresh).removeRecursively();
        emit notice(tr("无法复制运行库文件"), true);
        return false;
    }
    releasePin(s);
    const bool hadOld = QFileInfo::exists(target);
    if (hadOld && !QDir().rename(target, previous)) {
        QDir(fresh).removeRecursively();
        (void)currentEffectCapabilities(true);
        emit notice(tr("运行库正在使用中，请关闭画面后再试"), true);
        return false;
    }
    if (!QDir().rename(fresh, target)) {
        if (hadOld) QDir().rename(previous, target);
        QDir(fresh).removeRecursively();
        (void)currentEffectCapabilities(true);
        emit notice(tr("无法复制运行库文件"), true);
        return false;
    }
    QDir(previous).removeRecursively();
    // The installed copy is checked and held open by the capability scan; it must still be the offered bytes.
    (void)currentEffectCapabilities(true);
    const bool installed = s.pin != INVALID_HANDLE_VALUE && s.original.sha256 == sourceFacts.sha256;
    refreshComponentList();
    emit effectCapabilitiesChanged();
    if (!installed) {
        veyra::log::warn("nr-runtime-install", "the installed copy did not pass its check; it is not used");
        emit notice(tr("没有安装：%1").arg(refusalText(s.original.verdict.refusal)), true);
        return false;
    }
    veyra::log::info("nr-runtime-install", std::format("installed version={} tested={} bytes={} sha256={}",
        engine::pushNrVersionText(s.original.version), s.original.verdict.tested ? 1 : 0, s.original.bytes, s.original.sha256));
    pushNeuralApplyDefault();
    const QString versionText = QString::fromStdString(engine::pushNrVersionText(s.original.version));
    emit notice(tr("已安装 NVIDIA 原版神经渲染运行库 %1").arg(versionText) + (s.original.verdict.tested ? QString() : tr("（此版本未随本应用测试）")), false);
    return true;
}

bool QmlPlayerBridge::pushRemoveNeuralRuntime() {
    auto& s = neuralState();
    const QString target = QString::fromStdWString(originalDirectory().wstring());
    if (!QFileInfo::exists(target)) return false;
    releasePin(s);
    const bool removed = QDir(target).removeRecursively();
    (void)currentEffectCapabilities(true);
    refreshComponentList();
    emit effectCapabilitiesChanged();
    if (!removed) { emit notice(tr("运行库正在使用中，请关闭画面后再试"), true); return false; }
    veyra::log::info("nr-runtime-install", "removed by the user");
    emit notice(tr("已移除神经渲染运行库"), false);
    return true;
}

// The layer's strength as named looks (engine/PushNrRuntime.h kPushNrLooks): the first enhancement layer's tone
// and structure, written to every layer as the quick setup's looks are.
QVariantList QmlPlayerBridge::pushNeuralLooks() const {
    QVariantList out;
    for (const auto& look : engine::kPushNrLooks) {
        const QString id = QString::fromLatin1(look.id.data(), qsizetype(look.id.size()));
        const QString label = look.id == "conservative" ? tr("保守") : look.id == "subtle" ? tr("柔和") : look.id == "balanced" ? tr("均衡") : tr("强烈");
        out << QVariantMap{{"id", id}, {"label", label}, {"tone", double(look.tone)}, {"structure", double(look.structure)}};
    }
    return out;
}

bool QmlPlayerBridge::pushSetNeuralLook(const QString& name) {
    const auto* look = engine::pushNrLook(name.toStdString());
    if (!look) return false;
    bool ok = true;
    int layers = 0;
    // Collected first: setting a value can re-create the chain's nodes.
    std::vector<int> indices;
    for (uint32_t i = 0; i < impl_->chain.nodeCount; ++i)
        if (impl_->chain.nodes[i].type == engine::EffectType::NrEnhance) indices.push_back(int(i));
    for (const int index : indices) {
        ++layers;
        ok = setNrLayerParameter(index, QStringLiteral("tone"), double(look->tone)) && ok;
        ok = setNrLayerParameter(index, QStringLiteral("structure"), double(look->structure)) && ok;
    }
    return ok && layers > 0;
}

} // namespace veyra::ui
