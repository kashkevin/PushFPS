// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "present" R1: the bridge's composition of the in-picture items (src/ui/bridge/PushFps.cpp pushSetVideoOverlay) into
// the one straight-alpha picture the presenter draws (engine/PushVideoOverlay.h). Qt Gui only (no Windows), so it is tested
// on its own: tests/qml/PushInPictureComposeTests.cpp (Linux g++ and the Windows build).
// Review S1: a grabbed QImage can carry a device pixel ratio above 1 (Windows scaling of 125 % and more); QPainter would then
// draw it at size / ratio. Every layer is normalised to ratio 1 and drawn into its exact pixel rectangle.
#include "veyra/engine/PushVideoOverlay.h"
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QString>
#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <memory>

namespace veyra::ui {

struct InPictureLayer {
    QImage image;   // premultiplied RGBA8888, device pixel ratio 1
    int x = 0, y = 0;
    uint64_t version = 0;
};

// A grabbed picture as a layer's image: pixels are pixels (ratio 1), premultiplied RGBA8888.
inline QImage inPictureNormalized(const QImage& grabbed) {
    QImage image = grabbed.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(1.0);
    return image;
}

// Every layer over the union of their rectangles, straight alpha (nullptr when there is none, or the union is larger than
// 4096 pixels on a side).
inline std::shared_ptr<engine::PushVideoOverlayImage> inPictureCompose(const std::map<QString, InPictureLayer>& layers) {
    if (layers.empty()) return nullptr;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (const auto& [name, layer] : layers) {
        left = std::min(left, layer.x);
        top = std::min(top, layer.y);
        right = std::max(right, layer.x + layer.image.width());
        bottom = std::max(bottom, layer.y + layer.image.height());
    }
    if (right <= left || bottom <= top || right - left > 4096 || bottom - top > 4096) return nullptr;
    QImage canvas(right - left, bottom - top, QImage::Format_RGBA8888_Premultiplied);
    canvas.setDevicePixelRatio(1.0);
    canvas.fill(Qt::transparent);
    {
        QPainter painter(&canvas);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        for (const auto& [name, layer] : layers) {
            QImage pixels = layer.image;
            pixels.setDevicePixelRatio(1.0);
            painter.drawImage(QRect(layer.x - left, layer.y - top, pixels.width(), pixels.height()), pixels);
        }
    }
    const QImage straight = canvas.convertToFormat(QImage::Format_RGBA8888);
    auto composed = std::make_shared<engine::PushVideoOverlayImage>();
    composed->x = left;
    composed->y = top;
    composed->width = unsigned(straight.width());
    composed->height = unsigned(straight.height());
    composed->rgba.resize(size_t(composed->width) * composed->height * 4);
    for (unsigned row = 0; row < composed->height; ++row)
        std::memcpy(composed->rgba.data() + size_t(row) * composed->width * 4, straight.constScanLine(int(row)), size_t(composed->width) * 4);
    return composed;
}

}  // namespace veyra::ui
