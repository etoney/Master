#pragma once
// THE MASTER - per-track corrections through The Servant, decided from the MIX profile only
// (no instrument profiles, no stem splitting).
//
//   1. Every linked Servant reports what its track sends to the mix, in The Master's 10 bands,
//      plus a level fingerprint (100 ms steps). The Master finds each track's real gain into the
//      mix (faders, buses, sends) by fitting the tracks' fingerprints to its own input.
//   2. Tone: where the mix misses the genre in a band, the correction is shared out among the
//      tracks that actually make that band, biggest contributor moves most. ROLES steer it:
//      kick and bass keep the low end, the lead keeps its presence.
//   3. Balance (TRIM): the lead should carry a fair share of the 500 Hz - 4 kHz range.
//   4. Compression: if the mix's peaks stick out, the tracks that make the peaks get compressed.
#include "Cure.h"
#include <set>

namespace mst
{
struct TrackInfo
{
    juce::String id, name, role;
    std::array<double, kNumBands> bandPow {};   // mean power per band, as the track leaves its Servant
    double pow = 0, seconds = 0;
    std::vector<float> series;                  // linear power per 100 ms, aligned with the mix series
};

struct TrackPlan
{
    juce::String kind { "servant" };            // or "buzzkill" / "flatliner" (Helpers.h)
    juce::String id, name, role;
    juce::String note;                          // one-line summary for the helpers
    float share = 0;                            // of the mix's total power
    float gainToMix = 1;
    std::array<float, kNumBands> eq {};         // dB per band
    float trimDb = 0, comp = 0;
    bool compOn = false;
    juce::StringArray why;
    std::map<juce::String, float> params;       // what goes to The Servant
    juce::String summary() const
    {
        if (kind != "servant") return note;
        juce::StringArray s;
        int moves = 0; for (auto v : eq) if (std::abs (v) >= 0.5f) ++moves;
        if (moves > 0) s.add ("EQ " + juce::String (moves) + (moves == 1 ? " band" : " bands"));
        if (compOn) s.add ("COMP " + juce::String (juce::roundToInt (comp)) + "%");
        if (std::abs (trimDb) >= 0.2f) s.add ("TRIM " + juce::String (trimDb > 0 ? "+" : "") + juce::String (trimDb, 1));
        return s.isEmpty() ? juce::String ("no change") : s.joinIntoString ("  //  ");
    }
};

inline bool roleIs (const juce::String& r, std::initializer_list<const char*> names)
{
    for (auto* n : names) if (r.startsWithIgnoreCase (n)) return true;
    return false;
}

// how willing a track of this role is to move in this band, in this direction
inline float roleFactor (const juce::String& role, int band, bool cut)
{
    const bool low = band <= 1, presence = band >= 4 && band <= 6;
    const bool bottom = roleIs (role, { "Bass", "Kick" });
    const bool lead = roleIs (role, { "Lead" });
    if (low) return cut ? (bottom ? 0.4f : 1.6f) : (bottom ? 2.0f : 0.3f);
    if (presence) return cut ? (lead ? 0.3f : 1.3f) : (lead ? 2.0f : 0.6f);
    return 1.0f;
}

// Non-negative fit: mix(t) ~ sum_i g_i * track_i(t). Lee-Seung multiplicative updates.
inline std::vector<double> fitGains (const std::vector<float>& mix, const std::vector<std::vector<float>>& tr)
{
    const size_t n = tr.size(), T = mix.size();
    std::vector<double> g (n, 1.0);
    if (n == 0 || T < 30) return g;
    std::vector<double> atb (n, 0.0);
    std::vector<std::vector<double>> ata (n, std::vector<double> (n, 0.0));
    for (size_t t = 0; t < T; ++t)
        for (size_t i = 0; i < n; ++i)
        {
            atb[i] += (double) tr[i][t] * mix[t];
            for (size_t j = 0; j < n; ++j) ata[i][j] += (double) tr[i][t] * tr[j][t];
        }
    for (int it = 0; it < 400; ++it)
        for (size_t i = 0; i < n; ++i)
        {
            double den = 0;
            for (size_t j = 0; j < n; ++j) den += ata[i][j] * g[j];
            if (den > 1e-30 && atb[i] > 0) g[i] *= atb[i] / den; else if (atb[i] <= 0) g[i] = 0;
        }
    for (auto& v : g) v = juce::jlimit (0.0, 100.0, v);
    return g;
}

struct TracksResult
{
    std::vector<TrackPlan> tracks;
    std::array<float, kNumBands> residualEq {};   // what's left for The Master's own EQ
    juce::String note;
};

inline TracksResult planTracks (const Features& mix, const GenreProfile& g, std::vector<TrackInfo> tracks, const std::vector<float>& mixSeries,
                                const std::set<juce::String>& riddenByFlatliner = {})
{
    TracksResult res;
    if (tracks.empty() || ! mix.valid) return res;
    const auto G = g.name;

    // 1) each track's real gain into the mix
    std::vector<std::vector<float>> ser;
    for (auto& t : tracks) ser.push_back (t.series);
    bool seriesOk = mixSeries.size() >= 30;
    for (auto& s : ser) seriesOk = seriesOk && s.size() == mixSeries.size();
    const auto gains = seriesOk ? fitGains (mixSeries, ser) : std::vector<double> (tracks.size(), 1.0);
    if (! seriesOk) res.note = "Not enough playback heard with the Servants linked to measure each track's fader; assuming they're all at 0 dB.";

    const size_t n = tracks.size();
    std::vector<std::array<double, kNumBands>> c (n);
    std::array<double, kNumBands> tot {};
    double all = 0;
    for (size_t i = 0; i < n; ++i)
        for (int b = 0; b < kNumBands; ++b)
        {
            c[i][(size_t) b] = gains[i] * tracks[i].bandPow[(size_t) b];
            tot[(size_t) b] += c[i][(size_t) b];
            all += c[i][(size_t) b];
        }

    std::vector<TrackPlan> plans (n);
    for (size_t i = 0; i < n; ++i)
    {
        plans[i].id = tracks[i].id; plans[i].name = tracks[i].name.isNotEmpty() ? tracks[i].name : "Track " + juce::String ((int) i + 1);
        plans[i].role = tracks[i].role;
        plans[i].gainToMix = (float) gains[i];
        double mine = 0; for (int b = 0; b < kNumBands; ++b) mine += c[i][(size_t) b];
        plans[i].share = all > 0 ? (float) (mine / all) : 0.0f;
    }

    // 2) tone: what the whole mix needs, shared out by who makes each band
    const auto want = planFixes (mix, nullptr, 0.0f, false, g).p.eq;
    for (int b = 0; b < kNumBands; ++b)
    {
        const float W = want[(size_t) b];
        res.residualEq[(size_t) b] = std::abs (W) >= 0.5f ? W : 0.0f;    // unless the tracks can take it
        if (std::abs (W) < 0.5f || tot[(size_t) b] <= 0) continue;
        const bool cut = W < 0;
        std::vector<double> w (n, 0.0);
        double wsum = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double s = c[i][(size_t) b] / tot[(size_t) b];
            if (s < 0.03) continue;                               // barely in this band: leave it alone
            w[i] = s * roleFactor (plans[i].role, b, cut);
            wsum += w[i];
        }
        if (wsum <= 0) continue;
        for (auto& v : w) v /= wsum;
        const double target = std::pow (10.0, W / 10.0) * tot[(size_t) b];
        auto result = [&] (double k)
        {
            double sum = 0;
            for (size_t i = 0; i < n; ++i) sum += c[i][(size_t) b] * std::pow (10.0, juce::jlimit (-6.0, 6.0, k * w[i]) / 10.0);
            return sum;
        };
        double lo = cut ? -200.0 : 0.0, hi = cut ? 0.0 : 200.0;
        for (int it = 0; it < 60; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            if (result (mid) < target) lo = mid; else hi = mid;
        }
        const double k = 0.5 * (lo + hi);
        for (size_t i = 0; i < n; ++i)
        {
            if (w[i] <= 0) continue;
            const float x = (float) std::round (juce::jlimit (-6.0, 6.0, k * w[i]) * 10.0) / 10.0f;
            if (std::abs (x) < 0.5f) continue;
            plans[i].eq[(size_t) b] = x;
            const int pct = juce::roundToInt (100.0 * c[i][(size_t) b] / tot[(size_t) b]);
            plans[i].why.add (juce::String (bandDefs()[(size_t) b].label) + " " + (x > 0 ? "+" : "") + juce::String (x, 1) + " dB: this track makes "
                              + juce::String (pct) + "% of the mix's " + bandDefs()[(size_t) b].range + " Hz, which is " + (cut ? "heavy" : "light") + " for " + G + ".");
        }
        // what the tracks achieved; the rest goes to The Master's own EQ
        double after = 0;
        for (size_t i = 0; i < n; ++i) after += c[i][(size_t) b] * std::pow (10.0, plans[i].eq[(size_t) b] / 10.0);
        const float achieved = (float) (10.0 * std::log10 (after / tot[(size_t) b]));
        const float left = W - achieved;
        res.residualEq[(size_t) b] = std::abs (left) >= 1.0f ? std::round (left * 10.0f) / 10.0f : 0.0f;
    }

    // 3) balance: the lead should carry a fair share of 500 Hz - 4 kHz
    {
        double lead = 0, other = 0;
        for (size_t i = 0; i < n; ++i)
            for (int b = 4; b <= 6; ++b) (roleIs (plans[i].role, { "Lead" }) ? lead : other) += c[i][(size_t) b];
        if (lead > 0 && other > 0)
        {
            const double share = lead / (lead + other);
            double targetShare = -1;
            if (share < 0.25) targetShare = 0.30;
            else if (share > 0.65) targetShare = 0.55;
            if (targetShare > 0)
            {
                const double a = targetShare * other / ((1.0 - targetShare) * lead);
                const float t = (float) juce::jlimit (-4.0, 4.0, 10.0 * std::log10 (a));
                for (auto& p : plans)
                    if (roleIs (p.role, { "Lead" }))
                    {
                        p.trimDb = std::round (t * 10.0f) / 10.0f;
                        p.why.add ("TRIM " + juce::String (p.trimDb > 0 ? "+" : "") + juce::String (p.trimDb, 1) + " dB: the lead makes "
                                   + juce::String (juce::roundToInt (share * 100.0)) + "% of the mix's 500 Hz - 4 kHz range; it " + (t > 0 ? "gets buried" : "sits too far on top") + ".");
                    }
            }
        }
        double bottom = 0, rest = 0;
        for (size_t i = 0; i < n; ++i)
            for (int b = 0; b <= 1; ++b) (roleIs (plans[i].role, { "Bass", "Kick" }) ? bottom : rest) += c[i][(size_t) b];
        if (bottom + rest > 0 && bottom / (bottom + rest) < 0.5 && bottom > 0)
            res.note << (res.note.isEmpty() ? "" : " ") << "Kick and bass make only " << juce::roundToInt (100.0 * bottom / (bottom + rest))
                     << "% of the low end; high-pass the other tracks (The Servant's CLEAN-UP) to give them room.";
    }

    // 4) compression: the tracks that make the mix's peaks
    {
        const float zPlr = g.z (fPlr, mix.plr);
        if (zPlr > 1.0f && seriesOk)
        {
            std::vector<size_t> idx (mixSeries.size());
            for (size_t t = 0; t < idx.size(); ++t) idx[t] = t;
            std::sort (idx.begin(), idx.end(), [&] (size_t a, size_t b) { return mixSeries[a] > mixSeries[b]; });
            const size_t top = std::max<size_t> (3, idx.size() / 10);
            double mixTop = 0; for (size_t k = 0; k < top; ++k) mixTop += mixSeries[idx[k]];
            for (size_t i = 0; i < n; ++i)
            {
                double inTop = 0, overall = 0;
                for (size_t k = 0; k < top; ++k) inTop += gains[i] * ser[i][idx[k]];
                for (auto v : ser[i]) overall += gains[i] * v;
                overall /= (double) ser[i].size();
                const double shareTop = mixTop > 0 ? inTop / mixTop : 0;
                const double peakiness = overall > 0 ? (inTop / (double) top) / overall : 0;
                if (shareTop >= 0.15 && peakiness >= 1.5)
                {
                    auto& p = plans[i];
                    p.compOn = true;
                    p.comp = std::round (juce::jlimit (25.0f, 55.0f, 25.0f + 10.0f * (zPlr - 1.0f)));
                    p.why.add ("COMP " + juce::String (juce::roundToInt (p.comp)) + "%: this track makes " + juce::String (juce::roundToInt (shareTop * 100.0))
                               + "% of the mix's loudest moments and spikes " + juce::String (10.0 * std::log10 (peakiness), 1) + " dB over its own average; the mix's peaks stick out for " + G + ".");
                }
            }
        }
        // a lead that jumps around
        for (size_t i = 0; i < n; ++i)
        {
            if (! roleIs (plans[i].role, { "Lead" }) || ! seriesOk || riddenByFlatliner.count (plans[i].id) > 0) continue;   // Flatliner rides it instead
            std::vector<float> act;
            for (auto v : ser[i]) if (v > 1e-8f) act.push_back (10.0f * std::log10 (v));
            if (act.size() < 30) continue;
            std::sort (act.begin(), act.end());
            const float range = act[act.size() * 9 / 10] - act[act.size() / 10];
            if (range > 14.0f && ! plans[i].compOn)
            {
                plans[i].compOn = true; plans[i].comp = 35.0f;
                plans[i].why.add ("COMP 35%: the lead's level jumps " + juce::String (range, 0) + " dB between its quiet and loud parts. (Flatliner rides it even more smoothly.)");
            }
        }
    }

    // 5) what The Servant gets: the 6 biggest EQ moves on its tone bands (it has 6). Anything
    //    smaller goes back to The Master's own EQ, weighted by how much of the band this track makes.
    for (size_t i = 0; i < n; ++i)
    {
        auto& p = plans[i];
        std::vector<std::pair<float, int>> order;
        for (int b = 0; b < kNumBands; ++b) if (std::abs (p.eq[(size_t) b]) >= 0.5f) order.push_back ({ std::abs (p.eq[(size_t) b]), b });
        std::sort (order.begin(), order.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        for (size_t k = 6; k < order.size(); ++k)
        {
            const int b = order[k].second;
            const double sh = tot[(size_t) b] > 0 ? c[i][(size_t) b] / tot[(size_t) b] : 0.0;
            res.residualEq[(size_t) b] += (float) (p.eq[(size_t) b] * sh);
            const auto tag = juce::String (bandDefs()[(size_t) b].label) + " ";
            for (int w = p.why.size(); --w >= 0;) if (p.why[w].startsWith (tag)) p.why.remove (w);
            p.eq[(size_t) b] = 0.0f;
        }
        if (order.size() > 6) order.resize (6);
        static const float defaultHz[6] = { 100.0f, 250.0f, 630.0f, 1600.0f, 4000.0f, 10000.0f };
        for (int k = 0; k < 6; ++k)
        {
            const juce::String t = "t" + juce::String (k + 1);
            if (k < (int) order.size())
            {
                const int b = order[(size_t) k].second;
                const float hz = std::sqrt (bandDefs()[(size_t) b].lo * bandDefs()[(size_t) b].hi);
                p.params[t + "f"] = hz;
                p.params[t + "g"] = p.eq[(size_t) b];
                p.params[t + "q"] = b == 0 || b == kNumBands - 1 ? 0.8f : 1.4f;
            }
            else { p.params[t + "f"] = defaultHz[k]; p.params[t + "g"] = 0.0f; p.params[t + "q"] = 1.0f; }
        }
        p.params["toneOn"] = order.empty() ? 0.0f : 1.0f;
        p.params["compOn"] = p.compOn ? 1.0f : 0.0f;
        p.params["comp"] = p.comp;
        p.params["trim"] = p.trimDb;
        if (p.why.isEmpty()) p.why.add ("Already sits well in the mix. Nothing to change.");
    }
    res.tracks = std::move (plans);
    return res;
}
} // namespace mst
