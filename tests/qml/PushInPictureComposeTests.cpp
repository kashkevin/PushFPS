// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "present" R1 (ui/PushInPictureCompose.h): the bridge's composition of the in-picture items. Review S1: a grabbed
// picture that carries a device pixel ratio (125 %, 150 % Windows scaling) is composed pixel for pixel, not shrunk; two
// items land at their own places in one union picture; half-transparent pixels come out as straight alpha.
// Qt Gui only: runs with g++ on Linux and in the Windows build (-platform offscreen).
#include "veyra/ui/PushInPictureCompose.h"
#include <QGuiApplication>
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::printf("FAIL %s\n", what.c_str()); }
    else std::printf("ok   %s\n", what.c_str());
}
const uint8_t* at(const veyra::engine::PushVideoOverlayImage& i, unsigned x, unsigned y) { return &i.rgba[(size_t(y) * i.width + x) * 4]; }
}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    using namespace veyra::ui;
    for (const double ratio : {1.0, 1.25, 1.5, 2.0}) {
        const std::string tag = "ratio " + std::to_string(ratio);
        QImage grabbed(50, 20, QImage::Format_ARGB32_Premultiplied);
        grabbed.fill(QColor(255, 0, 0));
        grabbed.setDevicePixelRatio(ratio);   // what grabToImage may return at Windows scaling above 100 %
        std::map<QString, InPictureLayer> layers;
        layers[QStringLiteral("badge")] = {inPictureNormalized(grabbed), 10, 5, 1};
        const auto c = inPictureCompose(layers);
        check(c && c->valid() && c->x == 10 && c->y == 5 && c->width == 50 && c->height == 20, tag + ": one item keeps its pixel size and place");
        if (!c || !c->valid()) continue;
        check(at(*c, 0, 0)[0] == 255 && at(*c, 0, 0)[3] == 255, tag + ": top-left pixel is the item's");
        check(at(*c, 49, 19)[0] == 255 && at(*c, 49, 19)[3] == 255, tag + ": bottom-right pixel is the item's (not shrunk by the ratio)");
    }
    // Two items: the union, each at its own place, transparent between them; half-transparent white as straight alpha.
    QImage badge(20, 10, QImage::Format_ARGB32_Premultiplied);
    badge.fill(QColor(0, 255, 0));
    QImage panel(10, 10, QImage::Format_ARGB32);
    panel.fill(QColor(255, 255, 255, 128));
    std::map<QString, InPictureLayer> layers;
    layers[QStringLiteral("badge")] = {inPictureNormalized(badge), 15, 15, 1};
    layers[QStringLiteral("stats")] = {inPictureNormalized(panel), 15, 40, 2};
    const auto c = inPictureCompose(layers);
    check(c && c->x == 15 && c->y == 15 && c->width == 20 && c->height == 35, "two items: one picture over their union");
    if (c && c->valid()) {
        check(at(*c, 5, 5)[1] == 255 && at(*c, 5, 5)[3] == 255, "two items: the badge at its place");
        check(at(*c, 5, 15)[3] == 0, "two items: transparent between them");
        const uint8_t* w = at(*c, 2, 27);
        check(w[3] >= 127 && w[3] <= 129 && w[0] >= 250 && w[1] >= 250 && w[2] >= 250, "two items: half-transparent white comes out straight (not premultiplied)");
    }
    check(inPictureCompose({}) == nullptr, "no items: nothing to publish");
    std::map<QString, InPictureLayer> huge;
    huge[QStringLiteral("a")] = {QImage(10, 10, QImage::Format_RGBA8888_Premultiplied), 0, 0, 1};
    huge[QStringLiteral("b")] = {QImage(10, 10, QImage::Format_RGBA8888_Premultiplied), 5000, 0, 1};
    check(inPictureCompose(huge) == nullptr, "a union wider than 4096 pixels is refused");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
