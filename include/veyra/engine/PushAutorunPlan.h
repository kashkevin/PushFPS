// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushLatencyStats.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine::autorun {
// Test automation for the hardware test kit: VEYRA_TEST_RP_AUTORUN=<plan> (src/ui/bridge/PushAutorun.cpp,
// docs/LATENCY_MEASUREMENT.md, contract in docs/PS5_REMOTE_PLAY_EXPERIMENTS.md). Portable and header-only:
// the plan parser, the hook registry and the step runner (a pure state machine the bridge feeds once per tick).
// Tests: tests/remoteplay/AutorunPlanTests.inc.
//
// Plan: steps separated by ';' (spaces around them ignored), run in order once a PS5 stream is presenting:
//   settle:<s>                 frames presented steadily for s seconds
//   latency:<trials>[:<button>] the built-in latency test (button ps | options | dpad-right-left)
//   wait:<s>                   s seconds
//   sr:<s> | fg:<s> | quality  hook steps: run by a handler another part registers (registerStep)
//   exit                       close the app through its normal teardown
// A plan never changes saved preferences. Steps are numbered from 1 in the log (step=0 is the connection of
// VEYRA_TEST_RP_AUTOCONNECT). A plan that does not parse fails closed: its first bad step is logged as failed and
// the plan finishes (ok=0) on the first tick, before anything runs (no wait for a stream, no press); the app is
// closed only when the text holds a well-formed `exit` step. Once the stream was seen, the plan is abandoned
// (reason=stream-lost) when that session ends or is replaced (disconnect, reconnect, the user's Stop) or stops
// presenting for kStreamLostMs, and (reason=cancelled) when the user cancels the latency test it started: it never
// goes on pressing on a later session.

inline constexpr std::size_t kMaxSteps = 32;
inline constexpr std::size_t kMaxPlanBytes = 1024;
inline constexpr int kMaxSeconds = 3600;
inline constexpr int kMaxHookSeconds = 600;
// How long to wait for the stream to present before the first step fails (reason=no-stream).
inline constexpr std::int64_t kStreamWaitMs = 180000;
// How long a stream that was seen may stop presenting (same session) before the plan is abandoned.
inline constexpr std::int64_t kStreamLostMs = 10000;

enum class Kind : std::uint8_t { Settle, Latency, Wait, Hook, Exit, Invalid };
struct Step {
    Kind kind = Kind::Invalid;
    std::string name;           // log name: settle, latency, wait, sr, fg, quality, exit; "unknown" for an unknown step
    int seconds = 0;            // settle, wait, sr, fg
    int trials = 0;             // latency
    latencystats::Button button = latencystats::Button::Ps;
    std::string reason;         // Invalid: unknown-step | bad-argument | too-many-steps | plan-too-long
};
struct Plan {
    std::vector<Step> steps;    // stops after the first Invalid one (kept, so it is logged as failed)
    bool valid() const { return !steps.empty() && std::none_of(steps.begin(), steps.end(), [](const Step& s) { return s.kind == Kind::Invalid; }); }
    // The plan text asks to close the app at the end (even when a step before it is invalid): a failed plan still
    // closes the app then, so the kit never waits for a window nobody closes.
    bool exitRequested = false;
    bool hasExit() const { return exitRequested || std::any_of(steps.begin(), steps.end(), [](const Step& s) { return s.kind == Kind::Exit; }); }
};

namespace detail {
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}
// A whole positive decimal number within [low, high]; nullopt otherwise (no sign, no spaces, no suffix).
inline std::optional<int> number(std::string_view s, int low, int high) {
    if (s.empty() || s.size() > 6) return std::nullopt;
    int v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    if (v < low || v > high) return std::nullopt;
    return v;
}
inline std::vector<std::string_view> split(std::string_view s, char by) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == by) { out.push_back(s.substr(start, i - start)); start = i + 1; }
    return out;
}
} // namespace detail

inline bool isHookName(std::string_view name) { return name == "sr" || name == "fg" || name == "quality" || name == "probe"; }

inline Step parseStep(std::string_view text) {
    Step step;
    const auto parts = detail::split(detail::trim(text), ':');
    const std::string_view head = parts.empty() ? std::string_view() : parts[0];
    const auto bad = [&step](const char* name) { step.kind = Kind::Invalid; step.name = name; step.reason = "bad-argument"; return step; };
    if (head == "settle" || head == "wait") {
        const char* name = head == "settle" ? "settle" : "wait";
        if (parts.size() != 2) return bad(name);
        const auto s = detail::number(parts[1], head == "settle" ? 1 : 0, kMaxSeconds);
        if (!s) return bad(name);
        step.kind = head == "settle" ? Kind::Settle : Kind::Wait; step.name = name; step.seconds = *s;
        return step;
    }
    if (head == "latency") {
        if (parts.size() != 2 && parts.size() != 3) return bad("latency");
        const auto t = detail::number(parts[1], latencystats::kTrialsMin, latencystats::kTrialsMax);
        const auto b = parts.size() == 3 ? latencystats::parseButton(parts[2]) : std::optional(latencystats::Button::Ps);
        if (!t || !b || (parts.size() == 3 && parts[2].empty())) return bad("latency");
        step.kind = Kind::Latency; step.name = "latency"; step.trials = *t; step.button = *b;
        return step;
    }
    // probe:<s> (sprint 5): the pipeline probe, engine/PushPipelineProbe.h; its handler is registered in PushAutorun.cpp.
    if (head == "sr" || head == "fg" || head == "probe") {
        const char* name = head == "sr" ? "sr" : head == "fg" ? "fg" : "probe";
        if (parts.size() != 2) return bad(name);
        const auto s = detail::number(parts[1], 1, kMaxHookSeconds);
        if (!s) return bad(name);
        step.kind = Kind::Hook; step.name = name; step.seconds = *s;
        return step;
    }
    if (head == "quality" || head == "exit") {
        const char* name = head == "quality" ? "quality" : "exit";
        if (parts.size() != 1) return bad(name);
        step.kind = head == "quality" ? Kind::Hook : Kind::Exit; step.name = name;
        return step;
    }
    step.kind = Kind::Invalid; step.name = "unknown"; step.reason = "unknown-step";
    return step;
}

inline Plan parsePlan(std::string_view text) {
    Plan plan;
    if (text.size() > kMaxPlanBytes) {
        Step s; s.name = "plan"; s.reason = "plan-too-long"; plan.steps.push_back(s);
        return plan;
    }
    for (const auto part : detail::split(text, ';')) if (detail::trim(part) == "exit") plan.exitRequested = true;
    for (const auto part : detail::split(text, ';')) {
        if (detail::trim(part).empty()) continue;   // "a;;b" and a trailing ';' are fine
        if (plan.steps.size() == kMaxSteps) {
            Step s; s.name = "plan"; s.reason = "too-many-steps"; plan.steps.push_back(s);
            return plan;
        }
        plan.steps.push_back(parseStep(part));
        if (plan.steps.back().kind == Kind::Invalid) return plan;
    }
    return plan;
}

// How long a step may take before it fails with reason=timeout.
inline std::int64_t stepTimeoutMs(const Step& s) {
    switch (s.kind) {
    case Kind::Settle: return (std::int64_t(s.seconds) + 120) * 1000;
    case Kind::Wait: return (std::int64_t(s.seconds) + 10) * 1000;
    // The slowest button cycle (PS: open, answer, close, settle) is about 3.3 s; generous on top.
    // OPTIONS / D-pad add sham presses (latencystats::shamCount), each as long as a press.
    case Kind::Latency: return std::int64_t(s.trials + (s.button == latencystats::Button::Ps ? 0 : latencystats::shamCount(s.trials))) * 5000 + 60000;
    case Kind::Hook: return s.name == "quality" ? 600000 : std::max<std::int64_t>(600000, std::int64_t(s.seconds) * 1000 * 16 + 60000);
    default: return 10000;
    }
}

// ---- Hook steps (sr, fg, quality) ---------------------------------------------------------------------------
// Another part of the program registers a handler per step name, on the interface thread, before or while a plan
// runs. The runner calls it with Start once, then Poll every tick until it answers Done or Failed, and Cancel
// when the step times out or the plan is abandoned (the handler must then put the user's settings back).
enum class HookEvent : std::uint8_t { Start, Poll, Cancel };
enum class HookState : std::uint8_t { Running, Done, Failed };
struct HookResult {
    HookState state = HookState::Running;
    std::string reason;   // one word for Failed (e.g. not-available); ignored otherwise
};
using StepHandler = std::function<HookResult(HookEvent event, int seconds)>;
inline std::map<std::string, StepHandler, std::less<>>& stepRegistry() {
    static std::map<std::string, StepHandler, std::less<>> registry;   // interface thread only
    return registry;
}
inline void registerStep(std::string_view name, StepHandler handler) {
    if (handler) stepRegistry()[std::string(name)] = std::move(handler);
    else stepRegistry().erase(std::string(name));
}
inline const StepHandler* stepHandler(std::string_view name) {
    const auto& r = stepRegistry();
    const auto it = r.find(name);
    return it == r.end() || !it->second ? nullptr : &it->second;
}

// ---- Runner ---------------------------------------------------------------------------------------------------
enum class LatencyState : std::uint8_t { Idle, Running, Done, Failed };
// What the bridge reports each tick.
struct Inputs {
    std::int64_t nowMs = 0;
    bool presenting = false;            // a PS5 stream is running and presenting frames
    std::uint64_t session = 0;          // the running PS5 session's id, 0 when none runs
    std::int64_t steadySinceMs = -1;    // since when presentation has been steady (-1: not steady now)
    bool measuring = false;             // optimiser, quality test or calibration running
    LatencyState latency = LatencyState::Idle;
};
enum class Action : std::uint8_t { None, StartLatency, CancelLatency, Exit };
struct Output {
    Action action = Action::None;
    std::vector<std::string> lines;     // text for [slipstream-autorun] lines, in order
};

class Runner {
public:
    explicit Runner(Plan plan) : plan_(std::move(plan)) {}
    bool finished() const { return finished_; }
    bool ok() const { return ok_; }
    std::size_t index() const { return index_; }
    const Plan& plan() const { return plan_; }
    // The bridge could not start the latency test it was asked to start.
    void latencyRefused(std::string reason) { refused_ = reason.empty() ? "refused" : std::move(reason); }
    // VEYRA_EXP_RP_LATENCY_BUTTONS=1: latency steps may use OPTIONS / the D-pad. Off (the default): such a step fails
    // with reason=buttons-experimental when it starts, before anything is pressed (latencystats::buttonAllowed).
    void setButtonsAvailable(bool on) { buttonsAvailable_ = on; }

    Output tick(const Inputs& in) {
        Output out;
        if (finished_) return out;
        if (startMs_ < 0) startMs_ = in.nowMs;
        if (!plan_.valid()) {
            // Fail closed: name the bad step, run nothing. finish() closes the app only for a well-formed exit.
            index_ = plan_.steps.empty() ? 0 : plan_.steps.size() - 1;
            if (!plan_.steps.empty()) line(out, "failed", word(plan_.steps[index_].reason, "failed"));
            finish(out, false);
            if (plan_.hasExit()) out.action = Action::Exit;
            return out;
        }
        if (index_ >= plan_.steps.size()) { finish(out, true); return out; }
        // The stream the plan runs on went away: stop, and never carry on with a later session.
        if (streamSeen_) {
            if (in.presenting) lastPresentingMs_ = in.nowMs;
            if (in.session != session_ || in.nowMs - lastPresentingMs_ >= kStreamLostMs) { stop(out, "stream-lost"); return out; }
        }
        const Step& step = plan_.steps[index_];
        if (!started_) {
            if (step.kind == Kind::Invalid) { line(out, "start", "none"); fail(out, step.reason); return out; }
            // The plan begins once the stream presents; exit alone never waits for a stream.
            if (!streamSeen_ && step.kind != Kind::Exit) {
                if (!in.presenting) {
                    if (in.nowMs - startMs_ >= kStreamWaitMs) { line(out, "start", "none"); fail(out, "no-stream"); }
                    return out;
                }
                streamSeen_ = true; session_ = in.session; lastPresentingMs_ = in.nowMs;
            }
            started_ = true; stepStartMs_ = in.nowMs; refused_.clear(); hookStarted_ = false;
            line(out, "start", "none");
            if (step.kind == Kind::Latency && !latencystats::buttonAllowed(step.button, buttonsAvailable_)) {
                fail(out, latencystats::kButtonsExperimentalReason); return out;
            }
            if ((step.kind == Kind::Latency || step.kind == Kind::Hook) && in.measuring) { fail(out, "busy"); return out; }
            if (step.kind == Kind::Latency) { out.action = Action::StartLatency; latencySeenRunning_ = false; return out; }
            if (step.kind == Kind::Hook && !stepHandler(step.name)) { fail(out, "unsupported"); return out; }
            if (step.kind == Kind::Exit) { done(out); finish(out, true); return out; }
        }
        const std::int64_t elapsed = in.nowMs - stepStartMs_;
        const bool late = elapsed >= stepTimeoutMs(step);
        switch (step.kind) {
        case Kind::Settle:
            if (in.presenting && in.steadySinceMs >= 0 && in.nowMs - in.steadySinceMs >= std::int64_t(step.seconds) * 1000) done(out);
            else if (late) fail(out, "timeout");
            break;
        case Kind::Wait:
            if (elapsed >= std::int64_t(step.seconds) * 1000) done(out);
            break;
        case Kind::Latency:
            if (!refused_.empty()) { fail(out, refused_); break; }
            if (in.latency == LatencyState::Running) latencySeenRunning_ = true;
            if (in.latency == LatencyState::Done && latencySeenRunning_) done(out);
            else if (in.latency == LatencyState::Failed && latencySeenRunning_) fail(out, "test-failed");
            else if (in.latency == LatencyState::Idle && latencySeenRunning_) fail(out, "cancelled");   // the user's Cancel
            else if (late) { out.action = Action::CancelLatency; fail(out, "timeout"); }
            else if (!latencySeenRunning_ && elapsed > 5000) fail(out, "not-started");
            break;
        case Kind::Hook: {
            const StepHandler* handler = stepHandler(step.name);
            if (!handler) { fail(out, "unsupported"); break; }
            if (late) { (*handler)(HookEvent::Cancel, step.seconds); fail(out, "timeout"); break; }
            const HookResult r = (*handler)(hookStarted_ ? HookEvent::Poll : HookEvent::Start, step.seconds);
            hookStarted_ = true;
            if (r.state == HookState::Done) done(out);
            else if (r.state == HookState::Failed) fail(out, word(r.reason, "failed"));
            break;
        }
        default: break;
        }
        return out;
    }
    // The program is closing or the plan is abandoned: cancel what runs and log the end once.
    // Action::CancelLatency asks the bridge to release the button (pushLatencyTestCancel).
    Output abandon(const char* reason) {
        Output out;
        if (finished_) return out;
        stop(out, reason);
        return out;
    }
    // Only [a-z0-9-], at most 24 characters; `fallback` when nothing is left.
    static std::string word(std::string_view text, const char* fallback) {
        std::string w;
        for (const char c : text) {
            if (w.size() >= 24) break;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') w.push_back(c);
            else if (c >= 'A' && c <= 'Z') w.push_back(char(c - 'A' + 'a'));
        }
        return w.empty() ? std::string(fallback) : w;
    }

private:
    // Fails the current step (cancelling what it runs) and finishes the plan.
    void stop(Output& out, std::string_view reason) {
        if (index_ >= plan_.steps.size()) { finish(out, false); return; }
        const Step& step = plan_.steps[index_];
        if (started_ && step.kind == Kind::Hook) if (const StepHandler* h = stepHandler(step.name)) (*h)(HookEvent::Cancel, step.seconds);
        if (started_ && step.kind == Kind::Latency) out.action = Action::CancelLatency;
        fail(out, reason);
    }
    void line(Output& out, const char* state, std::string_view reason) const {
        const Step& step = plan_.steps[index_];
        out.lines.push_back("step=" + std::to_string(index_ + 1) + " name=" + step.name + " state=" + state + " reason=" + std::string(reason));
    }
    void done(Output& out) { line(out, "done", "ok"); ++index_; started_ = false; if (index_ >= plan_.steps.size()) finish(out, true); }
    void fail(Output& out, std::string_view reason) {
        line(out, "failed", word(reason, "failed"));
        // Stop: what is left is skipped. A plan with an exit still closes the app, so the kit never waits forever.
        finish(out, false);
        if (plan_.hasExit()) out.action = out.action == Action::CancelLatency ? Action::CancelLatency : Action::Exit;
        exitAfterCancel_ = plan_.hasExit();
    }
    void finish(Output& out, bool okay) {
        if (finished_) return;
        finished_ = true; ok_ = okay;
        out.lines.push_back("finished ok=" + std::string(okay ? "1" : "0") + " steps=" + std::to_string(plan_.steps.size()));
        if (okay && plan_.hasExit()) out.action = Action::Exit;
    }
public:
    // After a failure that cancelled the latency test, the bridge still closes the app when the plan had an exit.
    bool exitAfterCancel() const { return exitAfterCancel_; }
private:
    Plan plan_;
    std::size_t index_ = 0;
    bool buttonsAvailable_ = false;
    bool started_ = false, finished_ = false, ok_ = false, streamSeen_ = false, hookStarted_ = false, latencySeenRunning_ = false;
    bool exitAfterCancel_ = false;
    std::int64_t startMs_ = -1, stepStartMs_ = 0, lastPresentingMs_ = 0;
    std::uint64_t session_ = 0;
    std::string refused_;
};

// ---- [slipstream-config] pacing ---------------------------------------------------------------------------------
// The line is written at a session's start and when a value changes, at most once per kConfigLineGapMs. Values that
// move by themselves (fgActive, srActive, the window size) are written but never cause a line on their own.
inline constexpr std::int64_t kConfigLineGapMs = 5000;
inline bool configFlappingKey(std::string_view token) {
    return token.starts_with("fgActive=") || token.starts_with("srActive=") || token.starts_with("window=");
}
// The line without the keys that move by themselves: what decides whether a line is due.
inline std::string configTrigger(std::string_view line) {
    std::string out;
    for (const auto token : detail::split(line, ' ')) {
        if (token.empty() || configFlappingKey(token)) continue;
        if (!out.empty()) out.push_back(' ');
        out.append(token);
    }
    return out;
}
class ConfigLineGate {
public:
    // True when `line` should be written now (then it counts as written).
    bool due(std::string_view line, std::uint64_t session, std::int64_t nowMs) {
        std::string trigger = configTrigger(line);
        if (any_ && session == session_ && trigger == trigger_) return false;
        if (any_ && nowMs - lastMs_ < kConfigLineGapMs) return false;   // due later: checked again on a later tick
        any_ = true; session_ = session; trigger_ = std::move(trigger); lastMs_ = nowMs;
        return true;
    }
private:
    bool any_ = false;
    std::uint64_t session_ = 0;
    std::string trigger_;
    std::int64_t lastMs_ = 0;
};

} // namespace veyra::engine::autorun
