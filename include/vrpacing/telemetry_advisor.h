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
        scratch_.reserve(300);
        graphHistoryMs_.fill(0.0f);
        Reset();
    }

    static void ResolveDisplayMode(const SharedState& shm, bool& outIsSsw, double& outTargetHz, double& outPanelHz) {
        const bool manualHalf = (shm.frameGenDivisor > 1.05);
        const bool reportedLowHz = (shm.effectiveHz > 10.0 && shm.effectiveHz <= 60.5);
        outIsSsw = (shm.sswActive != 0) || manualHalf || reportedLowHz;

        if (outIsSsw) {
            if (manualHalf && shm.effectiveHz > 60.5) {
                outTargetHz = shm.effectiveHz * 0.5;
                outPanelHz = shm.effectiveHz;
            } else {
                outTargetHz = shm.effectiveHz;
                outPanelHz = (shm.rawPanelHz > 60.5) ? shm.rawPanelHz : (shm.effectiveHz * 2.0);
            }
        } else {
            outTargetHz = shm.effectiveHz;
            outPanelHz = (shm.rawPanelHz > 60.5) ? shm.rawPanelHz : shm.effectiveHz;
        }
    }

    void Reset() {
        shortWindowMs_.clear();
        evalWindowMs_.clear();
        graphHistoryMs_.fill(0.0f);
        lastPid_ = 0;
        lastAppliedCap_ = 0.0;
        lastDivisor_ = 1.0;
        lastReadIdx_ = 0;
        avgFps_ = 0.0;
        onePercentLowFps_ = 0.0;
        initialCollectionDone_ = false;
        hasLatchedVerdict_ = false;
        pendingTicks_ = 0;
        pendingState_ = static_cast<int32_t>(AdvisorState::Idle);
        pendingSummary_[0] = L'\0';

        currentReport_.state = static_cast<int32_t>(AdvisorState::Idle);
        wcscpy_s(currentReport_.summaryLine, L"Advisor: Waiting for VR session...");
        currentReport_.detailLine[0] = L'\0';
    }

    void Ingest(const SharedState& shm) {
        if (shm.activePid == 0 || shm.activePid != lastPid_) {
            const uint32_t newPid = shm.activePid;
            const uint32_t syncIdx = shm.frameTimeWriteIdx % 256u;
            Reset();
            lastPid_ = newPid;
            lastAppliedCap_ = shm.appliedCap;
            lastDivisor_ = shm.frameGenDivisor;
            lastReadIdx_ = syncIdx;
            if (newPid == 0) {
                return;
            }
        }

        // Si cambia el limite aplicado (cambio de Hz en el visor o cambio 1x / 1/2x),
        // reevaluar suavemente sin volver a mostrar la barra de Collecting
        if (shm.appliedCap > 0.0 && std::abs(shm.appliedCap - lastAppliedCap_) > 0.5) {
            lastAppliedCap_ = shm.appliedCap;
            lastDivisor_ = shm.frameGenDivisor;
            shortWindowMs_.clear();
            hasLatchedVerdict_ = false;
            pendingTicks_ = 0;
        }

        const uint32_t writeIdx = shm.frameTimeWriteIdx % 256u;
        const uint32_t count = (writeIdx >= lastReadIdx_)
                                   ? (writeIdx - lastReadIdx_)
                                   : (256u + writeIdx - lastReadIdx_);
        const uint32_t samplesToRead = std::min<uint32_t>(count, 256u);

        for (uint32_t i = 0; i < samplesToRead; ++i) {
            const uint32_t idx = (lastReadIdx_ + i) % 256u;
            const float dtMs = shm.frameTimesMs[idx];

            // Ignorar tirones puntuales de carga (>100 ms) sin vaciar las ventanas
            if (dtMs < 2.0f || dtMs > 100.0f) {
                continue;
            }

            shortWindowMs_.push_back(dtMs);
            if (shortWindowMs_.size() > 180) {
                shortWindowMs_.pop_front();
            }

            evalWindowMs_.push_back(dtMs);
            if (evalWindowMs_.size() > 300) {
                evalWindowMs_.pop_front();
            }

            std::rotate(graphHistoryMs_.begin(), graphHistoryMs_.begin() + 1, graphHistoryMs_.end());
            graphHistoryMs_.back() = dtMs;
        }
        lastReadIdx_ = writeIdx;

        if (evalWindowMs_.size() >= 90) {
            initialCollectionDone_ = true;
        }

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

        if (shm.activePid == 0 || shm.effectiveHz < 10.0) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Idle);
            swprintf_s(rawReport.summaryLine, L"Advisor: Waiting for VR session...");
            ApplyReportImmediate(rawReport);
            return;
        }

        if (!initialCollectionDone_ || shortWindowMs_.size() < 30) {
            if (!hasLatchedVerdict_) {
                rawReport.state = static_cast<int32_t>(AdvisorState::Collecting);
                const uint32_t pct = static_cast<uint32_t>(std::min<size_t>(99, (evalWindowMs_.size() * 100) / 90));
                swprintf_s(rawReport.summaryLine, L"Advisor: Measuring frame pacing... (%u%%)", pct);
                ApplyReportImmediate(rawReport);
            }
            return;
        }

        bool isSsw = false;
        double targetHz = shm.effectiveHz;
        double panelHz = shm.rawPanelHz;
        ResolveDisplayMode(shm, isSsw, targetHz, panelHz);

        const double cap = (shm.appliedCap > 0.0) ? shm.appliedCap : (targetHz - 0.10);

        // Banda muerta: si ya estaba en Sustained, se mantiene hasta que Avg FPS caiga mas de 3.0 FPS por debajo del cap
        const bool wasSustained = hasLatchedVerdict_ && (currentReport_.state == static_cast<int32_t>(AdvisorState::Sustained));
        const double sustainThreshold = wasSustained ? (cap - 3.0) : (cap - 1.5);

        if (cap > 0.0 && avgFps_ >= sustainThreshold) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Sustained);
            if (isSsw) {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Target Sustained | %.2f FPS Cap (%.0f/%.0f Hz SSW)",
                           cap, targetHz, panelHz);
            } else {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Target Sustained | %.2f FPS Cap (%.0f Hz Native)",
                           cap, panelHz);
            }
        } else {
            rawReport.state = static_cast<int32_t>(AdvisorState::NotSustained);

            if (avgFps_ >= 73.0) {
                constexpr double kPanelModes[] = { 120.0, 100.0, 96.0, 90.0, 80.0, 72.0 };
                double bestNativeHz = 72.0;
                for (double modeHz : kPanelModes) {
                    if (avgFps_ >= (modeHz + 1.0)) {
                        bestNativeHz = modeHz;
                        break;
                    }
                }
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Try %.0f Hz Native (%.1f FPS) | Or 60/120 Hz SSW (59.9 FPS)",
                           bestNativeHz, bestNativeHz - 0.1);
            } else if (avgFps_ >= 61.0) {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Try 60/120 Hz SSW (59.9 FPS) | Or lower res for 72 Hz");
            } else if (avgFps_ >= 37.0) {
                double sswTarget = 36.0;
                double sswPanel = 72.0;
                double sswCap = 35.9;

                if (avgFps_ >= 51.0) {
                    sswTarget = 50.0; sswPanel = 100.0; sswCap = 49.9;
                } else if (avgFps_ >= 49.0) {
                    sswTarget = 48.0; sswPanel = 96.0; sswCap = 47.9;
                } else if (avgFps_ >= 46.0) {
                    sswTarget = 45.0; sswPanel = 90.0; sswCap = 44.9;
                } else if (avgFps_ >= 41.0) {
                    sswTarget = 40.0; sswPanel = 80.0; sswCap = 39.9;
                }

                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Try %.0f/%.0f Hz SSW Always On (%.1f FPS cap)",
                           sswTarget, sswPanel, sswCap);
            } else {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Below 36 FPS SSW floor - Reduce resolution or settings");
            }
        }

        if (!hasLatchedVerdict_) {
            hasLatchedVerdict_ = true;
            ApplyReportImmediate(rawReport);
        } else {
            ApplyReportWithHysteresis(rawReport);
        }
    }

    void ApplyReportImmediate(const AdvisorReport& report) {
        currentReport_ = report;
        pendingTicks_ = 0;
        pendingState_ = report.state;
        wcscpy_s(pendingSummary_, report.summaryLine);
    }

    void ApplyReportWithHysteresis(const AdvisorReport& report) {
        if (report.state == currentReport_.state && wcscmp(report.summaryLine, currentReport_.summaryLine) == 0) {
            pendingTicks_ = 0;
            return;
        }

        if (report.state == pendingState_ && wcscmp(report.summaryLine, pendingSummary_) == 0) {
            pendingTicks_++;
            // 20 ticks de 250 ms = 5.0 segundos continuos antes de cambiar el veredicto enclavado
            if (pendingTicks_ >= 20) {
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
    double lastAppliedCap_ = 0.0;
    double lastDivisor_ = 1.0;
    uint32_t lastReadIdx_ = 0;

    double avgFps_ = 0.0;
    double onePercentLowFps_ = 0.0;
    bool initialCollectionDone_ = false;
    bool hasLatchedVerdict_ = false;

    AdvisorReport currentReport_;

    uint32_t pendingTicks_ = 0;
    int32_t pendingState_ = static_cast<int32_t>(AdvisorState::Idle);
    wchar_t pendingSummary_[160]{};
};

} // namespace vrpacing
