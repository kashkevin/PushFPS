// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): keyboard and mouse capture for a PS5 stream. The capture
// itself follows MoonlightInputCapture (native event filter ahead of Qt, raw relative mouse,
// hidden and clipped cursor); what is captured goes to a remoteplay::KeyboardMousePad instead of
// a host, and the pad poll lays its state over the physical pad's.
// Ctrl+Alt+Shift+Z gives keyboard and mouse back until the window is left and entered again.
// Losing the focus releases everything held. Header-only so that the experiment adds no build
// target; unverified on Windows until a build exists.
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <windows.h>

#include <chrono>
#include <utility>

#include "veyra/Log.h"
#include "veyra/moonlight/InputRouter.h"
#include "veyra/remoteplay/KeyboardMousePad.h"

namespace veyra::ui {

class RemotePlayKeyboardMouse final : public QObject, public QAbstractNativeEventFilter, private moonlight::InputSink {
public:
    explicit RemotePlayKeyboardMouse(remoteplay::KeyboardMousePad pad, QObject* parent = nullptr)
        : QObject(parent), pad_(std::move(pad)), router_(*this) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) { capture(false); suspended_ = false; }
        });
    }
    ~RemotePlayKeyboardMouse() override {
        capture(false);
        if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
    }

    void setWindow(HWND topLevel) { if (topLevel != window_) { capture(false); window_ = topLevel; } }
    bool captured() const { return captured_; }
    void stop() { capture(false); suspended_ = false; }

    // One pad poll. Takes the keyboard and mouse when the stream window is in front, and returns
    // the pad state with keyboard and mouse laid over it (unchanged while not captured).
    remoteplay::ControllerState apply(remoteplay::ControllerState pad, bool focused) {
        if (!captured_ && focused && !suspended_) capture(true);
        if (!captured_) return pad;
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return pad_.apply(std::move(pad), static_cast<std::uint64_t>(now));
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override {
        if (!captured_ || eventType != "windows_generic_MSG") return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (!msg->hwnd || GetAncestor(msg->hwnd, GA_ROOT) != window_) return false;
        switch (msg->message) {
        case WM_INPUT: {
            alignas(8) BYTE buffer[128];
            UINT size = sizeof(buffer);
            const UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));
            if (got != UINT(-1)) {
                const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
                if (raw->header.dwType == RIM_TYPEMOUSE && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
                    router_.rawMouse(raw->data.mouse.lLastX, raw->data.mouse.lLastY);
            }
            if (result) *result = 0;
            return true;
        }
        case WM_SETCURSOR:
            SetCursor(nullptr);
            if (result) *result = TRUE;
            return true;
        case WM_SIZE: case WM_MOVE:
            clipToWindow();
            return false;
        case WM_ACTIVATE:
            if (LOWORD(msg->wParam) == WA_INACTIVE) capture(false);
            return false;
        case WM_KILLFOCUS:
            capture(false);
            return false;
        default:
            break;
        }
        const bool consumed = router_.message(msg->message, msg->wParam, msg->lParam);
        if (consumed && result) *result = 0;
        // Only the release hotkey means something here; ending the session stays with the UI.
        if (router_.takeReserved() == moonlight::Reserved::ReleaseCapture) { capture(false); suspended_ = true; }
        return consumed;
    }

private:
    bool capture(bool on) {
        if (on == captured_) return true;
        if (on) {
            if (!window_ || GetForegroundWindow() != window_) return false;
            RAWINPUTDEVICE device{0x01, 0x02, 0, window_};   // generic desktop / mouse
            if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
                if (!warned_) veyra::log::warn("remoteplay-kbm", "RegisterRawInputDevices failed; keyboard and mouse not captured");
                warned_ = true;
                return false;
            }
            captured_ = true;
            router_.reset();
            pad_.releaseAll();
            clipToWindow();
            SetCursor(nullptr);
            veyra::log::info("remoteplay-kbm", "keyboard and mouse captured (Ctrl+Alt+Shift+Z releases)");
        } else {
            captured_ = false;
            RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&device, 1, sizeof(device));
            ClipCursor(nullptr);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            router_.reset();
            pad_.releaseAll();   // nothing stays pressed on the console: the next poll sends the bare pad
        }
        return true;
    }
    void clipToWindow() {
        RECT client{};
        if (!window_ || !GetClientRect(window_, &client)) return;
        POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
        ClientToScreen(window_, &topLeft);
        ClientToScreen(window_, &bottomRight);
        const RECT screen{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        ClipCursor(&screen);
    }
    void key(uint32_t virtualKey, uint32_t, bool, bool down) override { pad_.key(virtualKey, down); }
    void mouseMove(int dx, int dy) override { pad_.mouseMove(dx, dy); }
    void mouseButton(int button, bool down) override { pad_.mouseButton(button, down); }
    void scroll(int delta, bool horizontal) override { if (!horizontal) pad_.scroll(delta); }

    remoteplay::KeyboardMousePad pad_;
    moonlight::InputRouter router_;
    HWND window_ = nullptr;
    bool captured_ = false, suspended_ = false, warned_ = false;
};

} // namespace veyra::ui
