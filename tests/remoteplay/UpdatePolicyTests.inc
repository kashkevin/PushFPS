// SPDX-License-Identifier: GPL-3.0-only
// The decision whether a copy may use the in-app updater (veyra/ui/UpdatePolicy.h).
// Included by CoreTests.cpp directly above main().
#include "veyra/ui/UpdatePolicy.h"
#include <string_view>
namespace {
namespace up=veyra::ui;
}
TEST(update_policy_official_build_is_unchanged){
    // No Slipstream definition and no switch: exactly what every official build does.
    CHECK(up::updatePolicy(false,false,"")==up::UpdatePolicy::Enabled);
    CHECK(up::updatesAllowed(up::UpdatePolicy::Enabled));
    CHECK(up::updatePolicyReason(up::UpdatePolicy::Enabled).empty());
    // A switch that is present but says "off" (or nothing) asks for nothing.
    for(const std::string_view value:{"","0","off","OFF","no","No","false","FALSE"," 0 ","\toff\r\n"})
        CHECK(up::updatePolicy(false,true,value)==up::UpdatePolicy::Enabled);
    // A value is only looked at when the switch is set.
    CHECK(up::updatePolicy(false,false,"1")==up::UpdatePolicy::Enabled);
}
TEST(update_policy_slipstream_build_never_updates){
    // The build definition wins over everything: no switch value can turn the updater back on.
    for(const bool set:{false,true})
        for(const std::string_view value:{"","0","off","no","false","1","on","yes","banana"})
            CHECK(up::updatePolicy(true,set,value)==up::UpdatePolicy::SlipstreamBuild);
    CHECK(!up::updatesAllowed(up::UpdatePolicy::SlipstreamBuild));
    CHECK(up::updatePolicyReason(up::UpdatePolicy::SlipstreamBuild)=="Slipstream development build");
}
TEST(update_policy_switch_turns_updates_off){
    for(const std::string_view value:{"1","on","ON","yes","Yes","true","TRUE"," 1 ","1\r","2","anything else"})
        CHECK(up::updatePolicy(false,true,value)==up::UpdatePolicy::SwitchedOff);
    CHECK(!up::updatesAllowed(up::UpdatePolicy::SwitchedOff));
    CHECK(up::updatePolicyReason(up::UpdatePolicy::SwitchedOff)=="VEYRA_EXP_RP_NO_UPDATE is set");
    CHECK(up::kUpdateSwitchName=="VEYRA_EXP_RP_NO_UPDATE");
    // The settings file can only set names with this prefix (veyra/engine/SlipstreamSettings.h).
    CHECK(up::kUpdateSwitchName.substr(0,13)=="VEYRA_EXP_RP_");
    // An out-of-range value is treated as the safe side.
    CHECK(!up::updatesAllowed(static_cast<up::UpdatePolicy>(77)));
    CHECK(up::updatePolicyReason(static_cast<up::UpdatePolicy>(77))=="unknown policy");
}
TEST(update_policy_is_usable_at_compile_time){
    static_assert(up::updatePolicy(false,false,"")==up::UpdatePolicy::Enabled);
    static_assert(up::updatePolicy(true,false,"")==up::UpdatePolicy::SlipstreamBuild);
    static_assert(up::updatePolicy(false,true,"on")==up::UpdatePolicy::SwitchedOff);
    static_assert(!up::updatesAllowed(up::UpdatePolicy::SlipstreamBuild));
    CHECK(true);
}
