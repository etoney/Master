#pragma once
// THE MASTER - the other Castle Danger plugins on your tracks: BUZZKILL (noise / hum), FLATLINER
// (rides a lead that jumps around), LOW BLOW (kick/bass ducker), CHOP SHOP (clips drum peaks at the
// source) and DOMINATRIX (saturates a thin bass). Each one reports what it hears on its own track;
// The Master sets it from that, from the track's ROLE (its Servant's) and from the genre.
#include "Tracks.h"

namespace mst
{
struct HelperInfo
{
    juce::String id, kind, track, role;   // role: from The Servant on the same track, if there is one
    juce::var extra;                      // what the plugin reported
};

inline juce::String genreKey (const GenreProfile& g)
{
    const auto s = (g.id.isNotEmpty() ? g.id : g.name).toLowerCase().removeCharacters (" _-");
    return s;
}

inline TrackPlan makeHelperPlan (const HelperInfo& h, const juce::String& fallbackName)
{
    TrackPlan p;
    p.kind = h.kind; p.id = h.id; p.role = h.role;
    p.name = h.track.isNotEmpty() ? h.track : fallbackName;
    p.share = -1.0f;
    return p;
}

//------------------------------------------------------------------------------
// BUZZKILL: it learns the noise by itself. The Master picks how tight it closes, from the genre
// (metal and punk palm mutes want a hard stop, pop and ballads want tails), the part (a lead's
// sustain breathes, a rhythm part chugs) and how noisy the track really is.
// a track whose noise is too quiet to hear: BuzzKill has nothing to remove there
constexpr double kCleanFloorDb = -75.0;
inline bool buzzKillClean (const juce::var& extra)
{
    const double floorDb = extra.getProperty ("floorDb", -120.0);
    return floorDb > -119.0 && floorDb < kCleanFloorDb;
}

inline TrackPlan planBuzzKill (const HelperInfo& h, const Features& mix, const GenreProfile& g)
{
    auto p = makeHelperPlan (h, "Guitar track");
    const auto G = genreKey (g);
    float rel = 0.40f;
    juce::String style = "a natural close for " + g.name;
    if (G == "metal")         { rel = 0.75f; style = "tight, so palm mutes stop dead, like " + g.name + " records"; }
    else if (G == "punk")     { rel = 0.65f; style = "tight, for fast stops and chugs in " + g.name; }
    else if (G == "hardrock") { rel = 0.60f; style = "fairly tight, for " + g.name + " riffs"; }
    else if (G == "rock")     { rel = 0.45f; style = "between tight and natural, for " + g.name; }
    else if (G == "pop")      { rel = 0.30f; style = "natural, so the tails fade gently, like " + g.name + " records"; }

    if (roleIs (h.role, { "Lead" }))        { rel -= 0.15f; style << "; a little gentler on a lead so its sustain breathes"; }
    else if (roleIs (h.role, { "Bass" }))   { rel -= 0.10f; style << "; a little gentler on bass so the notes don't choke"; }
    else if (roleIs (h.role, { "Rhythm" })) { rel += 0.05f; }

    const double floorDb = h.extra.getProperty ("floorDb", -120.0);
    const double playDb = h.extra.getProperty ("playDb", -120.0);
    const double secs = h.extra.getProperty ("seconds", 0.0);
    const bool learned = (bool) h.extra.getProperty ("learned", false);
    const double snr = playDb - floorDb;
    if (learned && secs >= 2.0 && playDb > -100.0 && snr < 30.0) { rel += 0.10f; style << "; closing a bit sooner because this track is noisy"; }
    rel = juce::jlimit (0.10f, 0.90f, std::round (rel * 20.0f) / 20.0f);

    p.params["release"] = rel;
    p.params["listen"] = 0.0f;   // HEAR REMOVED off: never print the noise by mistake
    p.why.add ("RELEASE " + juce::String (juce::roundToInt (rel * 100.0f)) + "%: " + style + ".");
    if (buzzKillClean (h.extra))
    {
        p.why.add ("CLEAN: this track's noise sits at " + juce::String (floorDb, 0) + " dBFS, far too quiet to hear, so BuzzKill has nothing to remove here."
                   + (mix.humSeconds >= 1.0f ? juce::String (" The mix's " + juce::String (mix.humHz) + " Hz hum comes from another track.") : juce::String()));
        p.why.add ("HEAR REMOVED: OFF (so you never print the noise by mistake).");
        p.note = "RELEASE " + juce::String (juce::roundToInt (rel * 100.0f)) + "%  //  CLEAN, NOTHING TO REMOVE";
        return p;
    }
    if (mix.humSeconds >= 1.0f)
        p.why.add ("The mix has " + juce::String (mix.humHz) + " Hz hum in its quiet parts (" + juce::String ((int) mix.humSeconds)
                   + " s). BuzzKill removes it here, at the source, so The Master doesn't have to notch the whole mix.");
    if (learned && playDb > -100.0)
        p.why.add ("This track's noise floor is " + juce::String (floorDb, 0) + " dBFS, " + juce::String (snr, 0) + " dB under your playing"
                   + (snr < 30.0 ? ": noisy, worth gating." : "."));
    else
        p.why.add ("BuzzKill hasn't heard this track's noise yet: play a second of silence (nobody playing) and it learns it by itself.");
    p.why.add ("HEAR REMOVED: OFF (so you never print the noise by mistake).");
    p.note = "RELEASE " + juce::String (juce::roundToInt (rel * 100.0f)) + "%"
           + (learned && playDb > -100.0 ? "  //  NOISE " + juce::String (floorDb, 0) + " dB" : juce::String ("  //  NOISE NOT LEARNED YET"));
    return p;
}

//------------------------------------------------------------------------------
// FLATLINER: rides the lead's level. It reports its own input level distribution (100 ms RMS, the
// parts that are playing): the 10th / 50th / 90th percentiles and the floor between phrases.
inline float leadSwingDb (const std::vector<float>& series)
{
    std::vector<float> act;
    float mx = 0; for (auto v : series) mx = std::max (mx, v);
    if (mx <= 0) return 0.0f;
    for (auto v : series) if (v > mx * 1e-4f) act.push_back (10.0f * std::log10 (v));   // within 40 dB of the loudest: playing
    if (act.size() < 30) return 0.0f;
    std::sort (act.begin(), act.end());
    return act[act.size() * 9 / 10] - act[act.size() / 10];
}

inline TrackPlan planFlatliner (const HelperInfo& h, const GenreProfile& g)
{
    auto p = makeHelperPlan (h, "Lead track");
    const double secs = h.extra.getProperty ("seconds", 0.0);
    if (secs < 5.0)
    {
        p.why.add ("Flatliner has only heard " + juce::String (secs, 1) + " s of this track. Play the song again, then press UPDATE.");
        p.note = "NOT ENOUGH HEARD YET";
        return p;
    }
    const float p10 = (float) (double) h.extra.getProperty ("p10", -40.0);
    const float p50 = (float) (double) h.extra.getProperty ("p50", -20.0);
    const float p90 = (float) (double) h.extra.getProperty ("p90", -12.0);
    const float floor = (float) (double) h.extra.getProperty ("floorDb", -80.0);
    const float swing = p90 - p10;
    const auto G = genreKey (g);

    float factor = 0.6f, speed = 50.0f;
    if (G == "pop")           { factor = 0.75f; speed = 60.0f; }
    else if (G == "metal")    { factor = 0.65f; speed = 55.0f; }
    else if (G == "hardrock") { factor = 0.60f; speed = 50.0f; }
    else if (G == "rock")     { factor = 0.55f; speed = 45.0f; }
    else if (G == "punk")     { factor = 0.50f; speed = 40.0f; }

    const bool steady = swing < 6.0f;
    const float range = steady ? 3.0f : juce::jlimit (3.0f, 12.0f, std::round (swing * factor));
    const float target = juce::jlimit (-40.0f, 0.0f, std::round (p50 * 2.0f) / 2.0f);
    float gate = std::max (floor + 6.0f, -80.0f);
    gate = std::min (gate, p10 - 6.0f);
    gate = juce::jlimit (-80.0f, -20.0f, std::round (gate));

    p.params["ride"] = 1.0f;
    p.params["target"] = target;
    p.params["range"] = range;
    p.params["speed"] = speed;
    p.params["gate"] = gate;
    p.params["output"] = 0.0f;

    p.why.add ("RANGE " + juce::String (range, 0) + " dB: " + (steady
               ? juce::String ("the lead is already steady (it swings only ") + juce::String (swing, 1) + " dB), so just a gentle ride."
               : juce::String ("the lead swings ") + juce::String (swing, 1) + " dB between its quiet and loud parts; " + g.name
                 + (G == "pop" ? " keeps the vocal very flat." : " keeps the lead steady, with some life left in it.")));
    p.why.add ("TARGET " + juce::String (target, 1) + " dB: where the lead already sits on average, so the ride doesn't change its level. "
               "The Servant's TRIM does the balance against the band.");
    p.why.add ("SPEED " + juce::String (juce::roundToInt (speed)) + "%: " + (speed >= 55.0f ? "quick, to catch every word." : "natural, like a patient hand on the fader."));
    p.why.add ("GATE " + juce::String (gate, 0) + " dB: the gaps between phrases sit around " + juce::String (floor, 0)
               + " dB; below the gate the fader freezes, so breaths and room noise aren't pushed up.");
    p.note = "RANGE " + juce::String (range, 0) + " dB  //  TARGET " + juce::String (target, 1) + " dB  //  SWING WAS " + juce::String (swing, 0) + " dB";
    return p;
}
//------------------------------------------------------------------------------
// LOW BLOW: ducks the bass under each kick (the kick comes in on its sidechain).
// The clash comes from the Kick and Bass Servants: how close their levels are in the same low bands.
struct LowClash
{
    bool measured = false, found = false;
    int band = 1;                 // the low band where they fight most (0 SUB, 1 BASS, 2 LOW)
    int kickBand = 1;             // where the kick's thump sits
    float gapDb = 99.0f;          // level difference between kick and bass in that band (small = they fight)
    juce::String kickName, bassName;
};

inline LowClash findLowClash (const std::vector<TrackInfo>& tracks)
{
    LowClash c;
    const TrackInfo *kick = nullptr, *bass = nullptr;
    for (auto& t : tracks)
    {
        if (kick == nullptr && roleIs (t.role, { "Kick" })) kick = &t;
        if (bass == nullptr && roleIs (t.role, { "Bass" })) bass = &t;
    }
    if (kick == nullptr) for (auto& t : tracks) if (roleIs (t.role, { "Drums" })) { kick = &t; break; }
    if (kick == nullptr || bass == nullptr) return c;
    c.measured = true;
    c.kickName = kick->name.isNotEmpty() ? kick->name : juce::String ("the kick");
    c.bassName = bass->name.isNotEmpty() ? bass->name : juce::String ("the bass");
    auto db = [] (double p) { return (float) (10.0 * std::log10 (std::max (1e-14, p))); };
    float kMax = -200, bMax = -200;
    for (int b = 0; b < 3; ++b)
    {
        kMax = std::max (kMax, db (kick->bandPow[(size_t) b]));
        bMax = std::max (bMax, db (bass->bandPow[(size_t) b]));
        if (db (kick->bandPow[(size_t) b]) >= kMax) c.kickBand = b;
    }
    for (int b = 0; b < 3; ++b)
    {
        const float k = db (kick->bandPow[(size_t) b]), s = db (bass->bandPow[(size_t) b]);
        if (k < kMax - 10.0f || s < bMax - 10.0f) continue;          // only bands that matter to both
        const float gap = std::abs (k - s);
        if (gap < c.gapDb) { c.gapDb = gap; c.band = b; }
    }
    c.found = c.gapDb <= 10.0f;
    return c;
}

inline TrackPlan planLowBlow (const HelperInfo& h, const LowClash& clash, const GenreProfile& g)
{
    auto p = makeHelperPlan (h, "Bass track");
    const auto G = genreKey (g);
    float depth = 5.0f, release = 90.0f, hold = 15.0f;
    if (G == "metal")         { depth = 6.0f; release = 50.0f; hold = 5.0f; }
    else if (G == "punk")     { depth = 5.0f; release = 60.0f; hold = 10.0f; }
    else if (G == "hardrock") { depth = 5.0f; release = 70.0f; }
    else if (G == "rock")     { depth = 4.0f; release = 90.0f; }
    else if (G == "pop")      { depth = 6.0f; release = 120.0f; hold = 20.0f; }
    juce::String depthWhy = juce::String (depth, 0) + " dB is typical for " + g.name;
    if (clash.measured)
    {
        if (clash.found && clash.gapDb < 3.0f) { depth += 1.0f; depthWhy << ", one more because " << clash.kickName << " and " << clash.bassName << " are almost exactly as loud in the same range"; }
        else if (! clash.found)                { depth = std::max (2.0f, depth - 2.0f); depthWhy << ", but they hardly overlap here, so a lighter touch"; }
    }

    // crossover: just above where the kick's thump sits
    float xo = 120.0f;
    juce::String xoWhy = "the usual split between the kick's thump and the bass's growl";
    if (clash.measured)
    {
        static const float xos[] = { 90.0f, 140.0f, 160.0f };
        static const char* where[] = { "in the sub (20-60 Hz)", "around 60-120 Hz", "around 120-250 Hz" };
        xo = xos[clash.kickBand];
        xoWhy = clash.kickName + "'s thump sits " + where[clash.kickBand] + ", so only the bass below " + juce::String (juce::roundToInt (xo)) + " Hz makes way; its growl stays";
    }

    // release: back before the next hit
    const double gap = h.extra.getProperty ("gapMs", 0.0);
    juce::String relWhy = "a natural recovery for " + g.name;
    if (gap >= 80.0)
    {
        release = juce::jlimit (30.0f, 200.0f, std::round ((float) gap * 0.3f));
        relWhy = "your kicks land about every " + juce::String (juce::roundToInt ((float) gap)) + " ms, so the bass is fully back well before the next one";
    }

    p.params["depth"] = depth; p.params["attack"] = 1.0f; p.params["hold"] = hold; p.params["release"] = release;
    p.params["sensitivity"] = 60.0f; p.params["mode"] = 0.0f; p.params["crossover"] = xo; p.params["lookahead"] = 1.0f; p.params["listen"] = 0.0f;

    if (clash.measured && clash.found)
        p.why.add (clash.kickName + " and " + clash.bassName + " fight " + juce::String (bandDefs()[(size_t) clash.band].label) + " ("
                   + bandDefs()[(size_t) clash.band].range + " Hz): their levels there are only " + juce::String (clash.gapDb, 1) + " dB apart.");
    else if (! clash.measured)
        p.why.add ("No Servant with ROLE Kick (or Drums) and Bass, so The Master can't measure the clash: genre settings. Put The Servant on both for a closer fit.");
    p.why.add ("DEPTH -" + juce::String (depth, 0) + " dB: " + depthWhy + ".");
    p.why.add ("CROSSOVER " + juce::String (juce::roundToInt (xo)) + " Hz (LOWS ONLY): " + xoWhy + ".");
    p.why.add ("RELEASE " + juce::String (juce::roundToInt (release)) + " ms, HOLD " + juce::String (juce::roundToInt (hold)) + " ms: " + relWhy + ".");
    const bool side = (bool) h.extra.getProperty ("sidechain", false);
    if (! side)
        p.why.add ("Low Blow hears no kick on its sidechain yet. Send the kick track to Low Blow's sidechain input (in Studio Pro: the sidechain button in Low Blow's window). Until then it leaves the bass alone.");
    p.note = "DEPTH -" + juce::String (depth, 0) + " dB  //  " + juce::String (juce::roundToInt (xo)) + " Hz  //  REL " + juce::String (juce::roundToInt (release)) + " ms"
           + (side ? juce::String() : juce::String ("  //  NO KICK ON SIDECHAIN"));
    return p;
}

//------------------------------------------------------------------------------
// CHOP SHOP ON A DRUM TRACK: clip the hits that make the mix's peaks at the source. A clipper shaves
// the spike off each hit and keeps the punch, where a compressor softens it; The Servant's
// compressor on that track is switched off so the two don't double up.
inline bool wantsDrumClip (const TrackPlan& sv)
{
    return sv.kind == "servant" && sv.compOn && roleIs (sv.role, { "Kick", "Snare", "Drums" });
}

inline TrackPlan planTrackChop (const HelperInfo& h, const TrackInfo* t, const GenreProfile& g)
{
    auto p = makeHelperPlan (h, "Drum track");
    const bool kit = roleIs (h.role, { "Drums" });
    const auto G = genreKey (g);
    const double peak = h.extra.getProperty ("songPeakDb", -100.0);
    const double secs = h.extra.getProperty ("seconds", 0.0);
    if (secs < 5.0 || peak <= -90.0)
    {
        p.why.add ("Chop Shop on this track hasn't heard the song yet: press RESET on The Master, play the song, then UPDATE.");
        p.note = "NOT HEARD YET";
        return p;
    }
    float shave = 3.0f;
    juce::String shaveWhy = "a typical shave for a drum track";
    if (t != nullptr && t->transSeconds >= 5.0f)
    {
        shave = juce::jlimit (2.0f, 6.0f, std::round (t->transP95 * 0.3f * 2.0f) / 2.0f);
        shaveWhy = "its hits stick out " + juce::String (t->transP95, 0) + " dB over its body";
    }
    if (G == "metal" || G == "punk") { shave = std::min (6.0f, shave + 1.0f); shaveWhy << ", a little deeper for " << g.name; }
    const float ceiling = juce::jlimit (-24.0f, 0.0f, std::round ((float) (peak - shave) * 10.0f) / 10.0f);

    p.params["mode"] = 0.0f; p.params["shape"] = kit ? 1.0f : 0.0f; p.params["inputDb"] = 0.0f; p.params["ceilingDb"] = ceiling;
    p.params["color"] = 0.0f; p.params["mix"] = 100.0f; p.params["outputDb"] = 0.0f; p.params["oversample"] = 1.0f;
    p.params["gainMatch"] = 0.0f; p.params["scraps"] = 0.0f;

    p.why.add ("CEILING " + juce::String (ceiling, 1) + " dB: " + juce::String (shave, 1) + " dB under this track's loudest hit (" + juce::String (peak, 1)
               + " dBFS), because " + shaveWhy + ". Only the spikes are shaved.");
    p.why.add (kit ? "SHAPE SOFT: rounds the hits of a full kit, so cymbals don't crunch."
                   : "SHAPE HARD: a clean, flat cut keeps the crack of each hit.");
    p.why.add ("MODE MASTER, COLOR 0: transparent; nothing below the ceiling changes. OVERSAMPLING 2x keeps it clean without much CPU.");
    p.why.add ("The Servant's compressor on this track is off: Chop Shop handles its peaks, with more punch than compressing them.");
    p.note = "CEILING " + juce::String (ceiling, 1) + " dB  //  SHAVES " + juce::String (shave, 1) + " dB  //  " + (kit ? "SOFT" : "HARD");
    return p;
}

//------------------------------------------------------------------------------
// DOMINATRIX ON THE BASS: a bass whose 250 Hz - 2 kHz is thin all but disappears on phones and small
// speakers. Saturation adds the harmonics that let it be heard there, without changing its level.
inline float bassHarmonicsDb (const TrackInfo& t)
{
    const double low = t.bandPow[0] + t.bandPow[1], harm = t.bandPow[3] + t.bandPow[4] + t.bandPow[5];
    if (low <= 1e-12) return 0.0f;
    return (float) (10.0 * std::log10 (std::max (harm, 1e-12) / low));
}
constexpr float kThinBassDb = -10.0f;   // below this the bass needs harmonics to be heard on small speakers

inline TrackPlan planTrackDom (const HelperInfo& h, const TrackInfo* t, const GenreProfile& g)
{
    auto p = makeHelperPlan (h, "Bass track");
    const auto G = genreKey (g);
    const bool heavy = G == "metal" || G == "punk" || G == "hardrock";
    float sat = 25.0f;
    juce::String why = "no Servant measurement on this track, so a moderate amount";
    if (t != nullptr)
    {
        const float hd = bassHarmonicsDb (*t);
        sat = juce::jlimit (15.0f, 45.0f, std::round (25.0f + (kThinBassDb - hd) * 3.0f));
        why = "its 250 Hz - 2 kHz sits " + juce::String (-hd, 1) + " dB under its lows" + (hd < kThinBassDb ? juce::String (", thin on small speakers") : juce::String (", already fairly audible"));
    }
    p.params["discipline"] = 0.0f; p.params["saturation"] = sat; p.params["satType"] = heavy ? 4.0f : 1.0f; p.params["analog"] = 0.0f;
    p.params["mix"] = 100.0f; p.params["width"] = 100.0f; p.params["bassMono"] = 0.0f; p.params["outputDb"] = 0.0f;
    p.params["ceiling"] = 2.0f; p.params["quality"] = 1.0f; p.params["match"] = 1.0f;
    p.why.add ("SATURATION " + juce::String (juce::roundToInt (sat)) + "% " + (heavy ? "WHIP" : "TUBE") + ": " + why + ". "
               + (heavy ? "WHIP adds the bite heavy bass needs to cut through guitars." : "TUBE adds warm, even harmonics."));
    p.why.add ("DISCIPLINE 0, CEILING OFF: no compression or clipping here (The Servant compresses the bass).");
    p.why.add ("MATCH ON: the level stays the same, so the mix balance doesn't move.");
    p.note = "SAT " + juce::String (juce::roundToInt (sat)) + "% " + (heavy ? "WHIP" : "TUBE") + "  //  MATCH ON";
    return p;
}
} // namespace mst
