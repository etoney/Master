#pragma once
// THE MASTER - THE FREAK (dynamic EQ) on the master bus.
// The Master measures how much each frequency range jumps above its usual share of the mix in the loud
// moments (Features::swing). Ranges that jump more than is normal for them get a Freak band that cuts
// only when they do; the band's threshold sits above the range's ordinary ups and downs (the song's own
// dynamics), so it reacts to the excess and nothing else. Static tone stays with The Master's EQ.
#include "Profiles.h"

namespace mst
{
struct FreakBandPlan
{
    int range = -1;             // analyzer band (0 SUB .. 9 AIR)
    int type = 0;               // 0 bell, 1 low shelf, 2 high shelf
    float freq = 1000, q = 1, depth = -6, thr = 3;
    float swing = 0, over = 0;  // measured swing, and how far past normal it is
    juce::String why;
};

struct FreakPlan
{
    std::vector<FreakBandPlan> bands;     // lowest first, at most 6
    std::map<juce::String, float> params; // every Freak control
    juce::StringArray notes;              // short reason per Freak band (6 entries), shown in The Freak
};

struct FreakRange { int range; int type; float freq, q, normal; const char* problem; };

// Where a moving problem usually lives, and how much swing is ordinary for that range.
// (Kick and bass notes make the lows swing naturally, so they need more before The Freak steps in.)
inline const std::vector<FreakRange>& freakRanges()
{
    static const std::vector<FreakRange> r {
        { 0, 0,   45.0f, 1.0f, 9.0f, "sub notes that boom out" },
        { 1, 0,   85.0f, 1.2f, 8.0f, "bass notes that jump out" },
        { 2, 0,  180.0f, 1.2f, 7.0f, "low-end boom" },
        { 3, 0,  350.0f, 1.3f, 6.0f, "mud that builds up" },
        { 4, 0,  700.0f, 1.4f, 7.0f, "boxiness" },
        { 5, 0, 1400.0f, 1.4f, 7.0f, "honk" },
        { 6, 0, 2900.0f, 1.6f, 5.5f, "harshness" },
        { 7, 0, 5600.0f, 1.6f, 5.5f, "harsh, spitty peaks" },
        { 8, 2, 9000.0f, 0.71f, 6.0f, "cymbal crashes that splash over everything" } };
    return r;
}

inline FreakPlan planFreak (const Features& in, const GenreProfile& g)
{
    FreakPlan fp;
    std::vector<FreakBandPlan> cand;
    for (auto& r : freakRanges())
    {
        const float sw = in.swing[(size_t) r.range];
        if (sw <= 0.0f) continue;
        float normal = r.normal;
        const bool hot = g.z (fBand0 + r.range, in.band[(size_t) r.range]) > 1.0f;
        if (hot) normal -= 1.0f;                               // already too loud on average: step in a bit sooner
        // jumps on every beat (drum hits) are the music, not a problem: much more before The Freak touches them
        const bool rhythmic = in.swingSpread[(size_t) r.range] > 0.6f * sw;
        if (rhythmic) normal += 4.0f;
        const float over = sw - normal;
        if (over <= 0.0f) continue;
        FreakBandPlan b;
        b.range = r.range; b.type = r.type; b.freq = r.freq; b.q = r.q; b.swing = sw; b.over = over;
        b.depth = -std::round (juce::jlimit (2.0f, 6.0f, 2.0f + over * 0.6f));   // gentle: this is the whole mix
        // the range's ordinary ups and downs = its level swing minus the part that's excess for the mix
        b.thr = std::round (juce::jlimit (2.0f, 8.0f, in.levelSwing[(size_t) r.range] - sw + 1.5f) * 2.0f) / 2.0f;
        if (rhythmic) b.thr = std::max (b.thr, 6.0f);   // let ordinary hits through, catch only the ones that go over the top
        const auto& d = bandDefs()[(size_t) r.range];
        b.why = juce::String (d.label) + " (" + d.range + " Hz) jumps " + juce::String (sw, 1) + " dB above its usual share of the mix in the loud moments ("
              + r.problem + "); about " + juce::String (normal, 1) + " dB is normal" + (hot ? " for a range that's already hot" : "")
              + (rhythmic ? " when it jumps on the beat like this" : "")
              + ". The Freak cuts it up to " + juce::String (-b.depth, 0) + " dB, only while it does.";
        cand.push_back (b);
    }
    std::sort (cand.begin(), cand.end(), [] (const FreakBandPlan& a, const FreakBandPlan& b) { return a.over > b.over; });
    if (cand.size() > 6) cand.resize (6);
    std::sort (cand.begin(), cand.end(), [] (const FreakBandPlan& a, const FreakBandPlan& b) { return a.freq < b.freq; });
    fp.bands = cand;

    for (int i = 0; i < 6; ++i)
    {
        const auto n = "b" + juce::String (i + 1);
        if (i < (int) cand.size())
        {
            const auto& b = cand[(size_t) i];
            fp.params[n + "on"] = 1.0f; fp.params[n + "type"] = (float) b.type; fp.params[n + "f"] = b.freq; fp.params[n + "q"] = b.q;
            fp.params[n + "g"] = 0.0f; fp.params[n + "d"] = b.depth; fp.params[n + "t"] = b.thr;
            const auto& d = bandDefs()[(size_t) b.range];
            fp.notes.add (juce::String (d.label) + ": jumps " + juce::String (b.swing, 1) + " dB in the loud parts");
        }
        else
        {
            fp.params[n + "on"] = 0.0f;
            fp.notes.add ({});
        }
    }
    fp.params["amount"] = 100.0f; fp.params["speed"] = 50.0f; fp.params["output"] = 0.0f;
    return fp;
}
} // namespace mst
