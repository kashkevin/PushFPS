// SPDX-License-Identifier: GPL-3.0-only
// Neural-layer runtime choice, file verification verdicts and looks (veyra/engine/PushNrRuntime.h).
// Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushNrRuntime.h"
#include <string>
#include <vector>
namespace {
namespace nrr = veyra::engine;
nrr::PushNrSlotFacts nrSigned(bool tested = true) {
    nrr::PushNrSlotFacts f; f.present = true; f.checked = true; f.signedOriginal = true; f.tested = tested;
    f.version = {310, 8, 0, 0}; return f;
}
nrr::PushNrGpu nrGpu(bool original, bool sf = true) { nrr::PushNrGpu g; g.supportsOriginal = original; g.supportsSf = sf; return g; }
nrr::PushNrFileFacts nrGoodFile() {
    nrr::PushNrFileFacts f; f.exists = true; f.bytes = 165840496ull; f.trust = nrr::PushNrTrust::Valid; f.signerNvidia = true;
    f.originalFilename = "nvngx_dlssnr.dll"; f.companyName = "NVIDIA Corporation"; f.version = {310, 8, 0, 0};
    f.sha256 = "E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E"; return f;
}
}

TEST(nr_runtime_default_moves_to_signed_original_when_sf_file_absent){
    nrr::PushNrInstalled in; in.original = nrSigned();
    const auto d = nrr::pushNrDecide(in, nrGpu(true), {2, false});
    CHECK(d.slot == nrr::PushNrSlot::Original); CHECK(d.usable); CHECK(d.resolvedDefault);
    CHECK(d.blocker == nrr::PushNrBlocker::None);
    // Retired ids 0 and 1 are the same default.
    CHECK(nrr::pushNrDecide(in, nrGpu(true), {0, false}).slot == nrr::PushNrSlot::Original);
    CHECK(nrr::pushNrDecide(in, nrGpu(true), {1, false}).slot == nrr::PushNrSlot::Original);
}
TEST(nr_runtime_explicit_choice_is_never_changed){
    nrr::PushNrInstalled in; in.original = nrSigned();
    const auto d = nrr::pushNrDecide(in, nrGpu(true), {2, true});
    CHECK(d.slot == nrr::PushNrSlot::Sf); CHECK(!d.usable); CHECK(d.blocker == nrr::PushNrBlocker::RuntimeMissing); CHECK(!d.resolvedDefault);
    // An explicit original stays the original even when it cannot run.
    nrr::PushNrInstalled none;
    const auto o = nrr::pushNrDecide(none, nrGpu(true), {3, true});
    CHECK(o.slot == nrr::PushNrSlot::Original); CHECK(!o.usable); CHECK(o.blocker == nrr::PushNrBlocker::RuntimeMissing);
    // The AMD slot is never moved.
    CHECK(nrr::pushNrDecide(in, nrGpu(true), {4, false}).slot == nrr::PushNrSlot::Amd);
}
TEST(nr_runtime_default_keeps_installed_sf_slot){
    // A user who opted into the experimental extras keeps today's behaviour.
    nrr::PushNrInstalled in; in.sf.present = true; in.original = nrSigned();
    const auto d = nrr::pushNrDecide(in, nrGpu(true), {2, false});
    CHECK(d.slot == nrr::PushNrSlot::Sf); CHECK(d.usable); CHECK(!d.resolvedDefault);
}
TEST(nr_runtime_never_selects_a_file_that_is_not_a_signed_original){
    nrr::PushNrInstalled in; in.original.present = true; in.original.checked = true; in.original.signedOriginal = false;
    const auto d = nrr::pushNrDecide(in, nrGpu(true), {2, false});
    CHECK(d.slot == nrr::PushNrSlot::Sf); CHECK(!d.usable); CHECK(d.blocker == nrr::PushNrBlocker::NotSignedOriginal);
    const auto e = nrr::pushNrDecide(in, nrGpu(true), {3, true});
    CHECK(!e.usable); CHECK(e.blocker == nrr::PushNrBlocker::NotSignedOriginal);
    nrr::PushNrInstalled unchecked; unchecked.original.present = true;
    CHECK(nrr::pushNrDecide(unchecked, nrGpu(true), {3, true}).blocker == nrr::PushNrBlocker::SignatureUnchecked);
    CHECK(!nrr::pushNrDecide(unchecked, nrGpu(true), {2, false}).usable);
}
TEST(nr_runtime_blockers_are_precise){
    nrr::PushNrInstalled in; in.original = nrSigned();
    const auto gpu = nrr::pushNrDecide(in, nrGpu(false), {2, false});
    CHECK(!gpu.usable); CHECK(gpu.slot == nrr::PushNrSlot::Sf); CHECK(gpu.blocker == nrr::PushNrBlocker::GpuNotSupported);
    auto old = nrGpu(true); old.driverOk = nrr::PushNrTri::No;
    CHECK(nrr::pushNrDecide(in, old, {3, true}).blocker == nrr::PushNrBlocker::DriverTooOld);
    auto unknown = nrGpu(true); unknown.driverOk = nrr::PushNrTri::Unknown;
    CHECK(nrr::pushNrDecide(in, unknown, {3, true}).usable);
    nrr::PushNrInstalled none;
    CHECK(nrr::pushNrDecide(none, nrGpu(true), {2, false}).blocker == nrr::PushNrBlocker::RuntimeMissing);
    nrr::PushNrInstalled untested; untested.original = nrSigned(false);
    const auto u = nrr::pushNrDecide(untested, nrGpu(true), {2, false});
    CHECK(u.usable); CHECK(u.slot == nrr::PushNrSlot::Original); CHECK(u.blocker == nrr::PushNrBlocker::RuntimeVersionUntested);
    CHECK(nrr::pushNrBlockerName(nrr::PushNrBlocker::NotSignedOriginal) == "not-signed-original");
    CHECK(nrr::pushNrBlockerName(nrr::PushNrBlocker::RuntimeVersionUntested) == "runtime-version-untested");
}
TEST(nr_runtime_other_cards_get_a_clean_not_supported){
    // A card that runs neither NVIDIA slot (AMD, Intel, older NVIDIA): the card is the reason, whatever is installed.
    nrr::PushNrInstalled none;
    const auto d = nrr::pushNrDecide(none, nrGpu(false, false), {2, false});
    CHECK(!d.usable); CHECK(d.slot == nrr::PushNrSlot::Sf); CHECK(d.blocker == nrr::PushNrBlocker::GpuNotSupported); CHECK(!d.resolvedDefault);
    nrr::PushNrInstalled both; both.sf.present = true; both.original = nrSigned();
    CHECK(nrr::pushNrDecide(both, nrGpu(false, false), {2, false}).blocker == nrr::PushNrBlocker::GpuNotSupported);
    CHECK(nrr::pushNrDecide(both, nrGpu(false, false), {3, true}).blocker == nrr::PushNrBlocker::GpuNotSupported);
    // The AMD slot keeps its own answer.
    nrr::PushNrGpu amd; amd.supportsAmd = true;
    nrr::PushNrInstalled amdFiles; amdFiles.amd.present = true;
    CHECK(nrr::pushNrDecide(amdFiles, amd, {4, false}).usable);
    CHECK(nrr::pushNrDecide(none, amd, {4, false}).blocker == nrr::PushNrBlocker::RuntimeMissing);
    // A card that runs the original but not the default slot: the missing original is what to install.
    CHECK(nrr::pushNrDecide(none, nrGpu(true, false), {2, false}).blocker == nrr::PushNrBlocker::RuntimeMissing);
    nrr::PushNrInstalled original; original.original = nrSigned();
    const auto moved = nrr::pushNrDecide(original, nrGpu(true, false), {2, false});
    CHECK(moved.usable && moved.slot == nrr::PushNrSlot::Original && moved.resolvedDefault);
}
TEST(nr_runtime_status_line_has_contract_keys_and_no_paths){
    nrr::PushNrInstalled in; in.original = nrSigned();
    const auto d = nrr::pushNrDecide(in, nrGpu(true), {2, false});
    const auto line = nrr::pushNrStatusLine(in, nrGpu(true), d);
    CHECK(line == "original=present originalVersion=310.8.0.0 originalSigned=1 sf=absent selected=original selectedAvailable=1 "
                  "gpuSupportsOriginal=1 reason=none originalTested=1 resolvedDefault=1");
    CHECK(line.find('\\') == std::string::npos); CHECK(line.find(':') == std::string::npos);
    nrr::PushNrInstalled none;
    const auto missing = nrr::pushNrStatusLine(none, nrGpu(false), nrr::pushNrDecide(none, nrGpu(false), {2, false}));
    CHECK(missing.find("original=absent originalVersion=none originalSigned=0") == 0);
    CHECK(missing.find("selected=sf selectedAvailable=0") != std::string::npos);
    CHECK(missing.find("reason=runtime-missing") != std::string::npos);
}
TEST(nr_verify_accepts_recorded_signed_original_as_tested){
    const auto v = nrr::pushNrJudgeFile(nrGoodFile());
    CHECK(v.accepted); CHECK(v.tested); CHECK(v.refusal == nrr::PushNrRefusal::None);
}
TEST(nr_verify_accepts_unrecorded_signed_version_as_untested){
    auto f = nrGoodFile(); f.version = {310, 9, 16, 0}; f.bytes = 90ull << 20; f.sha256 = "00";
    const auto v = nrr::pushNrJudgeFile(f);
    CHECK(v.accepted); CHECK(!v.tested);
}
TEST(nr_verify_refuses_everything_else_with_its_reason){
    const auto refused = [](nrr::PushNrFileFacts f, nrr::PushNrRefusal r) { const auto v = nrr::pushNrJudgeFile(f); return !v.accepted && v.refusal == r; };
    auto f = nrGoodFile(); f.exists = false; CHECK(refused(f, nrr::PushNrRefusal::Missing));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::NotSigned; CHECK(refused(f, nrr::PushNrRefusal::NotSigned));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::HashMismatch; CHECK(refused(f, nrr::PushNrRefusal::HashMismatch));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::Untrusted; CHECK(refused(f, nrr::PushNrRefusal::Untrusted));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::Expired; CHECK(refused(f, nrr::PushNrRefusal::Expired));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::Revoked; CHECK(refused(f, nrr::PushNrRefusal::Revoked));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::RevocationUnchecked; CHECK(refused(f, nrr::PushNrRefusal::RevocationUnchecked));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::Unchecked; CHECK(refused(f, nrr::PushNrRefusal::Unchecked));
    f = nrGoodFile(); f.signerNvidia = false; CHECK(refused(f, nrr::PushNrRefusal::OtherSigner));
    f = nrGoodFile(); f.originalFilename = "nvngx_dlss.dll"; CHECK(refused(f, nrr::PushNrRefusal::WrongFile));
    f = nrGoodFile(); f.companyName = "Someone"; CHECK(refused(f, nrr::PushNrRefusal::WrongFile));
    f = nrGoodFile(); f.version = {1, 0, 0, 0}; f.bytes = 4096; CHECK(refused(f, nrr::PushNrRefusal::SizeOutOfRange));
    // The size is judged before anything that reads the file: an unchecked tiny or huge file is refused by size.
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::Unchecked; f.bytes = 0; CHECK(refused(f, nrr::PushNrRefusal::SizeOutOfRange));
    f = nrGoodFile(); f.trust = nrr::PushNrTrust::Unchecked; f.bytes = 3ull << 30; CHECK(refused(f, nrr::PushNrRefusal::SizeOutOfRange));
    CHECK(nrr::pushNrSizeInRange(165840496ull) && !nrr::pushNrSizeInRange(nrr::kPushNrMinBytes - 1));
    // Another NVIDIA-signed DLSS module (the upscaler) is not this file, whatever its product name says.
    f = nrGoodFile(); f.originalFilename = "nvngx_dlss.dll"; f.internalName = "nvngx_dlss"; f.productName = "NVIDIA DLSS";
    f.fileDescription = "NVIDIA DLSS"; f.version = {310, 4, 0, 0}; f.bytes = 50ull << 20; f.sha256 = "aa";
    CHECK(refused(f, nrr::PushNrRefusal::WrongFile));
    f = nrGoodFile(); f.originalFilename = ""; f.internalName = "nvngx_dlssnr"; CHECK(nrr::pushNrJudgeFile(f).accepted);
    // A recorded version number over other bytes is not that version.
    f = nrGoodFile(); f.sha256 = "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8f"; CHECK(refused(f, nrr::PushNrRefusal::KnownVersionChanged));
    f = nrGoodFile(); f.sha256 = ""; CHECK(refused(f, nrr::PushNrRefusal::KnownVersionChanged));
}
TEST(nr_verify_signer_fields_compared_whole){
    CHECK(nrr::pushNrSignerIsNvidia("NVIDIA Corporation", "NVIDIA Corporation"));
    CHECK(!nrr::pushNrSignerIsNvidia("NVIDIA Corporation Fake", "NVIDIA Corporation"));
    CHECK(!nrr::pushNrSignerIsNvidia("Not NVIDIA Corporation", "NVIDIA Corporation"));
    CHECK(!nrr::pushNrSignerIsNvidia("NVIDIA Corporation", "Example Ltd"));
    CHECK(!nrr::pushNrSignerIsNvidia("", ""));
}
TEST(nr_looks_table_keeps_quick_setup_values_and_adds_conservative){
    const auto* strong = nrr::pushNrLook("strong"); CHECK(strong && strong->tone == 1.0f && strong->structure == 1.0f);
    const auto* subtle = nrr::pushNrLook("subtle"); CHECK(subtle && subtle->tone == 0.4f && subtle->structure == 0.5f);
    const auto* balanced = nrr::pushNrLook("balanced"); CHECK(balanced && balanced->tone == 0.7f && balanced->structure == 0.8f);
    const auto* conservative = nrr::pushNrLook("conservative");
    CHECK(conservative && conservative->tone < subtle->tone && conservative->structure < subtle->structure);
    CHECK(conservative->tone > 0.0f && conservative->structure > 0.0f);
    // Clearly below NVIDIA's published RTX Remix defaults on the runtime's own scale (half of each).
    CHECK(conservative->tone <= nrr::kPushNrRemixTone * 0.5f + 0.001f && conservative->structure <= nrr::kPushNrRemixStructure * 0.5f + 0.001f);
    CHECK(nrr::pushNrLook("extreme") == nullptr);
    CHECK(nrr::pushNrLookOf(1.0f, 1.0f) == "strong"); CHECK(nrr::pushNrLookOf(0.152f, 0.35f) == "conservative");
    CHECK(nrr::pushNrLookOf(0.5f, 0.5f).empty());
    // Ordered from gentlest to strongest.
    for (std::size_t i = 1; i < nrr::kPushNrLooks.size(); ++i)
        CHECK(nrr::kPushNrLooks[i].tone > nrr::kPushNrLooks[i - 1].tone && nrr::kPushNrLooks[i].structure > nrr::kPushNrLooks[i - 1].structure);
}
TEST(nr_auto_by_capability_not_by_card){
    CHECK(nrr::pushNrAutoState(false, true, true) == nrr::PushNrAutoState::RuntimeUnusable);
    CHECK(nrr::pushNrAutoState(true, true, false) == nrr::PushNrAutoState::Available);
    CHECK(nrr::pushNrAutoState(true, false, false) == nrr::PushNrAutoState::NotVerifiedHere);
    CHECK(nrr::pushNrAutoState(true, false, true) == nrr::PushNrAutoState::AvailableUnverified);
}
TEST(nr_pipeline_hook_parses_contract_keys_profile_first){
    auto items = nrr::pushPipeParse("sr=dlss; srModel=M ;srTarget=qhd;clean=40;nr=1;nrRuntime=3;nrSize=1080;nrLook=conservative;fg=vfg;fgMultiplier=2;hdr=1;profile=enhanced-1440");
    CHECK(items.size() == 12);
    CHECK(items[0].key == "profile" && items[0].value == "enhanced-1440");
    // The runtime before everything that depends on it; the rest as written.
    CHECK(items[1].key == "nrRuntime" && items[1].value == "3");
    CHECK(items[2].key == "sr" && items[2].value == "dlss");
    CHECK(items[3].key == "srModel" && items[3].value == "m");
    CHECK(items[6].key == "nr" && items[7].key == "nrSize");
    CHECK(items[11].key == "hdr" && items[11].value == "1");
    for (auto& item : items) { CHECK(item.refusal.empty()); item.applied = true; }
    const auto line = nrr::pushPipeLine(items);
    CHECK(line.find("applied=12 refused=0 detail=profile:ok,nrRuntime:ok,sr:ok,srModel:ok") == 0);
    // The kit's arm spelling: nr before nrRuntime, no profile.
    const auto arm = nrr::pushPipeParse("hdr=0;nr=1;nrRuntime=3;nrLook=conservative");
    CHECK(arm.size() == 4 && arm[0].key == "nrRuntime" && arm[1].key == "hdr" && arm[2].key == "nr" && arm[3].key == "nrLook");
    // A refused runtime value is not moved and never applied.
    const auto bad = nrr::pushPipeParse("nr=1;nrRuntime=7");
    CHECK(bad.size() == 2 && bad[0].key == "nr" && bad[1].key == "nrRuntime" && bad[1].refusal == "bad-value");
}
TEST(nr_pipeline_hook_refuses_and_names_what_it_cannot_take){
    auto items = nrr::pushPipeParse("sr=magic;turbo=1;nr=1;nr=0;fgMultiplier=9;nrLook=extreme;clean=101;nrSize=native;=;a b=1;;");
    CHECK(items.size() == 10);
    CHECK(items[0].key == "sr" && items[0].refusal == "bad-value" && items[0].value.empty());
    CHECK(items[1].key == "turbo" && items[1].refusal == "unknown-key");
    CHECK(items[2].key == "nr" && items[2].refusal.empty());
    CHECK(items[3].key == "nr" && items[3].refusal == "duplicate");
    CHECK(items[4].refusal == "bad-value"); CHECK(items[5].refusal == "bad-value"); CHECK(items[6].refusal == "bad-value");
    // An unknown DLSS SR model letter is refused by name; K, M and L (any case) and their ids are taken.
    const auto models = nrr::pushPipeParse("srModel=q;srModel=K");
    CHECK(models.size() == 2 && models[0].refusal == "unknown-model" && models[1].refusal.empty() && models[1].value == "k");
    CHECK(nrr::pushPipeSrModel("k") == 0 && nrr::pushPipeSrModel("m") == 1 && nrr::pushPipeSrModel("l") == 2 && nrr::pushPipeSrModel("1") == 1 && nrr::pushPipeSrModel("j") == -1);
    CHECK(items[7].key == "nrSize" && items[7].refusal.empty());
    CHECK(items[8].key == "invalid" && items[8].refusal == "unknown-key");
    CHECK(items[9].key == "invalid" && items[9].refusal == "unknown-key");
    items[2].applied = true; items[7].applied = true; items[7].refusal = "no-layer";
    const auto line = nrr::pushPipeLine(items);
    CHECK(line == "applied=1 refused=9 detail=sr:refused:bad-value,turbo:refused:unknown-key,nr:ok,nr:refused:duplicate,fgMultiplier:refused:bad-value,"
                  "nrLook:refused:bad-value,clean:refused:bad-value,nrSize:refused:no-layer,invalid:refused:unknown-key,invalid:refused:unknown-key");
    // Only words the contract knows reach the log line.
    for (const char c : line) CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '=' || c == ':' || c == ',' || c == ' ');
}
TEST(nr_pipeline_hook_limits_and_empty){
    CHECK(nrr::pushPipeParse("").empty());
    CHECK(nrr::pushPipeLine({}) == "applied=0 refused=0 detail=none");
    const auto tooLong = nrr::pushPipeParse(std::string(nrr::kPushPipeMaxBytes + 1, 'a'));
    CHECK(tooLong.size() == 1 && tooLong[0].refusal == "too-long");
    std::string many;
    for (int i = 0; i < 20; ++i) many += "x" + std::to_string(i) + "=1;";
    const auto capped = nrr::pushPipeParse(many);
    CHECK(capped.size() == nrr::kPushPipeMaxItems + 1 && capped.back().refusal == "too-many");
    CHECK(nrr::pushPipeNumber("100", 0, 100) == 100); CHECK(nrr::pushPipeNumber("-1", 0, 100) == -1); CHECK(nrr::pushPipeNumber("12345", 0, 99999) == -1);
    // An applied flag without acceptance is never counted as ok.
    std::vector<nrr::PushPipeItem> one{{"nr", "1", "", false}};
    CHECK(nrr::pushPipeLine(one) == "applied=0 refused=1 detail=nr:refused:not-applied");
}
