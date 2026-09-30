// Arbitrary-ratio resampler for pairs of floats (complex IQ or stereo audio).
//
// A rational resampler (a polyphase bank of out / gcd(in, out) phases) is no
// use for nrsc5's input: nrsc5 wants 46511.71875 Hz on AM, which shares almost
// no factors with any receiver's rate. So this interpolates between a fixed
// number of phases of a windowed-sinc prototype instead, which takes any ratio
// at a cost of 2 * taps multiply-adds per output per channel.
//
// Each output y(t) = sum_j x[j] g(t - j), where g is a Kaiser-windowed sinc
// that spans `taps` input samples, tabulated at PHASES points per sample and
// linearly interpolated between them.
#pragma once
#include <cmath>
#include <vector>

class PairResampler {
public:
    // cutoff is in Hz. By default it sits just below the lower Nyquist rate.
    void init(double inRate, double outRate, int taps = 32, double cutoff = 0) {
        _taps = taps;
        _step = inRate / outRate;
        if (cutoff <= 0) { cutoff = 0.45 * std::min(inRate, outRate); }
        double fc = cutoff / inRate; // cycles per input sample

        // g(d) tabulated at d = m / PHASES - taps / 2, m = 0 .. taps * PHASES.
        const int len = _taps * PHASES + 1;
        std::vector<double> g(len);
        const double beta = 8.0;
        const double i0b = bessel_i0(beta);
        const double half = _taps / 2.0;
        for (int m = 0; m < len; m++) {
            double d = (double)m / PHASES - half;
            double x = d / half;
            // Pinned to zero at both ends, so that the last phase of one
            // sample and the first of the next are the same set of taps.
            double w = (std::fabs(x) < 1.0) ? (bessel_i0(beta * std::sqrt(1.0 - x * x)) - 1.0) / (i0b - 1.0) : 0.0;
            double s = (d == 0.0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * d) / (M_PI * d);
            g[m] = s * w;
        }

        // Row p holds the taps for a fractional position of p / PHASES, in
        // the order of the input samples they multiply. Each row is scaled to
        // unity gain at DC, so the interpolation between rows adds no ripple.
        _bank.assign((PHASES + 1) * _taps, 0.0f);
        for (int p = 0; p <= PHASES; p++) {
            double sum = 0;
            for (int k = 0; k < _taps; k++) { sum += g[p + (_taps - 1 - k) * PHASES]; }
            for (int k = 0; k < _taps; k++) { _bank[p * _taps + k] = (float)(g[p + (_taps - 1 - k) * PHASES] / sum); }
        }
        reset();
    }

    void reset() {
        _hist.assign(2 * _taps, 0.0f);
        _pos = _taps / 2 - 1;
    }

    // Consumes `count` pairs from `in`, appends the outputs to `out` (as
    // pairs) and returns how many it appended.
    int process(const float* in, int count, std::vector<float>& out) {
        _hist.insert(_hist.end(), in, in + 2 * count);
        const int avail = (int)(_hist.size() / 2);
        const int half = _taps / 2;
        int produced = 0;
        for (;;) {
            int i0 = (int)_pos;
            if (i0 + half >= avail) { break; }
            double ph = (_pos - i0) * PHASES;
            int p = (int)ph;
            float a = (float)(ph - p);
            const float* h0 = &_bank[p * _taps];
            const float* h1 = h0 + _taps;
            const float* x = &_hist[2 * (i0 - half + 1)];
            float r0 = 0, i0s = 0, r1 = 0, i1s = 0;
            for (int k = 0; k < _taps; k++) {
                r0 += x[2 * k] * h0[k];
                i0s += x[2 * k + 1] * h0[k];
                r1 += x[2 * k] * h1[k];
                i1s += x[2 * k + 1] * h1[k];
            }
            out.push_back(r0 + a * (r1 - r0));
            out.push_back(i0s + a * (i1s - i0s));
            produced++;
            _pos += _step;
        }
        // Drop the input no future output can reach.
        int drop = (int)_pos - half + 1;
        if (drop > 0) {
            if (drop > avail) { drop = avail; }
            _hist.erase(_hist.begin(), _hist.begin() + 2 * drop);
            _pos -= drop;
        }
        return produced;
    }

private:
    static constexpr int PHASES = 128;

    static double bessel_i0(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 50; k++) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
            if (term < 1e-12 * sum) { break; }
        }
        return sum;
    }

    int _taps = 32;
    double _step = 1;
    double _pos = 0;
    std::vector<float> _bank;
    std::vector<float> _hist;
};
