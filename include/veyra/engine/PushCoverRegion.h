// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// One-rectangle video window while something of the interface is over the picture
// (experiment VEYRA_EXP_PRESENT_RECT_REGION, sprint 5, stream "present").
//
// Why: the picture is a native child window; a menu, tip, badge or dialog over it is made visible by
// cutting its (rounded) outline out of the window's region (apps/veyra-qml/main.cpp syncVideoCovers).
// In all eight presentation runs of BENCH2 the slow regime (present to scan-out about 8 ms, variable
// refresh not active) lasted exactly while that region was complex (regionKind=3, 56 rectangles) and
// the fast regime (about 1 ms) began in the second in which it became one rectangle. Whether the shape
// of the region is the cause, or only the interface item being visible, is what this switch tests:
// with it the window stays ONE rectangle, the largest part of the picture that no cover touches. The
// strip of picture beside a cover is hidden while the cover is up (a visible cost, hence a switch).
// moonlight-qt never clips its window: it draws its overlays inside the swap chain
// (app/streaming/video/ffmpeg-renderers/d3d11va.cpp renderFrame); that is the proper fix if this
// experiment confirms the cause.
//
// Portable, allocation-free.
#include <algorithm>
#include <array>
#include <cstddef>

namespace veyra::engine {

struct PushRect {
    int left=0,top=0,right=0,bottom=0;   // right and bottom exclusive
    int width()const{return right>left?right-left:0;}
    int height()const{return bottom>top?bottom-top:0;}
    long long area()const{return static_cast<long long>(width())*height();}
    bool empty()const{return width()==0||height()==0;}
    bool operator==(const PushRect&)const=default;
};

inline constexpr std::size_t kPushCoverLimit=8;

// The largest rectangle inside 0,0,width,height that overlaps none of the covers. Covers may lie partly or
// wholly outside. Ties: the upper, then the left one. An empty rectangle when nothing is left (or the window
// has no size). More than kPushCoverLimit covers: only the first kPushCoverLimit are looked at and `complete`
// is false, so the caller can keep the old rule.
inline PushRect pushLargestClearRect(int width,int height,const PushRect* covers,std::size_t count,bool* complete=nullptr){
    if(complete)*complete=count<=kPushCoverLimit;
    if(width<=0||height<=0)return {};
    count=std::min(count,kPushCoverLimit);
    std::array<PushRect,kPushCoverLimit> clipped{};std::size_t n=0;
    std::array<int,2*kPushCoverLimit+2> xs{},ys{};std::size_t nx=0,ny=0;
    xs[nx++]=0;xs[nx++]=width;ys[ny++]=0;ys[ny++]=height;
    for(std::size_t i=0;i<count;++i){
        PushRect c{std::max(covers[i].left,0),std::max(covers[i].top,0),std::min(covers[i].right,width),std::min(covers[i].bottom,height)};
        if(c.empty())continue;
        clipped[n++]=c;xs[nx++]=c.left;xs[nx++]=c.right;ys[ny++]=c.top;ys[ny++]=c.bottom;
    }
    if(!n)return {0,0,width,height};
    std::sort(xs.begin(),xs.begin()+static_cast<std::ptrdiff_t>(nx));std::sort(ys.begin(),ys.begin()+static_cast<std::ptrdiff_t>(ny));
    PushRect best{};
    // A largest clear rectangle has every side on the window's edge or on a cover's edge.
    for(std::size_t t=0;t<ny;++t)for(std::size_t b=t+1;b<ny;++b){
        if(ys[b]<=ys[t])continue;
        for(std::size_t l=0;l<nx;++l)for(std::size_t r=l+1;r<nx;++r){
            if(xs[r]<=xs[l])continue;
            const PushRect candidate{xs[l],ys[t],xs[r],ys[b]};
            if(candidate.area()<=best.area())continue;
            bool clear=true;
            for(std::size_t i=0;i<n&&clear;++i)
                clear=clipped[i].left>=candidate.right||clipped[i].right<=candidate.left||clipped[i].top>=candidate.bottom||clipped[i].bottom<=candidate.top;
            if(clear)best=candidate;
        }
    }
    return best;
}

// How many covers reach into the picture at all (a cover that slid out of the window still counts as a
// cover in the interface, but cuts nothing).
inline std::size_t pushCoversInPicture(int width,int height,const PushRect* covers,std::size_t count){
    std::size_t inside=0;
    for(std::size_t i=0;i<count;++i)
        if(std::min(covers[i].right,width)>std::max(covers[i].left,0)&&std::min(covers[i].bottom,height)>std::max(covers[i].top,0))++inside;
    return inside;
}

} // namespace veyra::engine
