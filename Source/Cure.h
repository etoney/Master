#pragma once
// THE MASTER - AUTO FIX planner: turns "your mix vs the genre" into settings for the FixChain.
// The plan only sets controls; every one of them can be overridden afterwards.
#include "FixEngine.h"
#include "Profiles.h"

namespace mst
{
struct FixPlan
{
    bool valid = false;
    FixParams p;
    juce::String genreName;
    double seconds = 0;
    // one line per section saying why it's set the way it is
    juce::String whyEq, whyDyn, whyStereo, whyHum, whyLoud;
    juce::StringArray cantFix;     // problems that have to be fixed in the mix itself

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("genre", genreName);
        o->setProperty ("seconds", seconds);
        o->setProperty ("eqOn", p.eqOn);
        juce::Array<juce::var> e; for (auto v : p.eq) e.add ((double) v);
        o->setProperty ("eq", e);
        o->setProperty ("dynOn", p.dynOn); o->setProperty ("glue", (double) p.glue); o->setProperty ("leveler", (double) p.leveler);
        o->setProperty ("stereoOn", p.stereoOn); o->setProperty ("width", (double) p.width); o->setProperty ("bassMono", p.bassMono);
        o->setProperty ("humOn", p.humOn); o->setProperty ("humHz", p.humHz);
        o->setProperty ("loudOn", p.loudOn); o->setProperty ("gainDb", (double) p.gainDb); o->setProperty ("ceilingDb", (double) p.ceilingDb);
        o->setProperty ("whyEq", whyEq); o->setProperty ("whyDyn", whyDyn); o->setProperty ("whyStereo", whyStereo);
        o->setProperty ("whyHum", whyHum); o->setProperty ("whyLoud", whyLoud);
        o->setProperty ("cantFix", cantFix.joinIntoString ("\n"));
        return juce::var (o);
    }
    static FixPlan fromVar (const juce::var& v)
    {
        FixPlan f;
        if (! v.isObject()) return f;
        f.valid = true;
        f.genreName = v.getProperty ("genre", "").toString();
        f.seconds = v.getProperty ("seconds", 0.0);
        f.p.eqOn = v.getProperty ("eqOn", false);
        if (auto* e = v.getProperty ("eq", {}).getArray())
            for (int b = 0; b < juce::jmin (kNumBands, e->size()); ++b) f.p.eq[(size_t) b] = (float) (double) (*e)[b];
        f.p.dynOn = v.getProperty ("dynOn", false); f.p.glue = (float) (double) v.getProperty ("glue", 0.0); f.p.leveler = (float) (double) v.getProperty ("leveler", 0.0);
        f.p.stereoOn = v.getProperty ("stereoOn", false); f.p.width = (float) (double) v.getProperty ("width", 1.0); f.p.bassMono = v.getProperty ("bassMono", false);
        f.p.humOn = v.getProperty ("humOn", false); f.p.humHz = v.getProperty ("humHz", 60);
        f.p.loudOn = v.getProperty ("loudOn", false); f.p.gainDb = (float) (double) v.getProperty ("gainDb", 0.0); f.p.ceilingDb = (float) (double) v.getProperty ("ceilingDb", -1.0);
        f.whyEq = v.getProperty ("whyEq", "").toString(); f.whyDyn = v.getProperty ("whyDyn", "").toString();
        f.whyStereo = v.getProperty ("whyStereo", "").toString(); f.whyHum = v.getProperty ("whyHum", "").toString();
        f.whyLoud = v.getProperty ("whyLoud", "").toString();
        f.cantFix = juce::StringArray::fromLines (v.getProperty ("cantFix", "").toString());
        f.cantFix.removeEmptyStrings();
        return f;
    }
};

// in:   analysis of your mix (before any fixes)
// after: analysis of The Master's output with the current fix settings, if it has heard
//        enough (used to fine-tune the loudness on a second AUTO FIX)
// currentGainDb / loudnessWasOn: the LOUDNESS settings that 'after' was measured with
inline FixPlan planFixes (const Features& in, const Features* after, float currentGainDb, bool loudnessWasOn, const GenreProfile& g, bool headroom = false)
{
    FixPlan plan;
    if (! in.valid) return plan;
    plan.valid = true;
    plan.genreName = g.name;
    plan.seconds = in.seconds;
    auto& p = plan.p;
    p.on = true;
    p.amount = 1.0f;
    const auto G = g.name;

    // ---- tone: pull each band toward the genre, leaving a little character (half a
    //      standard deviation is left alone) and never more than 6 dB
    {
        std::array<float, kNumBands> gain {};
        double before = 0, after2 = 0;
        int moved = 0;
        for (int b = 0; b < kNumBands; ++b)
        {
            const auto st = g.stat (fBand0 + b);
            const float dev = in.band[(size_t) b] - st.mean;
            const float excess = std::max (0.0f, std::abs (dev) - 0.5f * st.sd);
            gain[(size_t) b] = juce::jlimit (-6.0f, 6.0f, -(dev > 0 ? 1.0f : -1.0f) * excess * 0.8f);
        }
        // keep the overall level where it was: the EQ fixes balance, LOUDNESS handles level
        for (int b = 0; b < kNumBands; ++b)
        {
            const double share = std::pow (10.0, in.band[(size_t) b] / 10.0);
            before += share;
            after2 += share * std::pow (10.0, gain[(size_t) b] / 10.0);
        }
        const float offset = (float) (10.0 * std::log10 (after2 / std::max (1e-12, before)));
        juce::StringArray biggest;
        std::vector<std::pair<float, int>> order;
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& v = gain[(size_t) b];
            v = std::round ((v - offset) * 10.0f) / 10.0f;
            if (std::abs (v) >= 0.5f) { ++moved; order.push_back ({ std::abs (v), b }); }
            else v = 0.0f;
        }
        std::sort (order.begin(), order.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        for (int i = 0; i < juce::jmin (3, (int) order.size()); ++i)
        {
            const int b = order[(size_t) i].second;
            biggest.add (juce::String (bandDefs()[(size_t) b].label) + " " + (gain[(size_t) b] > 0 ? "+" : "") + juce::String (gain[(size_t) b], 1));
        }
        p.eq = gain;
        p.eqOn = moved > 0;
        plan.whyEq = moved > 0 ? "Moves " + juce::String (moved) + " band" + (moved > 1 ? "s" : "") + " toward " + G + ": " + biggest.joinIntoString (", ") + " dB."
                               : "Tone already sits where " + G + " records sit. Nothing to do.";
    }

    // ---- dynamics
    {
        const auto plr = g.stat (fPlr), lra = g.stat (fLra);
        const float zPlr = g.z (fPlr, in.plr), zLra = g.z (fLra, in.lra);
        p.glue = zPlr > 1.0f ? juce::jlimit (0.15f, 0.7f, (in.plr - plr.mean) * 0.12f + 0.1f) : 0.0f;
        p.leveler = zLra > 1.5f ? juce::jlimit (0.15f, 0.7f, (in.lra - lra.mean) * 0.1f) : 0.0f;
        p.dynOn = p.glue > 0.0f || p.leveler > 0.0f;
        juce::StringArray why;
        if (p.glue > 0.0f) why.add ("TAME PEAKS: the peaks stick out " + juce::String (in.plr - plr.mean, 1) + " dB more than " + G + " usually allows.");
        if (p.leveler > 0.0f) why.add ("LEVELER lifts the quiet sections (loudness range " + juce::String (in.lra, 1) + " LU vs about " + juce::String (lra.mean, 1) + ").");
        if (zPlr < -1.5f)
        {
            why.add ("The mix is already squashed; no extra compression.");
            plan.cantFix.add ("Over-squashed: dynamics can't be put back afterwards. Ease off the limiter or bus compression in the mix.");
        }
        plan.whyDyn = why.isEmpty() ? "Punch and level changes are already typical for " + G + "." : why.joinIntoString (" ");
    }

    // ---- stereo
    {
        const auto w = g.stat (fWidth);
        const float zW = g.z (fWidth, in.width);
        float width = 1.0f;
        if (std::abs (zW) >= 1.2f) width = juce::jlimit (0.6f, 1.5f, std::pow (10.0f, (w.mean - in.width) / 20.0f));
        // low correlation caused by stereo bass is BASS MONO's job, not a reason to narrow everything
        if (in.correlation < 0.3f && in.lowCorrelation >= 0.9f) width = std::min (width, 0.8f);
        p.width = std::round (width * 100.0f) / 100.0f;
        p.bassMono = in.lowCorrelation < 0.9f;
        p.stereoOn = std::abs (p.width - 1.0f) >= 0.02f || p.bassMono;
        juce::StringArray why;
        if (p.width > 1.0f) why.add ("WIDTH " + juce::String (juce::roundToInt (p.width * 100)) + " %: the mix is narrower than " + G + ".");
        else if (p.width < 1.0f) why.add ("WIDTH " + juce::String (juce::roundToInt (p.width * 100)) + " %: the sides are too loud / phasey.");
        if (p.bassMono) why.add ("BASS MONO: the low end isn't centred (correlation " + juce::String (in.lowCorrelation, 2) + ").");
        plan.whyStereo = why.isEmpty() ? "Stereo picture is fine." : why.joinIntoString (" ");
        if (in.correlation < 0.0f) plan.cantFix.add ("Out of phase: narrowing helps, but find the flipped-polarity track or stereo widener in the mix.");
    }

    // ---- hum
    p.humOn = in.humSeconds >= 1.0f && in.humHz > 0;
    p.humHz = in.humHz > 0 ? in.humHz : 60;
    plan.whyHum = p.humOn ? "Notches out " + juce::String (in.humHz) + " Hz hum and 2 harmonics. BuzzKill on the guitar/bass tracks does it better, without touching the bass notes."
                          : "No hum found.";

    // ---- loudness + ceiling
    {
        const auto L = g.stat (fLufs);
        float gain;
        bool refined = false;
        if (after != nullptr && after->valid && after->seconds >= 20.0 && loudnessWasOn)
        {
            gain = currentGainDb + (L.mean - after->lufs);   // measured result: correct by what's left
            refined = true;
        }
        else
        {
            gain = L.mean - in.lufs;
            const float overshoot = in.truePeak + gain - (-1.0f);   // the limiter eats some of the gain
            if (overshoot > 0.0f) gain += juce::jmin (4.0f, overshoot * 0.3f);
        }
        if (headroom)
        {
            // -6 dB mode: peaks just under -6 dBFS, no loudness push (left for the mastering step)
            refined = after != nullptr && after->valid && after->seconds >= 20.0 && loudnessWasOn;
            gain = refined ? currentGainDb + (after->truePeak < -7.5f ? -6.3f - after->truePeak : 0.0f) : -6.3f - in.truePeak;
        }
        if (g.z (fPlr, in.plr) < -1.5f) gain = std::min (gain, 0.0f);   // don't squash a squashed mix further
        p.gainDb = juce::jlimit (-6.0f, 12.0f, std::round (gain * 10.0f) / 10.0f);
        p.ceilingDb = headroom ? -6.0f : -1.0f;
        p.loudOn = true;
        if (headroom)
            plan.whyLoud = "OUTPUT TARGET -6 dB: brings your peaks (" + juce::String (in.truePeak, 1) + " dBTP) to just under -6 dB and holds them there, "
                           "leaving 6 dB of headroom for a separate mastering step. No loudness push.";
        else if (refined)
            plan.whyLoud = "Fine-tuned from what came out last time: aiming for " + juce::String (L.mean, 1) + " LUFS (" + G + "), peaks held at -1 dBTP.";
        else
            plan.whyLoud = "Aims for " + juce::String (L.mean, 1) + " LUFS (typical " + G + ") from " + juce::String (in.lufs, 1)
                         + ". Peaks held at -1 dBTP. Play the song through and press AUTO FIX again to fine-tune.";
    }

    if (in.clipEvents >= 3)
        plan.cantFix.add ("Clipping already in the mix (" + juce::String (in.clipEvents) + " spots) can't be undone here. Pull the mix bus down 1-2 dB.");
    return plan;
}
} // namespace mst
