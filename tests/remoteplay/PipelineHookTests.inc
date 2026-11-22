// SPDX-License-Identifier: GPL-3.0-only
// VEYRA_TEST_RP_PIPELINE's pure parser and log line (veyra/engine/PushPipelineHook.h), without any extension and with
// a test extension standing in for one. Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushPipelineHook.h"
#include <string>
#include <vector>
namespace {
namespace pph = veyra::engine;
bool pipeHookTestKnown(std::string_view key) { return key == "extra" || key == "first"; }
bool pipeHookTestValue(std::string_view, std::string_view value) { return value == "1"; }
TEST(pipeline_hook_image_keys_profile_first){
    auto items = pph::pipeHookParse("sr=dlss; srModel=M ;srTarget=qhd;clean=40;fg=vfg;fgMultiplier=2;hdr=1;profile=Clean-1440");
    CHECK(items.size() == 8);
    CHECK(items[0].key == "profile" && items[0].value == "clean-1440");
    CHECK(items[1].key == "sr" && items[1].value == "dlss");
    CHECK(items[2].key == "srModel" && items[2].value == "m");
    CHECK(items[7].key == "hdr" && items[7].value == "1");
    for (auto& item : items) { CHECK(item.refusal.empty()); item.applied = true; }
    CHECK(pph::pipeHookLine(items) == "applied=8 refused=0 detail=profile:ok,sr:ok,srModel:ok,srTarget:ok,clean:ok,fg:ok,fgMultiplier:ok,hdr:ok");
}
TEST(pipeline_hook_without_extension_refuses_other_keys_by_name){
    // A build without the neural layer: its keys are unknown, the line says so, the image keys still apply.
    auto items = pph::pipeHookParse("nr=1;nrRuntime=3;nrLook=conservative;sr=nis");
    CHECK(items.size() == 4);
    CHECK(items[0].key == "nr" && items[0].refusal == "unknown-key");
    CHECK(items[1].key == "nrRuntime" && items[1].refusal == "unknown-key");
    CHECK(items[3].key == "sr" && items[3].refusal.empty());
    items[3].applied = true;
    CHECK(pph::pipeHookLine(items) == "applied=1 refused=3 detail=nr:refused:unknown-key,nrRuntime:refused:unknown-key,nrLook:refused:unknown-key,sr:ok");
}
TEST(pipeline_hook_extension_keys_and_early_key){
    const pph::PipeHookExtension extension{pipeHookTestKnown, pipeHookTestValue, "first"};
    const auto items = pph::pipeHookParse("hdr=0;extra=1;first=1;profile=original;extra=2", &extension);
    CHECK(items.size() == 5);
    CHECK(items[0].key == "profile" && items[1].key == "first" && items[2].key == "hdr" && items[3].key == "extra");
    CHECK(items[3].refusal.empty() && items[4].key == "extra" && items[4].refusal == "bad-value");
    // A refused early key is not moved.
    const auto bad = pph::pipeHookParse("extra=1;first=7", &extension);
    CHECK(bad.size() == 2 && bad[0].key == "extra" && bad[1].key == "first" && bad[1].refusal == "bad-value");
}
TEST(pipeline_hook_refusals_limits_and_empty){
    auto items = pph::pipeHookParse("sr=magic;turbo=1;fg=vfg;fg=off;fgMultiplier=9;clean=101;srModel=q;srModel=K;=;a b=1;;");
    CHECK(items.size() == 10);
    CHECK(items[0].key == "sr" && items[0].refusal == "bad-value" && items[0].value.empty());
    CHECK(items[1].key == "turbo" && items[1].refusal == "unknown-key");
    CHECK(items[2].refusal.empty() && items[3].refusal == "duplicate");
    CHECK(items[4].refusal == "bad-value" && items[5].refusal == "bad-value");
    CHECK(items[6].refusal == "unknown-model" && items[7].refusal.empty() && items[7].value == "k");
    CHECK(items[8].key == "invalid" && items[8].refusal == "unknown-key" && items[9].key == "invalid");
    const auto line = pph::pipeHookLine(items);
    for (const char c : line) CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '=' || c == ':' || c == ',' || c == ' ');
    CHECK(pph::pipeHookSrModel("l") == 2 && pph::pipeHookSrModel("1") == 1 && pph::pipeHookSrModel("j") == -1);
    CHECK(pph::pipeHookParse("").empty());
    CHECK(pph::pipeHookLine({}) == "applied=0 refused=0 detail=none");
    const auto tooLong = pph::pipeHookParse(std::string(pph::kPipeHookMaxBytes + 1, 'a'));
    CHECK(tooLong.size() == 1 && tooLong[0].refusal == "too-long");
    std::string many;
    for (int i = 0; i < 20; ++i) many += "x" + std::to_string(i) + "=1;";
    const auto capped = pph::pipeHookParse(many);
    CHECK(capped.size() == pph::kPipeHookMaxItems + 1 && capped.back().refusal == "too-many");
    CHECK(pph::pipeHookNumber("100", 0, 100) == 100 && pph::pipeHookNumber("-1", 0, 100) == -1 && pph::pipeHookNumber("12345", 0, 99999) == -1);
    std::vector<pph::PipeHookItem> one{{"sr", "nis", "", false}};
    CHECK(pph::pipeHookLine(one) == "applied=0 refused=1 detail=sr:refused:not-applied");
}
}  // namespace
