// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine::latencystats {
// Statistics for the Remote Play latency test (src/ui/bridge/PushLatencyTest.cpp, docs/LATENCY_MEASUREMENT.md).
// Portable and header-only; tests: tests/remoteplay/LatencyStatsTests.inc.
//
// Every press is split into two parts:
//   sendToArrival:   input sent -> network arrival of the first frame whose picture changed (the console's response,
//                    its encoder and the network; PC-side work changes only when the input goes out);
//   arrivalToScanout: that frame's arrival -> its scanout (or, while the scanout time is not known, its Present):
//                    the PC pipeline, the part our changes act on, with a much smaller spread.
// Times are host 100 ns ticks on one steady clock; 0 or less means unknown.

// The most presses one latency test accepts in this build (the original build: 20 in the bridge, 12 in the dialog).
inline constexpr int kTrialsMax = 500;
inline constexpr int kTrialsMin = 3;

enum class Button : std::uint8_t { Ps, Options, DpadRightLeft };
inline std::optional<Button> parseButton(std::string_view text) {
    if (text.empty() || text == "ps") return Button::Ps;
    if (text == "options") return Button::Options;
    if (text == "dpad-right-left") return Button::DpadRightLeft;
    return std::nullopt;
}
inline const char* buttonName(Button b) {
    return b == Button::Options ? "options" : b == Button::DpadRightLeft ? "dpad-right-left" : "ps";
}

// Linear-interpolation quantile (type 7) of a copy; NaN for an empty sample.
inline double quantile(std::vector<double> v, double q) {
    if (v.empty()) return std::nan("");
    std::sort(v.begin(), v.end());
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = std::min(v.size() - 1, lo + 1);
    const double frac = pos - static_cast<double>(lo);
    return v[lo] + (v[hi] - v[lo]) * frac;
}
inline double median(const std::vector<double>& v) { return quantile(v, 0.5); }
// Mean after dropping `trim` of the sample at each end (0.1 = 10 % low and 10 % high).
inline double trimmedMean(std::vector<double> v, double trim = 0.1) {
    if (v.empty()) return std::nan("");
    std::sort(v.begin(), v.end());
    const std::size_t cut = static_cast<std::size_t>(std::floor(std::clamp(trim, 0.0, 0.49) * static_cast<double>(v.size())));
    double sum = 0; std::size_t n = 0;
    for (std::size_t i = cut; i + cut < v.size(); ++i) { sum += v[i]; ++n; }
    return n ? sum / static_cast<double>(n) : std::nan("");
}
// 1.4826 x the median absolute deviation: the standard deviation of a normal sample, not moved by outliers.
inline double robustSigma(const std::vector<double>& v) {
    if (v.size() < 2) return std::nan("");
    const double m = median(v);
    std::vector<double> d; d.reserve(v.size());
    for (const double x : v) d.push_back(std::abs(x - m));
    return 1.4826 * median(d);
}

// Deterministic generator (xorshift64*): the same presses give the same intervals in every run and on every compiler.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        state_ ^= state_ >> 12; state_ ^= state_ << 25; state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
    std::size_t below(std::size_t n) { return n ? static_cast<std::size_t>(next() % n) : 0; }
private:
    std::uint64_t state_;
};

enum class Statistic : std::uint8_t { Median, TrimmedMean };
struct Interval { double low = std::nan(""), high = std::nan(""); bool valid() const { return std::isfinite(low) && std::isfinite(high); } };
// Percentile bootstrap 95 % interval of the statistic (resamples with replacement); invalid under 5 values.
inline Interval bootstrap95(const std::vector<double>& v, Statistic statistic, int resamples = 1000, std::uint64_t seed = 1) {
    Interval out;
    if (v.size() < 5 || resamples < 20) return out;
    Random random(seed);
    std::vector<double> stats; stats.reserve(static_cast<std::size_t>(resamples));
    std::vector<double> sample(v.size());
    for (int r = 0; r < resamples; ++r) {
        for (auto& x : sample) x = v[random.below(v.size())];
        stats.push_back(statistic == Statistic::Median ? median(sample) : trimmedMean(sample));
    }
    out.low = quantile(stats, 0.025);
    out.high = quantile(stats, 0.975);
    return out;
}
// The smallest true difference between two runs like this one (same number of presses, same spread) that a
// two-sided test at 5 % finds 80 % of the time: (1.96 + 0.84) x sqrt(2) x the standard error of the median,
// the standard error taken from the bootstrap interval when there is one, else 1.2533 x robust sigma / sqrt(n).
inline double detectableDifferenceMs(const std::vector<double>& v, const Interval& medianInterval) {
    if (v.size() < 5) return std::nan("");
    double se = medianInterval.valid() ? (medianInterval.high - medianInterval.low) / (2.0 * 1.96) : std::nan("");
    if (!std::isfinite(se) || se <= 0) {
        const double sigma = robustSigma(v);
        se = 1.2533 * sigma / std::sqrt(static_cast<double>(v.size()));
    }
    return std::isfinite(se) ? 2.80 * std::sqrt(2.0) * se : std::nan("");
}
// Presses needed for a detectable difference of `targetMs` at the spread `sigmaMs` (same design as above).
inline int pressesForDifference(double sigmaMs, double targetMs) {
    if (!(sigmaMs > 0) || !(targetMs > 0)) return -1;
    const double n = std::pow(2.80 * std::sqrt(2.0) * 1.2533 * sigmaMs / targetMs, 2.0);
    return n > 1e6 ? -1 : static_cast<int>(std::ceil(n));
}

struct Summary {
    std::size_t n = 0;
    double p50 = std::nan(""), p95 = std::nan(""), trimmed = std::nan(""), sigma = std::nan(""), detectableMs = std::nan("");
    Interval medianCi;
};
inline Summary summarize(const std::vector<double>& v, std::uint64_t seed = 1) {
    Summary s;
    s.n = v.size();
    if (v.empty()) return s;
    s.p50 = median(v); s.p95 = quantile(v, 0.95); s.trimmed = trimmedMean(v); s.sigma = robustSigma(v);
    s.medianCi = bootstrap95(v, Statistic::Median, 1000, seed);
    s.detectableMs = detectableDifferenceMs(v, s.medianCi);
    return s;
}

// One press, split. All in ms; -1 = unknown.
struct PressSplit {
    double sendToArrivalMs = -1, arrivalToScanoutMs = -1, arrivalToPresentMs = -1, totalMs = -1, phaseMs = -1;
};
// sent: the press went out; arrival: network arrival of the first changed frame; present: just before its Present;
// scanout: when it reached the screen (0 = unknown); lastArrival: the newest frame arrival before the press;
// intervalMs: the stream's frame interval.
inline PressSplit split(std::int64_t sent, std::int64_t arrival, std::int64_t present, std::int64_t scanout,
                        std::int64_t lastArrival, double intervalMs) {
    PressSplit p;
    if (sent <= 0) return p;
    const auto ms = [](std::int64_t from, std::int64_t to) { return static_cast<double>(to - from) / 10000.0; };
    const bool arrivalKnown = arrival > 0 && arrival >= sent;
    if (arrivalKnown) p.sendToArrivalMs = ms(sent, arrival);
    if (arrivalKnown && present > 0 && present >= arrival) p.arrivalToPresentMs = ms(arrival, present);
    if (arrivalKnown && scanout > 0 && scanout >= arrival) p.arrivalToScanoutMs = ms(arrival, scanout);
    if (scanout > 0 && scanout >= sent) p.totalMs = ms(sent, scanout);
    else if (present > 0 && present >= sent) p.totalMs = ms(sent, present);
    if (lastArrival > 0 && lastArrival <= sent && intervalMs > 0) p.phaseMs = std::fmod(ms(lastArrival, sent), intervalMs);
    return p;
}
// The wait before the next press: the fixed baseline plus a uniform share of `frames` stream frame intervals, so
// presses land at every phase of the stream's frame clock instead of locking to one (aliasing).
inline double pressGapMs(Random& random, double baselineMs, double intervalMs, int frames = 4) {
    const double interval = intervalMs > 1 && intervalMs < 200 ? intervalMs : 1000.0 / 60.0;
    return baselineMs + random.unit() * interval * static_cast<double>(std::max(1, frames));
}

// What the PC share of a test is built from: scanout times when every answered press has one, else Present.
enum class PcBasis : std::uint8_t { None, Present, Scanout };
inline const char* pcBasisName(PcBasis b) { return b == PcBasis::Scanout ? "scanout" : b == PcBasis::Present ? "present" : "none"; }

struct TestSummary {
    int trials = 0, answered = 0;
    Button button = Button::Ps;
    Summary console, pc, total;
    PcBasis basis = PcBasis::None;
};
inline TestSummary summarizeTest(const std::vector<PressSplit>& presses, int trials, Button button) {
    TestSummary t;
    t.trials = trials; t.answered = static_cast<int>(presses.size()); t.button = button;
    std::vector<double> console, scanout, present, total;
    for (const auto& p : presses) {
        if (p.sendToArrivalMs >= 0) console.push_back(p.sendToArrivalMs);
        if (p.arrivalToScanoutMs >= 0) scanout.push_back(p.arrivalToScanoutMs);
        if (p.arrivalToPresentMs >= 0) present.push_back(p.arrivalToPresentMs);
        if (p.totalMs >= 0) total.push_back(p.totalMs);
    }
    t.console = summarize(console, 11);
    t.total = summarize(total, 13);
    if (!scanout.empty() && scanout.size() == present.size()) { t.pc = summarize(scanout, 17); t.basis = PcBasis::Scanout; }
    else if (!present.empty()) { t.pc = summarize(present, 17); t.basis = PcBasis::Present; }
    return t;
}

// Log text (after the "[family] " prefix the logger adds). Numbers with '.' decimals whatever the locale; -1 unknown.
inline std::string number(double v, int decimals = 2) {
    if (!std::isfinite(v) || v < 0) return "-1";
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", decimals, v);
    for (char* c = b; *c; ++c) if (*c == ',') *c = '.';
    return b;
}
// [slipstream-press]: the contract's keys first, in order; arrivalToPresentMs appended.
inline std::string pressLine(int index, const PressSplit& p) {
    return "n=" + std::to_string(index) + " sendToArrivalMs=" + number(p.sendToArrivalMs) + " arrivalToScanoutMs=" + number(p.arrivalToScanoutMs) +
           " totalMs=" + number(p.totalMs) + " phaseMs=" + number(p.phaseMs) + " arrivalToPresentMs=" + number(p.arrivalToPresentMs);
}
// [slipstream-latency-test]: the contract's keys first, in order; the robust statistics appended.
inline std::string testLine(const TestSummary& t) {
    std::string s = "trials=" + std::to_string(t.trials) + " answered=" + std::to_string(t.answered) + " button=" + buttonName(t.button) +
        " consoleP50=" + number(t.console.p50) + " consoleP95=" + number(t.console.p95) + " pcP50=" + number(t.pc.p50) + " pcP95=" + number(t.pc.p95) +
        " totalP50=" + number(t.total.p50) + " totalP95=" + number(t.total.p95);
    s += std::string(" pcBasis=") + pcBasisName(t.basis);
    const auto add = [&s](const char* name, const Summary& m) {
        s += std::string(" ") + name + "N=" + std::to_string(m.n) + " " + name + "Trimmed=" + number(m.trimmed) + " " + name + "CiLow=" + number(m.medianCi.low) +
             " " + name + "CiHigh=" + number(m.medianCi.high) + " " + name + "Detectable=" + number(m.detectableMs);
    };
    add("console", t.console); add("pc", t.pc); add("total", t.total);
    return s;
}

} // namespace veyra::engine::latencystats
