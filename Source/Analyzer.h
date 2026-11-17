#pragma once
// THE MASTER - mix analysis engine.
//
// Shared by the plugin (fed live from the master bus on a background thread) and by the
// admin profile builder (fed whole reference songs from disk), so a profile built from
// reference tracks is measured exactly the way the plugin measures your mix.
//
// What it measures, over all the audio heard since the last reset:
//   * integrated loudness (ITU-R BS.1770 / EBU R128, gated), max short-term loudness and
//     loudness range (LRA, EBU Tech 3342)
//   * true peak (4x oversampled) and sample peak, flat-topped digital clipping
//   * PLR, peak-to-loudness ratio (true peak minus integrated loudness): how much the
//     transients stick out of the body of the mix
//   * tonal balance: energy share of 10 bands from 20 Hz to 20 kHz (long-term spectrum)
//   * stereo: mid/side width, L/R correlation, and correlation below 120 Hz (mono low end)
//   * hum: 50/60 Hz mains hum with its harmonics, looked for in the quiet parts
//     (count-ins, gaps, endings) where a noise gate would help
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>

namespace mst
{
constexpr int kAnalyzerVersion = 1;
constexpr int kNumBands = 10;

struct BandDef { const char* key; const char* label; const char* range; float lo, hi; };

inline const std::array<BandDef, kNumBands>& bandDefs()
{
    static const std::array<BandDef, kNumBands> b {{
        { "sub",      "SUB",       "20-60",   20.0f,    60.0f },
        { "bass",     "BASS",      "60-120",  60.0f,    120.0f },
        { "low",      "LOW",       "120-250", 120.0f,   250.0f },
        { "lowMid",   "LOW MID",   "250-500", 250.0f,   500.0f },
        { "mid",      "MID",       "500-1K",  500.0f,   1000.0f },
        { "upperMid", "HI MID", "1K-2K",   1000.0f,  2000.0f },
        { "presence", "PRESENCE",  "2K-4K",   2000.0f,  4000.0f },
        { "bite",     "BITE",      "4K-8K",   4000.0f,  8000.0f },
        { "sheen",    "SHEEN",     "8K-12K",  8000.0f,  12000.0f },
        { "air",      "AIR",       "12K-20K", 12000.0f, 20000.0f } }};
    return b;
}

// The numbers a genre profile describes (mean + spread of each across reference songs)
enum FeatureId { fLufs = 0, fPlr, fLra, fWidth, fCorr, fLowCorr, fBand0, numFeatures = fBand0 + kNumBands };

inline const char* featureKey (int f)
{
    static const char* k[] = { "lufs", "plr", "lra", "width", "correlation", "lowCorrelation" };
    return f < fBand0 ? k[f] : bandDefs()[(size_t) (f - fBand0)].key;
}

//==============================================================================
struct Features
{
    bool valid = false;
    double seconds = 0;                 // non-silent audio analysed
    float lufs = -100.0f, maxShortTerm = -100.0f, lra = 0.0f;
    float truePeak = -100.0f, samplePeak = -100.0f, plr = 0.0f;
    float width = -40.0f;               // side energy relative to mid energy, dB
    float correlation = 1.0f, lowCorrelation = 1.0f;
    std::array<float, kNumBands> band {};   // dB share of total energy (all bands sum to 0 dB)
    int clipEvents = 0;                 // runs of 3+ samples flat at full scale
    float humDb = 0.0f;                 // how far the hum fundamental sits above its neighbours (dB)
    int humHz = 0;                      // 50 or 60 when hum was found
    float humSeconds = 0.0f, quietSeconds = 0.0f;
    // How much each range jumps around (for The Freak). Measured in ~170 ms windows over the loud parts:
    // swing = how far the range's share of the mix rises in its loudest moments above its typical share (p90 - p50, dB);
    // levelSwing = the same for the range's own level (includes the song's ups and downs). 0 = not measured.
    std::array<float, kNumBands> swing {}, levelSwing {};
    std::array<float, kNumBands> swingSpread {};   // p75 - p25 of the share: big when the range jumps on every beat (drums), small for rare jumps

    float get (int f) const
    {
        switch (f)
        {
            case fLufs: return lufs;
            case fPlr: return plr;
            case fLra: return lra;
            case fWidth: return width;
            case fCorr: return correlation;
            case fLowCorr: return lowCorrelation;
            default: return band[(size_t) juce::jlimit (0, kNumBands - 1, f - fBand0)];
        }
    }
    void set (int f, float v)
    {
        switch (f)
        {
            case fLufs: lufs = v; break;
            case fPlr: plr = v; break;
            case fLra: lra = v; break;
            case fWidth: width = v; break;
            case fCorr: correlation = v; break;
            case fLowCorr: lowCorrelation = v; break;
            default: band[(size_t) juce::jlimit (0, kNumBands - 1, f - fBand0)] = v;
        }
    }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("analyzerVersion", kAnalyzerVersion);
        o->setProperty ("seconds", seconds);
        for (int f = 0; f < numFeatures; ++f) o->setProperty (featureKey (f), (double) get (f));
        o->setProperty ("maxShortTerm", (double) maxShortTerm);
        o->setProperty ("truePeak", (double) truePeak);
        o->setProperty ("samplePeak", (double) samplePeak);
        o->setProperty ("clipEvents", clipEvents);
        o->setProperty ("humDb", (double) humDb);
        o->setProperty ("humHz", humHz);
        o->setProperty ("humSeconds", (double) humSeconds);
        o->setProperty ("quietSeconds", (double) quietSeconds);
        juce::Array<juce::var> sw, lsw;
        for (int b = 0; b < kNumBands; ++b) { sw.add ((double) swing[(size_t) b]); lsw.add ((double) levelSwing[(size_t) b]); }
        o->setProperty ("swing", sw);
        o->setProperty ("levelSwing", lsw);
        juce::Array<juce::var> ssp; for (auto v : swingSpread) ssp.add ((double) v);
        o->setProperty ("swingSpread", ssp);
        return juce::var (o);
    }
    static Features fromVar (const juce::var& v)
    {
        Features x;
        if (! v.isObject()) return x;
        x.seconds = (double) v.getProperty ("seconds", 0.0);
        for (int f = 0; f < numFeatures; ++f) x.set (f, (float) (double) v.getProperty (featureKey (f), (double) x.get (f)));
        x.maxShortTerm = (float) (double) v.getProperty ("maxShortTerm", -100.0);
        x.truePeak = (float) (double) v.getProperty ("truePeak", -100.0);
        x.samplePeak = (float) (double) v.getProperty ("samplePeak", -100.0);
        x.clipEvents = (int) v.getProperty ("clipEvents", 0);
        x.humDb = (float) (double) v.getProperty ("humDb", 0.0);
        x.humHz = (int) v.getProperty ("humHz", 0);
        x.humSeconds = (float) (double) v.getProperty ("humSeconds", 0.0);
        x.quietSeconds = (float) (double) v.getProperty ("quietSeconds", 0.0);
        if (auto* a = v.getProperty ("swing", {}).getArray()) for (int b = 0; b < juce::jmin (kNumBands, a->size()); ++b) x.swing[(size_t) b] = (float) (double) (*a)[b];
        if (auto* a = v.getProperty ("swingSpread", {}).getArray()) for (int b = 0; b < juce::jmin (kNumBands, a->size()); ++b) x.swingSpread[(size_t) b] = (float) (double) (*a)[b];
        if (auto* a = v.getProperty ("levelSwing", {}).getArray()) for (int b = 0; b < juce::jmin (kNumBands, a->size()); ++b) x.levelSwing[(size_t) b] = (float) (double) (*a)[b];
        x.valid = x.seconds >= 1.0;
        return x;
    }
};

//==============================================================================
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    void setRaw (double B0, double B1, double B2, double A0, double A1, double A2)
    {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void lowpass (double sr, double f, double q)
    {
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sr * 0.49, f) / sr;
        const double cw = std::cos (w), al = std::sin (w) / (2.0 * q);
        setRaw ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al);
    }
};

// ITU-R BS.1770 K-weighting (pre-filter shelf + RLB high-pass), any sample rate
struct KWeight
{
    Biquad shelf, hp;
    void prepare (double sr)
    {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            shelf.setRaw (Vh + Vb * K / Q + K * K, 2.0 * (K * K - Vh), Vh - Vb * K / Q + K * K, 1.0 + K / Q + K * K, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            hp.b0 = 1.0; hp.b1 = -2.0; hp.b2 = 1.0;
            hp.a1 = 2.0 * (K * K - 1.0) / a0; hp.a2 = (1.0 - K / Q + K * K) / a0;
        }
        reset();
    }
    void reset() { shelf.reset(); hp.reset(); }
    inline double process (double x) noexcept { return hp.process (shelf.process (x)); }
};

//==============================================================================
// Not thread safe: feed it from one thread, call compute() from the same thread.
class MixAnalysis
{
public:
    static constexpr int kTpChunk = 256;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        kw[0].prepare (sr); kw[1].prepare (sr);
        for (auto* f : { &lp[0], &lp[1], &lp[2], &lp[3] }) f->lowpass (sr, 120.0, 0.70710678);   // LR4 at 120 Hz
        blockLen = juce::jmax (1, (int) std::lround (sr * 0.1));
        order = juce::jlimit (11, 14, (int) std::ceil (std::log2 (sr * 0.085)));
        N = 1 << order; hop = N / 2;
        fft = std::make_unique<juce::dsp::FFT> (order);
        window.resize ((size_t) N);
        for (int i = 0; i < N; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N);
        fftBuf.assign ((size_t) (2 * N), 0.0f);
        inL.assign ((size_t) N, 0.0f); inR.assign ((size_t) N, 0.0f);
        binPower.assign ((size_t) (N / 2 + 1), 0.0);
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        os->initProcessing ((size_t) kTpChunk);
        tpBuf.setSize (2, kTpChunk);
        humLen = juce::jmax (1, (int) std::lround (sr));
        humBuf.assign ((size_t) humLen, 0.0f);
        blocks.reserve (36000);
        reset();
    }

    double getSampleRate() const { return sr; }

    void reset()
    {
        kw[0].reset(); kw[1].reset();
        for (auto& f : lp) f.reset();
        blocks.clear(); rawBlocks = 0; nonSilentBlocks = 0;
        kAcc = 0; rawAcc = 0; blockFill = 0;
        sLL = sRR = sLR = sMM = sSS = 0; lLL = lRR = lLR = 0;
        std::fill (binPower.begin(), binPower.end(), 0.0);
        frames = 0; inFill = 0;
        winFrames = 0; winBand.fill (0.0); winRows.clear();
        std::fill (inL.begin(), inL.end(), 0.0f); std::fill (inR.begin(), inR.end(), 0.0f);
        if (os) os->reset();
        tpFill = 0; tpMax = 0; spMax = 0;
        clipRun[0] = clipRun[1] = 0; clipSign[0] = clipSign[1] = 0; clipEvents = 0;
        humFill = 0; humSeconds = 0; quietSeconds = 0; humBest = 0; humBestHz = 0;
    }

    void process (const float* L, const float* R, int n)
    {
        if (R == nullptr) R = L;
        for (int i = 0; i < n; ++i)
        {
            const float l = L[i], r = R[i];
            const double ld = l, rd = r;

            // loudness: K-weighted power in 100 ms blocks
            const double kl = kw[0].process (ld), kr = kw[1].process (rd);
            kAcc += kl * kl + kr * kr;
            rawAcc += 0.5 * (ld * ld + rd * rd);
            if (++blockFill >= blockLen)
            {
                blocks.push_back ((float) (kAcc / (double) blockLen));
                const double rawP = rawAcc / (double) blockLen;
                ++rawBlocks;
                if (rawP > kSilence) ++nonSilentBlocks;
                kAcc = rawAcc = 0; blockFill = 0;
            }

            // stereo
            sLL += ld * ld; sRR += rd * rd; sLR += ld * rd;
            const double m = 0.5 * (ld + rd), s = 0.5 * (ld - rd);
            sMM += m * m; sSS += s * s;
            const double llo = lp[1].process (lp[0].process (ld)), rlo = lp[3].process (lp[2].process (rd));
            lLL += llo * llo; lRR += rlo * rlo; lLR += llo * rlo;

            // peaks + flat-topped clipping
            spMax = std::max (spMax, std::max (std::abs (l), std::abs (r)));
            for (int c = 0; c < 2; ++c)   // a run of 3+ samples stuck at full scale (same sign) = one clip
            {
                const float v = c == 0 ? l : r;
                const int sgn = v >= 0.9995f ? 1 : (v <= -0.9995f ? -1 : 0);
                clipRun[c] = sgn != 0 && sgn == clipSign[c] ? clipRun[c] + 1 : (sgn != 0 ? 1 : 0);
                clipSign[c] = sgn;
                if (clipRun[c] == 3 && clipRun[1 - c] < 3) ++clipEvents;   // both channels clipping together counts once
            }

            tpBuf.setSample (0, tpFill, l); tpBuf.setSample (1, tpFill, r);
            if (++tpFill >= kTpChunk) flushTruePeak();

            // long-term spectrum
            inL[(size_t) inFill] = l; inR[(size_t) inFill] = r;
            if (++inFill >= N) analyseFrame();

            // hum, in 1 second mono pieces
            humBuf[(size_t) humFill] = 0.5f * (l + r);
            if (++humFill >= humLen) analyseHum();
        }
    }

    Features compute() const
    {
        Features f;
        f.seconds = (double) nonSilentBlocks * 0.1;
        f.clipEvents = clipEvents;
        f.humDb = humBest; f.humHz = humBestHz;
        f.humSeconds = (float) humSeconds; f.quietSeconds = (float) quietSeconds;
        const int B = (int) blocks.size();
        if (B < 4 || frames == 0) return f;

        // momentary (400 ms, 75 % overlap) -> gated integrated loudness
        std::vector<double> mom;
        mom.reserve ((size_t) B);
        double run = 0;
        for (int i = 0; i < B; ++i)
        {
            run += blocks[(size_t) i];
            if (i >= 4) run -= blocks[(size_t) (i - 4)];
            if (i >= 3) mom.push_back (run / 4.0);
        }
        f.lufs = gatedLoudness (mom, -10.0);

        // short-term (3 s windows, 10 Hz) -> max short-term + loudness range
        std::vector<double> st;
        const int W = juce::jmin (30, B);
        run = 0;
        for (int i = 0; i < B; ++i)
        {
            run += blocks[(size_t) i];
            if (i >= W) run -= blocks[(size_t) (i - W)];
            if (i >= W - 1) st.push_back (run / (double) W);
        }
        double maxSt = 0;
        for (auto p : st) maxSt = std::max (maxSt, p);
        f.maxShortTerm = toLufs (maxSt);
        f.lra = loudnessRange (st);

        f.samplePeak = gainToDb (spMax);
        f.truePeak = std::max (f.samplePeak, gainToDb (tpMax));
        f.plr = f.lufs > -70.0f ? f.truePeak - f.lufs : 0.0f;

        // tonal balance
        std::array<double, kNumBands> bp {};
        double total = 0;
        const double nyq = sr * 0.5;
        for (int k = 1; k <= N / 2; ++k)
        {
            const double hz = (double) k * sr / (double) N;
            if (hz < 20.0 || hz >= 20000.0 || hz >= nyq) continue;
            const double p = binPower[(size_t) k];
            total += p;
            for (int b = 0; b < kNumBands; ++b)
                if (hz >= bandDefs()[(size_t) b].lo && hz < bandDefs()[(size_t) b].hi) { bp[(size_t) b] += p; break; }
        }
        for (int b = 0; b < kNumBands; ++b)
            f.band[(size_t) b] = total > 0 ? (float) (10.0 * std::log10 (std::max (1e-12, bp[(size_t) b] / total))) : -60.0f;

        // how much each range jumps around, over the loud parts
        if (winRows.size() >= 20)
        {
            std::vector<float> tot;
            for (auto& w : winRows) tot.push_back (w.total);
            auto pct = [] (std::vector<float> v, double q) { std::sort (v.begin(), v.end()); return v[(size_t) juce::jlimit (0, (int) v.size() - 1, (int) std::lround (q * (double) (v.size() - 1)))]; };
            const float gate = pct (tot, 0.95) - 20.0f;
            std::vector<const WinRow*> loud;
            for (auto& w : winRows) if (w.total > gate) loud.push_back (&w);
            if (loud.size() >= 20)
                for (int b = 0; b < kNumBands; ++b)
                {
                    std::vector<float> sh, lv;
                    for (auto* w : loud) { sh.push_back (w->level[(size_t) b] - w->total); lv.push_back (w->level[(size_t) b]); }
                    f.swing[(size_t) b] = pct (sh, 0.9) - pct (sh, 0.5);
                    f.levelSwing[(size_t) b] = pct (lv, 0.9) - pct (lv, 0.5);
                    f.swingSpread[(size_t) b] = pct (sh, 0.75) - pct (sh, 0.25);
                }
        }

        // stereo
        f.correlation = sLL > 0 && sRR > 0 ? (float) juce::jlimit (-1.0, 1.0, sLR / std::sqrt (sLL * sRR)) : 1.0f;
        f.lowCorrelation = lLL > 1e-9 && lRR > 1e-9 ? (float) juce::jlimit (-1.0, 1.0, lLR / std::sqrt (lLL * lRR)) : 1.0f;
        f.width = sMM > 1e-12 ? (float) juce::jlimit (-40.0, 20.0, 10.0 * std::log10 (std::max (1e-12, sSS) / sMM)) : (sSS > 0 ? 20.0f : -40.0f);

        f.valid = f.seconds >= 1.0;
        return f;
    }

private:
    static constexpr double kSilence = 1e-7;    // -70 dBFS

    static float toLufs (double p) { return p <= 1e-12 ? -100.0f : (float) (-0.691 + 10.0 * std::log10 (p)); }
    static float gainToDb (float g) { return g <= 1e-6f ? -100.0f : 20.0f * std::log10 (g); }

    static float gatedLoudness (const std::vector<double>& pw, double relGate)
    {
        double s = 0; int c = 0;
        for (auto p : pw) if (toLufs (p) > -70.0f) { s += p; ++c; }
        if (c == 0) return -100.0f;
        const double gate = toLufs (s / c) + relGate;
        s = 0; c = 0;
        for (auto p : pw) if (toLufs (p) > -70.0f && toLufs (p) > gate) { s += p; ++c; }
        return c == 0 ? -100.0f : toLufs (s / c);
    }

    static float loudnessRange (const std::vector<double>& st)
    {
        std::vector<float> v;
        double s = 0; int c = 0;
        for (auto p : st) if (toLufs (p) > -70.0f) { s += p; ++c; }
        if (c < 2) return 0.0f;
        const double gate = toLufs (s / c) - 20.0;
        for (auto p : st) { const float l = toLufs (p); if (l > -70.0f && l > gate) v.push_back (l); }
        if (v.size() < 2) return 0.0f;
        std::sort (v.begin(), v.end());
        auto pct = [&] (double q) { return v[(size_t) juce::jlimit (0, (int) v.size() - 1, (int) std::lround (q * (double) (v.size() - 1)))]; };
        return pct (0.95) - pct (0.10);
    }

    void flushTruePeak()
    {
        juce::dsp::AudioBlock<float> blk (tpBuf.getArrayOfWritePointers(), 2, (size_t) tpFill);
        auto up = os->processSamplesUp (blk);
        for (size_t c = 0; c < 2; ++c)
        {
            const float* u = up.getChannelPointer (c);
            for (size_t i = 0; i < up.getNumSamples(); ++i) tpMax = std::max (tpMax, std::abs (u[i]));
        }
        tpFill = 0;
    }

    void analyseFrame()
    {
        double raw = 0;
        for (int i = 0; i < N; ++i) raw += 0.5 * ((double) inL[(size_t) i] * inL[(size_t) i] + (double) inR[(size_t) i] * inR[(size_t) i]);
        if (raw / (double) N > kSilence)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto& in = ch == 0 ? inL : inR;
                std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
                for (int i = 0; i < N; ++i) fftBuf[(size_t) i] = in[(size_t) i] * window[(size_t) i];
                fft->performRealOnlyForwardTransform (fftBuf.data(), true);
                int b = 0;
                for (int k = 1; k <= N / 2; ++k)
                {
                    const double re = fftBuf[(size_t) (2 * k)], im = fftBuf[(size_t) (2 * k + 1)];
                    const double pw = 0.5 * (re * re + im * im);
                    binPower[(size_t) k] += pw;
                    const double hz = (double) k * sr / (double) N;
                    if (hz < 20.0 || hz >= 20000.0) continue;
                    while (b < kNumBands - 1 && hz >= bandDefs()[(size_t) b].hi) ++b;
                    winBand[(size_t) b] += pw;
                }
            }
            ++frames;
            if (++winFrames >= 4)   // ~170 ms at 48 kHz
            {
                WinRow w;
                double tot = 0;
                for (int b = 0; b < kNumBands; ++b) { tot += winBand[(size_t) b]; w.level[(size_t) b] = (float) (10.0 * std::log10 (std::max (1e-20, winBand[(size_t) b]))); }
                w.total = (float) (10.0 * std::log10 (std::max (1e-20, tot)));
                winRows.push_back (w);
                winBand.fill (0.0); winFrames = 0;
            }
        }
        // slide by one hop
        std::copy (inL.begin() + hop, inL.end(), inL.begin());
        std::copy (inR.begin() + hop, inR.end(), inR.begin());
        inFill = N - hop;
    }

    static double goertzel (const float* x, int n, double f, double sr)
    {
        const double w = juce::MathConstants<double>::twoPi * f / sr, c = 2.0 * std::cos (w);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < n; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
        return std::max (1e-30, s1 * s1 + s2 * s2 - c * s1 * s2);
    }

    // How far a mains-hum family (f, 2f, 3f) stands out of the surrounding spectrum.
    double humRatioDb (double f0, double& secondDb) const
    {
        const float* x = humBuf.data();
        auto ratio = [&] (double f)
        {
            const double p = goertzel (x, humLen, f, sr);
            const double nb = 0.25 * (goertzel (x, humLen, f - 9.0, sr) + goertzel (x, humLen, f - 5.0, sr)
                                    + goertzel (x, humLen, f + 5.0, sr) + goertzel (x, humLen, f + 9.0, sr));
            return 10.0 * std::log10 (p / nb);
        };
        const double r1 = ratio (f0);
        secondDb = std::max (ratio (2.0 * f0), ratio (3.0 * f0));
        return r1;
    }

    void analyseHum()
    {
        double p = 0;
        for (int i = 0; i < humLen; ++i) p += (double) humBuf[(size_t) i] * humBuf[(size_t) i];
        p /= (double) humLen;
        const double db = 10.0 * std::log10 (std::max (1e-20, p));
        if (db > -90.0 && db < -42.0)      // quiet, but not digital silence
        {
            quietSeconds += 1.0;
            double h50 = 0, h60 = 0;
            const double r50 = humRatioDb (50.0, h50), r60 = humRatioDb (60.0, h60);
            const bool is60 = r60 >= r50;
            const double r = is60 ? r60 : r50, h = is60 ? h60 : h50;
            if (r > 12.0 && h > 6.0)
            {
                humSeconds += 1.0;
                if (r > humBest) { humBest = (float) r; humBestHz = is60 ? 60 : 50; }
            }
        }
        humFill = 0;
    }

    double sr = 48000.0;
    KWeight kw[2];
    std::array<Biquad, 4> lp;
    int blockLen = 4800, blockFill = 0;
    double kAcc = 0, rawAcc = 0;
    std::vector<float> blocks;
    int64_t rawBlocks = 0, nonSilentBlocks = 0;

    double sLL = 0, sRR = 0, sLR = 0, sMM = 0, sSS = 0, lLL = 0, lRR = 0, lLR = 0;

    int order = 12, N = 4096, hop = 2048, inFill = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, fftBuf, inL, inR;
    std::vector<double> binPower;
    struct WinRow { std::array<float, kNumBands> level {}; float total = -200.0f; };
    std::vector<WinRow> winRows;          // per ~170 ms window: each range's level and the total, dB
    std::array<double, kNumBands> winBand {};
    int winFrames = 0;
    int64_t frames = 0;

    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    juce::AudioBuffer<float> tpBuf;
    int tpFill = 0;
    float tpMax = 0, spMax = 0;
    int clipRun[2] { 0, 0 }, clipSign[2] { 0, 0 }, clipEvents = 0;

    std::vector<float> humBuf;
    int humLen = 48000, humFill = 0;
    double humSeconds = 0, quietSeconds = 0;
    float humBest = 0; int humBestHz = 0;
};
} // namespace mst
