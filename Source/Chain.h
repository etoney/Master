#pragma once
// THE MASTER - "MY PLUGINS" mode: a prescription of Castle Danger plugins for the master bus,
// the exact settings for each, and a plain-English reason for every change.
#include "Cure.h"

namespace mst
{
struct ChainChange
{
    juce::String plugin, param, label;   // plugin kind, parameter id, what the user sees ("OUTPUT")
    float from = 0, to = 0;              // 'from' is filled in when the plan is applied
    juce::String unit, why;
    bool hasFrom = false;
};

struct ChainStep
{
    juce::String kind, name, role;       // "chopshop", "Chop Shop", "Shaves the drum peaks..."
    std::map<juce::String, float> params;
};

struct ChainPlan
{
    bool valid = false;
    juce::String genreName;
    double seconds = 0;
    std::vector<ChainStep> steps;             // in order, first to last (The Master always comes after them)
    std::vector<ChainChange> changes;
    bool masterEq = false;                    // The Master's own TONE MATCH EQ is part of the plan
    FixParams masterFix;                      // its built-in settings in this mode (EQ + safety limiter only)
    juce::StringArray onTracks;               // advice for individual tracks (BuzzKill, Flatliner...)
    juce::StringArray cantFix;
    Features before;                          // what the plan was made from

    const ChainStep* step (const juce::String& kind) const
    {
        for (auto& s : steps) if (s.kind == kind) return &s;
        return nullptr;
    }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("genre", genreName);
        o->setProperty ("seconds", seconds);
        juce::Array<juce::var> st;
        for (auto& s : steps)
        {
            auto* so = new juce::DynamicObject();
            so->setProperty ("kind", s.kind); so->setProperty ("name", s.name); so->setProperty ("role", s.role);
            auto* po = new juce::DynamicObject();
            for (auto& [k, v] : s.params) po->setProperty (k, (double) v);
            so->setProperty ("params", juce::var (po));
            st.add (juce::var (so));
        }
        o->setProperty ("steps", st);
        juce::Array<juce::var> ch;
        for (auto& c : changes)
        {
            auto* co = new juce::DynamicObject();
            co->setProperty ("plugin", c.plugin); co->setProperty ("param", c.param); co->setProperty ("label", c.label);
            co->setProperty ("from", (double) c.from); co->setProperty ("to", (double) c.to); co->setProperty ("hasFrom", c.hasFrom);
            co->setProperty ("unit", c.unit); co->setProperty ("why", c.why);
            ch.add (juce::var (co));
        }
        o->setProperty ("changes", ch);
        o->setProperty ("masterEq", masterEq);
        FixPlan fp; fp.p = masterFix;
        o->setProperty ("masterFix", fp.toVar());
        o->setProperty ("onTracks", onTracks.joinIntoString ("\n"));
        o->setProperty ("cantFix", cantFix.joinIntoString ("\n"));
        o->setProperty ("before", before.toVar());
        return juce::var (o);
    }

    static ChainPlan fromVar (const juce::var& v)
    {
        ChainPlan c;
        if (! v.isObject()) return c;
        c.valid = true;
        c.genreName = v.getProperty ("genre", "").toString();
        c.seconds = v.getProperty ("seconds", 0.0);
        if (auto* st = v.getProperty ("steps", {}).getArray())
            for (auto& sv : *st)
            {
                ChainStep s;
                s.kind = sv.getProperty ("kind", "").toString(); s.name = sv.getProperty ("name", "").toString(); s.role = sv.getProperty ("role", "").toString();
                if (auto* po = sv.getProperty ("params", {}).getDynamicObject())
                    for (auto& p : po->getProperties()) s.params[p.name.toString()] = (float) (double) p.value;
                c.steps.push_back (s);
            }
        if (auto* ch = v.getProperty ("changes", {}).getArray())
            for (auto& cv : *ch)
            {
                ChainChange x;
                x.plugin = cv.getProperty ("plugin", "").toString(); x.param = cv.getProperty ("param", "").toString(); x.label = cv.getProperty ("label", "").toString();
                x.from = (float) (double) cv.getProperty ("from", 0.0); x.to = (float) (double) cv.getProperty ("to", 0.0); x.hasFrom = cv.getProperty ("hasFrom", false);
                x.unit = cv.getProperty ("unit", "").toString(); x.why = cv.getProperty ("why", "").toString();
                c.changes.push_back (x);
            }
        c.masterEq = v.getProperty ("masterEq", false);
        c.masterFix = FixPlan::fromVar (v.getProperty ("masterFix", {})).p;
        c.onTracks = juce::StringArray::fromLines (v.getProperty ("onTracks", "").toString()); c.onTracks.removeEmptyStrings();
        c.cantFix = juce::StringArray::fromLines (v.getProperty ("cantFix", "").toString()); c.cantFix.removeEmptyStrings();
        c.before = Features::fromVar (v.getProperty ("before", {}));
        return c;
    }
};

inline juce::String plugDisplayName (const juce::String& kind)
{
    if (kind == "chopshop") return "Chop Shop";
    if (kind == "dominatrix") return "Dominatrix";
    if (kind == "master") return "The Master";
    return kind;
}

// The prescription, from your mix as it was before any of these plugins were on the bus.
inline ChainPlan planChain (const Features& in, const GenreProfile& g)
{
    ChainPlan c;
    if (! in.valid) return c;
    c.valid = true;
    c.genreName = g.name;
    c.seconds = in.seconds;
    c.before = in;
    const auto G = g.name;
    auto change = [&] (const juce::String& plugin, const juce::String& param, const juce::String& label, float to, const juce::String& unit, const juce::String& why)
    {
        c.changes.push_back ({ plugin, param, label, 0.0f, to, unit, why, false });
    };

    const float zPlr = g.z (fPlr, in.plr), zLra = g.z (fLra, in.lra);
    const auto L = g.stat (fLufs);
    float gain = L.mean - in.lufs;                         // loudness still to find
    const bool squashed = zPlr < -1.5f;
    if (squashed) gain = std::min (gain, 0.0f);

    // ---- CHOP SHOP: shave the peaks that stick out, so the rest of the chain can get loud cleanly
    float shave = 0.0f;
    if (zPlr > 0.8f) shave = juce::jlimit (1.5f, 6.0f, in.plr - g.stat (fPlr).mean + 1.0f);
    else if (gain > 4.0f && ! squashed) shave = 2.0f;
    if (shave > 0.0f)
    {
        ChainStep s { "chopshop", "Chop Shop", "Shaves the drum peaks that stick out, so Dominatrix can make the mix louder without squashing it." };
        const float ceiling = juce::jlimit (-24.0f, 0.0f, std::round ((in.samplePeak - shave) * 10.0f) / 10.0f);
        s.params = { { "mode", 0.0f }, { "shape", 1.0f }, { "inputDb", 0.0f }, { "ceilingDb", ceiling }, { "color", 0.0f },
                     { "mix", 100.0f }, { "outputDb", 0.0f }, { "oversample", 2.0f }, { "gainMatch", 0.0f }, { "scraps", 0.0f } };
        c.steps.push_back (s);
        const juce::String why = zPlr > 0.8f
            ? "Your peaks sit " + juce::String (in.plr, 1) + " dB over the average level; " + G + " is usually " + juce::String (g.stat (fPlr).mean, 1)
              + ". The ceiling goes " + juce::String (shave, 1) + " dB under your loudest peak (" + juce::String (in.samplePeak, 1) + " dBFS), so only the spikes get shaved."
            : "You need " + juce::String (gain, 1) + " dB more loudness. Shaving " + juce::String (shave, 1) + " dB off the loudest peaks first lets Dominatrix get there more cleanly.";
        change ("chopshop", "mode", "MODE", 0.0f, "MASTER", "Transparent mode for the master bus.");
        change ("chopshop", "shape", "SHAPE", 1.0f, "SOFT", "Soft knee rounds the peaks instead of cutting them flat: smoother on a full mix.");
        change ("chopshop", "ceilingDb", "CEILING", ceiling, "dB", why);
        change ("chopshop", "oversample", "OVERSAMPLING", 2.0f, "4x", "Stops clipping harmonics from folding back as harshness.");
        change ("chopshop", "color", "COLOR", 0.0f, "%", "No extra saturation here; Dominatrix handles the colour.");
    }

    // ---- DOMINATRIX: glue, tone colour, stereo, loudness into the -1 dB ceiling
    {
        ChainStep s { "dominatrix", "Dominatrix", "Glues the mix, adds colour, fixes the stereo picture, and brings it up to " + G + " loudness with a -1 dB ceiling." };
        // glue
        float discipline = 25.0f;
        juce::String whyDisc = "Gentle glue (0.5-2 dB of gain reduction) that most " + G + " masters have.";
        if (squashed) { discipline = 0.0f; whyDisc = "Your mix is already squashed (peaks only " + juce::String (in.plr, 1) + " dB over the average). No more compression."; }
        else if (zLra > 1.5f) { discipline = 50.0f; whyDisc = "Big level swings between sections (" + juce::String (in.lra, 1) + " LU, " + G + " is about " + juce::String (g.stat (fLra).mean, 1) + "). Firmer glue evens them out."; }
        else if (zPlr > 1.0f && shave == 0.0f) { discipline = 40.0f; whyDisc = "Peaks stick out more than " + G + " usually allows; firmer glue tames them."; }

        // colour, from the tone
        auto bz = [&] (int b) { return g.z (fBand0 + b, in.band[(size_t) b]); };
        const float top = (bz (6) + bz (7)) * 0.5f, low = (bz (1) + bz (2)) * 0.5f;
        float sat = 12.0f; int type = 3;   // CONSOLE: subtle summing glue
        juce::String whySat = "A touch of CONSOLE saturation for summing glue; your tone doesn't need more.";
        if (top >= 1.0f) { type = 0; sat = juce::jlimit (15.0f, 40.0f, 15.0f + top * 8.0f); whySat = "Presence/bite are hot (harsh) for " + G + ". TAPE saturation smooths and softens the top end."; }
        else if (top <= -1.0f) { type = 4; sat = juce::jlimit (15.0f, 40.0f, 15.0f - top * 8.0f); whySat = "Presence/bite are dull for " + G + ". WHIP saturation adds bite and edge."; }
        else if (low <= -1.0f) { type = 2; sat = juce::jlimit (15.0f, 35.0f, 15.0f - low * 7.0f); whySat = "The bass/low range is thin for " + G + ". IRON saturation adds weight to the low end."; }

        // stereo
        float width = 100.0f;
        juce::String whyWidth = "Stereo width is typical for " + G + ".";
        const float zW = g.z (fWidth, in.width);
        if (std::abs (zW) >= 1.2f)
        {
            width = std::round (juce::jlimit (60.0f, 150.0f, 100.0f * std::pow (10.0f, (g.stat (fWidth).mean - in.width) / 20.0f)));
            whyWidth = width > 100.0f ? "The mix is narrower than " + G + "; widening the sides (the lows below 120 Hz stay put)."
                                      : "The sides are louder than " + G + " usually has; narrowing a little makes it hold up in mono.";
        }
        if (in.correlation < 0.3f && in.lowCorrelation >= 0.9f && width > 80.0f) { width = 80.0f; whyWidth = "Left and right fight each other (correlation " + juce::String (in.correlation, 2) + "); narrowing makes it safer in mono."; }
        const bool bassMono = in.lowCorrelation < 0.9f;

        // loudness: OUTPUT drives Dominatrix's soft clipper into the -1 dB true-peak ceiling
        float out = gain;
        const float peakAfter = in.truePeak - shave + gain;
        if (peakAfter > -1.0f) out += juce::jmin (4.0f, (peakAfter + 1.0f) * 0.3f);   // the clipper eats some of the gain
        out = juce::jlimit (-12.0f, 12.0f, std::round (out * 10.0f) / 10.0f);
        juce::String whyOut = "Your mix is " + juce::String (in.lufs, 1) + " LUFS; " + G + " masters are about " + juce::String (L.mean, 1)
                            + ". OUTPUT pushes into the -1 dB ceiling to get there. Play the song again and press REFINE to land it exactly.";
        if (squashed) whyOut = "Already loud and squashed; no more drive.";
        if (gain > 12.0f) whyOut += " (Capped at +12 dB: turn the mix up before the master bus.)";

        s.params = { { "discipline", discipline }, { "saturation", sat }, { "satType", (float) type }, { "analog", 0.0f }, { "mix", 100.0f },
                     { "width", width }, { "bassMono", bassMono ? 1.0f : 0.0f }, { "outputDb", out }, { "ceiling", 0.0f }, { "quality", 1.0f }, { "match", 0.0f } };
        c.steps.push_back (s);
        static const char* satNames[] = { "TAPE", "TUBE", "IRON", "CONSOLE", "WHIP" };
        change ("dominatrix", "discipline", "DISCIPLINE", discipline, "%", whyDisc);
        change ("dominatrix", "satType", "SATURATION TYPE", (float) type, satNames[type], whySat);
        change ("dominatrix", "saturation", "SATURATION", sat, "%", "How much of that colour.");
        change ("dominatrix", "analog", "ANALOG", 0.0f, "%", "No tape hiss or wobble on the final master.");
        change ("dominatrix", "width", "WIDTH", width, "%", whyWidth);
        change ("dominatrix", "bassMono", "BASS MONO", bassMono ? 1.0f : 0.0f, bassMono ? "ON" : "OFF",
                bassMono ? "Below 120 Hz the channels differ (correlation " + juce::String (in.lowCorrelation, 2) + "). Kick and bass belong dead centre."
                         : "The low end is already centred.");
        change ("dominatrix", "ceiling", "CEILING", 0.0f, "-1 dB", "-1 dB true peak is what Spotify, Apple Music and YouTube ask for.");
        change ("dominatrix", "outputDb", "OUTPUT", out, "dB", whyOut);
        change ("dominatrix", "match", "MATCH", 0.0f, "OFF", "MATCH must be off for the real loudness to come out.");
    }

    // ---- The Master itself: tone EQ (none of the linked plugins EQs) + a safety limiter at -1 dBTP
    {
        auto fx = planFixes (in, nullptr, 0.0f, false, g);
        FixParams mf;
        mf.on = true; mf.amount = 1.0f;
        mf.eqOn = fx.p.eqOn; mf.eq = fx.p.eq;
        mf.loudOn = true; mf.gainDb = 0.0f; mf.ceilingDb = -1.0f;      // catches anything the EQ pushes over
        c.masterFix = mf;
        c.masterEq = fx.p.eqOn;
        if (fx.p.eqOn) change ("master", "eq", "TONE MATCH EQ", 1.0f, "ON", fx.whyEq + " (None of your linked plugins is an EQ, so The Master does this part.)");
        change ("master", "loud", "SAFETY LIMITER", -1.0f, "dBTP", "Catches any peak the EQ pushes over -1 dBTP. With no gain it only acts on stray peaks.");
        c.cantFix = fx.cantFix;
    }

    // ---- advice for individual tracks
    if (in.humSeconds >= 1.0f)
        c.onTracks.add ("BUZZKILL on the guitar and bass tracks: " + juce::String (in.humHz) + " Hz hum in the quiet parts. Fix it at the source, not on the master.");
    if (zLra > 1.5f)
        c.onTracks.add ("FLATLINER on the lead vocal if it's the part jumping in level.");
    c.onTracks.add ("THE SERVANT on individual tracks (coming: it will link to The Master for per-instrument tone and balance).");
    return c;
}
} // namespace mst
