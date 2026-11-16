#pragma once
// THE MASTER - the correction chain behind AUTO FIX.
//
//   HUM NOTCHES (50/60 Hz + 2 harmonics)
//   TONE MATCH EQ (10 bands, same bands the analyser measures)
//   LEVELER (slow rider that lifts quiet sections, for big level swings)
//   PEAK TAMER (look-ahead limiting of peaks that stick out of the mix, threshold follows the music)
//   STEREO (width above 120 Hz, BASS MONO below)
//   LOUDNESS (gain into a look-ahead true-peak limiter at the ceiling)
//   MATCH (optional: plays the result back at the input's loudness, for a fair A/B)
//
// Every stage is exactly transparent at its neutral setting, and the latency is the same
// whatever is switched on, so A/B never shifts the timing.
#include "Analyzer.h"

namespace mst
{
struct FixParams
{
    bool on = false;
    float amount = 1.0f;                   // scales EQ, leveler, glue and width moves
    bool match = false;
    bool eqOn = false; std::array<float, kNumBands> eq {};   // dB
    bool dynOn = false; float glue = 0.0f, leveler = 0.0f;   // 0..1
    bool stereoOn = false; float width = 1.0f; bool bassMono = false;
    bool humOn = false; int humHz = 60;
    bool loudOn = false; float gainDb = 0.0f; float ceilingDb = -1.0f;
};

// where each EQ band's filter sits
struct EqBandShape { int kind; double hz, q; };   // kind: 0 low shelf, 1 peak, 2 high shelf
inline const std::array<EqBandShape, kNumBands>& eqShapes()
{
    static const std::array<EqBandShape, kNumBands> s {{
        { 0, 55.0, 0.7 }, { 1, 88.0, 1.3 }, { 1, 175.0, 1.4 }, { 1, 355.0, 1.4 }, { 1, 710.0, 1.4 },
        { 1, 1420.0, 1.4 }, { 1, 2830.0, 1.4 }, { 1, 5650.0, 1.4 }, { 1, 9800.0, 1.7 }, { 2, 13000.0, 0.7 } }};
    return s;
}

struct FBiquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};
    inline float process (float x, int c) noexcept
    {
        const double y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return (float) y;
    }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    void set (double B0, double B1, double B2, double A0, double A1, double A2) { b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0; }
    void design (int kind, double sr, double f, double q, double gainDb = 0.0)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (5.0, sr * 0.45, f) / sr;
        const double cw = std::cos (w), sw = std::sin (w), al = sw / (2.0 * q), sa = 2.0 * std::sqrt (A) * al;
        switch (kind)
        {
            case 0: set (A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa),
                         (A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa); break;
            case 1: set (1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A); break;
            case 2: set (A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa),
                         (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa); break;
            case 3: set (1, -2 * cw, 1, 1 + al, -2 * cw, 1 - al); break;           // notch
            case 4: set ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al); break;   // low-pass
            case 5: set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + al, -2 * cw, 1 - al); break; // high-pass
            default: break;
        }
    }
};

//==============================================================================
// Look-ahead true-peak limiter: 4x-oversampled peak estimate, gain dip that is fully in
// place by the time the peak goes out, smooth program-dependent release.
class TruePeakLimiter
{
public:
    void prepare (double sr, int maxBlock)
    {
        side = std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        side->initProcessing ((size_t) maxBlock);
        sideBuf.setSize (2, maxBlock);
        D = (int) std::ceil (side->getLatencyInSamples());
        L = juce::jlimit (16, 512, (int) std::lround (sr * 0.0025));
        len = juce::nextPowerOfTwo (D + L + 8);
        for (auto& b : x) b.assign ((size_t) len, 0.0f);
        gRing.assign ((size_t) L + 1, 1.0f);
        hRing.assign ((size_t) L, 1.0f);
        hSum = (double) L; pos = 0; gPos = 0; hPos = 0; hr = 1.0f; lastTp = 0.0f; resum = 0;
        relFast = (float) std::exp (-1.0 / (sr * 0.03));
        relSlow = (float) std::exp (-1.0 / (sr * 0.12));
        holdLvl = 1.0f;
        holdCoef = (float) std::exp (-1.0 / (sr * 1.5));
        gr.store (0.0f);
    }
    int latency() const { return D + L; }
    std::atomic<float> gr { 0.0f };   // dB, for the meter

    void process (float* const* ch, int numCh, int n, float thr, bool on)
    {
        for (int c = 0; c < 2; ++c) sideBuf.copyFrom (c, 0, ch[juce::jmin (c, numCh - 1)], n);
        juce::dsp::AudioBlock<float> blk (sideBuf.getArrayOfWritePointers(), 2, (size_t) n);
        auto up = side->processSamplesUp (blk);
        const int mask = len - 1;
        float worst = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            float tp = 0.0f;
            for (int c = 0; c < juce::jmin (2, numCh); ++c)
            {
                const float* u = up.getChannelPointer ((size_t) c) + 4 * i;
                tp = std::max ({ tp, std::abs (u[0]), std::abs (u[1]), std::abs (u[2]), std::abs (u[3]) });
            }
            const float tpw = std::max (tp, lastTp);
            lastTp = tp;
            for (int c = 0; c < numCh; ++c) x[(size_t) c][(size_t) pos] = ch[c][i];

            const float g = on && tpw > thr ? thr / tpw : 1.0f;
            gRing[(size_t) gPos] = g; gPos = (gPos + 1) % (L + 1);
            float h = 1.0f;
            for (auto v : gRing) h = std::min (h, v);
            // release: fast for isolated peaks, slower while it's working constantly (less distortion)
            holdLvl = holdCoef * holdLvl + (1.0f - holdCoef) * std::min (h, 1.0f);
            const float rel = holdLvl < 0.9f ? relSlow : relFast;
            hr = std::min (h, 1.0f + rel * (hr - 1.0f));
            hSum += (double) hr - (double) hRing[(size_t) hPos];
            hRing[(size_t) hPos] = hr; hPos = (hPos + 1) % L;
            const float gain = on ? (float) std::min (1.0, hSum / (double) L) : 1.0f;
            worst = std::min (worst, gain);
            const int outIdx = (pos - D - L) & mask;
            for (int c = 0; c < numCh; ++c) ch[c][i] = x[(size_t) c][(size_t) outIdx] * gain;
            pos = (pos + 1) & mask;
        }
        if (++resum > 64) { resum = 0; hSum = 0; for (auto v : hRing) hSum += v; }
        gr.store (std::max (gr.load(), -20.0f * std::log10 (std::max (1e-4f, worst))));
    }

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> side;
    juce::AudioBuffer<float> sideBuf;
    int D = 0, L = 120, len = 256, pos = 0, gPos = 0, hPos = 0, resum = 0;
    std::array<std::vector<float>, 2> x;
    std::vector<float> gRing, hRing;
    double hSum = 120;
    float hr = 1.0f, relFast = 0.999f, relSlow = 0.9999f, lastTp = 0.0f, holdLvl = 1.0f, holdCoef = 0.9999f;
};

//==============================================================================
class FixChain
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        limiter.prepare (sr, maxBlock);
        for (auto& b : eq) b.reset();
        for (auto& b : notch) b.reset();
        for (auto& v : eqNow) v = 0.0f;
        for (int k = 0; k < 4; ++k) { xo[(size_t) k].design (k < 2 ? 4 : 5, sr, 120.0, 0.70710678); xo[(size_t) k].reset(); }
        stereoMix = 0.0f;
        kw[0].prepare (sr); kw[1].prepare (sr); kwIn[0].prepare (sr); kwIn[1].prepare (sr);
        glueSlow = std::exp (-1.0 / (sr * 3.0));
        levA = std::exp (-1.0 / (sr * 1.0)); levLong = std::exp (-1.0 / (sr * 20.0)); levSmooth = std::exp (-1.0 / (sr * 0.5));
        levAtk = std::exp (-1.0 / (sr * 0.05)); levFast = std::exp (-1.0 / (sr * 0.03));
        tamer.prepare (sr, maxBlock); tameLvl = 0;
        matchA = std::exp (-1.0 / (sr * 2.0));
        levSt = 0; levLt = 0; levGain = 0;
        mIn = mOut = 0; matchGain = 1.0f;
        gainNow = 0.0f; widthNow = 1.0f; bassMonoNow = 0.0f;
        lastHum = -1;
        inL.assign ((size_t) maxBlock, 0.0f); inR.assign ((size_t) maxBlock, 0.0f);
        designEq (true);
    }
    int latency() const { return limiter.latency() + tamer.latency(); }
    float getLimiterGr() { return limiter.gr.exchange (0.0f); }
    float getTamerGr() const { return glueGrMeter.load(); }
    float getLevelerDb() const { return levMeter.load(); }
    float peekLimiterGr() const { return limiter.gr.load(); }

    void setParams (const FixParams& p) { params = p; }

    // l/r: in place. postTap is called with the corrected audio BEFORE the match stage.
    template <typename Tap>
    void process (float* l, float* r, int n, Tap&& postTap)
    {
        const auto& p = params;
        const bool on = p.on;
        const float amt = on ? p.amount : 0.0f;
        if (p.match && on) { std::copy (l, l + n, inL.data()); std::copy (r, r + n, inR.data()); }

        // ---- hum notches
        const int hum = on && p.humOn ? p.humHz : 0;
        if (hum != lastHum)
        {
            for (int k = 0; k < 3; ++k) { notch[(size_t) k].design (3, sr, (double) hum * (k + 1), 18.0 + 6.0 * k, 0); notch[(size_t) k].reset(); }
            lastHum = hum;
        }
        if (hum > 0)
            for (int i = 0; i < n; ++i)
                for (auto& f : notch) { l[i] = f.process (l[i], 0); r[i] = f.process (r[i], 1); }

        // ---- EQ (gains glide so moving a slider never clicks)
        bool eqActive = false;
        for (int b = 0; b < kNumBands; ++b)
        {
            const float target = on && p.eqOn ? juce::jlimit (-12.0f, 12.0f, p.eq[(size_t) b] * amt) : 0.0f;
            eqTarget[(size_t) b] = target;
            if (std::abs (target) > 1e-4f || std::abs (eqNow[(size_t) b]) > 1e-4f) eqActive = true;
        }
        if (eqActive)
        {
            for (int start = 0; start < n; start += 32)
            {
                designEq (false);
                const int end = juce::jmin (n, start + 32);
                for (int b = 0; b < kNumBands; ++b)
                {
                    if (std::abs (eqNow[(size_t) b]) < 1e-4f && std::abs (eqTarget[(size_t) b]) < 1e-4f) continue;
                    auto& f = eq[(size_t) b];
                    for (int i = start; i < end; ++i) { l[i] = f.process (l[i], 0); r[i] = f.process (r[i], 1); }
                }
            }
        }

        // ---- leveler (slow rider: lifts quiet sections toward the song's long-term level)
        const float lev = on && p.dynOn ? p.leveler * amt : 0.0f;
        if (lev > 0.0f || std::abs (levGain) > 1e-3)
        {
            for (int i = 0; i < n; ++i)
            {
                const double ms = 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
                levSt = (ms > levSt ? levAtk : levA) * levSt + (1.0 - (ms > levSt ? levAtk : levA)) * ms;   // rises fast, falls slow
                const double stDb = 10.0 * std::log10 (levSt + 1e-12);
                if (stDb > -50.0) levLt = levLong * levLt + (1.0 - levLong) * levSt;
                double want = 0.0;
                if (lev > 0.0f && stDb > -50.0 && levLt > 1e-10)
                    want = juce::jlimit (-4.0, 6.0, (10.0 * std::log10 (levLt) - stDb) * (double) lev);
                const double k = want < levGain ? levFast : levSmooth;   // turn down at once, lift gently
                levGain = k * levGain + (1.0 - k) * want;
                const float lg = (float) std::pow (10.0, levGain / 20.0);
                l[i] *= lg; r[i] *= lg;
            }
            levMeter.store ((float) levGain);
        }
        else levMeter.store (0.0f);

        // ---- peak tamer: look-ahead limiting of the peaks that stick out of the body of the mix.
        //      The threshold rides the program level, so it works the same on a quiet or a hot mix.
        const float tame = on && p.dynOn ? p.glue * amt : 0.0f;
        double blockMs = 0;
        for (int i = 0; i < n; ++i) blockMs += 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
        blockMs /= (double) juce::jmax (1, n);
        if (10.0 * std::log10 (blockMs + 1e-12) > -60.0)
        {
            const double a = std::pow (glueSlow, (double) n);
            tameLvl = a * tameLvl + (1.0 - a) * blockMs;
        }
        const float lvlDb = (float) (10.0 * std::log10 (tameLvl + 1e-12));
        // peaks allowed above the RMS level: ~17 dB at 0 %, ~8 dB at 100 %
        const float thrDb = juce::jmin (0.0f, lvlDb + 17.0f - 9.0f * tame);
        float* tch[2] = { l, r };
        tamer.process (tch, 2, n, juce::Decibels::decibelsToGain (thrDb), tame > 0.0f && tameLvl > 1e-9);
        glueGrMeter.store (tamer.gr.exchange (0.0f));

        // ---- stereo: width above 120 Hz, mono below. Uses a Linkwitz-Riley split (the mid gets the
        //      same all-pass so the image stays aligned); bypassed entirely when neutral.
        const float wT = on && p.stereoOn ? 1.0f + (p.width - 1.0f) * amt : 1.0f;
        const float bmT = on && p.stereoOn && p.bassMono ? 1.0f : 0.0f;
        const float engT = std::abs (wT - 1.0f) > 1e-4f || bmT > 0.0f ? 1.0f : 0.0f;
        if (engT > 0.0f || stereoMix > 0.0f)
        {
            const float w0 = widthNow, b0 = bassMonoNow, e0 = stereoMix;
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (i + 1) / (float) n;
                const float w = w0 + (wT - w0) * t, bm = b0 + (bmT - b0) * t, e = e0 + (engT - e0) * t;
                const float m = 0.5f * (l[i] + r[i]), s = 0.5f * (l[i] - r[i]);
                const float sLo = xo[1].process (xo[0].process (s, 0), 0), sHi = xo[3].process (xo[2].process (s, 0), 0);
                const float mAp = xo[1].process (xo[0].process (m, 1), 1) + xo[3].process (xo[2].process (m, 1), 1);
                const float s2 = sLo * (1.0f - bm) + sHi * w;
                l[i] = (1.0f - e) * l[i] + e * (mAp + s2);
                r[i] = (1.0f - e) * r[i] + e * (mAp - s2);
            }
            widthNow = wT; bassMonoNow = bmT; stereoMix = engT;
            if (engT == 0.0f) for (auto& f : xo) f.reset();
        }

        // ---- loudness: gain into the true-peak limiter
        const float gT = on && p.loudOn ? p.gainDb : 0.0f;
        const float g0 = juce::Decibels::decibelsToGain (gainNow), g1 = juce::Decibels::decibelsToGain (gT);
        if (std::abs (gT - gainNow) > 1e-5f || std::abs (gainNow) > 1e-5f)
            for (int i = 0; i < n; ++i) { const float g = g0 + (g1 - g0) * (float) (i + 1) / (float) n; l[i] *= g; r[i] *= g; }
        gainNow = gT;
        float* chs[2] = { l, r };
        limiter.process (chs, 2, n, juce::Decibels::decibelsToGain (p.ceilingDb - 0.15f), on && p.loudOn);   // small margin: other meters' true-peak estimates differ a little

        postTap (l, r, n);

        // ---- match: same loudness as the input, for honest A/B listening
        if (p.match && on)
        {
            // the dry copy is not delayed by the limiter look-ahead; at a 2 s time constant that doesn't matter
            for (int i = 0; i < n; ++i)
            {
                const double ki = kwIn[0].process (inL[(size_t) i]), kir = kwIn[1].process (inR[(size_t) i]);
                const double ko = kw[0].process (l[i]), kor = kw[1].process (r[i]);
                mIn = matchA * mIn + (1.0 - matchA) * (ki * ki + kir * kir);
                mOut = matchA * mOut + (1.0 - matchA) * (ko * ko + kor * kor);
                const float want = mOut > 1e-10 ? (float) juce::jlimit (0.06, 2.0, std::sqrt (mIn / mOut)) : 1.0f;
                matchGain += (want - matchGain) * 0.0005f;
                l[i] *= matchGain; r[i] *= matchGain;
            }
        }
        else matchGain = 1.0f;
    }

private:
    void designEq (bool force)
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& now = eqNow[(size_t) b];
            const float tgt = eqTarget[(size_t) b];
            if (! force && std::abs (now - tgt) < 1e-5f) continue;
            now = force ? tgt : now + juce::jlimit (-0.25f, 0.25f, tgt - now);
            if (std::abs (now) < 1e-4f && std::abs (tgt) < 1e-4f) now = 0.0f;
            const auto& s = eqShapes()[(size_t) b];
            eq[(size_t) b].design (s.kind, sr, s.hz, s.q, now);
        }
    }

    double sr = 48000.0;
    FixParams params;
    std::array<FBiquad, kNumBands> eq;
    std::array<float, kNumBands> eqNow {}, eqTarget {};
    std::array<FBiquad, 3> notch;
    int lastHum = -1;
    std::array<FBiquad, 4> xo;   // [0,1] LR4 low-pass, [2,3] LR4 high-pass; channel slot 0 = side, 1 = mid
    float stereoMix = 0.0f;
    double glueSlow = 0, tameLvl = 0;
    TruePeakLimiter tamer;
    double levAtk = 0, levFast = 0, levA = 0, levLong = 0, levSmooth = 0, levSt = 0, levLt = 0, levGain = 0;
    float widthNow = 1.0f, bassMonoNow = 0.0f, gainNow = 0.0f;
    KWeight kw[2], kwIn[2];
    double matchA = 0, mIn = 0, mOut = 0;
    float matchGain = 1.0f;
    TruePeakLimiter limiter;
    std::vector<float> inL, inR;
    std::atomic<float> glueGrMeter { 0.0f }, levMeter { 0.0f };
};
} // namespace mst
