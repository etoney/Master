#pragma once
// THE MASTER - turns the measured mix vs. the genre profile into plain-English mixing advice,
// pointing at Castle Danger plugins where one of them does the job.
#include "Profiles.h"

namespace mst
{
enum class Sev { fix = 0, check, good, info };

struct Suggestion
{
    Sev sev = Sev::info;
    juce::String area, title, body;
    juce::String plugin, tip;     // which Castle Danger plugin helps, and how
    float weight = 0.0f;          // ordering inside a severity
};

struct Report
{
    bool ready = false;
    int score = 0, fixes = 0, checks = 0, goods = 0;
    juce::String genreName;
    std::array<float, numFeatures> z {};
    std::vector<Suggestion> items;
};

namespace plug
{
inline const char* dominatrix = "DOMINATRIX";
inline const char* chopShop   = "CHOP SHOP";
inline const char* flatliner  = "FLATLINER";
inline const char* buzzKill   = "BUZZKILL";
}

inline juce::String fmt1 (float v) { return juce::String (v, 1); }
inline juce::String signedDb (float v) { return (v >= 0 ? "+" : "") + juce::String (v, 1) + " dB"; }
inline juce::String rangeOf (const Stat& s, int dec = 1, const juce::String& unit = {})
{
    return juce::String (s.mean - s.sd, dec) + " to " + juce::String (s.mean + s.sd, dec) + unit;
}

// Band-by-band advice. hot = too much energy in the band compared with the genre.
inline void bandAdvice (int b, bool hot, const juce::String& genre, const juce::String& gid, Suggestion& s)
{
    const bool heavy = gid == "metal" || gid == "hardRock" || gid == "punk";
    switch (b)
    {
        case 0:
            s.title = hot ? "TOO MUCH SUB" : "NOT MUCH SUB";
            s.body = hot ? "The 20-60 Hz rumble is heavy for " + genre + ". Check kick and bass tails (or the 808), and high-pass everything that isn't kick or bass at 60-100 Hz. Sub eats headroom you could spend on loudness."
                         : "Very little under 60 Hz for " + genre + ". A low shelf under 60 Hz on the bass, or more kick sub, gives the mix a floor (check on headphones or a sub, laptop speakers won't show it).";
            if (! hot) { s.plugin = plug::dominatrix; s.tip = "IRON saturation adds transformer weight to the low end without boosting rumble."; }
            break;
        case 1:
            s.title = hot ? "BOOMY LOW END" : "THIN BOTTOM";
            s.body = hot ? "60-120 Hz is piling up: kick and bass are probably fighting. Give each its own spot (kick around 60 Hz, bass around 100 Hz, or the other way round) and cut the other one there."
                         : "60-120 Hz is light for " + genre + ". Try a 2-3 dB low shelf around 90 Hz on the bass or kick, or turn the bass up a touch.";
            s.plugin = plug::dominatrix;
            s.tip = hot ? "Turn BASS MONO on so the low end stays tight and centred." : "IRON saturation fattens the bottom end.";
            break;
        case 2:
            s.title = hot ? "WARM TO MUDDY" : "LACKS BODY";
            s.body = hot ? "120-250 Hz is crowded. Guitars, bass and snare body all live here. A wide 2-3 dB cut around 200 Hz on the rhythm guitars usually clears it."
                         : "120-250 Hz is light, so the mix lacks body: snare fundamental, guitar and vocal chest. Try a gentle boost around 180 Hz on the guitar or vocal bus.";
            break;
        case 3:
            s.title = hot ? "MUDDY / BOXY MIDS" : "SCOOPED MIDS";
            s.body = hot ? "250-500 Hz is the classic mud zone and it's hot here. Sweep for boxiness at 300-400 Hz on guitars, toms and vocals and cut 2-4 dB with a medium Q."
                         : "250-500 Hz is scooped" + juce::String (heavy ? " (the classic heavy-guitar trap)" : "") + ". Guitars and vocals will disappear on small speakers and phones. Bring some back on the guitars around 400 Hz.";
            break;
        case 4:
            s.title = hot ? "HONKY MIDS" : "HOLLOW MIDS";
            s.body = hot ? "500 Hz-1 kHz is forward: a honky, nasal tone. Check guitars and vocals around 700-900 Hz."
                         : "500 Hz-1 kHz is thin and the mix sounds hollow. Guitars and snare usually carry this range, so try a small boost there.";
            break;
        case 5:
            s.title = hot ? "PUSHY UPPER MIDS" : "DISTANT UPPER MIDS";
            s.body = hot ? "1-2 kHz is pushy. This can read as aggressive or as cheap. Check distorted guitars and the vocal around 1.2-1.6 kHz."
                         : "1-2 kHz is laid back, so vocals and guitars lose definition. A little boost on the vocal or guitars around 1.5 kHz brings them forward.";
            if (! hot && gid == "pop") { s.plugin = plug::flatliner; s.tip = "If the vocal is getting buried, ride it steady with Flatliner before reaching for EQ."; }
            break;
        case 6:
            s.title = hot ? "HARSH PRESENCE" : "DULL PRESENCE";
            s.body = hot ? "2-4 kHz is hot. That's where ears are most sensitive, so it gets tiring fast: distorted guitars, cymbals and vocal edge. Dynamic EQ or a 1-2 dB cut at 2.5-3.5 kHz on the guitar bus is the usual fix."
                         : "2-4 kHz is soft, so vocals and guitars lack bite and the mix sounds far away. Try a 2 dB boost around 3 kHz on the vocal and guitars.";
            s.plugin = plug::dominatrix;
            s.tip = hot ? "TAPE saturation smooths and softens a harsh top end." : "WHIP saturation adds bite and edge.";
            if (! hot && gid == "pop") { s.plugin = plug::flatliner; s.tip = "If the vocal is the thing that's missing, Flatliner keeps it riding on top of the track."; }
            break;
        case 7:
            s.title = hot ? "SIZZLY / SIBILANT" : "NO EDGE";
            s.body = hot ? "4-8 kHz is sizzly: sibilance (S and T sounds) and splashy cymbals. De-ess the vocal and tame the hi-hats and overheads."
                         : "4-8 kHz is soft. Snare crack, pick attack and vocal clarity live here; a small high shelf from 5 kHz helps.";
            if (! hot) { s.plugin = plug::dominatrix; s.tip = "WHIP saturation brings up edge without EQ."; }
            break;
        case 8:
            s.title = hot ? "BRITTLE TOP" : "MISSING SHEEN";
            s.body = hot ? "8-12 kHz is bright and brittle, usually cymbals and hats. Pull the overheads down or shelve them 1-2 dB."
                         : "8-12 kHz is dark for " + genre + ". A gentle high shelf around 10 kHz on the mix bus adds sheen.";
            if (hot) { s.plugin = plug::dominatrix; s.tip = "TAPE saturation rounds off the very top."; }
            break;
        default:
            s.title = hot ? "TOO MUCH AIR" : "NO AIR";
            s.body = hot ? "12-20 kHz is high. If the quiet parts also hiss, it may be noise rather than air."
                         : "12-20 kHz is low, which can sound closed-in" + juce::String (gid == "pop" ? " (pop usually has plenty of air)" : "") + ". Try a high shelf at 12-16 kHz.";
            if (hot) { s.plugin = plug::buzzKill; s.tip = "If it's amp hiss, put BuzzKill on the guitar tracks."; }
            break;
    }
}

inline Report advise (const Features& x, const GenreProfile& g)
{
    Report r;
    r.genreName = g.name;
    const auto G = g.name;
    auto add = [&] (Sev sev, const juce::String& area, const juce::String& title, const juce::String& body,
                    const juce::String& plugin = {}, const juce::String& tip = {}, float w = 0.0f)
    {
        r.items.push_back ({ sev, area, title, body, plugin, tip, w });
    };

    if (! x.valid)
    {
        add (Sev::info, "LISTEN", "PLAY YOUR SONG",
             "Put The Master last on your master channel and play the song. Best is the whole song, or at least 30 seconds of the loudest chorus. The report builds as it listens.");
        return r;
    }
    r.ready = true;
    for (int f = 0; f < numFeatures; ++f) r.z[(size_t) f] = g.z (f, x.get (f));
    float penalty = 0.0f;
    auto pen = [&] (float z, float w) { penalty += w * 6.0f * juce::jlimit (0.0f, 3.0f, std::abs (z) - 1.0f); };
    auto sevFor = [] (float z) { return std::abs (z) >= 2.5f ? Sev::fix : Sev::check; };

    if (x.seconds < 30.0)
        add (Sev::info, "LISTEN", "KEEP PLAYING",
             "Only " + juce::String ((int) x.seconds) + " seconds heard so far. Loudness and dynamics readings settle after about 30 seconds; the whole song is best.");

    // ---- loudness
    {
        const auto st = g.stat (fLufs);
        const float z = r.z[fLufs], d = x.lufs - st.mean;
        pen (z, 1.0f);
        if (z <= -1.5f)
            add (sevFor (z), "LOUDNESS", "QUIETER THAN " + G.toUpperCase(),
                 "Integrated loudness is " + fmt1 (x.lufs) + " LUFS. " + G + " masters usually sit at " + rangeOf (st, 1, " LUFS")
                     + ", so you're about " + fmt1 (-d) + " dB short. Get there with glue and clipping, not just a harder limiter.",
                 plug::dominatrix, "Set CEILING to -1 dB and push OUTPUT into the clipper about " + fmt1 (-d) + " dB. For more, put Chop Shop in MASTER mode before it.", std::abs (z));
        else if (z >= 1.5f)
            add (sevFor (z), "LOUDNESS", "LOUDER THAN " + G.toUpperCase(),
                 "Integrated loudness is " + fmt1 (x.lufs) + " LUFS, hotter than " + G + " usually goes (" + rangeOf (st, 1, " LUFS")
                     + "). Streaming turns loud masters down anyway (Spotify and YouTube to about -14 LUFS), so the extra level only costs you punch.",
                 plug::dominatrix, "Back OUTPUT off about " + fmt1 (d) + " dB (or ease off your limiter) and compare with MATCH on.", std::abs (z));
        else
            add (Sev::good, "LOUDNESS", "LOUDNESS ON TARGET", fmt1 (x.lufs) + " LUFS sits right in the " + G + " range (" + rangeOf (st, 1, " LUFS") + ").");
    }

    // ---- peaks / clipping
    if (x.truePeak > -1.0f)
    {
        penalty += x.truePeak > 0.0f ? 12.0f : 6.0f;
        add (x.truePeak > 0.0f ? Sev::fix : Sev::check, "PEAKS", "TRUE PEAKS OVER -1 dBTP",
             "Peaks reach " + signedDb (x.truePeak) + "TP between samples. When streaming services encode to MP3/AAC these can turn into audible clipping; they ask for -1 dBTP.",
             plug::dominatrix, "Set CEILING to -1 dB. It's a true-peak ceiling, so nothing gets past it.", 2.0f + x.truePeak);
    }
    else
        add (Sev::good, "PEAKS", "PEAKS ARE SAFE", "True peak " + signedDb (x.truePeak) + "TP, under the -1 dBTP streaming limit.");

    if (x.clipEvents >= 3)
    {
        penalty += juce::jmin (15.0f, 3.0f + (float) x.clipEvents * 0.2f);
        add (x.clipEvents >= 20 ? Sev::fix : Sev::check, "PEAKS", "DIGITAL CLIPPING",
             juce::String (x.clipEvents) + " spots where the waveform flat-tops at full scale. That's hard digital clipping before The Master, usually a mix bus or limiter running into 0 dBFS. Pull the master fader down 1-2 dB.",
             plug::chopShop, "If you want the clipping, do it on purpose: Chop Shop in MASTER mode with SOFT clipping sounds far cleaner.", (float) x.clipEvents);
    }

    // ---- dynamics (PLR)
    {
        const auto st = g.stat (fPlr);
        const float z = r.z[fPlr];
        pen (z, 1.2f);
        if (z <= -1.5f)
            add (sevFor (z), "DYNAMICS", "OVER-SQUASHED",
                 "Peaks sit only " + fmt1 (x.plr) + " dB above the average level (" + G + " usually has " + rangeOf (st, 1, " dB")
                     + "). The transients are flattened, so the drums lose snap and the mix sounds smaller even though it's loud.",
                 plug::dominatrix, "Ease DISCIPLINE back to around 20-30 % and lower OUTPUT, or take a dB or two off the limiter.", std::abs (z));
        else if (z >= 1.5f)
            add (sevFor (z), "DYNAMICS", "PEAKS STICKING OUT",
                 "Peaks jump " + fmt1 (x.plr) + " dB over the average level, more than " + G + " usually allows (" + rangeOf (st, 1, " dB")
                     + "). Usually it's the kick and snare spiking, which stops the mix getting loud.",
                 plug::chopShop, "MASTER mode shaves 2-3 dB off the drum peaks without sounding squashed. Then Dominatrix DISCIPLINE at 30-50 % glues the rest.", std::abs (z));
        else
            add (Sev::good, "DYNAMICS", "PUNCH IS RIGHT", fmt1 (x.plr) + " dB peak-to-loudness, typical for " + G + ".");
    }

    // ---- loudness range
    {
        const auto st = g.stat (fLra);
        const float z = r.z[fLra];
        pen (z, 0.5f);
        if (z >= 1.5f)
            add (Sev::check, "DYNAMICS", "BIG LEVEL SWINGS",
                 "Loudness range is " + fmt1 (x.lra) + " LU (" + G + " is usually " + rangeOf (st, 1, " LU")
                     + "). Quiet sections may vanish in the car or on a phone. Ride the quiet parts up, or glue the whole thing more.",
                 plug::flatliner, "If it's the vocal jumping around, Flatliner rides it level. For the whole mix, Dominatrix DISCIPLINE.", std::abs (z));
        else if (z <= -2.0f)
            add (Sev::check, "DYNAMICS", "VERY FLAT ARRANGEMENT",
                 "Loudness range is only " + fmt1 (x.lra) + " LU, so every section hits at the same level. Let verses breathe a little so the choruses lift.",
                 {}, {}, std::abs (z));
    }

    // ---- stereo
    {
        const auto st = g.stat (fWidth);
        const float z = r.z[fWidth];
        pen (z, 0.7f);
        if (x.correlation < 0.0f)
        {
            penalty += 12.0f;
            add (Sev::fix, "STEREO", "OUT OF PHASE",
                 "Left and right are working against each other (correlation " + juce::String (x.correlation, 2)
                     + "). In mono, on a phone or a club system, parts of the mix will cancel out. Check for a flipped-polarity track or an extreme stereo widener.",
                 plug::dominatrix, "Bring WIDTH under 100 % and watch the PHASE meter stay on the right side.", 9.0f);
        }
        else if (z <= -1.5f)
            add (sevFor (z), "STEREO", "NARROW FOR " + G.toUpperCase(),
                 "The mix is narrower than " + G + " usually is. Double-tracked rhythm guitars panned hard left and right are the standard way to get width.",
                 plug::dominatrix, "WIDTH at 110-130 % widens it. The lows under 120 Hz stay mono, so the punch stays.", std::abs (z));
        else if (z >= 1.5f || x.correlation < 0.3f)
            add (Sev::check, "STEREO", "VERY WIDE",
                 "The sides are loud compared with the centre (correlation " + juce::String (x.correlation, 2) + "). It can sound huge on headphones and thin or phasey in mono.",
                 plug::dominatrix, "Pull WIDTH back to 90-100 % and check the PHASE meter.", std::abs (z));
        else
            add (Sev::good, "STEREO", "STEREO WIDTH IS GOOD", "The width is typical for " + G + " and the mix holds up in mono.");

        if (x.lowCorrelation < 0.8f)
        {
            penalty += x.lowCorrelation < 0.5f ? 10.0f : 6.0f;
            add (x.lowCorrelation < 0.5f ? Sev::fix : Sev::check, "LOW END", "LOW END ISN'T MONO",
                 "Below 120 Hz the left and right channels differ (correlation " + juce::String (x.lowCorrelation, 2)
                     + "). Stereo bass loses punch, and in mono or on vinyl it partly cancels. Kick and bass belong dead centre.",
                 plug::dominatrix, "Turn BASS MONO on.", 5.0f);
        }
        else
            add (Sev::good, "LOW END", "LOW END IS MONO", "Kick and bass sit solidly in the centre.");
    }

    // ---- tonal balance (the 3 biggest misses)
    {
        std::vector<std::pair<float, int>> off;
        int inRange = 0;
        float bandPen = 0;
        for (int b = 0; b < kNumBands; ++b)
        {
            const float z = r.z[(size_t) (fBand0 + b)];
            bandPen += 0.8f * 6.0f * juce::jlimit (0.0f, 3.0f, std::abs (z) - 1.0f);
            if (std::abs (z) >= 1.5f) off.push_back ({ std::abs (z), b }); else ++inRange;
        }
        penalty += juce::jmin (30.0f, bandPen);
        std::sort (off.begin(), off.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        for (int i = 0; i < juce::jmin (3, (int) off.size()); ++i)
        {
            const int b = off[(size_t) i].second;
            const float z = r.z[(size_t) (fBand0 + b)];
            const float d = x.band[(size_t) b] - g.stat (fBand0 + b).mean;
            Suggestion s;
            s.sev = sevFor (z);
            s.area = juce::String ("TONE ") + bandDefs()[(size_t) b].range + " Hz";
            s.weight = std::abs (z);
            bandAdvice (b, z > 0, G, g.id, s);
            s.body << " (" << signedDb (d) << " vs. typical " << G << ")";
            r.items.push_back (s);
        }
        if (inRange >= 8)
            add (Sev::good, "TONE", "TONAL BALANCE IN RANGE", juce::String (inRange) + " of 10 bands sit where " + G + " records sit.");
        if (off.size() > 3)
            add (Sev::info, "TONE", "ONE THING AT A TIME",
                 juce::String ((int) off.size()) + " bands are off. Bands move together, so fix the biggest one first and listen again before chasing the rest.");
    }

    // ---- hum
    if (x.humSeconds >= 1.0f)
    {
        penalty += 6.0f;
        add (x.humSeconds >= 3.0f ? Sev::fix : Sev::check, "NOISE", juce::String (x.humHz) + " Hz HUM IN THE QUIET PARTS",
             "Mains hum (" + juce::String (x.humHz) + " Hz plus harmonics, " + fmt1 (x.humDb) + " dB above the noise) shows up in "
                 + juce::String ((int) x.humSeconds) + " s of the gaps, count-ins or ending. Usually it's a guitar amp or a bass DI.",
             plug::buzzKill, "Put BuzzKill on the guitar and bass tracks, not the master. It removes the hum and keeps the sustain.", 4.0f);
    }

    r.score = juce::jlimit (0, 100, (int) std::lround (100.0f - penalty));
    std::stable_sort (r.items.begin(), r.items.end(), [] (const Suggestion& a, const Suggestion& b)
    {
        if (a.sev != b.sev) return (int) a.sev < (int) b.sev;
        return a.weight > b.weight;
    });
    for (auto& s : r.items)
    {
        if (s.sev == Sev::fix) ++r.fixes;
        else if (s.sev == Sev::check) ++r.checks;
        else if (s.sev == Sev::good) ++r.goods;
    }
    // info notes (keep playing, etc.) go first: they explain how to read the rest
    std::stable_partition (r.items.begin(), r.items.end(), [] (const Suggestion& s) { return s.sev == Sev::info && s.area == "LISTEN"; });
    return r;
}
} // namespace mst
