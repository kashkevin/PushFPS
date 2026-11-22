// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge for the Mouse & Keyboard page (qml/Veyra/PushKbmPage.qml, PushKbmBindings.qml, PushKbmWizard.qml): thin
// wrappers over the keyboard-and-mouse core of the input stream:
//   - the switch (preference kbmEnabled; VEYRA_EXP_RP_KBM set before the start still switches it on and wins), capture
//     state, release and allow-capture;
//   - the input performance profile (kbmInputProfile: legacy = exactly the earlier behaviour, the default; ultra-low-
//     latency, adaptive-precision, custom), Custom's values (kbmInputCustom, `key=value;...`), the manual context
//     (kbmContextOverride), the response curve's points for the page's plot;
//   - the key bindings of the walking, aiming and driving profile, reset to the built-in GTA-style layout, profile
//     files slipstream-kbm-<name>.txt next to settings.ini (the existing calibrated-profile files; slipstream-kbm.ini's
//     game= line picks the one read at the next start);
//   - the game-response calibration (the steps of Ctrl+Alt+Shift+K, driven by the page instead of the keys) and the
//     preference fit (the user's own test motions), applied only when the page asks;
//   - the live readout of the last window: software processing times inside the PC, never input-to-photon.
// Threads: the capture object lives on the interface thread (a native event filter and the pad poll QTimer), so every
// call here runs on that thread and only reads or sets plain values; nothing waits for the poll, the session or the
// network, and nothing is called from another thread. Nothing here aims, repeats or times an action for the player.
//
// The core is built by another stream, so every part of it is looked for on its own at compile time: the input
// profile header with __has_include, each group of capture functions with a requires-expression on the capture's
// type. A part that is not there reports itself as unavailable (`available` / `live` / `calibration` / `fit` in the
// maps) and changes nothing but the preferences. The local helpers are named pk* on purpose: a helper named kbm* that
// takes a core type would be ambiguous with a core function of the same name (argument-dependent lookup).
// Unverified on Windows.
#include "../QmlPlayerBridge_p.h"
#include "veyra/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef VEYRA_ENABLE_REMOTEPLAY
#include "veyra/remoteplay/ProfileStore.h"
#include "veyra/ui/RemotePlayKeyboardMouse.h"
#if __has_include("veyra/remoteplay/KbmInputProfile.h")
#define VEYRA_PUSH_KBM_CORE 1
#include "veyra/remoteplay/KbmInputProfile.h"
#if __has_include("veyra/remoteplay/KbmPreferenceFit.h")
#define VEYRA_PUSH_KBM_FIT 1
#include "veyra/remoteplay/KbmPreferenceFit.h"
#endif
#endif
#endif
#ifndef VEYRA_PUSH_KBM_CORE
#define VEYRA_PUSH_KBM_CORE 0
#endif
#ifndef VEYRA_PUSH_KBM_FIT
#define VEYRA_PUSH_KBM_FIT 0
#endif

namespace veyra::ui {
#ifdef VEYRA_ENABLE_REMOTEPLAY
// RemotePlay.cpp owns the capture (interface thread).
RemotePlayKeyboardMouse* ps5KbmCapture();
bool ps5KbmStart(HWND topLevel);
void ps5KbmEnd();
void ps5KbmUsePageLayout(remoteplay::KbmProfileSet& set);
bool ps5KbmTextBindsKeys(std::string_view text);
#endif
namespace {
const QStringList& pkInputProfileIds() {
    static const QStringList ids{QStringLiteral("legacy"), QStringLiteral("ultra-low-latency"), QStringLiteral("adaptive-precision"), QStringLiteral("custom")};
    return ids;
}
// Index = KbmContextOverride; index - 1 = KbmContext.
const QStringList& pkOverrideIds() {
    static const QStringList ids{QStringLiteral("auto"), QStringLiteral("walk"), QStringLiteral("aim"), QStringLiteral("sprint"), QStringLiteral("drive")};
    return ids;
}
constexpr qsizetype kPkCustomMaxBytes = 2048;
// A profile file's name as the page shows and asks it: the slug of slipstream-kbm-<slug>.txt.
constexpr qsizetype kPkProfileNameMax = 48;

// VEYRA_EXP_RP_KBM set (and not 0) before PushFPS started: switched on whatever the page says.
bool pkEnvironmentOn() {
    const QString mode = qEnvironmentVariable("VEYRA_EXP_RP_KBM").trimmed().toLower();
    return !mode.isEmpty() && mode != QLatin1String("0");
}
bool pkPlainText(const QString& text, qsizetype limit) {
    if (text.size() > limit) return false;
    for (const QChar c : text) if (c.unicode() < 0x20 || c.unicode() >= 0x7F) return false;
    return true;
}
QString pkPrefOr(const QVariantMap& prefs, const char* key, const QString& fallback) {
    const QString text = prefs.value(QLatin1String(key)).toString();
    return text.isEmpty() ? fallback : text;
}

// The Mouse & Keyboard page keeps the mouse for the interface. For a calibration or a test motion the page asks the
// capture to take it (pushKbmHold) and renews that while it runs: a page that went away gives the mouse back by
// itself. Steady-clock milliseconds; 0 = not held. Interface thread only.
constexpr std::int64_t kPkHoldMs = 3000;
std::int64_t pkHoldUntilMs = 0;
std::int64_t pkNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef VEYRA_ENABLE_REMOTEPLAY
namespace rp = remoteplay;
constexpr bool kPkRemotePlay = true;

// ---- What this build's capture object can do (each group on its own) --------------------------------------------
template <class C> concept PkCaptureStates = requires(C& c) { static_cast<int>(c.captureState()); c.releaseCapture(); c.allowCapture(); };
template <class C> concept PkProfiles = requires(C& c, rp::KbmProfileSet set) { rp::KbmProfileSet(c.profiles()); c.setProfiles(std::move(set)); };
template <class C> concept PkCalibration = requires(C& c) {
    static_cast<bool>(c.startCalibration()); c.calibrationTap(); c.calibrationSkip(); c.stopCalibration();
    static_cast<bool>(c.calibrating()); c.takeCalibrationMessages(); rp::KbmCalibration::Step(c.calibrationStep()); };
template <class C> concept PkReadout = requires(const C& c) {
    c.readout().source; c.readout().fallback; c.readout().profile; c.readout().context; c.readout().mouseHz;
    c.readout().translateUsP95; c.readout().rawToTranslateUsP95; c.readout().stateToSendMsP95; c.readout().eventToSendMsP95;
    c.readout().sendGapMsP95; c.readout().smoothingMs; c.readout().saturatedPct; c.readout().stuckReleases; c.readout().heldAtLoss; };
template <class S> concept PkTextWriter = requires(const S& set) { std::string(kbmProfileText(set)); };   // found through the set's namespace
#if VEYRA_PUSH_KBM_CORE
template <class C> concept PkInput = requires(C& c, const rp::KbmInputSettings& s) {
    c.setInputSettings(s); rp::KbmInputSettings(c.inputSettings()); c.setContextOverride(s.override_); rp::kbmContextId(c.context()); };
#else
template <class C> concept PkInput = false;
#endif
// The type of startFitRecording's only argument (the kind of test motion), whatever it returns.
template <class C, class R, class A> A pkOnlyArgument(R (C::*)(A));
template <class C, class R, class A> A pkOnlyArgument(R (C::*)(A) const);
template <class C> using PkFitKindOf = decltype(pkOnlyArgument(&C::startFitRecording));
template <class K> concept PkFitKinds = requires { K::Turn; K::Flick; K::Slow; };
template <class C> concept PkFitRecording = requires(C& c) {
    typename PkFitKindOf<C>;
    requires PkFitKinds<PkFitKindOf<C>>;
    c.startFitRecording(PkFitKindOf<C>::Turn);
    static_cast<double>(c.stopFitRecording().counts); static_cast<double>(c.stopFitRecording().seconds); };

// The core's leading-edge sending (setEarlySend / leadingEdgeActive), when this build's capture has it.
template <class C> concept PkLeadingEdge = requires(C& c, std::function<void(const rp::ControllerState&)> sink) {
    c.setEarlySend(std::move(sink)); static_cast<bool>(c.leadingEdgeActive()); };

using PkCapture = RemotePlayKeyboardMouse;
constexpr bool kPkInputProfiles = VEYRA_PUSH_KBM_CORE != 0;      // the settings types and their text form exist
constexpr bool kPkLiveInput = PkInput<PkCapture>;                 // a running capture takes them
constexpr bool kPkLiveBindings = PkProfiles<PkCapture>;           // a running capture takes edited bindings
constexpr bool kPkCalibration = PkCalibration<PkCapture>;
constexpr bool kPkFit = VEYRA_PUSH_KBM_FIT != 0 && PkFitRecording<PkCapture>;
constexpr bool kPkLeadingEdge = PkLeadingEdge<PkCapture>;
#ifdef VEYRA_PUSH_KBM_REQUIRE_CORE
// For a build that must contain the whole core (-DVEYRA_PUSH_KBM_REQUIRE_CORE): each missing part is named.
static_assert(kPkInputProfiles, "veyra/remoteplay/KbmInputProfile.h is missing");
static_assert(kPkLiveInput, "RemotePlayKeyboardMouse: setInputSettings / inputSettings / setContextOverride / context");
static_assert(kPkLiveBindings, "RemotePlayKeyboardMouse: profiles / setProfiles");
static_assert(PkCaptureStates<PkCapture>, "RemotePlayKeyboardMouse: captureState / releaseCapture / allowCapture");
static_assert(kPkCalibration, "RemotePlayKeyboardMouse: startCalibration / calibrationTap / calibrationSkip / stopCalibration / calibrating / takeCalibrationMessages / calibrationStep");
static_assert(PkReadout<PkCapture>, "RemotePlayKeyboardMouse: readout() with the fields of KbmInputReadout");
static_assert(PkTextWriter<rp::KbmProfileSet>, "kbmProfileText(const KbmProfileSet&)");
static_assert(kPkFit, "veyra/remoteplay/KbmPreferenceFit.h or startFitRecording(Turn|Flick|Slow) / stopFitRecording");
static_assert(kPkLeadingEdge, "RemotePlayKeyboardMouse: setEarlySend / leadingEdgeActive");
#endif

// ---- Key and action names (the profile file's own spelling, KeyboardPadBindings::keyCode / actionName) ------------
const std::vector<std::pair<std::string, std::uint32_t>>& pkKeys() {
    static const std::vector<std::pair<std::string, std::uint32_t>> keys = [] {
        std::vector<std::pair<std::string, std::uint32_t>> out;
        const auto add = [&out](const std::string& name) {
            const std::uint32_t code = rp::KeyboardPadBindings::keyCode(name);
            if (code == 0) return;
            for (const auto& known : out) if (known.second == code) return;
            out.emplace_back(name, code);
        };
        for (const char* name : {"Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5", "WheelUp", "WheelDown"}) add(name);
        for (char c = 'A'; c <= 'Z'; ++c) add(std::string(1, c));
        for (char c = '0'; c <= '9'; ++c) add(std::string(1, c));
        for (const char* name : {"Space", "Shift", "Ctrl", "Alt", "Tab", "Esc", "Enter", "Back", "Caps", "Up", "Down", "Left", "Right",
                                 "Home", "End", "Insert", "Delete", "PageUp", "PageDown", "Grave", "Minus", "Plus", "Comma", "Period",
                                 "Slash", "Semicolon", "Quote", "LBracket", "RBracket", "Backslash"}) add(name);
        for (int n = 1; n <= 12; ++n) add("F" + std::to_string(n));
        for (int n = 0; n <= 9; ++n) add("Num" + std::to_string(n));
        return out;
    }();
    return keys;
}
constexpr const char* kPkActions[] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options", "share",
                                      "touchpad", "ps", "dup", "ddown", "dleft", "dright", "lup", "ldown", "lleft", "lright", "walk", "fine", "drive"};
QString pkActionText(rp::PadAction action) {
    for (const char* name : kPkActions) {
        rp::PadAction known = rp::PadAction::None;
        if (rp::KeyboardPadBindings::actionName(name, known) && known == action) return QString::fromLatin1(name);
    }
    return QStringLiteral("none");
}
QVariantList pkBindingList(const rp::KeyboardPadBindings& bindings, const rp::KeyboardPadBindings* unless) {
    QVariantList out;
    for (const auto& [name, code] : pkKeys()) {
        const rp::PadAction action = bindings.action(code);
        if (unless ? action == unless->action(code) : action == rp::PadAction::None) continue;
        out << QVariantMap{{"key", QString::fromStdString(name)}, {"action", pkActionText(action)}};
    }
    return out;
}
std::optional<rp::KbmProfileId> pkBindingProfile(const QString& context) {
    if (context == QLatin1String("walk") || context == QLatin1String("sprint")) return rp::KbmProfileId::Walk;
    if (context == QLatin1String("aim")) return rp::KbmProfileId::Aim;
    if (context == QLatin1String("drive")) return rp::KbmProfileId::Drive;
    return std::nullopt;
}

// ---- Profile files -------------------------------------------------------------------------------------------------
// The bindings edited on the page in this run (interface thread). A new capture gets them too.
std::optional<rp::KbmProfileSet> pkEdited;
std::string pkEditedName;
unsigned pkEditedGeneration = 0;
const void* pkAppliedTo = nullptr;
unsigned pkAppliedGeneration = 0;

std::filesystem::path pkFolder() {
    try { return rp::profileDirectory(); } catch (const std::exception&) { return {}; }
}
// slipstream-kbm-<slug>.txt: a slug of lower-case letters, digits and '-' only (kbmGameSlug's alphabet).
bool pkValidSlug(const QString& name) {
    if (name.isEmpty() || name.size() > kPkProfileNameMax) return false;
    for (const QChar c : name) if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('-'))) return false;
    return true;
}
// The file makePs5KeyboardMouse (RemotePlay.cpp) reads at a session start: the slipstream-kbm.ini game's file.
std::string pkActiveName(const std::filesystem::path& folder) {
    if (folder.empty()) return {};
    bool found = false;
    const std::string game = rp::kbmGameName(rp::kbmReadSmallFile(folder / rp::kKbmGameSettingsFile, found));
    return game.empty() ? std::string{} : rp::kbmGameSlug(game);
}
// A profile file as a session start reads it (RemotePlay.cpp makePs5KeyboardMouse): on the built-in layout, or,
// for a start from the page, on the page's layout when the file binds no key.
std::optional<rp::KbmProfileSet> pkReadProfile(const std::filesystem::path& path, bool pageStart) {
    bool found = false;
    const std::string text = rp::kbmReadSmallFile(path, found);
    if (!found) return std::nullopt;
    rp::KbmProfileSet base = rp::kbmBuiltinProfiles();
    if (pageStart && !ps5KbmTextBindsKeys(text)) ps5KbmUsePageLayout(base);
    auto parsed = rp::parseKbmProfiles(text, std::move(base));
    return std::move(parsed.set);
}
// The page's own starting layout (the GTA VI layout from press tables when the core has it).
rp::KbmProfileSet pkPageLayout() {
    rp::KbmProfileSet set = rp::kbmBuiltinProfiles();
    ps5KbmUsePageLayout(set);
    return set;
}
template <class C> std::optional<rp::KbmProfileSet> pkCaptureSet([[maybe_unused]] const C* capture) {
    if constexpr (PkProfiles<C>) { if (capture) return rp::KbmProfileSet(capture->profiles()); }
    return std::nullopt;
}
// This run's edits follow the capture they were applied to: a calibration changes that capture's numbers.
template <class C> void pkFollowCapture([[maybe_unused]] const C* capture) {
    if constexpr (PkProfiles<C>) {
        if (capture && pkEdited && pkAppliedTo == capture && pkAppliedGeneration == pkEditedGeneration)
            *pkEdited = rp::KbmProfileSet(capture->profiles());
    }
}
// What the editor shows: this run's edits, else the running capture's set, else what a session would start with
// (VEYRA_EXP_RP_KBM set: the switch's path with the earlier built-in layout; else the page's path).
rp::KbmProfileSet pkCurrentSet() {
    pkFollowCapture(ps5KbmCapture());
    if (pkEdited) return *pkEdited;
    if (auto set = pkCaptureSet(ps5KbmCapture())) return std::move(*set);
    const bool pageStart = !pkEnvironmentOn();
    const auto folder = pkFolder();
    const std::string name = pkActiveName(folder);
    if (!name.empty()) {
        if (auto set = pkReadProfile(folder / rp::kbmCalibratedFileName(name), pageStart)) return std::move(*set);
    }
    return pageStart ? pkPageLayout() : rp::kbmBuiltinProfiles();
}
// Edited bindings into a running capture (which releases every held key: a binding change is made outside play).
template <class C> void pkApplyEdited([[maybe_unused]] C* capture) {
    if constexpr (PkProfiles<C>) {
        if (!capture || !pkEdited) return;
        if (pkAppliedTo == capture && pkAppliedGeneration == pkEditedGeneration) return;
        capture->setProfiles(*pkEdited);
        pkAppliedTo = capture;
        pkAppliedGeneration = pkEditedGeneration;
    }
}
void pkStore(rp::KbmProfileSet set) {
    pkEdited = std::move(set);
    ++pkEditedGeneration;
    pkApplyEdited(ps5KbmCapture());
}
// The binding lines of a set, on top of the built-in layout the parser starts from (so an unbound key is written
// as none). Walking's lines reach aiming too; aiming's own lines follow only where it differs.
std::string pkBindingLines(const rp::KbmProfileSet& set) {
    const rp::KbmProfileSet builtin = rp::kbmBuiltinProfiles();
    std::string out;
    const auto lines = [&](rp::KbmProfileId id, const rp::KeyboardPadBindings& base) {
        for (const auto& [name, code] : pkKeys()) {
            const rp::PadAction action = set[id].bindings.action(code);
            if (action == base.action(code)) continue;
            out += std::string(rp::kbmProfileName(id)) + ".bind." + name + "=" + pkActionText(action).toStdString() + "\n";
        }
    };
    lines(rp::KbmProfileId::Walk, builtin[rp::KbmProfileId::Walk].bindings);
    // After the walking lines the parser's aiming profile holds: its built-in keys, overwritten by every walking line.
    rp::KeyboardPadBindings aimBase = builtin[rp::KbmProfileId::Aim].bindings;
    for (const auto& [name, code] : pkKeys()) {
        const rp::PadAction walk = set[rp::KbmProfileId::Walk].bindings.action(code);
        if (walk != builtin[rp::KbmProfileId::Walk].bindings.action(code)) aimBase.bind(code, walk);
    }
    lines(rp::KbmProfileId::Aim, aimBase);
    lines(rp::KbmProfileId::Drive, builtin[rp::KbmProfileId::Drive].bindings);
    return out;
}
constexpr std::string_view kPkBindingsBegin = "# --- key bindings (written by the Mouse & Keyboard page; edit there) ---";
constexpr std::string_view kPkBindingsEnd = "# --- end of key bindings ---";
// `existing` without bind lines and without an earlier block of this page, with the set's binding lines at the end.
// Every other line (the player's numbers, comments, a calibration block) stays as it is.
std::string pkMergedBindings(std::string_view existing, const rp::KbmProfileSet& set) {
    std::string kept;
    bool inside = false;
    while (!existing.empty()) {
        const auto end = existing.find('\n');
        const std::string_view raw = existing.substr(0, end == std::string_view::npos ? existing.size() : end + 1);
        existing = end == std::string_view::npos ? std::string_view{} : existing.substr(end + 1);
        std::string_view line = raw;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (!inside && line == kPkBindingsBegin) { inside = true; continue; }
        if (inside) { if (line == kPkBindingsEnd) inside = false; continue; }
        std::string lower(line.substr(0, std::min<std::size_t>(line.size(), 12)));
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const auto dot = lower.find('.');
        if (dot != std::string::npos && lower.compare(dot, 6, ".bind.") == 0 && line.front() != '#' && line.front() != ';') continue;
        kept += raw;
    }
    if (!kept.empty() && kept.back() != '\n') kept += "\n";
    kept += kPkBindingsBegin; kept += "\n";
    kept += pkBindingLines(set);
    kept += kPkBindingsEnd; kept += "\n";
    return kept;
}
// The whole set as a profile file: the core's writer when this build has one, else the file as it is with the
// binding lines replaced.
template <class S> std::string pkProfileFileText(const S& set, const std::filesystem::path& path) {
    if constexpr (PkTextWriter<S>) {
        (void)path;
        return std::string(kbmProfileText(set));
    } else {
        bool found = false;
        const std::string existing = rp::kbmReadSmallFile(path, found);
        return pkMergedBindings(found ? std::string_view(existing) : std::string_view(rp::kbmNewProfileText(set)), set);
    }
}
// slipstream-kbm.ini names the profile read at the next session start (RemotePlay.cpp makePs5KeyboardMouse).
bool pkChooseForNextStart(const std::filesystem::path& folder, const std::string& slug) {
    const auto result = rp::kbmWriteProfile(folder / rp::kKbmGameSettingsFile, "game=" + slug + "\n");
    if (!result.ok) veyra::log::warn("push-kbm", "slipstream-kbm.ini not written: " + result.error);
    return result.ok;
}

// ---- Leading-edge sending ---------------------------------------------------------------------------------------------
const void* pkSenderFor = nullptr;   // the capture that has the sender (a new capture needs it again)
// The capture the page's input choice was last given to, and that choice (profile, Custom's text; the manual
// context apart). Given again only to a new capture or after the choice changed, so another kbm* preference or a
// reconnection never replaces a fallback in force with the slower profile.
const void* pkInputFor = nullptr;
QString pkInputChoice;
QString pkOverrideChoice;
template <class C, class F> void pkEarlySend([[maybe_unused]] C* capture, [[maybe_unused]] F sink) {
    if constexpr (PkLeadingEdge<C>) {
        if (!capture || pkSenderFor == capture) return;
        capture->setEarlySend(std::function<void(const rp::ControllerState&)>(std::move(sink)));
        pkSenderFor = capture;
    }
}
template <class C> bool pkLeadingEdgeActive([[maybe_unused]] const C* capture) {
    if constexpr (PkLeadingEdge<C>) return capture && static_cast<bool>(capture->leadingEdgeActive());
    else return false;
}

// ---- Capture state, calibration, readout ----------------------------------------------------------------------------
template <class C> QString pkCaptureText(const C& capture) {
    if constexpr (PkCaptureStates<C>) {
        const int state = static_cast<int>(capture.captureState());   // KbmCaptureState::State: Released, Captured, Suspended
        return state == 1 ? QStringLiteral("captured") : state == 2 ? QStringLiteral("suspended") : QStringLiteral("released");
    } else {
        return capture.captured() ? QStringLiteral("captured") : QStringLiteral("released");
    }
}
template <class C> bool pkRelease([[maybe_unused]] C* capture, [[maybe_unused]] bool allow) {
    if constexpr (PkCaptureStates<C>) {
        if (!capture) return false;
        if (allow) capture->allowCapture(); else capture->releaseCapture();
        return true;
    } else {
        return false;
    }
}
const char* pkStepId(rp::KbmCalibration::Step step) {
    switch (step) {
    case rp::KbmCalibration::Step::DeadzoneUp: return "deadzone-up";
    case rp::KbmCalibration::Step::DeadzoneDown: return "deadzone-down";
    case rp::KbmCalibration::Step::TurnTime: return "turn-time";
    case rp::KbmCalibration::Step::Response: return "response";
    case rp::KbmCalibration::Step::RatioUp: return "ratio-up";
    case rp::KbmCalibration::Step::RatioDown: return "ratio-down";
    case rp::KbmCalibration::Step::Intro: break;
    }
    return "intro";
}
template <class C> QVariantMap pkCalibration([[maybe_unused]] C* capture, [[maybe_unused]] const QString& request) {
    if constexpr (PkCalibration<C>) {
        if (!capture) return {{"available", true}, {"ok", false}, {"running", false}, {"reason", QStringLiteral("no-capture")}};
        bool ok = true;
        if (request == QLatin1String("start")) ok = static_cast<bool>(capture->startCalibration());
        else if (request == QLatin1String("tap")) capture->calibrationTap();
        else if (request == QLatin1String("skip")) capture->calibrationSkip();
        else if (request == QLatin1String("stop")) capture->stopCalibration();
        else if (request != QLatin1String("status")) return {{"available", true}, {"ok", false}, {"running", false}, {"reason", QStringLiteral("request")}};
        QStringList messages;
        for (const auto& line : capture->takeCalibrationMessages()) messages << QString::fromStdString(std::string(line));
        return {{"available", true}, {"ok", ok}, {"running", static_cast<bool>(capture->calibrating())},
                {"step", QString::fromLatin1(pkStepId(rp::KbmCalibration::Step(capture->calibrationStep())))},
                {"captured", capture->captured()}, {"messages", messages}};
    } else {
        return {{"available", false}, {"ok", false}, {"running", false}, {"reason", QStringLiteral("unavailable")}};
    }
}
#if VEYRA_PUSH_KBM_CORE
QString pkLatin(const char* text, const char* fallback) { return QString::fromLatin1(text ? text : fallback); }
#endif
template <class C> QVariantMap pkReadout([[maybe_unused]] const C* capture) {
#if VEYRA_PUSH_KBM_CORE
    if constexpr (PkReadout<C>) {
        if (!capture) return {{"available", true}, {"active", false}};
        const auto r = capture->readout();
        const auto number = [](double v) { return std::isfinite(v) ? v : -1.0; };
        return {{"available", true}, {"active", true}, {"source", pkLatin(r.source, "none")},
                {"profile", QString::fromLatin1(rp::kbmInputProfileId(r.profile))},
                {"context", QString::fromLatin1(rp::kbmContextId(r.context))}, {"mouseHz", static_cast<int>(r.mouseHz)},
                {"translateUsP95", number(r.translateUsP95)}, {"rawToTranslateUsP95", number(r.rawToTranslateUsP95)},
                {"stateToSendMsP95", number(r.stateToSendMsP95)}, {"eventToSendMsP95", number(r.eventToSendMsP95)},
                {"sendGapMsP95", number(r.sendGapMsP95)}, {"smoothingMs", number(r.smoothingMs)}, {"saturatedPct", number(r.saturatedPct)},
                {"stuckReleases", static_cast<qulonglong>(r.stuckReleases)}, {"heldAtLoss", static_cast<qulonglong>(r.heldAtLoss)},
                {"fallback", pkLatin(r.fallback, "none")}};
    }
#endif
    return {{"available", false}, {"active", false}};
}

#if VEYRA_PUSH_KBM_CORE
// ---- Input performance profiles -------------------------------------------------------------------------------------
QVariantMap pkTuningMap(const rp::KbmContextTuning& t) {
    return QVariantMap{{"sensX", t.sensX}, {"sensY", t.sensY},
                       {"curve", t.curve.kind == rp::KbmCurveKind::Power ? QStringLiteral("power")
                               : t.curve.kind == rp::KbmCurveKind::PrecisionSpeed ? QStringLiteral("precision-speed") : QStringLiteral("linear")},
                       {"exponent", t.curve.exponent}, {"kneeIn", t.curve.kneeIn}, {"kneeOut", t.curve.kneeOut},
                       {"deadzone", t.deadzone}, {"fullSpeed", t.fullSpeedCounts},
                       {"saturation", t.saturation == rp::KbmSaturation::Carry ? QStringLiteral("carry") : QStringLiteral("drop")},
                       {"carryMs", t.carryMs}, {"maxTilt", t.maxTilt}, {"game", t.game}};
}
QVariantMap pkSettingsMap(const rp::KbmInputSettings& s) {
    QVariantMap contexts;
    for (std::size_t c = 0; c < s.contexts.size() && c + 1 < static_cast<std::size_t>(pkOverrideIds().size()); ++c)
        contexts.insert(pkOverrideIds().at(static_cast<qsizetype>(c + 1)), pkTuningMap(s.contexts[c]));
    const auto overrideIndex = static_cast<qsizetype>(s.override_);
    return QVariantMap{{"available", true}, {"profile", QString::fromLatin1(rp::kbmInputProfileId(s.kind))}, {"ads", s.adsFactor},
                       {"smoothingMs", s.smoothingMs},
                       {"transmit", s.transmit == rp::KbmTransmit::LeadingEdge ? QStringLiteral("leading-edge") : QStringLiteral("proven")},
                       {"contextOverride", overrideIndex >= 0 && overrideIndex < pkOverrideIds().size() ? pkOverrideIds().at(overrideIndex) : QStringLiteral("auto")},
                       {"contexts", contexts}};
}
rp::KbmInputProfileKind pkKindOf(const QString& id) {
    return rp::kbmInputProfileFromId(id.toStdString()).value_or(rp::KbmInputProfileKind::Legacy);
}
rp::KbmContextOverride pkOverrideOf(const QString& id) {
    const auto index = pkOverrideIds().indexOf(id);
    return static_cast<rp::KbmContextOverride>(index > 0 ? index : 0);
}
// Custom's values: kbmInputCustom over Custom's built-in values.
rp::KbmInputSettings pkCustomSettings(const QString& text) {
    auto settings = rp::parseKbmInputSettings(text.toStdString(), rp::kbmInputPreset(rp::KbmInputProfileKind::Custom)).settings;
    settings.kind = rp::KbmInputProfileKind::Custom;
    return rp::kbmSanitized(settings);
}
rp::KbmInputSettings pkSettingsFor(rp::KbmInputProfileKind kind, const QString& customText, const QString& overrideId) {
    auto settings = kind == rp::KbmInputProfileKind::Custom ? pkCustomSettings(customText) : rp::kbmInputPreset(kind);
    settings.kind = kind;
    settings.override_ = pkOverrideOf(overrideId);
    return rp::kbmSanitized(settings);
}
// Plain decimal text (never an exponent), at most 4 decimals, no trailing zeros. "" for NaN / infinity, which every
// caller refuses (never turned into 0).
std::string pkNumber(double v) {
    if (!std::isfinite(v)) return {};
    QString text = QString::number(v, 'f', 4);
    while (text.contains(QLatin1Char('.')) && (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) text.chop(1);
    return text.toStdString();
}
template <class C> void pkApplyInput([[maybe_unused]] C* capture, [[maybe_unused]] const rp::KbmInputSettings& settings) {
    if constexpr (PkInput<C>) {
        if (!capture) return;
        // Never releases a held key and never adds a gap (the core's contract); a fallback in force is replaced only
        // by a change the user made.
        if (!(rp::KbmInputSettings(capture->inputSettings()) == settings)) capture->setInputSettings(settings);
        capture->setContextOverride(settings.override_);
    }
}
template <class C> void pkApplyOverride([[maybe_unused]] C* capture, [[maybe_unused]] rp::KbmContextOverride override_) {
    if constexpr (PkInput<C>) { if (capture) capture->setContextOverride(override_); }
}
template <class C> void pkInUse([[maybe_unused]] const C* capture, [[maybe_unused]] QVariantMap& out) {
    if constexpr (PkInput<C>) {
        if (!capture) return;
        out["inUse"] = QString::fromLatin1(rp::kbmInputProfileId(rp::KbmInputSettings(capture->inputSettings()).kind));
        out["context"] = QString::fromLatin1(rp::kbmContextId(capture->context()));
    }
}
#endif

#if VEYRA_PUSH_KBM_FIT
std::vector<rp::KbmFitMotion> pkMotions(const QVariant& list) {
    std::vector<rp::KbmFitMotion> out;
    for (const auto& item : list.toList()) {
        const QVariantMap m = item.toMap();
        const double counts = m.value("counts").toDouble(), seconds = m.value("seconds").toDouble();
        if (std::isfinite(counts) && std::isfinite(seconds) && seconds > 0) out.push_back(rp::KbmFitMotion{counts, seconds});
        if (out.size() >= 10) break;
    }
    return out;
}
template <class C> QVariantMap pkFitRecord([[maybe_unused]] C* capture, [[maybe_unused]] bool start, [[maybe_unused]] const QString& kind) {
    if constexpr (PkFitRecording<C>) {
        if (!capture) return {{"available", true}, {"ok", false}, {"reason", QStringLiteral("no-capture")}};
        if (start) {
            using Kind = PkFitKindOf<C>;
            capture->startFitRecording(kind == QLatin1String("flick") ? Kind::Flick : kind == QLatin1String("slow") ? Kind::Slow : Kind::Turn);
            return {{"available", true}, {"ok", true}};
        }
        const auto motion = capture->stopFitRecording();
        return {{"available", true}, {"ok", true}, {"counts", static_cast<double>(motion.counts)}, {"seconds", static_cast<double>(motion.seconds)}};
    } else {
        return {{"available", false}, {"ok", false}, {"reason", QStringLiteral("unavailable")}};
    }
}
#endif
#else
constexpr bool kPkRemotePlay = false;
constexpr bool kPkInputProfiles = false, kPkLiveInput = false, kPkLiveBindings = false, kPkCalibration = false, kPkFit = false, kPkLeadingEdge = false;
#endif
} // namespace

bool QmlPlayerBridge::pushKbmValidPreference(const QString& key, const QVariant& value, QVariant& stored) const {
    if (key == QLatin1String("kbmEnabled") || key == QLatin1String("kbmFirstRunDone") || key == QLatin1String("pushKbmWizardDone")) {
        stored = value.toBool();
        return true;
    }
    const QString text = value.toString();
    if (key == QLatin1String("kbmInputProfile")) {
        if (!pkInputProfileIds().contains(text)) return false;
    } else if (key == QLatin1String("kbmContextOverride")) {
        if (!pkOverrideIds().contains(text)) return false;
    } else if (key == QLatin1String("kbmInputCustom")) {
        if (!pkPlainText(text, kPkCustomMaxBytes)) return false;
#if VEYRA_PUSH_KBM_CORE
        if (remoteplay::parseKbmInputSettings(text.toStdString(), remoteplay::KbmInputSettings{}).refused != 0) return false;
#endif
    } else {
        return false;
    }
    stored = text;
    return true;
}

void QmlPlayerBridge::applyPushKbm() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    const bool want = pkEnvironmentOn() || i.prefBool("kbmEnabled", false);
    if (!want) {
        if (ps5KbmCapture()) {
            pkFollowCapture(ps5KbmCapture());
            ps5KbmEnd();
            pkAppliedTo = nullptr;
            pkSenderFor = nullptr;
            pkInputFor = nullptr;
            veyra::log::info("push-kbm", "keyboard and mouse switched off on the page (everything released)");
        }
        return;
    }
    // The pad poll runs only while a PS5 stream (not view-only) does: start the capture now, else at the next connection.
    const bool live = i.controllerTimer && i.controllerTimer->isActive();
    if (!ps5KbmCapture() && live && i.videoWindow && ps5KbmStart(GetAncestor(i.videoWindow, GA_ROOT)))
        veyra::log::info("push-kbm", "keyboard and mouse switched on on the page");
    auto* capture = ps5KbmCapture();
    if (!capture) return;
    // The core's leading-edge sending (off unless VEYRA_EXP_RP_KBM_EVENT_SEND=1 or Custom's transmit=leading-edge): the
    // press of a plain button goes to the session's sender when its message arrives, as the pad poll's state does
    // (same thread, same call), and never while the latency test sends its own presses. Inert without the switch.
    pkEarlySend(capture, [this](const remoteplay::ControllerState& state) {
        if (!impl_->latency.active) impl_->engine.remotePlayController(state);
    });
#if VEYRA_PUSH_KBM_CORE
    // Only a choice made on the page. Without one the capture keeps what it started with (the profile file's
    // input.* lines, else "as before").
    const QString choice = i.prefs.contains(QStringLiteral("kbmInputProfile"))
        ? pkPrefOr(i.prefs, "kbmInputProfile", QStringLiteral("legacy")) + QLatin1Char('\n') + i.prefString("kbmInputCustom") : QString();
    const QString overrideId = pkPrefOr(i.prefs, "kbmContextOverride", QStringLiteral("auto"));
    const bool newCapture = pkInputFor != capture;
    if (!choice.isEmpty() && (newCapture || choice != pkInputChoice))
        pkApplyInput(capture, pkSettingsFor(pkKindOf(pkPrefOr(i.prefs, "kbmInputProfile", QStringLiteral("legacy"))), i.prefString("kbmInputCustom"), overrideId));
    else if (i.prefs.contains(QStringLiteral("kbmContextOverride")) && (newCapture || overrideId != pkOverrideChoice))
        pkApplyOverride(capture, pkOverrideOf(overrideId));
    pkInputFor = capture;
    pkInputChoice = choice;
    pkOverrideChoice = overrideId;
#endif
    pkApplyEdited(capture);
#endif
}

QVariantMap QmlPlayerBridge::pushKbmState() const {
    const auto& i = *impl_;
    const bool forced = pkEnvironmentOn();
    QVariantMap out{{"available", kPkRemotePlay}, {"core", kPkInputProfiles}, {"live", kPkLiveInput}, {"liveBindings", kPkLiveBindings},
                    {"calibration", kPkCalibration}, {"fit", kPkFit}, {"forced", forced},
                    // VEYRA_EXP_RP_KBM_INPUT_PROFILE / _INPUT set: the capture keeps the switch's settings, whatever the page says.
                    {"inputForced", !qEnvironmentVariable("VEYRA_EXP_RP_KBM_INPUT_PROFILE").trimmed().isEmpty() || !qEnvironmentVariable("VEYRA_EXP_RP_KBM_INPUT").trimmed().isEmpty()},
                    // Sending on change (the core's leading-edge experiment) can be chosen in Custom only when this
                    // build's capture has it; leadingEdge: it is in effect in the running capture.
                    {"eventSend", kPkLeadingEdge}, {"leadingEdge", false}, {"profileChosen", i.prefs.contains(QStringLiteral("kbmInputProfile"))},
                    {"enabled", forced || i.prefBool("kbmEnabled", false)},
                    {"streaming", i.snapshot.remotePlay && i.snapshot.running},
                    {"active", false}, {"canRelease", false}, {"held", pkHoldUntilMs != 0 && pkNowMs() < pkHoldUntilMs},
                    {"releaseKeys", QStringLiteral("Ctrl+Alt+Shift+Z")}, {"capture", QStringLiteral("off")},
                    {"inputProfile", pkPrefOr(i.prefs, "kbmInputProfile", QStringLiteral("legacy"))},
                    {"contextOverride", pkPrefOr(i.prefs, "kbmContextOverride", QStringLiteral("auto"))},
                    {"inUse", QString()}, {"context", QStringLiteral("none")}, {"fallback", QStringLiteral("none")}, {"source", QStringLiteral("none")}};
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if (const auto* capture = ps5KbmCapture()) {
        out["active"] = true;
        out["canRelease"] = PkCaptureStates<PkCapture>;
        out["capture"] = pkCaptureText(*capture);
        out["leadingEdge"] = pkLeadingEdgeActive(capture);
#if VEYRA_PUSH_KBM_CORE
        pkInUse(capture, out);
#endif
        const QVariantMap readout = pkReadout(capture);
        if (readout.value("active").toBool()) { out["fallback"] = readout.value("fallback"); out["source"] = readout.value("source"); }
    }
#endif
    return out;
}

bool QmlPlayerBridge::pushKbmSetEnabled(bool on) {
    if (!setPreference(QStringLiteral("kbmEnabled"), on)) return false;
    applyPushKbm();   // also when the stored value did not change (a capture to start for a session already running)
    return true;
}

bool QmlPlayerBridge::pushKbmHold(bool on) {
    pkHoldUntilMs = on ? pkNowMs() + kPkHoldMs : 0;
    return on;
}

// Asked by the pad poll (RemotePlay.cpp) at every tick: cheap. Everywhere but on the Mouse & Keyboard page the answer
// is yes, as before that page existed.
bool QmlPlayerBridge::pushKbmCaptureWanted() const {
    if (impl_->currentPage != QLatin1String("kbm")) return true;
    return pkHoldUntilMs != 0 && pkNowMs() < pkHoldUntilMs;
}

bool QmlPlayerBridge::pushKbmRelease() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    return pkRelease(ps5KbmCapture(), false);
#else
    return false;
#endif
}

bool QmlPlayerBridge::pushKbmAllowCapture() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    return pkRelease(ps5KbmCapture(), true);
#else
    return false;
#endif
}

QVariantMap QmlPlayerBridge::pushKbmInputSettings(const QString& profile) const {
#if VEYRA_PUSH_KBM_CORE
    const auto& i = *impl_;
    const QString id = profile.isEmpty() ? pkPrefOr(i.prefs, "kbmInputProfile", QStringLiteral("legacy")) : profile;
    if (!pkInputProfileIds().contains(id)) return {{"available", false}};
    return pkSettingsMap(pkSettingsFor(pkKindOf(id), i.prefString("kbmInputCustom"), pkPrefOr(i.prefs, "kbmContextOverride", QStringLiteral("auto"))));
#else
    Q_UNUSED(profile);
    return {{"available", false}};
#endif
}

bool QmlPlayerBridge::pushKbmSetInputProfile(const QString& profile) {
    if (!pkInputProfileIds().contains(profile)) return false;
    if (!setPreference(QStringLiteral("kbmInputProfile"), profile)) return false;
    veyra::log::info("push-kbm", "input profile chosen on the page: " + profile.toStdString());
    return true;
}

bool QmlPlayerBridge::pushKbmSetCustom(const QString& key, const QVariant& value) {
#if VEYRA_PUSH_KBM_CORE
    if (key.isEmpty() || !pkPlainText(key, 32) || key.contains(QLatin1Char('=')) || key.contains(QLatin1Char(';'))) return false;
    const QString text = value.typeId() == QMetaType::Bool ? (value.toBool() ? QStringLiteral("1") : QStringLiteral("0"))
        : value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Int || value.typeId() == QMetaType::Float
        ? QString::fromStdString(pkNumber(value.toDouble())) : value.toString();
    if (text.isEmpty() || !pkPlainText(text, 64) || text.contains(QLatin1Char(';')) || text.contains(QLatin1Char('='))) return false;
    bool isNumber = false;
    const double number = text.toDouble(&isNumber);   // also "nan" / "inf" given as text
    if (isNumber && !std::isfinite(number)) return false;
    const auto base = pkCustomSettings(impl_->prefString("kbmInputCustom"));
    const auto parsed = remoteplay::parseKbmInputSettings((key + QLatin1Char('=') + text).toStdString(), base);
    if (parsed.refused != 0 || parsed.applied == 0) return false;
    auto settings = remoteplay::kbmSanitized(parsed.settings);
    settings.kind = remoteplay::KbmInputProfileKind::Custom;
    return setPreference(QStringLiteral("kbmInputCustom"), QString::fromStdString(remoteplay::kbmInputSettingsText(settings)));
#else
    Q_UNUSED(key); Q_UNUSED(value);
    return false;
#endif
}

bool QmlPlayerBridge::pushKbmCustomFrom(const QString& profile) {
#if VEYRA_PUSH_KBM_CORE
    if (!pkInputProfileIds().contains(profile)) return false;
    // "As before" has no values of its own here (they come from the profile file): Custom's built-in values.
    const auto kind = pkKindOf(profile) == remoteplay::KbmInputProfileKind::Legacy ? remoteplay::KbmInputProfileKind::Custom : pkKindOf(profile);
    auto settings = pkSettingsFor(kind, impl_->prefString("kbmInputCustom"), QStringLiteral("auto"));
    settings.kind = remoteplay::KbmInputProfileKind::Custom;
    if (!setPreference(QStringLiteral("kbmInputCustom"), QString::fromStdString(remoteplay::kbmInputSettingsText(settings)))) return false;
    return pushKbmSetInputProfile(QStringLiteral("custom"));
#else
    Q_UNUSED(profile);
    return false;
#endif
}

bool QmlPlayerBridge::pushKbmSetContextOverride(const QString& context) {
    if (!pkOverrideIds().contains(context)) return false;
    return setPreference(QStringLiteral("kbmContextOverride"), context);
}

QVariantList QmlPlayerBridge::pushKbmCurvePoints(const QVariantMap& curve, int count) const {
    QVariantList out;
#if VEYRA_PUSH_KBM_CORE
    remoteplay::KbmCurve c;
    const QString kind = curve.value("curve").toString();
    c.kind = kind == QLatin1String("power") ? remoteplay::KbmCurveKind::Power
           : kind == QLatin1String("precision-speed") ? remoteplay::KbmCurveKind::PrecisionSpeed : remoteplay::KbmCurveKind::Linear;
    const auto within = [&curve](const char* name, double fallback, double low, double high) {
        const double v = curve.value(QLatin1String(name), fallback).toDouble();
        return std::isfinite(v) ? std::clamp(v, low, high) : fallback;
    };
    c.exponent = within("exponent", 1.0, 0.25, 4.0);
    c.kneeIn = within("kneeIn", 0.25, 0.05, 0.95);
    c.kneeOut = within("kneeOut", 0.15, 0.01, 0.99);
    const int n = std::clamp(count, 2, 256);
    for (int k = 0; k < n; ++k) {
        const double u = double(k) / double(n - 1);
        out << QVariantMap{{"u", u}, {"out", remoteplay::kbmCurve(c, u)}};
    }
#else
    Q_UNUSED(curve); Q_UNUSED(count);
#endif
    return out;
}

QVariantMap QmlPlayerBridge::pushKbmBindings() const {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    const auto set = pkCurrentSet();
    QVariantList actions, keys;
    for (const char* name : kPkActions) actions << QString::fromLatin1(name);
    for (const auto& known : pkKeys()) keys << QString::fromStdString(known.first);
    const auto& walk = set[remoteplay::KbmProfileId::Walk].bindings;
    return {{"available", true}, {"live", kPkLiveBindings}, {"actions", actions}, {"keys", keys},
            {"walk", pkBindingList(walk, nullptr)},
            {"aim", pkBindingList(set[remoteplay::KbmProfileId::Aim].bindings, &walk)},   // only where aiming differs from walking
            {"drive", pkBindingList(set[remoteplay::KbmProfileId::Drive].bindings, nullptr)},
            {"file", QString::fromStdString(pkEdited && !pkEditedName.empty() ? pkEditedName : pkActiveName(pkFolder()))},
            {"edited", pkEdited.has_value()}, {"name", QString::fromStdString(set.name)},
            {"layoutName", QString::fromStdString(pkPageLayout().name)}};   // the core's own words for the built-in layout
#else
    return {{"available", false}};
#endif
}

QVariantMap QmlPlayerBridge::pushKbmSetBinding(const QString& context, const QString& key, const QString& action, bool replace) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    namespace rp = remoteplay;
    const auto profile = pkBindingProfile(context);
    if (!profile) return {{"ok", false}, {"reason", QStringLiteral("context")}};
    if (!pkPlainText(key, 16) || !pkPlainText(action, 16)) return {{"ok", false}, {"reason", QStringLiteral("unknown-key")}};
    const std::uint32_t code = rp::KeyboardPadBindings::keyCode(key.toStdString());
    if (code == 0) return {{"ok", false}, {"reason", QStringLiteral("unknown-key")}};
    rp::PadAction wanted = rp::PadAction::None;
    if (!rp::KeyboardPadBindings::actionName(action.toStdString(), wanted)) return {{"ok", false}, {"reason", QStringLiteral("unknown-action")}};
    auto set = pkCurrentSet();
    const rp::PadAction existing = set[*profile].bindings.action(code);
    // One key, one action: a key already doing something else is named and refused unless the user chose to move it.
    if (wanted != rp::PadAction::None && existing != rp::PadAction::None && existing != wanted && !replace)
        return {{"ok", false}, {"reason", QStringLiteral("conflict")}, {"conflict", pkActionText(existing)}};
    // Aiming uses walking's keys unless it has its own for this key (docs/KEYBOARD_MOUSE.md).
    const bool aimFollows = *profile == rp::KbmProfileId::Walk && set[rp::KbmProfileId::Aim].bindings.action(code) == existing;
    set[*profile].bindings.bind(code, wanted);
    if (aimFollows) set[rp::KbmProfileId::Aim].bindings.bind(code, wanted);
    pkStore(std::move(set));
    veyra::log::info("push-kbm", std::format("binding {} {}={}", rp::kbmProfileName(*profile), key.toStdString(), action.toStdString()));
    return {{"ok", true}, {"replaced", existing != rp::PadAction::None && existing != wanted ? pkActionText(existing) : QString()}};
#else
    Q_UNUSED(context); Q_UNUSED(key); Q_UNUSED(action); Q_UNUSED(replace);
    return {{"ok", false}, {"reason", QStringLiteral("unavailable")}};
#endif
}

bool QmlPlayerBridge::pushKbmResetBindings() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto set = pkCurrentSet();
    ps5KbmUsePageLayout(set);   // keys only; every number stays
    pkStore(std::move(set));
    veyra::log::info("push-kbm", "bindings reset to the page's built-in layout");
    return true;
#else
    return false;
#endif
}

QVariantList QmlPlayerBridge::pushKbmProfileFiles() const {
    QVariantList out;
#ifdef VEYRA_ENABLE_REMOTEPLAY
    const auto folder = pkFolder();
    if (folder.empty()) return out;
    const std::string active = pkActiveName(folder);
    std::error_code error;
    std::filesystem::directory_iterator it(folder, error);
    const std::filesystem::directory_iterator end;
    for (; !error && it != end && out.size() < 200; it.increment(error)) {
        const QString file = QString::fromStdWString(it->path().filename().wstring());
        constexpr qsizetype kPrefix = 15, kSuffix = 4;   // "slipstream-kbm-", ".txt"
        if (!file.startsWith(QLatin1String("slipstream-kbm-")) || !file.endsWith(QLatin1String(".txt")) || file.size() <= kPrefix + kSuffix) continue;
        const QString name = file.mid(kPrefix, file.size() - kPrefix - kSuffix);
        if (!pkValidSlug(name)) continue;
        out << QVariantMap{{"name", name}, {"active", name.toStdString() == active}};
    }
#endif
    return out;
}

bool QmlPlayerBridge::pushKbmLoadProfile(const QString& name) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if (!pkValidSlug(name)) return false;
    const auto folder = pkFolder();
    if (folder.empty()) return false;
    auto set = pkReadProfile(folder / remoteplay::kbmCalibratedFileName(name.toStdString()), !pkEnvironmentOn());
    if (!set) { emit notice(tr("没能读取这个按键方案文件"), true); return false; }
    pkEditedName = name.toStdString();
    pkStore(std::move(*set));
    pkChooseForNextStart(folder, pkEditedName);
    veyra::log::info("push-kbm", "profile file loaded on the page: " + pkEditedName);
    return true;
#else
    Q_UNUSED(name);
    return false;
#endif
}

bool QmlPlayerBridge::pushKbmSaveProfile(const QString& name) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if (!pkPlainText(name, 64)) return false;
    const std::string slug = remoteplay::kbmGameSlug(name.trimmed().toStdString());
    if (!pkValidSlug(QString::fromStdString(slug))) return false;
    const auto folder = pkFolder();
    if (folder.empty()) return false;
    auto set = pkCurrentSet();
    if (set.game.empty()) set.game = slug;
    // The old file stays next to the new one as .bak (kbmWriteProfile).
    const auto path = folder / remoteplay::kbmCalibratedFileName(slug);
    const auto result = remoteplay::kbmWriteProfile(path, pkProfileFileText(set, path));
    if (!result.ok) {
        veyra::log::warn("push-kbm", "profile file not written: " + result.error);
        emit notice(tr("按键方案没有保存"), true);
        return false;
    }
    pkEditedName = slug;
    pkStore(std::move(set));
    pkChooseForNextStart(folder, slug);
    veyra::log::info("push-kbm", "profile file saved on the page: " + slug);
    return true;
#else
    Q_UNUSED(name);
    return false;
#endif
}

QVariantMap QmlPlayerBridge::pushKbmCalibration(const QString& request) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if (request == QLatin1String("keep")) {
        // After a calibration that ran to its end: its numbers are in the running capture's set. The whole set (keys,
        // numbers, input lines) becomes the profile file read at the next start too; without this a calibration made
        // before any game was named in slipstream-kbm.ini would not be read again.
        if (!pkCaptureSet(ps5KbmCapture())) return {{"available", kPkLiveBindings}, {"ok", false}, {"running", false}, {"reason", QStringLiteral("no-capture")}};
        pkFollowCapture(ps5KbmCapture());
        std::string name = !pkEditedName.empty() ? pkEditedName : pkActiveName(pkFolder());
        if (name.empty()) name = "default";
        const bool ok = pushKbmSaveProfile(QString::fromStdString(name));
        return {{"available", true}, {"ok", ok}, {"running", false}, {"file", QString::fromStdString(name)}};
    }
    return pkCalibration(ps5KbmCapture(), request);
#else
    Q_UNUSED(request);
    return {{"available", false}, {"ok", false}, {"running", false}, {"reason", QStringLiteral("unavailable")}};
#endif
}

QVariantMap QmlPlayerBridge::pushKbmFit(const QString& request, const QVariantMap& args) {
#if VEYRA_PUSH_KBM_FIT
    namespace rp = remoteplay;
    const auto& i = *impl_;
    const QString context = args.value("context", QStringLiteral("walk")).toString();
    const qsizetype contextIndex = pkOverrideIds().indexOf(context) - 1;   // walk 0, aim 1, sprint 2, drive 3
    if (request == QLatin1String("record") || request == QLatin1String("stop"))
        return pkFitRecord(ps5KbmCapture(), request == QLatin1String("record"), args.value("kind").toString());
    const auto base = pkSettingsFor(pkKindOf(pkPrefOr(i.prefs, "kbmInputProfile", QStringLiteral("legacy"))), i.prefString("kbmInputCustom"), QStringLiteral("auto"));
    if (request == QLatin1String("fit")) {
        if (contextIndex < 0 || contextIndex > 3) return {{"available", true}, {"ok", false}, {"reason", QStringLiteral("context")}};
        rp::KbmFitInput input;
        for (const auto& v : args.value("turnCounts").toList()) {
            const double counts = v.toDouble();
            if (std::isfinite(counts) && counts > 0 && input.turnCounts.size() < 10) input.turnCounts.push_back(counts);
        }
        input.flicks = pkMotions(args.value("flicks"));
        input.slow = pkMotions(args.value("slow"));
        const double turn = args.value("gameFullTurnSeconds").toDouble();
        input.gameFullTurnSeconds = std::isfinite(turn) && turn > 0 ? turn : 0;
        const auto result = rp::kbmFitPreference(input, base.contexts[static_cast<std::size_t>(contextIndex)]);
        return {{"available", true}, {"ok", result.ok}, {"reason", QString::fromStdString(result.reason)}, {"tuning", pkTuningMap(result.tuning)},
                {"countsPer360", result.countsPer360}, {"flickP90", result.flickP90CountsPerSec}, {"slowP10", result.slowP10CountsPerSec},
                {"explanation", QString::fromStdString(result.explanation)}};
    }
    if (request == QLatin1String("apply")) {
        // The user confirmed the fitted numbers: they become Custom's values for that context ("all": the three on
        // foot) and Custom is chosen. Nothing else changes.
        const QVariantMap t = args.value("tuning").toMap();
        auto settings = base.kind == rp::KbmInputProfileKind::Legacy ? pkCustomSettings(i.prefString("kbmInputCustom")) : base;
        settings.kind = rp::KbmInputProfileKind::Custom;
        std::vector<std::size_t> targets;
        if (context == QLatin1String("all")) targets = {0, 1, 2};
        else if (contextIndex >= 0 && contextIndex <= 3) targets = {static_cast<std::size_t>(contextIndex)};
        else return {{"available", true}, {"ok", false}, {"reason", QStringLiteral("context")}};
        std::string text;
        for (const std::size_t c : targets) {
            const std::string p = pkOverrideIds().at(static_cast<qsizetype>(c + 1)).toStdString() + ".";
            for (const char* name : {"sensX", "sensY", "exponent", "kneeIn", "kneeOut", "deadzone", "fullSpeed", "carryMs", "maxTilt"})
                if (t.contains(QLatin1String(name))) {
                    const std::string number = pkNumber(t.value(QLatin1String(name)).toDouble());
                    if (number.empty()) return {{"available", true}, {"ok", false}, {"reason", QStringLiteral("number")}};   // NaN / infinity
                    text += p + name + "=" + number + ";";
                }
            if (t.contains(QLatin1String("game"))) text += p + "game=" + (t.value(QLatin1String("game")).toBool() ? "1" : "0") + ";";
            for (const char* name : {"curve", "saturation"}) {
                const QString word = t.value(QLatin1String(name)).toString();
                if (!word.isEmpty() && pkPlainText(word, 24) && !word.contains(QLatin1Char(';')) && !word.contains(QLatin1Char('=')))
                    text += p + name + "=" + word.toStdString() + ";";
            }
        }
        const auto parsed = rp::parseKbmInputSettings(text, settings);
        if (parsed.refused != 0) return {{"available", true}, {"ok", false}, {"reason", QStringLiteral("refused")}};
        auto applied = rp::kbmSanitized(parsed.settings);
        applied.kind = rp::KbmInputProfileKind::Custom;
        const bool ok = setPreference(QStringLiteral("kbmInputCustom"), QString::fromStdString(rp::kbmInputSettingsText(applied)))
                     && pushKbmSetInputProfile(QStringLiteral("custom"));
        veyra::log::info("push-kbm", std::format("calibration result applied by the user: context={} ok={}", context.toStdString(), ok));
        return {{"available", true}, {"ok", ok}};
    }
    return {{"available", true}, {"ok", false}, {"reason", QStringLiteral("request")}};
#else
    Q_UNUSED(request); Q_UNUSED(args);
    return {{"available", false}, {"ok", false}, {"reason", QStringLiteral("unavailable")}};
#endif
}

QVariantMap QmlPlayerBridge::pushKbmReadout() const {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    const auto& i = *impl_;
    // Only while a PS5 stream runs: between sessions the last window's numbers would be stale.
    if (!(i.snapshot.remotePlay && i.snapshot.running)) return {{"available", PkReadout<PkCapture> && kPkInputProfiles}, {"active", false}};
    return pkReadout(static_cast<const PkCapture*>(ps5KbmCapture()));
#else
    return {{"available", false}, {"active", false}};
#endif
}

} // namespace veyra::ui
