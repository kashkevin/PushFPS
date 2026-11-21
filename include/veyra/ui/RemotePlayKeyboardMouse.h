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
//
// Calibration (remoteplay::KbmCalibration, docs/KEYBOARD_MOUSE.md): Ctrl+Alt+Shift+K starts it;
// while it runs Space / left mouse tap, Enter skips and Esc stops, every other key still goes to
// the game. It sends plain right-stick deflections (and holds the aim button in the aiming steps,
// after the player agreed) only while a step the player started runs, and talks through
// [remoteplay-kbm] log lines. At its end the profile file is written (old file kept as .bak) and
// the new numbers are used at once. Any release of the capture stops it without writing.
//
// The mouse's report rate is measured from when reports are read (remoteplay::KbmMouseRate) and
// logged; it sets the shortest smoothing window (two report intervals) so that a poll between two
// reports of a slow mouse does not read as a stop.
//
// Sprint 5 (third directive, docs/KEYBOARD_MOUSE.md "Where input time goes"; unverified on Windows):
//   - input performance profiles (remoteplay::KbmInputProfile.h): setInputSettings / the switches
//     VEYRA_EXP_RP_KBM_INPUT_PROFILE=<legacy|ultra-low-latency|adaptive-precision|custom> and
//     VEYRA_EXP_RP_KBM_INPUT=<key=value;...>. Unset, and with no input.* line in the profile file
//     and no choice on the page: legacy, exactly the behaviour before.
//   - measurements (remoteplay::KbmInputStats.h): with VEYRA_EXP_RP_STATS=1 one [slipstream-input]
//     and one [slipstream-input-poll] line per 5 s. One clock read at the start of a poll, one
//     after the translation, one for the first input event after a poll; no allocation and no lock
//     on the way (the session owner's side is a lock-free probe). With a legacy profile, no
//     statistics and no page reading the readout, none of it runs.
//   - the self-check: with adaptive-precision or custom in use, every 16th poll with mouse movement
//     times the transform in use and the simple one on copies of the same counts; three bad 5 s
//     windows in a row switch to the simple path (held keys stay) and say why.
//   - everything a settings page needs is a plain method here, to be called on the interface
//     thread only (this object lives there: native event filter and pad poll).
//   - leading-edge transmission (off by default; VEYRA_EXP_RP_KBM_EVENT_SEND=1, or transmit =
//     leading-edge in the custom profile; needs setEarlySend from the bridge): the press of a plain
//     button or of a trigger without a ramp is handed to the sender when its message arrives
//     instead of with the next pad poll, at most once between two polls. Sticks, ramps, releases and
//     the mouse keep the proven schedule.
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "veyra/Log.h"
#include "veyra/moonlight/InputRouter.h"
#include "veyra/remoteplay/KbmCalibration.h"
#include "veyra/remoteplay/KbmCalibrationFile.h"
#include "veyra/remoteplay/KbmCalibrationRate.h"
#include "veyra/remoteplay/KbmCapture.h"
#include "veyra/remoteplay/KbmInputProfile.h"
#include "veyra/remoteplay/KbmInputStats.h"
#include "veyra/remoteplay/KbmPreferenceFit.h"
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
        applyEnvironment();
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
    // The pad poll interval in use (for what the mouse's report rate means).
    void setPollMs(int pollMs) { pollMs_ = pollMs; pad_.immediateAim().setHoldUs(remoteplay::kbmHoldUsFor(rate_.classHz(), pollMs_)); }
    // Where a finished calibration writes, and how the log shows that path.
    void setCalibrationOutput(std::filesystem::path path, std::string shown) {
        calibrationPath_ = std::move(path); calibrationShown_ = std::move(shown);
        // The profile's name for the [slipstream-input] line: the file's stem (never its folder) when
        // that file exists, i.e. was read at the start; else "builtin".
        std::error_code ec;
        profileLabel_ = "builtin";
        if (std::filesystem::is_regular_file(calibrationPath_, ec)) {
            std::string stem;
            for (const wchar_t c : calibrationPath_.stem().wstring()) {
                const bool plain = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.';
                stem += plain ? static_cast<char>(c) : '_';
                if (stem.size() >= 48) break;
            }
            if (!stem.empty()) profileLabel_ = stem;
        }
    }

    // Leading-edge transmission: where an early pad state goes (the same call the pad poll makes
    // with its state). Without it the experiment stays inert and the schedule is the proven one.
    void setEarlySend(std::function<void(const remoteplay::ControllerState&)> sink) { earlySend_ = std::move(sink); }
    // The experiment is asked for and can act (a sender was given).
    bool leadingEdgeActive() const {
        return earlySend_ && (eventSendSwitch_ || pad_.inputSettings().transmit == remoteplay::KbmTransmit::LeadingEdge);
    }

    // --- For a settings page. Interface thread only, like everything else here. -------------------
    remoteplay::KbmCaptureState::State captureState() const { return state_.state(); }
    // The same as Ctrl+Alt+Shift+Z: everything held is released, the cursor comes back.
    void releaseCapture() { handle(Event::ReleaseHotkey, "released by the page"); }
    // After a release by hotkey or page: the next pad poll with the stream window in front captures again.
    void allowCapture() { handle(Event::Allow, "allowed by the page"); }
    // Another input performance profile or other numbers in it: from the next pad poll on. Nothing
    // held is released and no poll is skipped. While VEYRA_EXP_RP_KBM_INPUT_PROFILE or
    // VEYRA_EXP_RP_KBM_INPUT is set those win (as the other VEYRA_EXP_RP_KBM_* switches do) and
    // this call changes nothing. A choice made here ends a fallback of the self-check.
    void setInputSettings(const remoteplay::KbmInputSettings& settings) {
        if (inputFromEnvironment_) {
            if (!environmentNoted_) veyra::log::info("remoteplay-kbm", "input profile: the environment switch is set and wins over the page's choice");
            environmentNoted_ = true;
            return;
        }
        fallback_.reset();
        pad_.setInputSettings(settings);
        logInputProfile("chosen");
    }
    // What is in use (after a fallback: the simpler path).
    const remoteplay::KbmInputSettings& inputSettings() const { return pad_.inputSettings(); }
    void setContextOverride(remoteplay::KbmContextOverride override_) { pad_.setContextOverride(override_); }
    remoteplay::KbmContext context() const { return pad_.context(); }
    const remoteplay::KbmProfileSet& profiles() const { return pad_.profiles(); }
    // Other keys and per-profile numbers. Everything held is released (a binding change is made
    // outside play). The input performance profile in use stays: that is setInputSettings' business.
    void setProfiles(remoteplay::KbmProfileSet set) {
        const remoteplay::KbmInputSettings input = pad_.inputSettings();
        pad_.setProfiles(std::move(set));
        pad_.setInputSettings(input);
    }
    // The game calibration from the page. While the stream has keyboard and mouse it starts at once;
    // else it is armed and starts with the next capture (false only when one already runs).
    bool startCalibration() {
        if (calibration_.active() || calibrationArmed_) return false;
        if (!state_.captured()) { calibrationArmed_ = true; return true; }
        beginCalibration();
        return true;
    }
    void calibrationTap() { if (calibration_.active()) { calibration_.tap(nowUs()); logCalibration(); } }
    void calibrationSkip() { if (calibration_.active()) { calibration_.skip(nowUs()); logCalibration(); } }
    void stopCalibration() {
        calibrationArmed_ = false;
        if (calibration_.active()) { calibration_.stop("stopped from the page"); logCalibration(); }
    }
    bool calibrating() const { return calibration_.active() || calibrationArmed_; }
    remoteplay::KbmCalibration::Step calibrationStep() const { return calibration_.step(); }
    // The calibration's messages since the last call (English log text; at most the newest 64).
    std::vector<std::string> takeCalibrationMessages() { return std::exchange(pageMessages_, {}); }
    // One test motion of the preference fit, taken from the raw mouse while the stream has it. The
    // camera stands still meanwhile: the movement goes to the recorder, not to the stick. A release of
    // the capture does not discard what was recorded; a recording nobody stops ends by itself after
    // kFitRecordingUs (the mouse turns the camera again) and its motion is kept for the next stop call.
    void startFitRecording(remoteplay::KbmFitKind kind) {
        pad_.immediateAim().reset(); pad_.aim().reset();
        fitDone_ = {};
        fitStartedUs_ = nowUs();
        fit_.start(kind);
    }
    remoteplay::KbmFitMotion stopFitRecording() { return fit_.active() ? fit_.stop() : std::exchange(fitDone_, {}); }
    // The last finished 5 s window. Asking keeps the measurements running for 4,096 more polls (about 16 s at a 4 ms poll).
    remoteplay::KbmInputReadout readout() const {
        readoutPolls_ = 4096;
        remoteplay::KbmInputReadout out = readout_;
        out.profile = pad_.inputSettings().kind;
        out.context = pad_.context();
        out.mouseHz = rate_.classHz();
        out.stuckReleases = releases_.stuckReleases; out.heldAtLoss = releases_.heldAtLoss;
        out.fallback = fallback_.reason();
        return out;
    }

    // One pad poll. Takes the keyboard and mouse when the stream window is in front, gives them
    // back (releasing everything) when it is not, and returns the pad state with keyboard and
    // mouse laid over it (unchanged while not captured).
    remoteplay::ControllerState apply(remoteplay::ControllerState pad, bool focused) {
        const bool front = focused && window_ && GetForegroundWindow() == window_;
        handle(front ? Event::PollFront : Event::PollAway, "window not in front");
        const bool measure = measuring();
        if (!state_.captured()) {
            if (measure) { const std::int64_t at = ticks(); const bool busy = padBusy(pad); pollBegins(at); pollEnds(pad, at, at, false, busy); }
            haveLastOut_ = false; early_.reset();
            return pad;
        }
        // One clock read: the poll's time for the translation and the start of its measurement.
        const std::int64_t begin = ticks();
        const std::uint64_t now = static_cast<std::uint64_t>(begin / 10);
        const bool busy = measure && padBusy(pad);
        if (measure) { pollBegins(begin); shadowPrepare(now); }
        lastApplyUs_ = now;
        early_.polled(now, static_cast<std::uint64_t>(pollMs_) * 1000u);
        auto out = pad_.apply(std::move(pad), now);
        const std::int64_t translated = measure ? ticks() : 0;
        if (calibration_.active()) out = calibrate(std::move(out), now);
        if (calibration_.finished()) finishCalibration();
        // The player's own switch between foot and car (not the aim profile, which comes and goes
        // with every aim): one line, so that a tuning session can be read back from the log.
        if (pad_.driving() != loggedDriving_) {
            loggedDriving_ = pad_.driving();
            veyra::log::info("remoteplay-kbm", loggedDriving_ ? "profile: drive (every key released)" : "profile: walk (every key released)");
        }
        if (measure) pollEnds(out, begin, translated, true, busy);
        // Leading edge: the base of the next early state. Forgotten whenever the experiment is not
        // active, so that switching it on again never builds on an old state (KbmEarlyGate also
        // refuses a base older than two poll intervals: a stalled poll, the latency test).
        if (leadingEdgeActive()) { lastOut_ = out; haveLastOut_ = true; }
        else { haveLastOut_ = false; early_.reset(); }
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
            noteEvent();
            if (measuring()) { meter_.mouseEvents += batch.moved; meter_.rawBacklogMax = (std::max)(meter_.rawBacklogMax, static_cast<std::uint32_t>(batch.reports)); }
            rate_.add(nowUs(), batch.moved);
            if (rate_.takeChange()) mouseRateChanged();
            if (!batch.empty()) router_.rawMouse(batch.x(), batch.y());
            // Microsoft's WM_INPUT documentation: DefWindowProc must see the message so that the system
            // can clean up after a RIM_INPUT report. Returning true stops Qt from calling it, so call it
            // here; the message stays handled for Qt (true, *result = 0).
            (void)DefWindowProcW(msg->hwnd, WM_INPUT, msg->wParam, msg->lParam);
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
            // Counted: a release that had something to let go of (it would have stayed pressed), and
            // how many inputs were held when the session itself went away (connection loss, end).
            releases_.released(pad_.heldCount(), event == Event::SessionEnded);
            haveLastOut_ = false;   // no early state is ever built on a state from before a release
            // A calibration in progress ends without writing (its timing is gone).
            if (calibration_.active()) { calibration_.stop(why); logCalibration(); }
            calKeys_.reset();
            router_.reset();
            pad_.releaseAll();
            devices_.clear();
            if (event == Event::DeviceRemoved) veyra::log::warn("remoteplay-kbm", "a keyboard or mouse in use was removed; every key released");
        }
        if (actions.restart) pad_.restart();   // the session ended: the next one starts on foot (this releases as well)
        if (actions.engage && !engage()) handle(Event::EngageFailed, "capture failed");
        // A calibration the page asked for while the stream did not have keyboard and mouse.
        if (actions.engage && state_.captured() && calibrationArmed_) beginCalibration();
    }
    void beginCalibration() {
        calibrationArmed_ = false;
        pad_.releaseAll();
        calibration_.start(pad_.profiles());
        logCalibration();
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
    static std::uint64_t nowUs() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    // A key or button the calibration may take. True when it did.
    bool calibrationInput(remoteplay::KbmCalibrationKeys::Action action) {
        using Action = remoteplay::KbmCalibrationKeys::Action;
        const std::uint64_t now = nowUs();
        switch (action) {
        case Action::Pass: return false;
        case Action::Consume: return true;
        case Action::Toggle:
            if (calibration_.active()) calibration_.stop("Ctrl+Alt+Shift+K");
            else {
                // The chord's modifiers reached the game on the way: let go of everything first.
                pad_.releaseAll();
                calibration_.start(pad_.profiles());
            }
            break;
        case Action::Tap: calibration_.tap(now); break;
        case Action::Skip: calibration_.skip(now); break;
        case Action::Exit: calibration_.stop("Esc"); break;
        }
        logCalibration();
        return true;
    }
    void key(uint32_t virtualKey, uint32_t, bool, bool down) override {
        noteEvent();
        if (calibrationInput(calKeys_.key(virtualKey, down, calibration_.active()))) return;
        pad_.key(virtualKey, down);
        if (down) sendEarly(virtualKey);
    }
    void mouseMove(int dx, int dy) override {
        if (fit_.active()) {
            // A test motion of the preference fit: the camera stands still.
            const std::uint64_t now = nowUs();
            if (now - fitStartedUs_ < kFitRecordingUs) { fit_.add(dx, dy, now); return; }
            fitDone_ = fit_.stop();
        }
        if (!calibration_.running()) pad_.mouseMove(dx, dy);            // the step has the right stick
    }
    void mouseButton(int button, bool down) override {
        noteEvent();
        if (calibrationInput(calKeys_.mouseButton(button, down, calibration_.active()))) return;
        pad_.mouseButton(button, down);
        if (down && button >= 1 && button <= 5) sendEarly(remoteplay::kMouseButtonBase + static_cast<std::uint32_t>(button));
    }
    // Leading-edge transmission: the press that just arrived, laid over the last poll's state and
    // handed to the sender now. At most one between two polls (remoteplay::KbmEarlyGate), never
    // during a calibration, never on a state from before a release. The next poll sends the full
    // state as always; if it is the same, the queue drops it.
    void sendEarly(std::uint32_t code) {
        if (!haveLastOut_ || !state_.captured() || calibration_.active() || !leadingEdgeActive()) return;
        const auto nowUs = static_cast<std::uint64_t>(ticks() / 10);
        if (!early_.fresh(nowUs)) { haveLastOut_ = false; return; }
        const auto state = pad_.earlyPress(code, lastOut_);
        if (!state || !early_.take(nowUs)) return;
        lastOut_ = *state;
        if (measuring()) {
            const std::int64_t at = ticks();
            remoteplay::kbmInputProbe().handed(at, oldestEventTicks_ > 0 ? oldestEventTicks_ : at);
            lastHanded_ = *state;
        }
        earlySend_(*state);
    }
    // The calibration's deflection and aim button laid over this poll's state.
    remoteplay::ControllerState calibrate(remoteplay::ControllerState out, std::uint64_t now) {
        const auto step = calibration_.poll(now);
        if (step.deflect) { out.rightX = calibration_.stickValue(step.x); out.rightY = calibration_.stickValue(step.y); }
        if (step.holdAim) {
            using remoteplay::PadAction;
            using remoteplay::ControllerState;
            switch (pad_.profiles().aimWhile) {
            case PadAction::L2: out.l2 = 255; break;
            case PadAction::R2: out.r2 = 255; break;
            case PadAction::L1: out.buttons |= ControllerState::L1; break;
            case PadAction::R1: out.buttons |= ControllerState::R1; break;
            case PadAction::L3: out.buttons |= ControllerState::L3; break;
            case PadAction::R3: out.buttons |= ControllerState::R3; break;
            case PadAction::Cross: out.buttons |= ControllerState::Cross; break;
            case PadAction::Circle: out.buttons |= ControllerState::Circle; break;
            case PadAction::Square: out.buttons |= ControllerState::Square; break;
            case PadAction::Triangle: out.buttons |= ControllerState::Triangle; break;
            default: break;
            }
        }
        logCalibration();
        return out;
    }
    void logCalibration() {
        for (auto& line : calibration_.takeMessages()) {
            veyra::log::info("remoteplay-kbm", line);
            if (pageMessages_.size() >= 64) pageMessages_.erase(pageMessages_.begin());
            pageMessages_.push_back(std::move(line));
        }
    }
    // The end: numbers from the measurements, the file written, the numbers used.
    void finishCalibration() {
        logCalibration();
        const auto measured = calibration_.measured();
        calibration_ = remoteplay::KbmCalibration{};
        bool any = false;
        for (const auto& m : measured) any = any || m.any();
        if (!any) return;
        const auto set = remoteplay::kbmCalibratedProfiles(pad_.profiles(), measured);
        const std::string block = remoteplay::kbmCalibrationBlock(set, measured);
        if (calibrationPath_.empty()) {
            veyra::log::warn("remoteplay-kbm", "calibration: no folder to write the profile to; the numbers are used until PushFPS closes");
        } else {
            bool found = false;
            const std::string existing = remoteplay::kbmReadSmallFile(calibrationPath_, found);
            std::string refused;
            const auto text = remoteplay::kbmCalibratedProfileText(existing, found, set, block, refused);
            remoteplay::KbmWriteResult written;
            if (text) written = remoteplay::kbmWriteProfile(calibrationPath_, *text);
            else written.error = refused;
            if (written.ok)
                veyra::log::info("remoteplay-kbm", std::format("calibration: profile written to {}{}", calibrationShown_,
                    written.backedUp ? " (the previous file is kept next to it as .bak)"
                    : written.backupKept ? " (the earlier .bak, from before the first calibration, was kept and not replaced)" : ""));
            else
                veyra::log::warn("remoteplay-kbm", std::format("calibration: profile not written to {}: {}; the numbers are used until PushFPS closes", calibrationShown_, written.error));
        }
        // The block, line by line, so a tuning session can be read back from the log.
        std::size_t begin = 0;
        while (begin < block.size()) {
            const std::size_t end = block.find('\n', begin);
            const std::string line = block.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!line.empty() && line[0] != '#') veyra::log::info("remoteplay-kbm", std::format("calibration: {}", line));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        pad_.setProfiles(set);
        veyra::log::info("remoteplay-kbm", "calibration: the new numbers are in use (every key released)");
    }
    // The mouse's report rate settled on another class.
    void mouseRateChanged() {
        const int hz = rate_.classHz();
        const auto accumulation = remoteplay::kbmAccumulationFor(hz, pollMs_, pad_.aim().config().smoothingMs);
        pad_.aim().setWindowFloorMs(accumulation.windowFloorMs);
        // The input profiles have no window: a mouse slower than the poll repeats its last value for
        // at most one report interval instead (0 for 250 Hz and faster at a 4 ms poll).
        pad_.immediateAim().setHoldUs(remoteplay::kbmHoldUsFor(hz, pollMs_));
        veyra::log::info("remoteplay-kbm", std::format("mouse reports at about {} Hz (measured {:.0f} a second over the busiest moments); poll {} ms{}; shortest smoothing window {:.1f} ms{}",
            hz, rate_.measuredPerSecond(), pollMs_,
            accumulation.pollWastes ? std::format(" is shorter than one report interval ({:.1f} ms): some polls find nothing new", accumulation.reportIntervalMs) : std::string(" finds a new report every time"),
            accumulation.windowFloorMs, accumulation.floorRaises ? " (longer than the profile's: a slow mouse would otherwise read as stops between reports)" : ""));
    }
    void scroll(int delta, bool horizontal) override { if (!horizontal) pad_.scroll(delta); }

    // --- Sprint 5: input profile switches, measurements, self-check --------------------------------
    // The same clock as remoteplay::monotonic100ns (steady_clock, QueryPerformanceCounter on Windows), in 100 ns.
    static std::int64_t ticks() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    }
    static constexpr std::int64_t kTicksPerSecond = 10000000, kWindowTicks = 5 * kTicksPerSecond;
    // VEYRA_EXP_RP_KBM_INPUT_PROFILE / VEYRA_EXP_RP_KBM_INPUT / VEYRA_EXP_RP_STATS, read once.
    void applyEnvironment() {
        statsOn_ = qEnvironmentVariable("VEYRA_EXP_RP_STATS").trimmed() == QStringLiteral("1");
        eventSendSwitch_ = qEnvironmentVariable("VEYRA_EXP_RP_KBM_EVENT_SEND").trimmed() == QStringLiteral("1");
        if (eventSendSwitch_) veyra::log::info("remoteplay-kbm", "leading-edge transmission asked for (VEYRA_EXP_RP_KBM_EVENT_SEND=1): button presses go to the sender when they arrive, at most one between two polls; everything else keeps the pad poll");
        const std::string id = qEnvironmentVariable("VEYRA_EXP_RP_KBM_INPUT_PROFILE").trimmed().toLower().toStdString();
        const std::string text = qEnvironmentVariable("VEYRA_EXP_RP_KBM_INPUT").trimmed().toStdString();
        if (id.empty() && text.empty()) { if (pad_.inputSettings().immediate()) logInputProfile("from the profile file"); return; }
        remoteplay::KbmInputSettings settings = pad_.inputSettings();
        if (!id.empty()) {
            const auto kind = remoteplay::kbmInputProfileFromId(id);
            if (!kind) veyra::log::warn("remoteplay-kbm", "VEYRA_EXP_RP_KBM_INPUT_PROFILE is not legacy, ultra-low-latency, adaptive-precision or custom; ignored");
            else if (*kind != settings.kind) settings = remoteplay::kbmInputPreset(*kind);
        }
        if (!text.empty()) {
            const auto parsed = remoteplay::parseKbmInputSettings(text, settings);
            settings = parsed.settings;
            if (parsed.refused) veyra::log::warn("remoteplay-kbm", std::format("VEYRA_EXP_RP_KBM_INPUT: {} of its settings were refused (the first: {}); the others apply", parsed.refused, plainText(parsed.firstProblem)));
        }
        inputFromEnvironment_ = true;
        pad_.setInputSettings(settings);
        logInputProfile("from the environment switch");
    }
    static std::string plainText(const std::string& text) {
        std::string out;
        for (const char c : text) out += c >= 0x20 && c < 0x7F ? c : '?';
        return out;
    }
    void logInputProfile(const char* how) {
        const auto& input = pad_.inputSettings();
        veyra::log::info("remoteplay-kbm", std::format("input profile: {} ({}); smoothing {:.1f} ms; settings {}", remoteplay::kbmInputProfileId(input.kind), how,
            input.immediate() ? input.smoothingMs : pad_.aim().windowMs(), input.immediate() ? remoteplay::kbmInputSettingsText(input) : std::string("as before (profile file)")));
    }
    // Statistics asked for, a page reading the readout, or a profile whose self-check needs the
    // timings. With the legacy profile and neither of the first two: nothing below runs.
    bool measuring() const {
        if (statsOn_ || readoutPolls_ > 0) return true;
        const auto kind = pad_.inputSettings().kind;
        return kind == remoteplay::KbmInputProfileKind::AdaptivePrecision || kind == remoteplay::KbmInputProfileKind::Custom;
    }
    // The first keyboard or mouse event after a poll: its time, for raw -> translated and event -> sent.
    void noteEvent() {
        eventSeen_ = true;
        if (oldestEventTicks_ == 0 && measuring()) oldestEventTicks_ = ticks();
    }
    // The physical controller is doing something (not its drift).
    static bool padBusy(const remoteplay::ControllerState& pad) {
        if (!pad.inputActive) return false;
        const auto pushed = [](std::int16_t v) { return v > 8000 || v < -8000; };
        return pad.buttons != 0 || pad.l2 > 0 || pad.r2 > 0 || pushed(pad.leftX) || pushed(pad.leftY) || pushed(pad.rightX) || pushed(pad.rightY);
    }
    void pollBegins(std::int64_t at) {
        meter_.poll(lastPollTicks_ > 0 ? double(at - lastPollTicks_) / 10.0 : 0.0, pollMs_);
        lastPollTicks_ = at;
        if (windowStartTicks_ == 0) windowStartTicks_ = at;
        if (oldestEventTicks_ > 0 && at >= oldestEventTicks_) meter_.rawToTranslate.add(double(at - oldestEventTicks_) / 10.0);
    }
    // Every 16th poll with mouse movement, while a profile with a self-check is in use: the transform
    // in use and the simple one, each on a copy of the same counts, in alternating order. Before
    // the poll only the counts are copied (no allocation); the two timed runs happen after the
    // poll's state has been handed off (afterHandOff), never in front of it.
    void shadowPrepare(std::uint64_t now) {
        const auto kind = pad_.inputSettings().kind;
        if (kind != remoteplay::KbmInputProfileKind::AdaptivePrecision && kind != remoteplay::KbmInputProfileKind::Custom) return;
        if (shadowDue_ || !pad_.immediateAim().hasInput() || (++shadowTick_ % 16) != 0) return;
        shadowAim_ = pad_.immediateAim();
        shadowElapsedUs_ = lastApplyUs_ > 0 && now > lastApplyUs_ ? now - lastApplyUs_ : 4000;
        shadowNowUs_ = now;
        shadowDue_ = true;
        scheduleAfterHandOff();
    }
    void shadowTiming() {
        const std::uint64_t elapsed = shadowElapsedUs_, now = shadowNowUs_;
        shadowSimpleFirst_ = !shadowSimpleFirst_;
        const std::int64_t a = ticks();
        (void)pad_.shadowStep(shadowAim_, shadowSimpleFirst_, elapsed, now);
        const std::int64_t b = ticks();
        (void)pad_.shadowStep(shadowAim_, !shadowSimpleFirst_, elapsed, now);
        const std::int64_t c = ticks();
        (shadowSimpleFirst_ ? meter_.shadowSimple : meter_.shadowAdaptive).add(double(b - a) / 10.0);
        (shadowSimpleFirst_ ? meter_.shadowAdaptive : meter_.shadowSimple).add(double(c - b) / 10.0);
    }
    void pollEnds(const remoteplay::ControllerState& out, std::int64_t begin, std::int64_t translated, bool captured, bool padIsBusy) {
        if (readoutPolls_ > 0) --readoutPolls_;
        if (captured) {
            meter_.translate.add(double(translated - begin) / 10.0);
            if (eventSeen_ || pad_.heldCount() > 0) ++meter_.kbmPolls;
        }
        if (padIsBusy) ++meter_.padPolls;
        // Handed to the engine right after this returns. Only a state the queue will send: one that
        // differs from the last (the queue drops a repeat without waking anyone).
        if (!(out == lastHanded_)) {
            remoteplay::kbmInputProbe().handed(translated, oldestEventTicks_);
            lastHanded_ = out;
        }
        oldestEventTicks_ = 0;
        eventSeen_ = false;
        meter_.takeProbe(remoteplay::kbmInputProbe());
        if (!windowDue_ && begin - windowStartTicks_ >= kWindowTicks) { windowDue_ = true; scheduleAfterHandOff(); }
    }
    // The bridge hands the poll's state to the engine right after apply() returns. Statistics, the
    // self-check and the shadow timing run from the event loop after that (one queued call per
    // poll at most, and only on a poll that has such work), never between translation and hand-off.
    void scheduleAfterHandOff() {
        if (afterHandOffQueued_) return;
        afterHandOffQueued_ = true;
        QMetaObject::invokeMethod(this, [this]() { afterHandOff(); }, Qt::QueuedConnection);
    }
    void afterHandOff() {
        afterHandOffQueued_ = false;
        if (shadowDue_) { shadowDue_ = false; shadowTiming(); }
        if (windowDue_) { windowDue_ = false; closeWindow(ticks()); }
    }
    // A 5 s window ends: the lines (statistics only), the readout, the self-check.
    void closeWindow(std::int64_t at) {
        const auto& input = pad_.inputSettings();
        const bool immediate = input.immediate() && pad_.aim().config().mode == remoteplay::MouseAimConfig::Mode::Stick;
        meter_.windowSeconds = double(at - windowStartTicks_) / double(kTicksPerSecond);
        const std::uint64_t moving = pad_.movingPolls() - movingBase_, saturated = pad_.saturatedPolls() - saturatedBase_;
        remoteplay::KbmInputLineInfo info;
        info.source = remoteplay::kbmInputSource(meter_.kbmPolls, meter_.padPolls);
        info.inputProfile = remoteplay::kbmInputProfileId(input.kind);
        info.context = state_.captured() ? remoteplay::kbmContextId(pad_.context()) : "none";
        info.profile = profileLabel_;
        info.mouseHz = rate_.classHz() > 0 ? double(rate_.classHz()) : -1.0;
        info.smoothingMs = immediate ? input.smoothingMs : pad_.aim().windowMs();
        info.saturatedPct = !immediate ? -1.0 : moving ? 100.0 * double(saturated) / double(moving) : 0.0;
        info.curve = immediate ? remoteplay::kbmCurveId(input[pad_.context()].curve.kind) : "legacy";
        info.switches = pad_.contextSwitches() - switchesBase_;
        info.stuckReleases = releases_.stuckReleases;
        info.heldAtLoss = releases_.heldAtLoss;
        selfCheck();
        info.fallback = fallback_.reason();
        if (statsOn_) {
            veyra::log::info("slipstream-input", meter_.line(info));
            veyra::log::info("slipstream-input-poll", meter_.pollLine(pollMs_));
        }
        readout_.source = info.source;
        readout_.translateUsP95 = meter_.translate.percentile(.95);
        readout_.rawToTranslateUsP95 = meter_.rawToTranslate.percentile(.95);
        readout_.stateToSendMsP95 = meter_.stateToSend.percentile(.95);
        readout_.eventToSendMsP95 = meter_.eventToSend.percentile(.95);
        readout_.sendGapMsP95 = meter_.sendGap.percentile(.95);
        readout_.smoothingMs = info.smoothingMs;
        readout_.saturatedPct = info.saturatedPct;
        meter_.clear();
        movingBase_ = pad_.movingPolls(); saturatedBase_ = pad_.saturatedPolls(); switchesBase_ = pad_.contextSwitches();
        windowStartTicks_ = at;
    }
    // remoteplay::KbmFallbackCheck with this window's numbers. A window of the legacy or the
    // ultra-low-latency profile gives the state-to-send baseline the queue rule compares with.
    void selfCheck() {
        const auto& input = pad_.inputSettings();
        const bool adaptive = input.kind == remoteplay::KbmInputProfileKind::AdaptivePrecision || input.kind == remoteplay::KbmInputProfileKind::Custom;
        const double stateToSend = meter_.stateToSend.percentile(.95);
        if (!adaptive) { if (stateToSend >= 0) baselineStateToSendMs_ = stateToSend; return; }
        remoteplay::KbmFallbackWindow window;
        window.adaptiveTranslateUsP95 = meter_.shadowAdaptive.percentile(.95);
        window.simpleTranslateUsP95 = meter_.shadowSimple.percentile(.95);
        window.stateToSendMsP95 = stateToSend;
        window.baselineStateToSendMsP95 = baselineStateToSendMs_;
        if (const char* reason = fallback_.update(window)) {
            // The simpler path from the next poll on; held keys, triggers and movement stay.
            pad_.setInputSettings(remoteplay::kbmFallbackSettings(input));
            veyra::log::warn("remoteplay-kbm", std::format("input profile: fell back to the simple path ({}): the adaptive one measured worse for three windows in a row; choose a profile again to retry", reason));
        }
    }

    remoteplay::KeyboardMousePad pad_;
    moonlight::InputRouter router_;
    HWND window_ = nullptr;
    remoteplay::KbmCaptureState state_;
    remoteplay::KbmDeviceSet devices_;
    remoteplay::KbmCalibration calibration_;
    remoteplay::KbmCalibrationKeys calKeys_;
    remoteplay::KbmMouseRate rate_;
    std::filesystem::path calibrationPath_;
    std::string calibrationShown_;
    int pollMs_ = 4;
    bool warned_ = false, deviceNotify_ = false, loggedDriving_ = false;

    // Sprint 5.
    std::function<void(const remoteplay::ControllerState&)> earlySend_;
    remoteplay::KbmEarlyGate early_;
    remoteplay::ControllerState lastOut_;
    bool haveLastOut_ = false, eventSendSwitch_ = false;
    remoteplay::KbmFitRecorder fit_;
    remoteplay::KbmFitMotion fitDone_;
    std::uint64_t fitStartedUs_ = 0;
    static constexpr std::uint64_t kFitRecordingUs = 15000000;
    remoteplay::KbmInputMeter meter_;
    remoteplay::KbmFallbackCheck fallback_;
    remoteplay::KbmInputReadout readout_;
    remoteplay::ControllerState lastHanded_;
    std::vector<std::string> pageMessages_;
    std::string profileLabel_ = "builtin";
    std::int64_t lastPollTicks_ = 0, windowStartTicks_ = 0, oldestEventTicks_ = 0;
    remoteplay::KbmReleaseCounters releases_;
    std::uint64_t lastApplyUs_ = 0, movingBase_ = 0, saturatedBase_ = 0, switchesBase_ = 0;
    double baselineStateToSendMs_ = -1;
    unsigned shadowTick_ = 0;
    remoteplay::KbmImmediateAim shadowAim_;
    std::uint64_t shadowElapsedUs_ = 0, shadowNowUs_ = 0;
    bool shadowDue_ = false, windowDue_ = false, afterHandOffQueued_ = false;
    mutable int readoutPolls_ = 0;
    bool statsOn_ = false, inputFromEnvironment_ = false, environmentNoted_ = false, eventSeen_ = false, calibrationArmed_ = false, shadowSimpleFirst_ = false;

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
