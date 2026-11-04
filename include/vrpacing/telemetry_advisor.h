#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <vector>
#include "vrpacing/shared_state.h"

namespace vrpacing {

enum class AdvisorState : int32_t {
    Idle = 0,
    Collecting = 1,
    Sustained = 2,
    NotSustained = 3
};

struct AdvisorReport {
    int32_t state = static_cast<int32_t>(AdvisorState::Idle);
    wchar_t summaryLine[160]{};
    wchar_t detailLine[160]{};
};

class TelemetryAnalyzer {
public:
    TelemetryAnalyzer() {
        scratch_.reserve(1200);
        graphHistoryMs_.fill(0.0f);
        Reset();
    }

    void Reset() {
        shortWindowMs_.clear();
        evalWindowMs_.clear();
        graphHistoryMs_.fill(0.0f);
        lastPid_ = 0;
        lastRawHz_ = 0.0;
        lastDivisor_ = 1.0;
        lastReadIdx_ = 0;
        avgFps_ = 0.0;
        onePercentLowFps_ = 0.0;
        pendingTicks_ = 0;
        pendingState_ = static_cast<int32_t>(AdvisorState::Idle);
        pendingSummary_[0] = L'\0';

        currentReport_.state = static_cast<int32_t>(AdvisorState::Idle);
        wcscpy_s(currentReport_.summaryLine, L"Advisor: Waiting for VR session...");
        currentReport_.detailLine[0] = L'\0';
    }

    void Ingest(const SharedState& shm) {
        // Automatic context reset on PID change, exit, or major mode change
        if (shm.activePid == 0 ||
            shm.activePid != lastPid_ ||
            std::abs(shm.rawPanelHz - lastRawHz_) > 0.5 ||
            std::abs(shm.frameGenDivisor - lastDivisor_) > 0.05) {

            const uint32_t newPid = shm.activePid;
            const double newRawHz = shm.rawPanelHz;
            const double newDivisor = shm.frameGenDivisor;
            const uint32_t syncIdx = shm.frameTimeWriteIdx % 256u;

            Reset();

            lastPid_ = newPid;
            lastRawHz_ = newRawHz;
            lastDivisor_ = newDivisor;
            lastReadIdx_ = syncIdx;

            if (newPid == 0 || newRawHz < 10.0) {
                return;
            }
        }

        const uint32_t writeIdx = shm.frameTimeWriteIdx % 256u;
        const uint32_t count = (writeIdx >= lastReadIdx_)
                                   ? (writeIdx - lastReadIdx_)
                                   : (256u + writeIdx - lastReadIdx_);
        const uint32_t samplesToRead = std::min<uint32_t>(count, 256u);

        for (uint32_t i = 0; i < samplesToRead; ++i) {
            const uint32_t idx = (lastReadIdx_ + i) % 256u;
            const float dtMs = shm.frameTimesMs[idx];

            // Valid gameplay sample (ignore loading freezes and pauses)
            if (dtMs >= 2.0f && dtMs <= 200.0f) {
                shortWindowMs_.push_back(dtMs);
                if (shortWindowMs_.size() > 180) {
                    shortWindowMs_.pop_front();
                }

                evalWindowMs_.push_back(dtMs);
                if (evalWindowMs_.size() > 1200) {
                    evalWindowMs_.pop_front();
                }

                // Shift graph history and append latest sample
                std::rotate(graphHistoryMs_.begin(), graphHistoryMs_.begin() + 1, graphHistoryMs_.end());
                graphHistoryMs_.back() = dtMs;
            }
        }
        lastReadIdx_ = writeIdx;

        ComputeMetrics();
        EvaluateAdvisor(shm);
    }

    double GetAvgFps() const { return avgFps_; }
    double GetOnePercentLowFps() const { return onePercentLowFps_; }
    const AdvisorReport& GetReport() const { return currentReport_; }
    const std::array<float, 180>& GetGraphHistory() const { return graphHistoryMs_; }
    size_t GetSampleCount() const { return evalWindowMs_.size(); }

private:
    void ComputeMetrics() {
        // 1. Avg FPS over shortWindowMs_ (at least 15 samples)
        if (shortWindowMs_.size() >= 15) {
            double sumMs = 0.0;
            for (float dt : shortWindowMs_) {
                sumMs += dt;
            }
            const double meanDt = sumMs / static_cast<double>(shortWindowMs_.size());
            avgFps_ = (meanDt > 0.001) ? (1000.0 / meanDt) : 0.0;
        } else {
            avgFps_ = 0.0;
        }

        // 2. 1% Low FPS over evalWindowMs_ (at least 60 samples)
        if (evalWindowMs_.size() >= 60) {
            scratch_.assign(evalWindowMs_.begin(), evalWindowMs_.end());
            const size_t k = std::max<size_t>(1, evalWindowMs_.size() / 100);
            std::partial_sort(scratch_.begin(), scratch_.begin() + k, scratch_.end(), std::greater<float>());

            double sumWorstMs = 0.0;
            for (size_t i = 0; i < k; ++i) {
                sumWorstMs += scratch_[i];
            }
            const double meanWorstDt = sumWorstMs / static_cast<double>(k);
            onePercentLowFps_ = (meanWorstDt > 0.001) ? (1000.0 / meanWorstDt) : 0.0;
        } else {
            onePercentLowFps_ = (avgFps_ > 0.0) ? avgFps_ : 0.0;
        }
    }

    void EvaluateAdvisor(const SharedState& shm) {
        AdvisorReport rawReport;

        // Estado 0 — Sin sesión
        if (shm.activePid == 0 || shm.rawPanelHz < 10.0) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Idle);
            swprintf_s(rawReport.summaryLine, L"Advisor: Waiting for VR session...");
            ApplyReportImmediate(rawReport);
            return;
        }

        // Estado 1 — Recopilando datos (< 240 muestras)
        if (evalWindowMs_.size() < 240) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Collecting);
            const uint32_t pct = static_cast<uint32_t>((evalWindowMs_.size() * 100) / 240);
            swprintf_s(rawReport.summaryLine, L"Advisor: Collecting gameplay data... (%u%%)", pct);
            ApplyReportImmediate(rawReport);
            return;
        }

        // Estado 2 — Objetivo Actual Sostenido
        const double targetCap = (shm.appliedCap > 5.0) ? shm.appliedCap : ((shm.effectiveHz > 10.0) ? shm.effectiveHz : 72.0);
        const float budgetMs = static_cast<float>(1000.0 / targetCap) + 0.65f;

        size_t metBudgetCount = 0;
        for (float dt : evalWindowMs_) {
            if (dt <= budgetMs) {
                metBudgetCount++;
            }
        }
        const double budgetRatio = static_cast<double>(metBudgetCount) / static_cast<double>(evalWindowMs_.size());

        if (budgetRatio >= 0.95 && avgFps_ >= (targetCap - 1.2)) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Sustained);
            if (shm.sswActive != 0 || shm.frameGenDivisor > 1.05) {
                const double displayHz = (shm.rawPanelHz > 10.0) ? shm.rawPanelHz : (shm.effectiveHz * 2.0);
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Target Sustained \u2014 Flat %.1f FPS at %.0f Hz (SSW / Half-Rate)",
                           avgFps_, displayHz);
            } else {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Target Sustained \u2014 Flat %.1f FPS at %.0f Hz (Native)",
                           avgFps_, shm.effectiveHz);
            }
        } else {
            // Estado 3 — Objetivo Actual NO Sostenido (Jerarquía estricta)
            rawReport.state = static_cast<int32_t>(AdvisorState::NotSustained);

            constexpr double kPanelModes[] = { 144.0, 120.0, 100.0, 96.0, 90.0, 80.0, 72.0 };
            double bestNativeHz = 0.0;
            for (double modeHz : kPanelModes) {
                const double fCap = modeHz - 0.1;
                if (avgFps_ >= (fCap + 2.0) && onePercentLowFps_ >= (fCap * 0.86)) {
                    bestNativeHz = modeHz;
                    break;
                }
            }

            if (bestNativeHz >= 72.0) {
                // Caso 3A — Sostiene algún modo Nativo >= 72 Hz
                const double suggestedCap = bestNativeHz - 0.1;
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Try %.0f Hz Native (%.1f FPS) | Or 120 Hz + SSW Always On (59.9 FPS)",
                           bestNativeHz, suggestedCap);
            } else if (avgFps_ >= 60.0) {
                // Caso 3B — No llega a 72 Hz nativos, pero sostiene >= 60 FPS
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Try 120 Hz + SSW Always On (59.9 FPS), or lower res for 72 Hz Native");
            } else {
                // Caso 3C — Juegos pesados por debajo de 60 FPS
                struct SswStep {
                    double panelHz;
                    double fSsw;
                };
                constexpr SswStep kSswSteps[] = {
                    { 100.0, 49.9 },
                    {  96.0, 47.9 },
                    {  90.0, 44.9 },
                    {  80.0, 39.9 },
                    {  72.0, 35.9 }
                };

                double bestSswPanelHz = 0.0;
                double bestSswCap = 0.0;
                for (const auto& step : kSswSteps) {
                    if (avgFps_ >= (step.fSsw + 1.5) && onePercentLowFps_ >= (step.fSsw * 0.84)) {
                        bestSswPanelHz = step.panelHz;
                        bestSswCap = step.fSsw;
                        break;
                    }
                }

                if (bestSswPanelHz >= 72.0) {
                    swprintf_s(rawReport.summaryLine,
                               L"Advisor: Try %.0f Hz + SSW Always On (%.1f FPS cap)",
                               bestSswPanelHz, bestSswCap);
                } else {
                    // Caso 3D — Por debajo de 36 FPS
                    swprintf_s(rawReport.summaryLine,
                               L"Advisor: Below 36 FPS SSW floor \u2014 Reduce resolution or graphics settings");
                }
            }
        }

        ApplyReportWithHysteresis(rawReport);
    }

    void ApplyReportImmediate(const AdvisorReport& report) {
        currentReport_ = report;
        pendingTicks_ = 0;
        pendingState_ = report.state;
        wcscpy_s(pendingSummary_, report.summaryLine);
    }

    void ApplyReportWithHysteresis(const AdvisorReport& report) {
        // If state and text match current, clear pending
        if (report.state == currentReport_.state && wcscmp(report.summaryLine, currentReport_.summaryLine) == 0) {
            pendingTicks_ = 0;
            return;
        }

        // Check if candidate matches previous pending
        if (report.state == pendingState_ && wcscmp(report.summaryLine, pendingSummary_) == 0) {
            pendingTicks_++;
            // 12 ticks of 250 ms = ~3.0 seconds of stable recommendation
            if (pendingTicks_ >= 12) {
                currentReport_ = report;
                pendingTicks_ = 0;
            }
        } else {
            pendingState_ = report.state;
            wcscpy_s(pendingSummary_, report.summaryLine);
            pendingTicks_ = 1;
        }
    }

    std::deque<float> shortWindowMs_;
    std::deque<float> evalWindowMs_;
    std::array<float, 180> graphHistoryMs_{};
    std::vector<float> scratch_;

    uint32_t lastPid_ = 0;
    double lastRawHz_ = 0.0;
    double lastDivisor_ = 1.0;
    uint32_t lastReadIdx_ = 0;

    double avgFps_ = 0.0;
    double onePercentLowFps_ = 0.0;

    AdvisorReport currentReport_;

    // Hysteresis tracking (12 ticks @ 250ms = 3.0 seconds)
    int32_t pendingTicks_ = 0;
    int32_t pendingState_ = 0;
    wchar_t pendingSummary_[160]{};
};

} // namespace vrpacing
