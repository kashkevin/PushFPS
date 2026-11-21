// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushCoverRegion.h (VEYRA_EXP_PRESENT_RECT_REGION): the one rectangle the video window
// keeps while interface items are over the picture.
#include "veyra/engine/PushCoverRegion.h"
#include <array>
#include <cstdint>

TEST(cover_region_without_covers_is_the_whole_picture){
    using veyra::engine::PushRect;
    CHECK((veyra::engine::pushLargestClearRect(2560,1440,nullptr,0)==PushRect{0,0,2560,1440}));
    CHECK(veyra::engine::pushLargestClearRect(0,1440,nullptr,0).empty());
    // A cover that slid out of the window (the BENCH2 start-up item after two seconds) cuts nothing.
    const std::array<PushRect,2> outside{{{900,-80,1660,-10},{2560,100,2700,300}}};
    CHECK((veyra::engine::pushLargestClearRect(2560,1440,outside.data(),outside.size())==PushRect{0,0,2560,1440}));
    CHECK(veyra::engine::pushCoversInPicture(2560,1440,outside.data(),outside.size())==0);
}

TEST(cover_region_keeps_the_largest_part_no_cover_touches){
    using veyra::engine::PushRect;
    // A pill at the top centre: the picture below it stays, full width.
    const std::array<PushRect,1> top{{{900,16,1660,76}}};
    CHECK((veyra::engine::pushLargestClearRect(2560,1440,top.data(),1)==PushRect{0,76,2560,1440}));
    CHECK(veyra::engine::pushCoversInPicture(2560,1440,top.data(),1)==1);
    // A badge in the top right corner: losing its 40 rows costs less than losing its 200 columns.
    const std::array<PushRect,1> badge{{{2360,0,2560,40}}};
    CHECK((veyra::engine::pushLargestClearRect(2560,1440,badge.data(),1)==PushRect{0,40,2560,1440}));
    // A side panel over the right third: the left part stays.
    const std::array<PushRect,1> panel{{{1700,0,2560,1440}}};
    CHECK((veyra::engine::pushLargestClearRect(2560,1440,panel.data(),1)==PushRect{0,0,1700,1440}));
    // A dialog in the middle: the widest clear band (here the left side, 900 x 1440, against 2560 x 320 above).
    const std::array<PushRect,1> dialog{{{900,320,1660,1120}}};
    CHECK((veyra::engine::pushLargestClearRect(2560,1440,dialog.data(),1)==PushRect{0,0,900,1440}));
    // Two covers: a top bar and a corner card.
    const std::array<PushRect,2> two{{{0,0,2560,60},{2160,1200,2560,1440}}};
    CHECK((veyra::engine::pushLargestClearRect(2560,1440,two.data(),2)==PushRect{0,60,2160,1440}));
    // Everything covered: nothing of the picture is left.
    const std::array<PushRect,1> all{{{-10,-10,3000,2000}}};
    CHECK(veyra::engine::pushLargestClearRect(2560,1440,all.data(),1).empty());
}

TEST(cover_region_result_is_clear_and_cannot_grow){
    using veyra::engine::PushRect;
    // Property check on made-up layouts: the result overlaps no cover, lies inside the window, and no
    // rectangle one pixel larger on any side is clear too.
    std::uint32_t seed=12345;
    const auto next=[&seed](int bound){seed=seed*1664525u+1013904223u;return static_cast<int>((seed>>8)%static_cast<std::uint32_t>(bound));};
    const auto isClear=[](const PushRect& r,const PushRect* covers,std::size_t n,int w,int h){
        if(r.left<0||r.top<0||r.right>w||r.bottom>h)return false;
        for(std::size_t i=0;i<n;++i)
            if(std::min(covers[i].right,r.right)>std::max(covers[i].left,r.left)&&std::min(covers[i].bottom,r.bottom)>std::max(covers[i].top,r.top))return false;
        return true;
    };
    for(int round=0;round<300;++round){
        const int w=64+next(200),h=64+next(200);
        std::array<PushRect,veyra::engine::kPushCoverLimit> covers{};const std::size_t n=static_cast<std::size_t>(1+next(6));
        for(std::size_t i=0;i<n;++i){const int l=next(w+40)-20,t=next(h+40)-20;covers[i]={l,t,l+1+next(90),t+1+next(90)};}
        bool complete=false;
        const auto best=veyra::engine::pushLargestClearRect(w,h,covers.data(),n,&complete);
        CHECK(complete);
        if(best.empty())continue;
        CHECK(isClear(best,covers.data(),n,w,h));
        CHECK(!isClear({best.left-1,best.top,best.right,best.bottom},covers.data(),n,w,h));
        CHECK(!isClear({best.left,best.top-1,best.right,best.bottom},covers.data(),n,w,h));
        CHECK(!isClear({best.left,best.top,best.right+1,best.bottom},covers.data(),n,w,h));
        CHECK(!isClear({best.left,best.top,best.right,best.bottom+1},covers.data(),n,w,h));
    }
    // Too many covers: says so, so the caller keeps the old rule.
    std::array<PushRect,9> many{};for(std::size_t i=0;i<many.size();++i)many[i]={static_cast<int>(i)*10,0,static_cast<int>(i)*10+5,5};
    bool complete=true;(void)veyra::engine::pushLargestClearRect(200,200,many.data(),many.size(),&complete);
    CHECK(!complete);
}
