// Headless checks (run locally and in CI):
//  1) analyser: LUFS / true peak / LRA accuracy, tonal bands, stereo, clipping, hum
//  2) profiles: factory pack, genre detector, building + pooling references, user packs
//  3) advisor: suggestions + plugin references for typical problems
//  4) processor: audio passes through untouched, background analysis, state save/restore
//  5) renders the editor to PNGs
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "BinaryData.h"
#include "../Source/CDLink.h"

static bool writePng (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    if (auto os = f.createOutputStream()) return juce::PNGImageFormat().writeImageToStream (img, *os);
    return false;
}

using Gen = std::function<float (int64_t n, int ch)>;
constexpr double SR = 48000.0;

static float noise (int64_t k)
{
    uint32_t h = (uint32_t) k * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return (float) (h & 0xffff) / 32768.0f - 1.0f;
}

static Gen sine (float amp, float freq, float phase = 0.0f)
{
    return [=] (int64_t n, int) { return amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * (double) n / SR + phase); };
}

// a crude but repeatable "mix": kick, snare, hats, bass and a sustained chord
static Gen drumMix (float level)
{
    return [=] (int64_t n, int ch)
    {
        const double t = (double) n / SR;
        const double tb = std::fmod (t, 0.5), ts = std::fmod (t + 0.25, 0.5), th = std::fmod (t, 0.125);
        const double kick = std::exp (-tb * 14.0) * std::sin (juce::MathConstants<double>::twoPi * 52.0 * tb * (1.0 + 1.5 * std::exp (-tb * 30.0)));
        const double snare = (ts < 0.25 ? std::exp (-ts * 22.0) : 0.0) * (0.55 * noise (n + ch) + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 190.0 * ts));
        const double hat = std::exp (-th * 90.0) * 0.12 * noise (n * 7 + 3 + ch);
        const double bass = 0.25 * std::sin (juce::MathConstants<double>::twoPi * 82.4 * t);
        const double pad = 0.09 * (std::sin (juce::MathConstants<double>::twoPi * 220.0 * t) + std::sin (juce::MathConstants<double>::twoPi * 277.2 * t + ch)
                                   + std::sin (juce::MathConstants<double>::twoPi * 329.6 * t));
        return level * (float) (0.6 * kick + snare + hat + bass + pad);
    };
}

// pink noise (Paul Kellet), independent per channel unless `mono`
struct Pink
{
    double b[7] {};
    float next (float white)
    {
        b[0] = 0.99886 * b[0] + white * 0.0555179; b[1] = 0.99332 * b[1] + white * 0.0750759;
        b[2] = 0.96900 * b[2] + white * 0.1538520; b[3] = 0.86650 * b[3] + white * 0.3104856;
        b[4] = 0.55000 * b[4] + white * 0.5329522; b[5] = -0.7616 * b[5] - white * 0.0168980;
        const double out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362;
        b[6] = white * 0.115926;
        return (float) (out * 0.11);
    }
};

static mst::Features analyse (const Gen& g, double seconds, double sr = SR)
{
    mst::MixAnalysis a;
    a.prepare (sr);
    std::vector<float> l (1024), r (1024);
    const int64_t total = (int64_t) (seconds * sr);
    for (int64_t pos = 0; pos < total; pos += 1024)
    {
        const int n = (int) std::min<int64_t> (1024, total - pos);
        for (int i = 0; i < n; ++i) { l[(size_t) i] = g (pos + i, 0); r[(size_t) i] = g (pos + i, 1); }
        a.process (l.data(), r.data(), n);
    }
    return a.compute();
}

static mst::Features meansOf (const mst::GenreProfile& g)
{
    mst::Features f;
    for (int i = 0; i < mst::numFeatures; ++i) f.set (i, g.stat (i).mean);
    f.valid = true; f.seconds = 180; f.truePeak = -1.2f; f.samplePeak = -1.5f;
    return f;
}

static bool hasItem (const mst::Report& r, mst::Sev sev, const juce::String& titlePart, const juce::String& plugin = {})
{
    for (auto& s : r.items)
        if (s.sev == sev && s.title.containsIgnoreCase (titlePart) && (plugin.isEmpty() || s.plugin == plugin)) return true;
    return false;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir (argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File::getCurrentWorkingDirectory());
    dir.createDirectory();
    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what) { std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); std::fflush (stdout); if (! ok) ++failures; };

    // ---------------- analyser
    {
        auto f = analyse (sine (0.1f, 1000.0f), 10.0);
        check (std::abs (f.lufs + 20.0f) < 0.1f, juce::String::formatted ("1 kHz at -20 dBFS reads -20 LUFS (%.2f)", f.lufs));
        check (std::abs (f.seconds - 10.0) < 0.15, juce::String::formatted ("counts the seconds analysed (%.1f)", f.seconds));
        check (f.lra < 0.3f, juce::String::formatted ("steady tone has ~0 LU loudness range (%.2f)", f.lra));
        check (f.correlation > 0.999f && f.width <= -39.0f, "mono signal: correlation 1, no side");
        for (double sr : { 44100.0, 96000.0 })
        {
            auto g = analyse ([=] (int64_t n, int) { return 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) n / sr); }, 6.0, sr);
            check (std::abs (g.lufs + 20.0f) < 0.15f, juce::String::formatted ("LUFS right at %.0f Hz (%.2f)", sr, g.lufs));
        }
    }
    {   // inter-sample peak: fs/4 sine at 45 degrees -> samples at 0.707, real peak at 1.0
        auto f = analyse (sine (1.0f, (float) SR / 4.0f, juce::MathConstants<float>::pi / 4.0f), 3.0);
        check (f.samplePeak < -2.9f && f.truePeak > -0.6f, juce::String::formatted ("true peak finds the inter-sample peak (sample %.2f, true %.2f dB)", f.samplePeak, f.truePeak));
    }
    {   // loudness range: 10 s at -30, 10 s at -18 -> ~12 LU
        auto f = analyse ([] (int64_t n, int) { const float a = (n / (int64_t) (10 * SR)) % 2 == 0 ? 0.0316f : 0.126f; return a * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) n / SR); }, 40.0);
        check (f.lra > 10.0f && f.lra < 13.5f, juce::String::formatted ("loudness range of a 12 dB step (%.1f LU)", f.lra));
    }
    {   // pink noise: about equal energy per octave
        Pink pl, pr;
        auto f = analyse ([&] (int64_t n, int ch) { return ch == 0 ? pl.next (noise (n)) : pr.next (noise (n + 99991)); }, 20.0);
        juce::String s;
        for (auto v : f.band) s << juce::String (v, 1) << " ";
        const float octaveSpread = std::max ({ f.band[1], f.band[2], f.band[3], f.band[4], f.band[5], f.band[6], f.band[7] })
                                 - std::min ({ f.band[1], f.band[2], f.band[3], f.band[4], f.band[5], f.band[6], f.band[7] });
        check (octaveSpread < 1.0f, "pink noise: one-octave bands within 1 dB of each other (" + s + ")");
        float total = 0; for (auto v : f.band) total += std::pow (10.0f, v / 10.0f);
        check (std::abs (10.0f * std::log10 (total)) < 0.05f, "band shares add up to the whole spectrum");
        check (std::abs (f.correlation) < 0.05f && std::abs (f.lowCorrelation) < 0.1f && f.width > -1.0f && f.width < 1.0f,
               juce::String::formatted ("independent channels: correlation %.2f, low %.2f, width %.1f dB", f.correlation, f.lowCorrelation, f.width));
    }
    {
        auto f = analyse ([] (int64_t n, int ch) { const float v = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 300.0 * (double) n / SR); return ch == 0 ? v : -v; }, 4.0);
        check (f.correlation < -0.99f, "L = -R reads correlation -1");
    }
    {   // stereo bass vs mono bass
        auto f = analyse ([] (int64_t n, int ch) { return 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 60.0 * (double) n / SR + (ch ? 1.6 : 0.0)); }, 4.0);
        check (f.lowCorrelation < 0.1f, juce::String::formatted ("out-of-phase stereo bass detected (low correlation %.2f)", f.lowCorrelation));
        auto m = analyse (drumMix (0.8f), 6.0);
        check (m.lowCorrelation > 0.95f, juce::String::formatted ("mono bass reads mono (%.2f)", m.lowCorrelation));
    }
    {
        auto f = analyse ([] (int64_t n, int) { return std::sin (juce::MathConstants<double>::twoPi * 100.0 * (double) n / SR) > 0 ? 1.0f : -1.0f; }, 2.0);
        check (f.clipEvents > 100, juce::String::formatted ("flat-topped clipping found (%d runs)", f.clipEvents));
        auto c = analyse (sine (0.98f, 440.0f), 2.0);
        check (c.clipEvents == 0, "a clean loud sine isn't clipping");
    }
    {   // hum: music, then quiet passages with 60 Hz hum + harmonics in a little noise
        auto humGen = [] (int64_t n, int ch)
        {
            const double t = (double) n / SR;
            const bool music = std::fmod (t, 8.0) < 4.0;
            if (music) return drumMix (0.7f) (n, ch);
            const double hum = 0.0018 * std::sin (juce::MathConstants<double>::twoPi * 60.0 * t) + 0.0012 * std::sin (juce::MathConstants<double>::twoPi * 120.0 * t)
                             + 0.0008 * std::sin (juce::MathConstants<double>::twoPi * 180.0 * t);
            return (float) (hum + 0.0003 * noise (n * 3 + ch));
        };
        auto f = analyse (humGen, 24.0);
        check (f.humHz == 60 && f.humSeconds >= 6.0f, juce::String::formatted ("60 Hz hum found in the quiet parts (%d Hz, %.0f s, %.1f dB)", f.humHz, f.humSeconds, f.humDb));
        auto quiet = analyse ([] (int64_t n, int ch) { return 0.0005f * noise (n * 5 + ch); }, 8.0);
        check (quiet.humSeconds == 0.0f && quiet.quietSeconds >= 7.0f, juce::String::formatted ("no hum reported in plain quiet noise (%.0f s hum, %.1f dB, %.0f s quiet)", quiet.humSeconds, quiet.humDb, quiet.quietSeconds));
        auto mix = analyse (drumMix (0.8f), 10.0);
        check (mix.humSeconds == 0.0f, "no hum reported in a busy mix");
    }
    {   // serialisation
        auto f = analyse (drumMix (0.7f), 6.0);
        auto g = mst::Features::fromVar (juce::JSON::parse (juce::JSON::toString (f.toVar())));
        bool same = g.valid && std::abs (g.seconds - f.seconds) < 1e-6;
        for (int i = 0; i < mst::numFeatures; ++i) same = same && std::abs (g.get (i) - f.get (i)) < 1e-4f;
        check (same, "analysis survives a JSON round trip");
    }

    // ---------------- profiles
    const auto factoryJson = juce::String::fromUTF8 (BinaryData::factory_json, BinaryData::factory_jsonSize);
    mst::ProfilePack factory;
    {
        juce::String err;
        check (mst::ProfilePack::parse (factoryJson, factory, err), "factory pack parses " + err);
        juce::StringArray names;
        for (auto& g : factory.genres) names.add (g.name);
        check (names.joinIntoString (",") == "Rock,Metal,Punk,Hard Rock,Pop", "factory genres: " + names.joinIntoString (", "));
        for (auto& g : factory.genres)
        {
            float t = 0; for (int b = 0; b < mst::kNumBands; ++b) t += std::pow (10.0f, g.stat (mst::fBand0 + b).mean / 10.0f);
            check (std::abs (10.0f * std::log10 (t)) < 0.05f, g.name + " band shares add up");
        }
        for (int i = 0; i < (int) factory.genres.size(); ++i)
        {
            auto guess = mst::detectGenre (meansOf (factory.genres[(size_t) i]), factory.genres);
            check (! guess.empty() && guess[0].index == i, "detector picks " + factory.genres[(size_t) i].name + " for a typical " + factory.genres[(size_t) i].name
                   + " mix (" + juce::String (juce::roundToInt (guess[0].probability * 100)) + "%)");
        }
        mst::ProfilePack again;
        check (mst::ProfilePack::parse (factory.toJson(), again, err) && again.genres.size() == factory.genres.size()
               && std::abs (again.genres[2].stat (mst::fPlr).mean - factory.genres[2].stat (mst::fPlr).mean) < 1e-3f, "pack JSON round trip");
        mst::ProfilePack bad;
        check (! mst::ProfilePack::parse ("{\"format\":\"nope\"}", bad, err), "rejects a file that isn't a profile pack");
        check (! mst::ProfilePack::parse ("{\"format\":\"the-master-profiles\",\"analyzerVersion\":99,\"genres\":[]}", bad, err), "rejects a pack from a newer analyser");
    }
    {   // building + pooling
        std::vector<mst::Features> refs;
        for (float d : { -1.0f, 0.0f, 1.0f })
        {
            auto f = meansOf (factory.genres[0]);
            f.lufs = -9.0f + d; f.plr = 9.0f + d * 0.1f;
            refs.push_back (f);
        }
        auto p = mst::buildProfile ("rock", "Rock", refs);
        check (p.tracks == 3 && std::abs (p.stat (mst::fLufs).mean + 9.0f) < 1e-4f && std::abs (p.stat (mst::fLufs).sd - 1.0f) < 1e-4f,
               "profile from 3 songs: mean -9 LUFS, sd 1.0");
        check (std::abs (p.stat (mst::fPlr).sd - mst::minSd (mst::fPlr)) < 1e-4f, "tiny spreads are floored");
        auto seed = factory.genres[0];
        auto one = meansOf (seed); one.lufs = seed.stat (mst::fLufs).mean - 6.0f;
        mst::addReference (seed, one);
        check (seed.tracks == 1 && seed.source == "references" && std::abs (seed.stat (mst::fLufs).mean - (factory.genres[0].stat (mst::fLufs).mean - 1.0f)) < 0.01f,
               "one captured reference moves a seed profile 1/6 of the way");
    }
    {   // the library with a user pack on top
        auto tmp = juce::File::createTempFile (".json");
        mst::ProfileLibrary lib;
        lib.load (factoryJson, tmp);
        check (! lib.usingUserPack() && lib.genres().size() == 5, "library starts on the factory pack");
        auto fx = meansOf (lib.genres()[(size_t) lib.indexOf ("metal")]);
        fx.lufs = -6.0f;
        check (lib.addReference ("metal", fx).isEmpty() && tmp.existsAsFile() && lib.usingUserPack(), "admin capture writes the user pack");
        check (lib.genres()[(size_t) lib.indexOf ("metal")].tracks == 1 && lib.genres()[(size_t) lib.indexOf ("rock")].tracks == 0, "only Metal changed");
        mst::ProfileLibrary lib2;
        lib2.load (factoryJson, tmp);
        check (lib2.usingUserPack() && lib2.genres()[(size_t) lib2.indexOf ("metal")].tracks == 1, "user pack is picked up by the next instance");

        // an imported pack can add a genre
        mst::ProfilePack extra;
        extra.packVersion = "test";
        auto country = factory.genres[4];
        country.id = "country"; country.name = "Country"; country.tracks = 12; country.source = "references";
        extra.genres.push_back (country);
        auto packFile = juce::File::createTempFile (".json");
        packFile.replaceWithText (extra.toJson());
        check (lib.importPack (packFile).isEmpty() && lib.indexOf ("country") == 5 && lib.genres().size() == 6, "import adds a new genre (Country)");
        check (lib.genres()[(size_t) lib.indexOf ("metal")].tracks == 0, "import replaces the previous user pack");
        auto exported = juce::File::createTempFile (".json");
        check (lib.exportPack (exported, "x").isEmpty() && [&] { mst::ProfilePack p; juce::String e; return mst::ProfilePack::parse (exported.loadFileAsString(), p, e) && p.genres.size() == 6; }(),
               "export writes every genre");
        packFile.replaceWithText ("garbage");
        check (lib.importPack (packFile).isNotEmpty() && lib.genres().size() == 6, "a broken file is refused and nothing changes");
        check (lib.restoreFactory().isEmpty() && ! tmp.existsAsFile() && lib.genres().size() == 5, "restore factory");
        packFile.deleteFile(); exported.deleteFile();
    }

    // ---------------- advisor
    {
        const auto& rock = factory.genres[0];
        auto ok = meansOf (rock);
        auto r = mst::advise (ok, rock);
        check (r.ready && r.score >= 95 && r.fixes == 0 && r.checks == 0, juce::String::formatted ("a typical Rock master scores high (%d, %d fix, %d check)", r.score, r.fixes, r.checks));

        auto quiet = ok; quiet.lufs = rock.stat (mst::fLufs).mean - 5.0f;
        r = mst::advise (quiet, rock);
        check (hasItem (r, mst::Sev::fix, "QUIETER", mst::plug::dominatrix), "too quiet -> FIX, points at Dominatrix");

        auto hot = ok; hot.truePeak = 0.4f; hot.clipEvents = 40;
        r = mst::advise (hot, rock);
        check (hasItem (r, mst::Sev::fix, "TRUE PEAKS", mst::plug::dominatrix) && hasItem (r, mst::Sev::fix, "CLIPPING", mst::plug::chopShop), "over 0 dBTP + clipping -> Dominatrix CEILING and Chop Shop");

        auto spiky = ok; spiky.plr = rock.stat (mst::fPlr).mean + 4.0f;
        r = mst::advise (spiky, rock);
        check (hasItem (r, mst::Sev::fix, "PEAKS STICKING OUT", mst::plug::chopShop), "too dynamic -> Chop Shop");

        auto wideBass = ok; wideBass.lowCorrelation = 0.3f;
        r = mst::advise (wideBass, rock);
        check (hasItem (r, mst::Sev::fix, "LOW END", mst::plug::dominatrix), "stereo bass -> Dominatrix BASS MONO");

        auto hum = ok; hum.humSeconds = 5; hum.humHz = 50; hum.humDb = 25;
        r = mst::advise (hum, rock);
        check (hasItem (r, mst::Sev::fix, "50 Hz HUM", mst::plug::buzzKill), "hum -> BuzzKill");

        auto swings = ok; swings.lra = rock.stat (mst::fLra).mean + 5.0f;
        r = mst::advise (swings, rock);
        check (hasItem (r, mst::Sev::check, "SWINGS", mst::plug::flatliner), "big level swings -> Flatliner");

        auto muddy = ok; muddy.band[3] += 7.0f; muddy.band[6] -= 4.0f;
        r = mst::advise (muddy, rock);
        check (hasItem (r, mst::Sev::fix, "MUDDY") && hasItem (r, mst::Sev::check, "DULL PRESENCE"), "muddy + dull mix gets band advice");
        check (r.score < 90, juce::String::formatted ("problems lower the score (%d)", r.score));

        mst::Features none;
        r = mst::advise (none, rock);
        check (! r.ready && r.items.size() == 1 && r.items[0].title == "PLAY YOUR SONG", "no audio yet -> how to start");
    }


    // ---------------- fix chain
    {
        auto run = [&] (const mst::FixParams& fp, const Gen& g, double secs, std::vector<float>* outL = nullptr, std::vector<float>* outR = nullptr)
        {
            mst::FixChain ch;
            ch.prepare (SR, 512);
            ch.setParams (fp);
            mst::MixAnalysis a; a.prepare (SR);
            std::vector<float> l (512), r (512);
            const int64_t total = (int64_t) (secs * SR);
            for (int64_t pos = 0; pos < total; pos += 512)
            {
                for (int i = 0; i < 512; ++i) { l[(size_t) i] = g (pos + i, 0); r[(size_t) i] = g (pos + i, 1); }
                ch.process (l.data(), r.data(), 512, [] (const float*, const float*, int) {});
                a.process (l.data(), r.data(), 512);
                if (outL) outL->insert (outL->end(), l.begin(), l.end());
                if (outR) outR->insert (outR->end(), r.begin(), r.end());
            }
            return std::make_pair (a.compute(), ch.latency());
        };
        Pink pl, pr;
        auto pink = [&] (int64_t n, int ch) { return 0.5f * (ch == 0 ? pl.next (noise (n)) : pr.next (noise (n + 99991))); };

        mst::FixParams neutral; neutral.on = true;
        neutral.eqOn = neutral.dynOn = neutral.stereoOn = neutral.humOn = true; neutral.loudOn = false;
        std::vector<float> ol;
        auto dm = drumMix (0.6f);
        auto [nf, lat] = run (neutral, dm, 3.0, &ol);
        float diff = 0;
        for (size_t i = 0; i < ol.size(); ++i) if ((int) i >= lat) diff = std::max (diff, std::abs (ol[i] - dm ((int64_t) i - lat, 0)));
        mst::FixParams allNeutral; allNeutral.on = true; allNeutral.eqOn = allNeutral.dynOn = allNeutral.stereoOn = true;
        ol.clear();
        run (allNeutral, dm, 3.0, &ol);
        diff = 0;
        for (size_t i = 0; i < ol.size(); ++i) if ((int) i >= lat) diff = std::max (diff, std::abs (ol[i] - dm ((int64_t) i - lat, 0)));
        check (diff < 1e-5f, juce::String::formatted ("every fix at its neutral setting is transparent (max diff %.2g)", diff));

        auto base = run (mst::FixParams {}, pink, 12.0).first;
        mst::FixParams eqp; eqp.on = eqp.eqOn = true; eqp.eq[3] = 6.0f; eqp.eq[8] = -6.0f;
        auto e = run (eqp, pink, 12.0).first;
        const float upLm = e.band[3] - base.band[3], dnSh = e.band[8] - base.band[8];
        check (upLm > 3.5f && dnSh < -3.5f, juce::String::formatted ("EQ +6 LOW MID / -6 SHEEN moves those bands (%+.1f / %+.1f dB share)", upLm, dnSh));
        auto half = eqp; half.amount = 0.5f;
        auto e2 = run (half, pink, 12.0).first;
        check (e2.band[3] - base.band[3] < upLm * 0.7f && e2.band[3] - base.band[3] > upLm * 0.3f, "AMOUNT 50 % does about half");

        mst::FixParams loud; loud.on = loud.loudOn = true; loud.gainDb = 10.0f; loud.ceilingDb = -1.0f;
        auto d0 = run (mst::FixParams {}, drumMix (0.9f), 10.0).first;
        auto d1 = run (loud, drumMix (0.9f), 10.0).first;
        check (d1.truePeak <= -1.0f, juce::String::formatted ("limiter holds -1 dBTP with +10 dB of gain (%.2f dBTP)", d1.truePeak));
        check (d1.lufs - d0.lufs > 4.0f, juce::String::formatted ("LOUDNESS gain makes it louder (%+.1f LU)", d1.lufs - d0.lufs));
        loud.ceilingDb = -2.0f;
        check (run (loud, drumMix (0.9f), 6.0).first.truePeak <= -1.9f, "-2 dBTP ceiling holds");

        mst::FixParams bm; bm.on = bm.stereoOn = true; bm.bassMono = true;
        auto stBass = [] (int64_t n, int ch) { const double t = (double) n / SR; return (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 60.0 * t + (ch ? 1.6 : 0.0)) + 0.1 * std::sin (juce::MathConstants<double>::twoPi * 2000.0 * t + (ch ? 0.7 : 0.0))); };
        auto b0 = run (mst::FixParams {}, stBass, 4.0).first, b1 = run (bm, stBass, 4.0).first;
        check (b0.lowCorrelation < 0.1f && b1.lowCorrelation > 0.97f, juce::String::formatted ("BASS MONO centres the low end (%.2f -> %.2f)", b0.lowCorrelation, b1.lowCorrelation));
        mst::FixParams wide; wide.on = wide.stereoOn = true; wide.width = 1.5f;
        Pink ql, qr;
        auto pink2 = [&] (int64_t n, int ch) { const float m = 0.3f * ql.next (noise (n)); const float sd = 0.1f * qr.next (noise (n + 7)); return ch == 0 ? m + sd : m - sd; };
        auto w0 = run (mst::FixParams {}, pink2, 6.0).first;
        auto w1 = run (wide, pink2, 6.0).first;
        check (w1.width - w0.width > 2.5f && w1.width - w0.width < 4.5f, juce::String::formatted ("WIDTH 150 %% widens the sides (%+.1f dB)", w1.width - w0.width));

        auto humOnly = [] (int64_t n, int ch) { const double t = (double) n / SR; return (float) (0.01 * std::sin (juce::MathConstants<double>::twoPi * 60.0 * t) + 0.002 * noise (n * 3 + ch)); };
        mst::FixParams hm; hm.on = hm.humOn = true; hm.humHz = 60;
        std::vector<float> h0, h1;
        run (mst::FixParams {}, humOnly, 3.0, &h0); run (hm, humOnly, 3.0, &h1);
        auto tone = [] (const std::vector<float>& x) { double re = 0, im = 0; for (size_t i = 48000; i < x.size(); ++i) { const double ph = juce::MathConstants<double>::twoPi * 60.0 * (double) i / SR; re += x[i] * std::cos (ph); im += x[i] * std::sin (ph); } return 10.0 * std::log10 (re * re + im * im + 1e-20); };
        check (tone (h0) - tone (h1) > 25.0, juce::String::formatted ("hum notch removes 60 Hz (%.0f dB)", tone (h0) - tone (h1)));

        mst::FixParams gl; gl.on = gl.dynOn = true; gl.glue = 0.6f;
        auto g0 = run (mst::FixParams {}, drumMix (0.7f), 10.0).first, g1 = run (gl, drumMix (0.7f), 10.0).first;
        check (g1.plr < g0.plr - 0.8f && std::abs (g1.lufs - g0.lufs) < 2.5f, juce::String::formatted ("GLUE reduces peak-to-loudness (%.1f -> %.1f dB) and keeps the level (%+.1f)", g0.plr, g1.plr, g1.lufs - g0.lufs));
        mst::FixParams lv; lv.on = lv.dynOn = true; lv.leveler = 0.7f;
        auto steps = [] (int64_t n, int) { const float a = (n / (int64_t) (8 * SR)) % 2 == 0 ? 0.03f : 0.2f; return a * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * (double) n / SR); };
        auto l0 = run (mst::FixParams {}, steps, 64.0).first, l1 = run (lv, steps, 64.0).first;
        check (l1.lra < l0.lra - 3.0f, juce::String::formatted ("LEVELER shrinks big level swings (%.1f -> %.1f LU)", l0.lra, l1.lra));

        mst::FixParams mt = loud; mt.match = true; mt.ceilingDb = -1.0f;
        auto m1 = run (mt, drumMix (0.5f), 16.0).first;
        auto m0 = run (mst::FixParams {}, drumMix (0.5f), 16.0).first;
        check (std::abs (m1.lufs - m0.lufs) < 1.0f, juce::String::formatted ("MATCH plays the fixed mix back at the input's loudness (%+.2f LU)", m1.lufs - m0.lufs));
    }

    // ---------------- AUTO FIX planner
    {
        const auto& rock = factory.genres[0];
        auto bad = meansOf (rock);
        bad.lufs = -13.0f; bad.truePeak = -2.0f; bad.plr = 11.0f;
        bad.band[3] += 5.0f; bad.band[6] -= 4.0f;
        float t = 0; for (auto v : bad.band) t += std::pow (10.0f, v / 10.0f);
        for (auto& v : bad.band) v -= 10.0f * std::log10 (t);
        bad.lowCorrelation = 0.6f; bad.humSeconds = 3; bad.humHz = 50; bad.lra = rock.stat (mst::fLra).mean + 5.0f;
        auto plan = mst::planFixes (bad, nullptr, 0, false, rock);
        check (plan.valid && plan.p.eqOn && plan.p.eq[3] < -1.5f && plan.p.eq[6] > 1.5f, juce::String::formatted ("plan cuts the mud and lifts presence (%+.1f / %+.1f dB)", plan.p.eq[3], plan.p.eq[6]));
        check (plan.p.loudOn && plan.p.gainDb > 3.0f && plan.p.ceilingDb == -1.0f, juce::String::formatted ("plan adds loudness (%+.1f dB) with a -1 dBTP ceiling", plan.p.gainDb));
        check (plan.p.stereoOn && plan.p.bassMono && plan.p.humOn && plan.p.humHz == 50 && plan.p.dynOn && plan.p.leveler > 0 && plan.p.glue > 0,
               "plan: bass mono, 50 Hz hum notch, glue and leveler");
        auto good = mst::planFixes (meansOf (rock), nullptr, 0, false, rock);
        bool flat = ! good.p.eqOn && ! good.p.dynOn && ! good.p.stereoOn && ! good.p.humOn && std::abs (good.p.gainDb) < 1.0f;
        check (flat, "a mix that's already on target gets (almost) nothing");
        auto squashed = meansOf (rock); squashed.plr = 4.0f; squashed.lufs = -12.0f;
        auto sp = mst::planFixes (squashed, nullptr, 0, false, rock);
        check (sp.p.gainDb <= 0.0f && sp.cantFix.size() > 0, "an over-squashed mix gets no extra gain and a 'fix it in the mix' note");
        auto after = meansOf (rock); after.lufs = rock.stat (mst::fLufs).mean - 1.5f;
        auto ref = mst::planFixes (bad, &after, 6.0f, true, rock);
        check (std::abs (ref.p.gainDb - 7.5f) < 0.11f, juce::String::formatted ("second AUTO FIX fine-tunes the gain from the measured result (6.0 -> %.1f dB)", ref.p.gainDb));
        auto rt = mst::FixPlan::fromVar (juce::JSON::parse (juce::JSON::toString (plan.toVar())));
        check (rt.valid && std::abs (rt.p.eq[3] - plan.p.eq[3]) < 1e-4f && rt.p.humHz == 50 && rt.whyEq == plan.whyEq, "plan survives a save/load");
    }

    // ---------------- processor
    {
        MasterProcessor p;
        p.setPlayConfigDetails (2, 2, SR, 512);
        p.setNonRealtime (true);    // like an offline bounce: much faster than real time, nothing may be dropped
        p.prepareToPlay (SR, 512);
        juce::MidiBuffer midi;
        float maxDiff = 0;
        auto g = sine (0.1f, 1000.0f);
        int64_t pos = 0;
        const int lat = p.getLatencySamples();
        for (int b = 0; b < (int) (8 * SR / 512); ++b, pos += 512)
        {
            juce::AudioBuffer<float> buf (2, 512);
            for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) buf.setSample (c, i, g (pos + i, c));
            p.processBlock (buf, midi);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) { const float d = std::abs (buf.getSample (c, i) - (pos + i - lat >= 0 ? g (pos + i - lat, c) : 0.0f)); maxDiff = std::max (maxDiff, d); }
        }
        check (maxDiff < 1e-6f && lat > 0 && lat < 0.01 * SR, juce::String::formatted ("FIX off: audio passes through untouched (latency %d samples, max diff %.2g)", lat, maxDiff));
        check (p.getWorker().waitUntilIdle (5000), "background analysis keeps up");
        bool saved = true;
        auto fx = p.getDisplayFeatures (saved);
        check (! saved && std::abs (fx.lufs + 20.0f) < 0.15f && std::abs (fx.seconds - 8.0) < 0.05, juce::String::formatted ("live analysis through the plugin: %.2f LUFS, %.1f s", fx.lufs, fx.seconds));

        p.setGenreChoice ("punk");
        juce::MemoryBlock state;
        p.getStateInformation (state);
        MasterProcessor q;
        q.setStateInformation (state.getData(), (int) state.getSize());
        auto rf = q.getDisplayFeatures (saved);
        check (saved && std::abs (rf.lufs - fx.lufs) < 1e-3f && q.getGenreChoice() == "punk", "the report and genre are saved with the project");
        check (q.resolveTarget (rf).index == q.getLibrary().indexOf ("punk") && ! q.resolveTarget (rf).automatic, "locked genre is used");
        q.resetAnalysis();
        rf = q.getDisplayFeatures (saved);
        check (! saved && ! rf.valid, "RESET clears the saved report");

        p.getWorker().requestReset();
        p.getWorker().waitUntilIdle (2000);
        p.apvts.getParameter ("listen")->setValueNotifyingHost (0.0f);
        for (int b = 0; b < 100; ++b) { juce::AudioBuffer<float> buf (2, 512); buf.clear(); for (int i = 0; i < 512; ++i) buf.setSample (0, i, 0.5f); p.processBlock (buf, midi); }
        juce::Thread::sleep (300);
        p.getDisplayFeatures (saved);
        mst::Features after; p.getWorker().getSnapshot (after);
        check (after.seconds == 0.0, "LISTEN off: nothing is analysed");
    }


    // ---------------- AUTO FIX end to end: quiet, muddy, stereo-bass mix -> better score after
    {
        MasterProcessor p;
        p.setPlayConfigDetails (2, 2, SR, 512);
        p.setNonRealtime (true);
        p.prepareToPlay (SR, 512);
        p.setGenreChoice ("rock");
        juce::MidiBuffer midi;
        mst::FBiquad mud[2]; mud[0].design (1, SR, 330.0, 1.0, 8.0); mud[1].design (2, SR, 3000.0, 0.7, -6.0);
        auto bad = drumMix (0.25f);
        int64_t pos = 0;
        auto play = [&] (double secs)
        {
            for (int b = 0; b < (int) (secs * SR / 512); ++b, pos += 512)
            {
                juce::AudioBuffer<float> buf (2, 512);
                for (int i = 0; i < 512; ++i)
                    for (int c = 0; c < 2; ++c)
                    {
                        const double t = (double) (pos + i) / SR;
                        float v = bad (pos + i, c) + 0.08f * (float) std::sin (juce::MathConstants<double>::twoPi * 55.0 * t + (c ? 2.0 : 0.0));
                        v = mud[1].process (mud[0].process (v, c), c);
                        buf.setSample (c, i, v);
                    }
                p.processBlock (buf, midi);
            }
            p.getWorker().waitUntilIdle (8000); p.getPostWorker().waitUntilIdle (8000);
        };
        play (30.0);
        bool saved = false;
        auto before = p.getDisplayFeatures (saved);
        const auto& gp = p.getLibrary().genres()[(size_t) p.getLibrary().indexOf ("rock")];
        const auto r0 = mst::advise (before, gp);
        check (p.applyAutoFix().isEmpty() && p.isFixOn() && p.getPlan().valid, "AUTO FIX sets up the fixes");
        check (! p.isOverridden (MasterProcessor::secEq) && ! p.isOverridden (MasterProcessor::secLoud), "fresh AUTO FIX: nothing overridden");
        play (30.0);
        auto after = p.getAfterFeatures();
        const auto r1 = mst::advise (after, gp);
        check (after.valid && r1.score > r0.score + 15, juce::String::formatted ("score after AUTO FIX: %d -> %d (loudness %.1f -> %.1f LUFS, low-end corr %.2f -> %.2f, true peak %.1f dBTP)",
               r0.score, r1.score, before.lufs, after.lufs, before.lowCorrelation, after.lowCorrelation, after.truePeak));
        check (after.truePeak <= -1.0f, juce::String::formatted ("fixed output respects -1 dBTP (%.2f)", after.truePeak));
        // second press fine-tunes the loudness from what came out
        p.applyAutoFix();
        play (30.0);
        auto after2 = p.getAfterFeatures();
        check (std::abs (after2.lufs - gp.stat (mst::fLufs).mean) < std::abs (after.lufs - gp.stat (mst::fLufs).mean) + 0.2f
               && std::abs (after2.lufs - gp.stat (mst::fLufs).mean) < 1.5f,
               juce::String::formatted ("second AUTO FIX lands near the target (%.1f LUFS, target %.1f)", after2.lufs, gp.stat (mst::fLufs).mean));

        // override + revert
        p.apvts.getParameter ("eq3")->setValueNotifyingHost (p.apvts.getParameter ("eq3")->convertTo0to1 (4.0f));
        check (p.isOverridden (MasterProcessor::secEq) && ! p.isOverridden (MasterProcessor::secStereo), "moving an EQ band marks only that section as overridden");
        p.revertToAuto (MasterProcessor::secEq);
        check (! p.isOverridden (MasterProcessor::secEq) && std::abs (p.currentFixParams().eq[3] - p.getPlan().p.eq[3]) < 0.05f, "AUTO puts the section back");

        juce::MemoryBlock st; p.getStateInformation (st);
        MasterProcessor q; q.setStateInformation (st.getData(), (int) st.getSize());
        check (q.getPlan().valid && q.isFixOn() && std::abs (q.currentFixParams().gainDb - p.currentFixParams().gainDb) < 0.05f && ! q.isOverridden (MasterProcessor::secLoud),
               "fixes and the AUTO plan are saved with the project");
        check (q.getLatencySamples() == 0 || q.getLatencySamples() == p.getLatencySamples(), "latency is constant");
        p.apvts.getParameter ("fixOn")->setValueNotifyingHost (0.0f);
        check (p.getLatencySamples() > 0 && ! p.isFixOn(), "FIX off keeps the same latency (A/B doesn't shift timing)");
    }


    // ---------------- MY PLUGINS: prescription, chain detection, APPLY over Castle Danger Link
    {
        MasterProcessor p;
        p.setPlayConfigDetails (2, 2, SR, 512);
        p.prepareToPlay (SR, 512);
        p.setGenreChoice ("rock");
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

        // fake Dominatrix + Chop Shop living in "another plugin binary" (a separate node: real socket)
        auto other = cdlink::Node::createIsolatedForTesting();
        juce::var masterEnv;
        struct Fake
        {
            juce::String kind; std::map<juce::String, double> params; juce::var lastSet; int sets = 0;
            std::unique_ptr<cdlink::Endpoint> ep;
        };
        Fake dom { "dominatrix", { { "discipline", 30 }, { "outputDb", 0 }, { "ceiling", 0 }, { "bassMono", 0 }, { "width", 100 }, { "satType", 0 }, { "saturation", 20 } } };
        Fake chop { "chopshop", { { "ceilingDb", 0 }, { "mode", 0 }, { "shape", 0 } } };
        juce::var watcherEnv;
        cdlink::Endpoint watcher ("watch", [&] (const juce::var& m) { if (m.getProperty ("kind", "").toString() == "master" && m.getProperty ("type", "").toString() == "status") watcherEnv = m.getProperty ("env", {}); }, other);
        for (auto* f : { &dom, &chop })
            f->ep = std::make_unique<cdlink::Endpoint> (f->kind, [f] (const juce::var& m)
            {
                if (m.getProperty ("type", "").toString() != "set") return;
                f->lastSet = m; ++f->sets;
                if (auto* o = m.getProperty ("params", {}).getDynamicObject()) for (auto& pr : o->getProperties()) f->params[pr.name.toString()] = (double) pr.value;
                auto r = cdlink::Endpoint::makeMessage ("applied");
                auto* po = new juce::DynamicObject(); for (auto& [k, v] : f->params) po->setProperty (k, v);
                r->setProperty ("params", juce::var (po));
                f->ep->send (r, m.getProperty ("from", "").toString());
            }, other);
        auto sendStatus = [&] (Fake& f, const juce::var& e)
        {
            auto m = cdlink::Endpoint::makeMessage ("status");
            auto* po = new juce::DynamicObject(); for (auto& [k, v] : f.params) po->setProperty (k, v);
            m->setProperty ("params", juce::var (po)); m->setProperty ("env", e);
            f.ep->send (m);
        };
        // envelope helpers: copy of The Master's input fingerprint, with a different "shape" for upstream points
        auto envWith = [] (const juce::var& base, const char* inShape, const char* outShape)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("slot", base.getProperty ("slot", 0.0));
            juce::Array<juce::var> in, out;
            if (auto* a = base.getProperty ("in", {}).getArray())
                for (int i = 0; i < a->size(); ++i)
                {
                    const double v = (*a)[i];
                    auto shape = [&] (const char* s) { return juce::String (s) == "same" ? v : (juce::String (s) == "wobble" ? v + ((i % 3) - 1) * 2.0 : v + ((i % 5) - 2) * 3.0); };
                    in.add (shape (inShape)); out.add (shape (outShape));
                }
            o->setProperty ("in", in); o->setProperty ("out", out);
            return juce::var (o);
        };

        // 1) your mix, nothing on the bus yet: quiet and peaky
        auto g = drumMix (0.3f);
        int64_t pos = 0;
        auto play = [&] (double secs, bool realtime)
        {
            const int blocks = (int) (secs * SR / 512);
            for (int b = 0; b < blocks; ++b, pos += 512)
            {
                juce::AudioBuffer<float> buf (2, 512);
                for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) buf.setSample (c, i, g (pos + i, c));
                juce::MidiBuffer midi; p.processBlock (buf, midi);
                if (realtime && b % 8 == 0) pump (85);
            }
        };
        p.setNonRealtime (true);
        play (30.0, false);
        p.getWorker().waitUntilIdle (8000);
        check (p.applyChain().isNotEmpty(), "APPLY needs a prescription first");
        check (p.prescribe().isEmpty() && p.getChainStage() == 1, "PRESCRIBE from the analysis");
        const auto& cp = p.getChainPlan();
        check (cp.steps.size() == 2 && cp.steps[0].kind == "chopshop" && cp.steps[1].kind == "dominatrix", "prescription: Chop Shop, then Dominatrix, then The Master");
        check (cp.step ("dominatrix")->params.at ("outputDb") > 3.0f && cp.step ("dominatrix")->params.at ("ceiling") == 0.0f, "Dominatrix: OUTPUT drives into the -1 dB ceiling");
        check (cp.step ("chopshop")->params.at ("ceilingDb") < cp.before.samplePeak - 1.0f && cp.step ("chopshop")->params.at ("shape") == 1.0f, "Chop Shop: soft ceiling under the loudest peaks");
        bool allWhy = ! cp.changes.empty(); for (auto& c : cp.changes) allWhy = allWhy && c.why.isNotEmpty();
        check (allWhy, juce::String::formatted ("every planned change has a reason (%d changes)", (int) cp.changes.size()));
        check (p.applyChain().contains ("Chop Shop") , "APPLY says which plugins are missing");

        // 2) the plugins appear (and keep reporting 4x a second, like the real ones);
        //    play in real time so The Master can check the order
        struct Pinger : juce::Timer { std::function<void()> f; void timerCallback() override { f(); } } pinger;
        pinger.f = [&]
        {
            if (! watcherEnv.isObject()) return;
            sendStatus (dom, envWith (watcherEnv, "wobble", "same"));     // Dominatrix's output = The Master's input
            sendStatus (chop, envWith (watcherEnv, "spiky", "wobble"));   // Chop Shop's output = Dominatrix's input
        };
        pinger.startTimer (250);
        p.setNonRealtime (false);
        for (int round = 0; round < 30; ++round) { play (0.2, true); pump (30); }
        auto st = p.chainStatus();
        check (st.size() == 2 && st[0].linked && st[1].linked, "both prescribed plugins are linked");
        check (p.chainOrderKnown() && st[0].position == 0 && st[1].position == 1 && st[0].orderOk && st[1].orderOk,
               juce::String::formatted ("chain order detected from the level fingerprints (chop %d, dom %d)", st[0].position, st[1].position));

        p.setPluginMode (true);
        std::unique_ptr<juce::AudioProcessorEditor> ped (p.createEditor());
        auto* pme = dynamic_cast<MasterEditor*> (ped.get());
        pme->showPage (1);
        pump (900);
        pme->refreshNow();
        check (writePng (ped->createComponentSnapshot (ped->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_plugins_ready.png")), "MY PLUGINS: prescription, both linked, in order");

        // 3) APPLY (through the big button, like a user)
        pme->pressAutoFix();
        pump (60);
        check (p.getChainStage() == 2, "APPLY sends the settings");
        for (int i = 0; i < 40 && (dom.sets == 0 || chop.sets == 0); ++i) pump (50);
        pump (300);
        check (dom.sets == 1 && std::abs (dom.params["outputDb"] - cp.step ("dominatrix")->params.at ("outputDb")) < 0.01 && chop.sets == 1
               && std::abs (chop.params["ceilingDb"] - cp.step ("chopshop")->params.at ("ceilingDb")) < 0.01, "Dominatrix and Chop Shop received their settings");
        bool confirmed = true; int n = 0;
        for (auto& e : p.getLog()) { if (e.plugin != "The Master") { confirmed = confirmed && e.confirmed; ++n; } }
        check (n > 5 && confirmed, juce::String::formatted ("the log explains every change and shows both plugins confirmed (%d entries)", n));
        bool fromShown = false;
        for (auto& e : p.getLog()) if (e.plugin == "Dominatrix" && e.title.startsWith ("OUTPUT  0.0 > ")) fromShown = true;
        check (fromShown, "the log shows old > new values");
        check (p.isFixOn() && p.currentFixParams().loudOn && std::abs (p.currentFixParams().gainDb) < 0.01f && ! p.currentFixParams().dynOn && ! p.currentFixParams().stereoOn,
               "The Master's own part: EQ + safety limiter only");
        pump (800);
        pme->refreshNow();
        check (writePng (ped->createComponentSnapshot (ped->getLocalBounds(), true, 2.0f), dir.getChildFile ("gui_plugins_applied.png")), "MY PLUGINS: applied, with the change log");

        // 4) REFINE from what came out
        check (p.refineChain().isNotEmpty(), "REFINE waits for a play-through");
        dom.params["outputDb"] = 2.0;   // you turned Dominatrix's OUTPUT down by hand (the fakes don't change the audio, so the mix is still quiet)
        p.setNonRealtime (true);
        play (25.0, false);
        p.getPostWorker().waitUntilIdle (8000);
        pump (400);
        const int before = dom.sets;
        check (p.refineChain().isEmpty(), "REFINE after a play-through");
        for (int i = 0; i < 20 && dom.sets == before; ++i) pump (50);
        check (dom.sets > before && p.getLog().back().detail.isNotEmpty(), "REFINE adjusts Dominatrix's OUTPUT from the measured loudness and explains it");
        pinger.stopTimer();
        ped.reset();

        juce::MemoryBlock stt; p.getStateInformation (stt);
        MasterProcessor q; q.setStateInformation (stt.getData(), (int) stt.getSize());
        check (q.getChainStage() == 2 && q.getChainPlan().steps.size() == 2 && ! q.getLog().empty(), "the prescription and log are saved with the project");
    }

    // ---------------- GUI
    {
        MasterProcessor q;
        q.setPlayConfigDetails (2, 2, SR, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (q.createEditor());
        auto* me = dynamic_cast<MasterEditor*> (ed.get());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_empty.png")), "empty snapshot");

        // a believable rock mix with a few problems: a bit quiet, muddy, dull, wide bass, hot peaks
        const auto& rock = factory.genres[0];
        auto fx = meansOf (rock);
        fx.seconds = 201; fx.lufs = -11.4f; fx.truePeak = -0.3f; fx.samplePeak = -0.6f; fx.plr = fx.truePeak - fx.lufs;
        fx.band[3] += 4.4f; fx.band[2] += 2.2f; fx.band[6] -= 3.6f; fx.band[7] -= 2.4f; fx.band[0] -= 1.0f;
        float tot = 0; for (auto v : fx.band) tot += std::pow (10.0f, v / 10.0f);
        for (auto& v : fx.band) v -= 10.0f * std::log10 (tot);
        fx.width = -13.5f; fx.lowCorrelation = 0.72f; fx.correlation = 0.86f; fx.lra = 6.4f;
        fx.humSeconds = 4; fx.humHz = 60; fx.humDb = 21;
        juce::ValueTree st ("TheMaster");
        st.setProperty ("genre", "auto", nullptr);
        st.setProperty ("analysis", juce::JSON::toString (fx.toVar()), nullptr);
        juce::MemoryBlock mb;
        if (auto xml = st.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, mb);
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        me->refreshNow();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), dir.getChildFile ("gui.png")), "main snapshot");

        q.setGenreChoice ("metal");
        me->refreshNow();
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_metal.png")), "locked to Metal snapshot");
        q.setGenreChoice ("auto");

        // FIX IT page, before and after AUTO FIX, then with an override
        me->showPage (1);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_fix_empty.png")), "FIX IT page before AUTO FIX");
        me->pressAutoFix();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        me->refreshNow();
        check (q.isFixOn() && q.getPlan().valid, "AUTO FIX button works from the saved report");
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), dir.getChildFile ("gui_fix.png")), "FIX IT page after AUTO FIX");
        q.apvts.getParameter ("eq6")->setValueNotifyingHost (q.apvts.getParameter ("eq6")->convertTo0to1 (6.0f));
        q.apvts.getParameter ("width")->setValueNotifyingHost (q.apvts.getParameter ("width")->convertTo0to1 (125.0f));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        me->refreshNow();
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_fix_override.png")), "FIX IT page with overrides");
        q.revertToAuto (MasterProcessor::secEq); q.revertToAuto (MasterProcessor::secStereo);
        me->showPage (0);

        // live audio through the real thread
        q.resetAnalysis();
        q.setGenreChoice ("auto");
        q.setNonRealtime (true);
        q.prepareToPlay (SR, 512);
        juce::MidiBuffer midi;
        auto g = drumMix (0.9f);
        int64_t pos = 0;
        for (int i = 0; i < 40; ++i)
        {
            for (int k = 0; k < 4; ++k, pos += 512)
            {
                juce::AudioBuffer<float> buf (2, 512);
                for (int s = 0; s < 512; ++s) for (int c = 0; c < 2; ++c) buf.setSample (c, s, g (pos + s, c));
                q.processBlock (buf, midi);
            }
        }
        for (int b = 0; b < (int) (20 * SR / 512); ++b, pos += 512)
        {
            juce::AudioBuffer<float> buf (2, 512);
            for (int s = 0; s < 512; ++s) for (int c = 0; c < 2; ++c) buf.setSample (c, s, g (pos + s, c));
            q.processBlock (buf, midi);
        }
        q.getWorker().waitUntilIdle (5000);
        me->refreshNow();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_live.png")), "live analysis snapshot");
        me->pressAutoFix();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);   // the click is delivered asynchronously
        check (q.isFixOn(), "AUTO FIX from live audio");
        for (int b = 0; b < (int) (20 * SR / 512); ++b, pos += 512)
        {
            juce::AudioBuffer<float> buf (2, 512);
            for (int s2 = 0; s2 < 512; ++s2) for (int c = 0; c < 2; ++c) buf.setSample (c, s2, g (pos + s2, c));
            q.processBlock (buf, midi);
        }
        q.getPostWorker().waitUntilIdle (5000);
        me->refreshNow();
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_live_after.png")), "report on the fixed output");
        me->showPage (1);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_live_fix.png")), "FIX IT page with live meters");
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
