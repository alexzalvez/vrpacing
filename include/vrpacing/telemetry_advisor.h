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
        // Reinicio automatico de contexto ante cambio de PID, cierre o cambio brusco de Hz
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

            // 1. Purga automatica tras pausas de carga, enganche de shaders o cambio de escena
            if (dtMs > 100.0f) {
                shortWindowMs_.clear();
                evalWindowMs_.clear();
                continue;
            }

            // 2. Muestras validas de juego (2.0 ms a 100.0 ms)
            if (dtMs >= 2.0f) {
                shortWindowMs_.push_back(dtMs);
                if (shortWindowMs_.size() > 180) {
                    shortWindowMs_.pop_front();
                }

                evalWindowMs_.push_back(dtMs);
                if (evalWindowMs_.size() > 300) {
                    evalWindowMs_.pop_front();
                }

                // Rotar buffer de 180 muestras para la grafica de la Fase 4
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
        // 1. Avg FPS sobre ventana rapida (180 muestras / ~2.5 - 3s)
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

        // 2. 1% Low FPS sobre ventana informativa (300 muestras / ~4 - 5s)
        // NOTA DE ARQUITECTURA: onePercentLowFps_ es unicamente informativo para la UI
        // y JAMAS participa en las condiciones de decision del Asesor.
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

        // Estado 0 — Sin sesion
        if (shm.activePid == 0 || shm.rawPanelHz < 10.0) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Idle);
            swprintf_s(rawReport.summaryLine, L"Advisor: Waiting for VR session...");
            ApplyReportImmediate(rawReport);
            return;
        }

        // Estado 1 — Recopilando datos (solo pide 120 muestras validas / ~2s de juego real)
        if (evalWindowMs_.size() < 120) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Collecting);
            const uint32_t pct = static_cast<uint32_t>((evalWindowMs_.size() * 100) / 120);
            swprintf_s(rawReport.summaryLine, L"Advisor: Collecting gameplay data... (%u%%)", pct);
            ApplyReportImmediate(rawReport);
            return;
        }

        // Estado 2 — Objetivo Actual Sostenido (Regla directa basada 100% en Avg FPS)
        if (shm.appliedCap > 0.0 && avgFps_ >= (shm.appliedCap - 1.5)) {
            rawReport.state = static_cast<int32_t>(AdvisorState::Sustained);
            if (shm.sswActive != 0 || shm.frameGenDivisor > 1.05) {
                const double displayHz = (shm.rawPanelHz > 10.0) ? shm.rawPanelHz : (shm.effectiveHz * 2.0);
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Target Sustained — %.1f Avg FPS at %.0f Hz (SSW / Half-Rate)",
                           avgFps_, displayHz);
            } else {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Target Sustained — %.1f Avg FPS at %.0f Hz (Native)",
                           avgFps_, shm.effectiveHz);
            }
        } else {
            // Estado 3 — Objetivo Actual NO Sostenido (Jerarquia basada 100% en Avg FPS)
            rawReport.state = static_cast<int32_t>(AdvisorState::NotSustained);

            // Tramo 1 — avgFps >= 73.0 (Puede sostener un modo Nativo >= 72 Hz)
            if (avgFps_ >= 73.0) {
                constexpr double kPanelModes[] = { 120.0, 100.0, 96.0, 90.0, 80.0, 72.0 };
                double bestNativeHz = 0.0;
                for (double modeHz : kPanelModes) {
                    if (avgFps_ >= (modeHz + 1.0)) {
                        bestNativeHz = modeHz;
                        break;
                    }
                }

                if (bestNativeHz >= 72.0) {
                    const double suggestedCap = bestNativeHz - 0.1;
                    swprintf_s(rawReport.summaryLine,
                               L"Advisor: Try %.0f Hz Native (%.1f FPS) | Or 120 Hz + SSW Always On (59.9 FPS)",
                               bestNativeHz, suggestedCap);
                } else {
                    swprintf_s(rawReport.summaryLine,
                               L"Advisor: Try 72 Hz Native (71.9 FPS) | Or 120 Hz + SSW Always On (59.9 FPS)");
                }
            }
            // Tramo 2 — 61.0 <= avgFps < 73.0 (No llega a 72 Hz nativos, pero supera los 59.9 FPS de 120 Hz SSW)
            else if (avgFps_ >= 61.0) {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Try 120 Hz + SSW Always On (59.9 FPS), or lower res for 72 Hz Native");
            }
            // Tramo 3 — 37.0 <= avgFps < 61.0 (Escalones SSW ordenados de mayor a menor)
            else if (avgFps_ >= 37.0) {
                double sswHz = 72.0;
                double sswCap = 35.9;

                if (avgFps_ >= 51.0) {
                    sswHz = 100.0;
                    sswCap = 49.9;
                } else if (avgFps_ >= 49.0) {
                    sswHz = 96.0;
                    sswCap = 47.9;
                } else if (avgFps_ >= 46.0) {
                    sswHz = 90.0;
                    sswCap = 44.9;
                } else if (avgFps_ >= 41.0) {
                    sswHz = 80.0;
                    sswCap = 39.9;
                } else {
                    sswHz = 72.0;
                    sswCap = 35.9;
                }

                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Try %.0f Hz + SSW Always On (%.1f FPS cap)",
                           sswHz, sswCap);
            }
            // Tramo 4 — avgFps < 37.0 (Por debajo del suelo de 36 FPS SSW)
            else {
                swprintf_s(rawReport.summaryLine,
                           L"Advisor: Below 36 FPS SSW floor — Reduce resolution or graphics settings");
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
        // Si el estado y el texto coinciden con el actual, no hay cambio pendiente
        if (report.state == currentReport_.state && wcscmp(report.summaryLine, currentReport_.summaryLine) == 0) {
            pendingTicks_ = 0;
            return;
        }

        // Verificar si el candidato coincide con el cambio pendiente
        if (report.state == pendingState_ && wcscmp(report.summaryLine, pendingSummary_) == 0) {
            pendingTicks_++;
            // 12 ticks de 250 ms = ~3.0 segundos de estabilidad antes de cambiar texto
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

    // Variables de histeresis para evitar parpadeos visuales
    uint32_t pendingTicks_ = 0;
    int32_t pendingState_ = static_cast<int32_t>(AdvisorState::Idle);
    wchar_t pendingSummary_[160]{};
};

} // namespace vrpacing
