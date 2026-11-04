#include "nopeaks/Optimizer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nopeaks {
namespace {
constexpr std::size_t maxTracks = 64;
constexpr std::size_t maxSamples = 128;
constexpr double tiny = 1.0e-18;

double finite(double value, double replacement = 0.0) noexcept {
    return std::isfinite(value) ? value : replacement;
}
double positive(double value) noexcept { return std::max(0.0, finite(value)); }
double weight(float priority) noexcept {
    const auto p = std::clamp(finite(priority, 0.5), 0.0, 1.0);
    return 1.0 + 99.0 * p * p;
}
double sample(const PeakTrackView& track, std::size_t n, bool right) noexcept {
    const auto* data = right && track.right ? track.right : track.left;
    return data ? std::clamp(finite(data[n]), -1.0e6, 1.0e6) : 0.0;
}
double ratioDb(double numerator, double denominator) noexcept {
    if (numerator <= tiny && denominator <= tiny) return 0.0;
    return 10.0 * std::log10(std::max(tiny, numerator) / std::max(tiny, denominator));
}

// Convex water-filling in retained POWER, not retained amplitude:
// min sum w_i(1-r_i)^2, sum P_i*r_i <= target, floor_i <= r_i <= 1.
// The factor two in the Lagrange multiplier is absorbed into lambda.
double allocatePower(const BandTrack* tracks, std::size_t count,
                     const std::array<double, maxTracks>& powers,
                     double target, double floor,
                     std::array<double, maxTracks>& retained) noexcept {
    double total = 0.0, minimum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        retained[i] = 1.0;
        total += powers[i];
        minimum += powers[i] * (tracks[i].protect ? 1.0 : floor);
    }
    target = std::max(0.0, finite(target));
    if (total <= tiny || target >= total) return total;
    if (target <= minimum + tiny) {
        for (std::size_t i = 0; i < count; ++i)
            retained[i] = tracks[i].protect || powers[i] <= tiny ? 1.0 : floor;
        return minimum;
    }
    const auto at = [&](double lambda) noexcept {
        double sum = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto r = tracks[i].protect || powers[i] <= tiny ? 1.0
                : std::clamp(1.0 - lambda * powers[i] / weight(tracks[i].priority), floor, 1.0);
            sum += powers[i] * r;
        }
        return sum;
    };
    // Normalize the multiplier by the largest power to remain level invariant.
    double largest = tiny;
    for (std::size_t i = 0; i < count; ++i) largest = std::max(largest, powers[i]);
    double low = 0.0, high = 100.0 / largest;
    for (unsigned k = 0; k < 56; ++k) {
        const auto middle = (low + high) * 0.5;
        if (at(middle) > target) low = middle; else high = middle;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        retained[i] = tracks[i].protect || powers[i] <= tiny ? 1.0
            : std::clamp(1.0 - high * powers[i] / weight(tracks[i].priority), floor, 1.0);
        sum += powers[i] * retained[i];
    }
    return sum;
}
} // namespace

PeakResult solvePeaks(const PeakTrackView* tracks, std::size_t count,
                      std::size_t samples, const PeakOptions& options) noexcept {
    PeakResult result;
    result.gains.fill(1.0f);
    if (!tracks) return result;
    count = std::min(count, maxTracks);
    samples = std::min(samples, maxSamples);
    const auto ceiling = std::clamp(finite(options.ceiling, 0.891250938), 1.0e-6, 1.0e6);
    const auto amount = std::clamp(finite(options.amount, 1.0), 0.0, 1.0);
    const auto limit = std::min(options.iterations, 96u);
    std::array<double, maxTracks> z {}, gains {}, floors {}, invWeights {};
    std::array<double, maxSamples * 4> dual {};
    for (std::size_t i = 0; i < count; ++i) {
        gains[i] = z[i] = 1.0;
        floors[i] = tracks[i].protect ? 1.0
            : std::clamp(finite(tracks[i].minimumGain, 0.25), 0.0, 1.0);
        invWeights[i] = tracks[i].protect ? 0.0 : 1.0 / weight(tracks[i].priority);
    }
    const auto peakOf = [&](const std::array<double, maxTracks>& g) noexcept {
        double peak = 0.0;
        for (std::size_t n = 0; n < samples; ++n)
            for (unsigned c = 0; c < 2; ++c) {
                double sum = 0.0;
                for (std::size_t i = 0; i < count; ++i) sum += g[i] * sample(tracks[i], n, c != 0);
                peak = std::max(peak, std::abs(sum));
            }
        return peak;
    };
    result.inputPeak = static_cast<float>(peakOf(gains));
    if (result.inputPeak > ceiling && amount > 0.0) {
        // Dual coordinate ascent for the box-constrained quadratic problem.
        // Each constraint is one signed sample in either stereo channel.
        // z stores unconstrained primal values; gains = clip(z, floor, 1).
        for (unsigned iteration = 0; iteration < limit; ++iteration) {
            double bestScore = 0.0;
            std::size_t chosen = 0;
            for (std::size_t n = 0; n < samples; ++n) {
                for (unsigned c = 0; c < 2; ++c) {
                    double sum = 0.0, norm = 0.0;
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto x = sample(tracks[i], n, c != 0);
                        sum += gains[i] * x;
                        norm += x * x * invWeights[i];
                    }
                    for (unsigned s = 0; s < 2; ++s) {
                        const auto index = (n * 2 + c) * 2 + s;
                        const auto residual = (s == 0 ? sum : -sum) - ceiling;
                        const auto violation = dual[index] > 1.0e-12 ? std::abs(residual)
                                                                 : std::max(0.0, residual);
                        const auto score = violation / std::sqrt(std::max(norm, 1.0e-18));
                        if (score > bestScore) { bestScore = score; chosen = index; }
                    }
                }
            }
            if (bestScore <= 1.0e-7) break;
            const auto n = chosen / 4;
            const auto right = (chosen / 2) % 2 != 0;
            const auto sign = chosen % 2 == 0 ? 1.0 : -1.0;
            const auto evaluate = [&](double delta) noexcept {
                double sum = 0.0;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto a = sign * sample(tracks[i], n, right);
                    sum += a * std::clamp(z[i] - delta * a * invWeights[i], floors[i], 1.0);
                }
                return sum - ceiling;
            };
            double low = -dual[chosen], high = std::max(1.0, low + 1.0);
            double delta = low;
            if (evaluate(low) > 0.0) {
                // Bound expansion; an infeasible protected/floor constraint
                // eventually saturates without unbounded numerical state.
                for (unsigned k = 0; k < 40 && evaluate(high) > 0.0; ++k)
                    high = std::min(1.0e12, high * 2.0 + 1.0);
                if (evaluate(high) > 1.0e-7) {
                    for (std::size_t i = 0; i < count; ++i) {
                        if (tracks[i].protect) continue;
                        const auto a = sign * sample(tracks[i], n, right);
                        if (a > 0.0) gains[i] = floors[i];
                        else if (a < 0.0) gains[i] = 1.0;
                    }
                    result.iterations = iteration + 1;
                    break;
                }
                for (unsigned k = 0; k < 40; ++k) {
                    const auto middle = (low + high) * 0.5;
                    if (evaluate(middle) > 0.0) low = middle; else high = middle;
                }
                delta = high;
            }
            dual[chosen] = std::max(0.0, dual[chosen] + delta);
            for (std::size_t i = 0; i < count; ++i) {
                z[i] -= delta * sign * sample(tracks[i], n, right) * invWeights[i];
                gains[i] = std::clamp(z[i], floors[i], 1.0);
            }
            result.iterations = iteration + 1;
        }
        // A bounded iteration count may stop before feasibility. A common
        // additional reduction of FREE stems is permitted only when the actual
        // signed constraints admit it, so cancellation is never blindly removed.
        if (peakOf(gains) > ceiling + 1.0e-7) {
            double lower = 0.0, upper = 1.0;
            for (std::size_t i = 0; i < count; ++i)
                if (!tracks[i].protect && gains[i] > tiny)
                    lower = std::max(lower, floors[i] / gains[i]);
            for (std::size_t n = 0; n < samples; ++n)
                for (unsigned c = 0; c < 2; ++c) {
                    double protectedSum = 0.0, freeSum = 0.0;
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto x = gains[i] * sample(tracks[i], n, c != 0);
                        if (tracks[i].protect) protectedSum += x; else freeSum += x;
                    }
                    if (std::abs(freeSum) <= tiny) {
                        if (std::abs(protectedSum) > ceiling) upper = -1.0;
                    } else {
                        const auto a = (-ceiling - protectedSum) / freeSum;
                        const auto b = (ceiling - protectedSum) / freeSum;
                        lower = std::max(lower, std::min(a, b));
                        upper = std::min(upper, std::max(a, b));
                    }
                }
            if (upper >= lower && upper >= 0.0) {
                const auto factor = std::clamp(upper, 0.0, 1.0);
                for (std::size_t i = 0; i < count; ++i)
                    if (!tracks[i].protect) gains[i] = std::max(floors[i], gains[i] * factor);
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        gains[i] = tracks[i].protect ? 1.0 : 1.0 - amount * (1.0 - gains[i]);
        result.gains[i] = static_cast<float>(std::clamp(gains[i], floors[i], 1.0));
        gains[i] = result.gains[i];
    }
    result.predictedPeak = static_cast<float>(peakOf(gains));
    result.residual = static_cast<float>(std::max(0.0, result.predictedPeak - ceiling));
    result.feasible = result.residual <= static_cast<float>(std::max(2.0e-6, ceiling * 2.0e-6));
    return result;
}

BandResult solveTone(const BandTrack* tracks, std::size_t count,
                     const std::array<float, 12>& targetDeltaDb, float maxCutDb) noexcept {
    BandResult result;
    if (!tracks) return result;
    count = std::min(count, maxTracks);
    const auto cut = std::clamp(finite(maxCutDb, 6.0), 0.0, 6.0);
    const auto floor = std::pow(10.0, -cut / 10.0);
    for (std::size_t b = 0; b < 12; ++b) {
        std::array<double, maxTracks> powers {}, retained {};
        double total = 0.0;
        for (std::size_t i = 0; i < count; ++i) total += powers[i] = positive(tracks[i].power[b]);
        const auto delta = std::clamp(finite(targetDeltaDb[b]), -60.0, 0.0);
        const auto target = total * std::pow(10.0, delta / 10.0);
        const auto predicted = allocatePower(tracks, count, powers, target, floor, retained);
        for (std::size_t i = 0; i < count; ++i)
            result.gainsDb[i][b] = static_cast<float>(10.0 * std::log10(std::max(tiny, retained[i])));
        result.predictedPower[b] = static_cast<float>(predicted);
        result.residualDb[b] = static_cast<float>(std::max(0.0, ratioDb(predicted, target)));
    }
    return result;
}

BandResult solveWidth(const BandTrack* tracks, std::size_t count,
                      const std::array<float, 12>& masterMidPower,
                      const std::array<float, 12>& masterSidePower,
                      float minimumCorrelation, float maxCutDb) noexcept {
    BandResult result;
    if (!tracks) return result;
    count = std::min(count, maxTracks);
    const auto rho = std::clamp(finite(minimumCorrelation), 0.0, 0.95);
    const auto cut = std::clamp(finite(maxCutDb, 18.0), 0.0, 18.0);
    const auto floor = std::pow(10.0, -cut / 10.0);
    for (std::size_t b = 0; b < 12; ++b) {
        const auto mid = positive(masterMidPower[b]);
        const auto side = positive(masterSidePower[b]);
        result.predictedPower[b] = static_cast<float>(side);
        const auto allowed = mid * (1.0 - rho) / (1.0 + rho);
        if (side <= tiny || side <= allowed) continue;
        std::array<double, maxTracks> powers {}, retained {};
        double total = 0.0;
        for (std::size_t i = 0; i < count; ++i) total += powers[i] = positive(tracks[i].sidePower[b]);
        if (total <= tiny) {
            result.residualDb[b] = static_cast<float>(std::max(0.0, ratioDb(side, std::max(tiny, allowed))));
            continue;
        }
        const auto target = total * std::clamp(allowed / side, 0.0, 1.0);
        const auto predictedStemPower = allocatePower(tracks, count, powers, target, floor, retained);
        const auto predictedSide = side * predictedStemPower / total;
        for (std::size_t i = 0; i < count; ++i)
            result.gainsDb[i][b] = static_cast<float>(10.0 * std::log10(std::max(tiny, retained[i])));
        result.predictedPower[b] = static_cast<float>(predictedSide);
        result.residualDb[b] = static_cast<float>(std::max(0.0, ratioDb(predictedSide, std::max(tiny, allowed))));
    }
    return result;
}

BandResult conflictCuts(const BandTrack* tracks, std::size_t count,
                        float amount, float maxCutDb) noexcept {
    BandResult result;
    if (!tracks) return result;
    count = std::min(count, maxTracks);
    const auto strength = std::clamp(finite(amount), 0.0, 1.0);
    const auto cut = std::clamp(finite(maxCutDb, 3.0), 0.0, 3.0);
    for (std::size_t b = 0; b < 12; ++b) {
        double largest = 0.0;
        for (std::size_t i = 0; i < count; ++i) largest = std::max(largest, positive(tracks[i].power[b]));
        for (std::size_t i = 0; i < count; ++i) {
            const auto own = positive(tracks[i].power[b]);
            double higher = 0.0;
            const auto p = std::clamp(finite(tracks[i].priority, 0.5), 0.0, 1.0);
            if (!tracks[i].protect && own > std::max(tiny, largest * 1.0e-4)) {
                for (std::size_t j = 0; j < count; ++j) {
                    const auto q = std::clamp(finite(tracks[j].priority, 0.5), 0.0, 1.0);
                    if (q > p + 1.0e-3) higher += positive(tracks[j].power[b]) * (q - p);
                }
                // Symmetric overlap ratio is bounded: one dominant source alone
                // never makes arbitrarily deep cuts in a negligible quiet source.
                const auto overlap = higher <= tiny ? 0.0 : 2.0 * std::min(own, higher) / (own + higher);
                result.gainsDb[i][b] = static_cast<float>(-strength * cut * overlap);
            }
            result.predictedPower[b] += static_cast<float>(own * std::pow(10.0, result.gainsDb[i][b] / 10.0));
        }
    }
    return result;
}

RoutingMatch routingMatch(const float* predictedLeft, const float* predictedRight,
                          const float* observedLeft, const float* observedRight,
                          std::size_t samples, float maximumResidual) noexcept {
    RoutingMatch result;
    if (!predictedLeft || !observedLeft || samples < 16) return result;
    double pp = 0.0, oo = 0.0, po = 0.0;
    std::array<double, 2> sumP {}, sumO {};
    const auto* pr = predictedRight ? predictedRight : predictedLeft;
    const auto* oright = observedRight ? observedRight : observedLeft;
    for (std::size_t n = 0; n < samples; ++n)
        for (unsigned c = 0; c < 2; ++c) {
            const auto p = finite(c == 0 ? predictedLeft[n] : pr[n]);
            const auto o = finite(c == 0 ? observedLeft[n] : oright[n]);
            pp += p * p; oo += o * o; po += p * o; sumP[c] += p; sumO[c] += o;
        }
    const auto size = static_cast<double>(samples) * 2.0;
    if (pp <= size * 1.0e-12 || oo <= size * 1.0e-12) return result;
    const auto n = static_cast<double>(samples);
    const auto varianceP = pp - (sumP[0] * sumP[0] + sumP[1] * sumP[1]) / n;
    const auto varianceO = oo - (sumO[0] * sumO[0] + sumO[1] * sumO[1]) / n;
    if (varianceP <= pp * 1.0e-5 || varianceO <= oo * 1.0e-5) return result;
    const auto gain = po / pp;
    double error = 0.0;
    for (std::size_t n = 0; n < samples; ++n)
        for (unsigned c = 0; c < 2; ++c) {
            const auto p = finite(c == 0 ? predictedLeft[n] : pr[n]);
            const auto o = finite(c == 0 ? observedLeft[n] : oright[n]);
            const auto e = o - gain * p;
            error += e * e;
        }
    result.gain = static_cast<float>(gain);
    result.normalizedResidual = static_cast<float>(std::sqrt(error / oo));
    const auto limit = std::clamp(finite(maximumResidual, 0.08), 0.0, 0.5);
    result.trusted = gain >= 0.9 && gain <= 1.1 && result.normalizedResidual <= limit;
    return result;
}
} // namespace nopeaks
