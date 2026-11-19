// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): keyboard and mouse capture for a PS5 stream. The capture
// itself follows MoonlightInputCapture (native event filter ahead of Qt, raw relative mouse,
// hidden and clipped cursor); what is captured goes to a remoteplay::KeyboardMousePad instead of
// a host, and the pad poll lays its state over the physical pad's.
// Ctrl+Alt+Shift+Z gives keyboard and mouse back until the window is left and entered again.
// Header-only so that the experiment adds no build target; unverified on Windows until a build
// exists.
//
// Nothing may stay pressed on the console. When the capture is on and when everything held is
// released is decided by remoteplay::KbmCaptureState (portable, unit-tested); this class only
// reports events to it and carries out its actions: losing the focus, the window leaving the
// foreground, Ctrl+Alt+Shift+Z, the end of the session, another window, and the removal of a
// keyboard or mouse that typed during this capture (Raw Input device notifications; the
// keyboard is registered for them and for nothing else, keys still arrive as window messages).
//
// Mouse movement is read the way Microsoft documents for high report rates: the WM_INPUT message
// being handled is read with GetRawInputData, then GetRawInputBuffer takes every report still
// queued behind it in one call (and removes those messages). A 1000 Hz mouse queues four reports
// per 4 ms pad poll and an 8000 Hz one thirty-two; one message per report would let the queue
// grow whenever the interface thread is busy. Absolute-mode devices (tablets, remote desktop) are
// ignored. The mouse is registered without RIDEV_NOLEGACY: Qt and the button / wheel handling
// below need the ordinary mouse messages. VEYRA_EXP_RP_KBM_RAW_DRAIN=0 reads one report per
// message as before; a failing buffered read falls back to that by itself.
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <windows.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <utility>

#include "veyra/Log.h"
#include "veyra/moonlight/InputRouter.h"
#include "veyra/remoteplay/KbmCapture.h"
#include "veyra/remoteplay/KbmProfile.h"
#include "veyra/remoteplay/KeyboardMousePad.h"

namespace veyra::ui {

class RemotePlayKeyboardMouse final : public QObject, public QAbstractNativeEventFilter, private moonlight::InputSink {
public:
    explicit RemotePlayKeyboardMouse(remoteplay::KeyboardMousePad pad, QObject* parent = nullptr)
        : QObject(parent), pad_(std::move(pad)), router_(*this) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) handle(Event::FocusLost, "application inactive");
        });
    }
    ~RemotePlayKeyboardMouse() override {
        handle(Event::SessionEnded, "closing");
        if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
    }

    void setWindow(HWND topLevel) { if (topLevel != window_) { handle(Event::WindowChanged, "window changed"); window_ = topLevel; } }
    bool captured() const { return state_.captured(); }
    // The stream ended or is view-only: release, and capture again with the next session.
    void stop() { handle(Event::SessionEnded, "session ended"); }
    // Off: one report per WM_INPUT message, as before the buffered read (for A/B runs).
    void setBufferedRead(bool on) { bufferedRead_ = on && kBufferedReadSupported; }

    // One pad poll. Takes the keyboard and mouse when the stream window is in front, gives them
    // back (releasing everything) when it is not, and returns the pad state with keyboard and
    // mouse laid over it (unchanged while not captured).
    remoteplay::ControllerState apply(remoteplay::ControllerState pad, bool focused) {
        const bool front = focused && window_ && GetForegroundWindow() == window_;
        handle(front ? Event::PollFront : Event::PollAway, "window not in front");
        if (!state_.captured()) return pad;
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        auto out = pad_.apply(std::move(pad), static_cast<std::uint64_t>(now));
        // The player's own switch between foot and car (not the aim profile, which comes and goes
        // with every aim): one line, so that a tuning session can be read back from the log.
        if (pad_.driving() != loggedDriving_) {
            loggedDriving_ = pad_.driving();
            veyra::log::info("remoteplay-kbm", loggedDriving_ ? "profile: drive (every key released)" : "profile: walk (every key released)");
        }
        return out;
    }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override {
        if (!state_.captured() || eventType != "windows_generic_MSG") return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (!msg->hwnd || GetAncestor(msg->hwnd, GA_ROOT) != window_) return false;
        switch (msg->message) {
        case WM_INPUT: {
            remoteplay::RawMouseBatch batch;
            // This message has already left the queue: its report is read by handle.
            alignas(8) BYTE buffer[128];
            UINT size = sizeof(buffer);
            const UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));
            if (got != UINT(-1) && got >= sizeof(RAWINPUTHEADER)) addRaw(*reinterpret_cast<const RAWINPUT*>(buffer), got, batch);
            // (a keyboard report only tells which keyboard typed: see addRaw)
            // Then every report that queued up behind it, in one go.
            const bool drained = bufferedRead_ && drainRawInput(batch);
            rawStats_.message(batch, drained);
            if (!batch.empty()) router_.rawMouse(batch.x(), batch.y());
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
        case WM_INPUT_DEVICE_CHANGE:
            // A keyboard or mouse that typed is gone: its keys can never send their release.
            if (msg->wParam == GIDC_REMOVAL && devices_.remove(static_cast<std::uintptr_t>(msg->lParam)))
                handle(Event::DeviceRemoved, "device removed");
            return false;
        case WM_ACTIVATE:
            if (LOWORD(msg->wParam) == WA_INACTIVE) handle(Event::FocusLost, "window inactive");
            return false;
        case WM_KILLFOCUS:
            handle(Event::FocusLost, "focus lost");
            return false;
        default:
            break;
        }
        const bool consumed = router_.message(msg->message, msg->wParam, msg->lParam);
        if (consumed && result) *result = 0;
        // Only the release hotkey means something here; ending the session stays with the UI.
        if (router_.takeReserved() == moonlight::Reserved::ReleaseCapture) handle(Event::ReleaseHotkey, "Ctrl+Alt+Shift+Z");
        return consumed;
    }

private:
    using Event = remoteplay::KbmCaptureState::Event;
    // Reports an event and carries out what the state machine asks for. `why` is for the log.
    void handle(Event event, const char* why) {
        const auto actions = state_.on(event);
        if (actions.disengage) disengage(why);
        if (actions.releaseInputs) {
            // Nothing stays pressed on the console: the next poll sends the physical pad alone.
            router_.reset();
            pad_.releaseAll();
            devices_.clear();
            if (event == Event::DeviceRemoved) veyra::log::warn("remoteplay-kbm", "a keyboard or mouse in use was removed; every key released");
        }
        if (actions.restart) pad_.restart();   // the session ended: the next one starts on foot (this releases as well)
        if (actions.engage && !engage()) handle(Event::EngageFailed, "capture failed");
    }
    bool engage() {
        if (!window_ || GetForegroundWindow() != window_) return false;
        // Generic desktop page: mouse (movement) and keyboard (only to know which keyboard typed
        // and to be told when it is unplugged). Neither with RIDEV_NOLEGACY.
        RAWINPUTDEVICE both[2] = {{0x01, 0x02, RIDEV_DEVNOTIFY, window_}, {0x01, 0x06, RIDEV_DEVNOTIFY, window_}};
        deviceNotify_ = RegisterRawInputDevices(both, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
        if (!deviceNotify_) {
            // As in the first round: the mouse alone, without removal notifications.
            RAWINPUTDEVICE mouse{0x01, 0x02, 0, window_};
            if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
                if (!warned_) veyra::log::warn("remoteplay-kbm", "RegisterRawInputDevices failed; keyboard and mouse not captured");
                warned_ = true;
                return false;
            }
            if (!warned_) veyra::log::warn("remoteplay-kbm", "no Raw Input device notifications; unplugging a keyboard will not release its keys");
            warned_ = true;
        }
        clipToWindow();
        SetCursor(nullptr);
        veyra::log::info("remoteplay-kbm", "keyboard and mouse captured (Ctrl+Alt+Shift+Z releases)");
        return true;
    }
    void disengage(const char* why) {
        RAWINPUTDEVICE both[2] = {{0x01, 0x02, RIDEV_REMOVE, nullptr}, {0x01, 0x06, RIDEV_REMOVE, nullptr}};
        RegisterRawInputDevices(both, deviceNotify_ ? 2 : 1, sizeof(RAWINPUTDEVICE));
        deviceNotify_ = false;
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        veyra::log::info("remoteplay-kbm", std::format("keyboard and mouse released ({}); raw input: messages={} reports={} largestBacklog={} bufferedReads={} absoluteIgnored={}",
            why, rawStats_.messages, rawStats_.reports, rawStats_.largest, rawStats_.drains, rawStats_.absolute));
        rawStats_ = {};
    }
    // One RAWINPUT block of `available` bytes. Only relative mouse movement is used; buttons, the
    // wheel and keys arrive as ordinary window messages. Every report says which device sent it.
    void addRaw(const RAWINPUT& raw, std::size_t available, remoteplay::RawMouseBatch& batch) {
        if (raw.header.dwType == RIM_TYPEMOUSE || raw.header.dwType == RIM_TYPEKEYBOARD)
            devices_.note(reinterpret_cast<std::uintptr_t>(raw.header.hDevice));
        if (raw.header.dwType != RIM_TYPEMOUSE || available < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) return;
        batch.add(raw.data.mouse.usFlags, static_cast<std::int32_t>(raw.data.mouse.lLastX), static_cast<std::int32_t>(raw.data.mouse.lLastY));
    }
    // Takes the reports still queued for this thread (mouse, and keyboard for addRaw's device
    // note). Returns false when there were none. The interface thread is the only one that
    // registers Raw Input here: SDL reads its game controllers on its own joystick thread
    // (SDL_HINT_JOYSTICK_THREAD, the SDL 3 default), so nothing of another reader is taken.
    // Bounded: at most kDrainRounds buffers per message.
    bool drainRawInput(remoteplay::RawMouseBatch& batch) {
        bool any = false;
        for (unsigned round = 0; round < kDrainRounds; ++round) {
            UINT size = sizeof(rawBuffer_);
            const UINT count = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(rawBuffer_), &size, sizeof(RAWINPUTHEADER));
            if (count == 0) break;
            if (count == UINT(-1)) {
                // The reports stay queued and arrive as messages of their own. Give up on the
                // buffered read when it keeps failing.
                if (++drainFailures_ >= kDrainFailureLimit) {
                    bufferedRead_ = false;
                    veyra::log::warn("remoteplay-kbm", std::format("GetRawInputBuffer failed (error {}); reading one report per message", GetLastError()));
                }
                break;
            }
            drainFailures_ = 0;
            any = true;
            std::size_t offset = 0;
            for (UINT i = 0; i < count; ++i) {
                if (!remoteplay::rawInputBlockFits(offset, sizeof(RAWINPUTHEADER), sizeof(RAWINPUTHEADER), sizeof(rawBuffer_))) break;
                const auto* raw = reinterpret_cast<const RAWINPUT*>(rawBuffer_ + offset);
                const std::size_t blockSize = raw->header.dwSize;
                if (!remoteplay::rawInputBlockFits(offset, blockSize, sizeof(RAWINPUTHEADER), sizeof(rawBuffer_))) break;
                addRaw(*raw, blockSize, batch);
                offset = remoteplay::rawInputNextOffset(offset, blockSize);
            }
        }
        return any;
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
    remoteplay::KbmCaptureState state_;
    remoteplay::KbmDeviceSet devices_;
    bool warned_ = false, deviceNotify_ = false, loggedDriving_ = false;

    // The buffered read walks 64-bit RAWINPUT blocks. A 32-bit process on 64-bit Windows gets
    // them with the 64-bit header (the WOW64 note of GetRawInputBuffer); PushFPS is built for
    // x64 only, so that layout is not handled and the buffered read stays off there.
#ifdef _WIN64
    static constexpr bool kBufferedReadSupported = true;
#else
    static constexpr bool kBufferedReadSupported = false;
#endif
    static constexpr unsigned kDrainRounds = 16, kDrainFailureLimit = 8;
    bool bufferedRead_ = kBufferedReadSupported;
    unsigned drainFailures_ = 0;
    remoteplay::RawInputStats rawStats_;
    alignas(8) BYTE rawBuffer_[64 * 48];   // 64 mouse reports of 48 bytes per call (a keyboard report is 40)
};

} // namespace veyra::ui
