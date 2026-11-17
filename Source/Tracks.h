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
    // sibilance, measured by The Servant (dB of the "s" band over the whole signal, every 10 ms)
    float sibP50 = -45, sibP95 = -45, sibPerMin = 0, sibSeconds = 0, sibHz = 6500;
    // transients, measured by The Servant: how far its hits jump over its own body (95th percentile, dB)
    float transP95 = 0, transSeconds = 0;
};

struct TrackPlan
{
    juce::String kind { "servant" };            // or "buzzkill" / "flatliner" (Helpers.h)
    juce::String id, name, role;
    juce::String note;                          // one-line summary for the helpers
    float share = 0;                            // of the mix's total power
    float gainToMix = 1;
    std::array<float, kNumBands> eq {};         // dB per band
    float trimDb = 0, comp = 0, deess = 0, transAtk = 0, transSus = 0;
    bool compOn = false, deessOn = false, transOn = false;
    juce::StringArray why;
    std::map<juce::String, float> params;       // what goes to The Servant
    juce::String summary() const
    {
        if (kind != "servant") return note;
        juce::StringArray s;
        int moves = 0; for (auto v : eq) if (std::abs (v) >= 0.5f) ++moves;
        if (moves > 0) s.add ("EQ " + juce::String (moves) + (moves == 1 ? " band" : " bands"));
        if (compOn) s.add ("COMP " + juce::String (juce::roundToInt (comp)) + "%");
        if (deessOn) s.add ("DE-ESS " + juce::String (juce::roundToInt (deess)) + "%");
        if (transOn && std::abs (transAtk) >= 1.0f) s.add ("ATTACK " + juce::String (transAtk > 0 ? "+" : "") + juce::String (juce::roundToInt (transAtk)) + "%");
        if (transOn && std::abs (transSus) >= 1.0f) s.add ("SUSTAIN " + juce::String (transSus > 0 ? "+" : "") + juce::String (juce::roundToInt (transSus)) + "%");
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

    // 5) de-essing: tracks whose "s" sounds jump out (usually the lead vocal). Not drums or bass:
    //    cymbals and pick noise are supposed to be bright.
    {
        const float biteZ = g.z (fBand0 + 7, mix.band[7]);
        const bool pop = G.equalsIgnoreCase ("pop");
        for (size_t i = 0; i < n; ++i)
        {
            const auto& t = tracks[i];
            if (roleIs (plans[i].role, { "Kick", "Snare", "Drums", "Bass" }) || t.sibSeconds < 5.0f) continue;
            const float spike = t.sibP95 - t.sibP50;
            if (spike < 10.0f || t.sibP95 < -9.0f || t.sibPerMin < 4.0f) continue;
            auto& p = plans[i];
            p.deessOn = true;
            p.deess = std::round (juce::jlimit (20.0f, 80.0f, 30.0f + 3.0f * (t.sibP95 + 9.0f) + 10.0f * juce::jmax (0.0f, biteZ) + (pop ? 10.0f : 0.0f)));
            p.why.add ("DE-ESS " + juce::String (juce::roundToInt (p.deess)) + "%: this track's \"s\" sounds jump " + juce::String (spike, 0) + " dB above the rest of it, about "
                       + juce::String (juce::roundToInt (t.sibPerMin)) + " times a minute" + (biteZ > 1.0f ? juce::String (", and the mix's bite (4-8 kHz) is already hot for ") + G : juce::String())
                       + ". Only the top end above " + juce::String (t.sibHz / 1000.0f, 1) + " kHz is turned down, only during an \"s\".");
        }
    }

    // 5b) transients: drums get more snap when the mix is flat (or their own hits are soft), a kick that
    //     booms in a crowded low end gets a shorter tail, spiky picking on a rhythm part is softened when
    //     the mix's peaks already stick out. Never on a track whose hits are already hard.
    {
        const auto gk = (g.id.isNotEmpty() ? g.id : g.name).toLowerCase().removeCharacters (" _-");
        float boost = 15.0f;
        if (gk == "metal") boost = 30.0f; else if (gk == "hardrock") boost = 25.0f; else if (gk == "punk") boost = 20.0f; else if (gk == "pop") boost = 10.0f;
        const float zPlr = g.z (fPlr, mix.plr);
        const float lowZ = (g.z (fBand0 + 0, mix.band[0]) + g.z (fBand0 + 1, mix.band[1]) + g.z (fBand0 + 2, mix.band[2])) / 3.0f;
        const float lowMidZ = g.z (fBand0 + 3, mix.band[3]);
        auto pct = [] (float v) { return (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + "%"; };
        for (size_t i = 0; i < n; ++i)
        {
            const auto& t = tracks[i];
            auto& p = plans[i];
            const bool known = t.transSeconds >= 5.0f;
            const bool drumHit = roleIs (p.role, { "Kick", "Snare" }), kit = roleIs (p.role, { "Drums" });
            if (drumHit || kit)
            {
                const float soft = kit ? 7.0f : 9.0f, hard = kit ? 14.0f : 16.0f;
                const bool tooHard = known && t.transP95 > hard;
                if (! tooHard && zPlr <= 1.0f && (zPlr < -0.5f || (known && t.transP95 < soft)))
                {
                    p.transAtk = zPlr < -1.5f ? boost + 15.0f : boost;
                    p.why.add ("ATTACK " + pct (p.transAtk) + ": " + (zPlr < -0.5f
                               ? "the mix's peaks sit only " + juce::String (mix.plr, 1) + " dB over its average (flat for " + G + "), so the hits get more snap at the source"
                               : "this track's hits only jump " + juce::String (t.transP95, 0) + " dB over its body: soft for a " + p.role.toLowerCase())
                               + ". It lifts the first few ms of each hit, not the level.");
                }
                else if (! roleIs (p.role, { "Kick" }) && zPlr > 1.0f)
                {
                    p.transAtk = -15.0f;
                    p.why.add ("ATTACK " + pct (p.transAtk) + ": the mix's peaks stick out (" + juce::String (mix.plr, 1) + " dB over the average); taking a little off each hit here is cleaner than squashing the whole mix.");
                }
                if (roleIs (p.role, { "Kick" }) && (lowZ > 0.8f || gk == "metal"))
                {
                    p.transSus = lowZ > 0.8f ? -30.0f : -20.0f;
                    p.why.add ("SUSTAIN " + pct (p.transSus) + ": " + (lowZ > 0.8f ? juce::String ("the mix's low end is crowded for ") + G + ", so the kick's boom is shortened and the bass gets the space between hits"
                                                                                 : juce::String ("a tighter kick, like ") + G + " records"));
                }
                if (kit && lowMidZ > 0.8f)
                {
                    p.transSus = -20.0f;
                    p.why.add ("SUSTAIN " + pct (p.transSus) + ": the mix's low mids (250-500 Hz) are thick for " + G + "; less room ring from the kit clears them.");
                }
            }
            else if (roleIs (p.role, { "Rhythm" }) && known && t.transP95 > 14.0f && zPlr > 0.5f)
            {
                p.transAtk = -20.0f;
                p.why.add ("ATTACK " + pct (p.transAtk) + ": the picking jumps " + juce::String (t.transP95, 0) + " dB over the chords' body and the mix's peaks already stick out; a softer pick attack lets the guitars sit under the drums.");
            }
            p.transOn = std::abs (p.transAtk) >= 1.0f || std::abs (p.transSus) >= 1.0f;
        }
    }

    // 6) what The Servant gets: the 6 biggest EQ moves on its tone bands (it has 6). Anything
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
        p.params["deessOn"] = p.deessOn ? 1.0f : 0.0f;
        if (p.deessOn) p.params["deess"] = p.deess;
        p.params["transOn"] = p.transOn ? 1.0f : 0.0f;
        if (p.transOn) { p.params["transAtk"] = p.transAtk; p.params["transSus"] = p.transSus; }
        if (p.compOn)
        {
            // The Servant's compressor STYLE follows the ROLE (AUTO); The Master leaves a style you picked alone
            juce::String st = "GLUE: gentle and slow, it just holds the part together";
            if (roleIs (p.role, { "Kick", "Snare", "Drums" })) st = "PUNCH: lets each hit's crack through, then holds the body";
            else if (roleIs (p.role, { "Lead" }))            st = "SMOOTH: catches the loud words early and lets go slowly, like vocal levelling";
            else if (roleIs (p.role, { "Bass" }))            st = "TIGHT: every note at the same level";
            p.why.add ("COMP STYLE " + st + " (AUTO, from the role).");
        }
        if (p.why.isEmpty()) p.why.add ("Already sits well in the mix. Nothing to change.");
    }
    res.tracks = std::move (plans);
    return res;
}
} // namespace mst
