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
    // your own song-folder profiles live in a scratch folder for the test
    const auto myRoot = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("the-master-test-my-profiles");
    myRoot.deleteRecursively();
    mst::SongFolders::rootOverride() = myRoot;
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
        check (names.joinIntoString (",") == "Rock,Hard Rock,Punk,Metal", "factory genres: " + names.joinIntoString (", "));
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
        check (! lib.usingUserPack() && lib.genres().size() == 4, "library starts on the factory pack");
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
        auto country = factory.genres[3];
        country.id = "country"; country.name = "Country"; country.tracks = 12; country.source = "references";
        extra.genres.push_back (country);
        auto packFile = juce::File::createTempFile (".json");
        packFile.replaceWithText (extra.toJson());
        check (lib.importPack (packFile).isEmpty() && lib.indexOf ("country") == 4 && lib.genres().size() == 5, "import adds a new genre (Country)");
        check (lib.genres()[(size_t) lib.indexOf ("metal")].tracks == 0, "import replaces the previous user pack");
        auto exported = juce::File::createTempFile (".json");
        check (lib.exportPack (exported, "x").isEmpty() && [&] { mst::ProfilePack p; juce::String e; return mst::ProfilePack::parse (exported.loadFileAsString(), p, e) && p.genres.size() == 5; }(),
               "export writes every genre");
        packFile.replaceWithText ("garbage");
        check (lib.importPack (packFile).isNotEmpty() && lib.genres().size() == 5, "a broken file is refused and nothing changes");
        check (lib.restoreFactory().isEmpty() && ! tmp.existsAsFile() && lib.genres().size() == 4, "restore factory");
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
        check (hasItem (r, mst::Sev::fix, "TRUE PEAKS", mst::plug::dominatrix) && hasItem (r, mst::Sev::fix, "CLIPPING", mst::plug::chopShop), "over 0 dBTP + clipping -> Dominatrix CEILING and Cutthroat");

        auto spiky = ok; spiky.plr = rock.stat (mst::fPlr).mean + 4.0f;
        r = mst::advise (spiky, rock);
        check (hasItem (r, mst::Sev::fix, "PEAKS STICKING OUT", mst::plug::chopShop), "too dynamic -> Cutthroat");

        auto wideBass = ok; wideBass.lowCorrelation = 0.3f;
        r = mst::advise (wideBass, rock);
        check (hasItem (r, mst::Sev::fix, "LOW END", mst::plug::dominatrix), "stereo bass -> Dominatrix BASS MONO");

        auto hum = ok; hum.humSeconds = 5; hum.humHz = 50; hum.humDb = 25;
        r = mst::advise (hum, rock);
        check (hasItem (r, mst::Sev::fix, "50 Hz HUM", mst::plug::buzzKill), "hum -> Dead Air");

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


    // ---------------- per-track planning (The Servant), from the MIX profile only
    {
        const auto& rock = factory.genres[0];
        auto mix = meansOf (rock);
        mix.band[3] += 5.0f; mix.band[5] -= 2.5f; mix.band[6] -= 2.5f;      // muddy and a bit dull
        float t = 0; for (auto v : mix.band) t += std::pow (10.0f, v / 10.0f);
        for (auto& v : mix.band) v -= 10.0f * std::log10 (t);
        mix.plr = rock.stat (mst::fPlr).mean + 3.5f;                          // peaks stick out
        // three tracks: rhythm guitar (lots of 250-500), bass (lows), lead vocal (1-4k, buried), snare (peaky)
        const int T = 400;
        std::vector<float> mixS (T, 0.0f);
        std::vector<mst::TrackInfo> tr (4);
        const char* names[] = { "Rhythm Gtr", "Bass DI", "Lead Vox", "Snare" };
        const char* roles[] = { "Rhythm Guitar", "Bass", "Lead Vocal", "Snare" };   // The Servant 0.7 role names
        const double bp[4][10] = { { 0.01, 0.05, 0.4, 1.0, 0.5, 0.3, 0.2, 0.05, 0.01, 0.005 },
                                   { 0.6, 1.0, 0.3, 0.1, 0.03, 0.01, 0.005, 0.001, 0.0, 0.0 },
                                   { 0.0, 0.005, 0.03, 0.05, 0.06, 0.07, 0.05, 0.02, 0.01, 0.005 },
                                   { 0.01, 0.05, 0.1, 0.1, 0.1, 0.08, 0.08, 0.06, 0.02, 0.01 } };
        const double fader[4] = { 1.0, 0.5, 2.0, 1.0 };   // what the faders do after The Servant
        for (int i = 0; i < 4; ++i)
        {
            tr[(size_t) i].id = "t" + juce::String (i); tr[(size_t) i].name = names[i]; tr[(size_t) i].role = roles[i]; tr[(size_t) i].seconds = 40;
            double total = 0; for (int b = 0; b < 10; ++b) { tr[(size_t) i].bandPow[(size_t) b] = bp[i][b]; total += bp[i][b]; }
            for (int k = 0; k < T; ++k)
            {
                double v = total;
                if (i == 0) v *= 1.0 + 0.4 * std::sin (k * 0.13);
                if (i == 1) v *= 1.0 + 0.5 * std::sin (k * 0.31 + 1.0);
                if (i == 2) v *= (k / 40) % 2 == 0 ? 1.6 : 0.4;
                if (i == 3) v *= k % 5 == 0 ? 6.0 : 0.3;                  // the snare hits = the mix's peaks
                tr[(size_t) i].series.push_back ((float) v);
                mixS[(size_t) k] += (float) (v * fader[i]);
            }
        }
        // sibilance as The Servants measured it: the lead vocal hisses, the guitar is just bright, the snare spikes (but it's a snare)
        tr[2].sibP50 = -30; tr[2].sibP95 = -1; tr[2].sibPerMin = 40; tr[2].sibSeconds = 40; tr[2].sibHz = 5600;
        tr[0].sibP50 = -13; tr[0].sibP95 = -12; tr[0].sibPerMin = 2; tr[0].sibSeconds = 40;
        tr[3].sibP50 = -25; tr[3].sibP95 = 0; tr[3].sibPerMin = 120; tr[3].sibSeconds = 40;
        auto res = mst::planTracks (mix, rock, tr, mixS);
        check (res.tracks.size() == 4, "a plan for every linked track");
        bool gainsOk = true;
        for (int i = 0; i < 4; ++i) gainsOk = gainsOk && std::abs (res.tracks[(size_t) i].gainToMix - (float) fader[i]) < 0.1f * (float) fader[i];
        check (gainsOk, juce::String::formatted ("each track's fader found from the fingerprints (%.2f %.2f %.2f %.2f)", res.tracks[0].gainToMix, res.tracks[1].gainToMix, res.tracks[2].gainToMix, res.tracks[3].gainToMix));
        check (res.tracks[0].eq[3] < -1.0f && res.tracks[0].eq[3] < res.tracks[1].eq[3] && res.tracks[0].eq[3] < res.tracks[3].eq[3],
               juce::String::formatted ("the mud (250-500 Hz) is cut mostly on the rhythm guitar that makes it (%.1f dB)", res.tracks[0].eq[3]));
        check (res.tracks[1].eq[0] >= 0.0f && res.tracks[1].eq[1] >= 0.0f, juce::String::formatted ("the bass keeps its low end (sub %+.1f, bass %+.1f dB)", res.tracks[1].eq[0], res.tracks[1].eq[1]));
        {
            auto quiet = tr;
            for (auto& v : quiet[2].bandPow) v *= 0.25;
            for (auto& v : quiet[2].series) v *= 0.25f;
            std::vector<float> qMix (mixS);
            for (size_t k = 0; k < qMix.size(); ++k) qMix[k] -= 0.75f * tr[2].series[k] * (float) fader[2];
            const auto rq = mst::planTracks (mix, rock, quiet, qMix);
            check (rq.tracks[2].trimDb > 0.5f, juce::String::formatted ("the buried lead gets turned up (TRIM %+.1f dB)", rq.tracks[2].trimDb));
            check (std::abs (res.tracks[2].trimDb) < 0.01f, juce::String::formatted ("a lead with a fair share of the mids is left alone (TRIM %+.1f dB)", res.tracks[2].trimDb));
        }
        check (res.tracks[3].compOn && ! res.tracks[0].compOn, juce::String::formatted ("the snare that makes the peaks gets compressed (%.0f%%), the guitar doesn't", res.tracks[3].comp));
        bool punchWhy = false; for (auto& w : res.tracks[3].why) if (w.startsWith ("COMP STYLE PUNCH")) punchWhy = true;
        check (punchWhy && res.tracks[3].params.count ("compStyle") == 0, "the snare's compressor reason names its style (PUNCH); The Master leaves STYLE on AUTO");
        check (! res.tracks[0].why.isEmpty() && res.tracks[0].why[0].contains ("% of the mix"), "every track's changes come with reasons");
        bool maxSix = true;
        for (auto& tp : res.tracks) { int k = 0; for (auto v : tp.eq) if (std::abs (v) >= 0.5f) ++k; maxSix = maxSix && k <= 6; }
        check (maxSix, "no track gets more EQ moves than The Servant has bands (6)");
        check (res.tracks[2].deessOn && res.tracks[2].params.at ("deessOn") == 1.0f && res.tracks[2].deess >= 20.0f && res.tracks[2].summary().contains ("DE-ESS"),
               juce::String::formatted ("the hissy lead vocal gets de-essed (%.0f%%)", res.tracks[2].deess));
        check (! res.tracks[0].deessOn && ! res.tracks[3].deessOn && res.tracks[3].params.at ("deessOn") == 0.0f,
               "a bright guitar and a snare are left alone");
        bool deWhy = false; for (auto& w : res.tracks[2].why) if (w.startsWith ("DE-ESS") && w.contains ("5.6 kHz")) deWhy = true;
        check (deWhy, "the reason names how far the s sounds jump and where the split is");
        check (res.tracks[0].params.at ("toneOn") == 1.0f && std::abs (res.tracks[0].params.at ("t1g") - res.tracks[0].eq[3]) < 0.01f, "the biggest move goes on The Servant's band 1");

        // a dark mix: nothing gets boosted above a track's own low-pass (an amp sim's fizz)
        {
            auto dark = mix; dark.band[8] -= 10.0f; dark.band[9] -= 12.0f;
            auto lp = tr; lp[0].lpf = 10700.0f;
            const auto open = mst::planTracks (dark, rock, tr, mixS);
            const auto shut = mst::planTracks (dark, rock, lp, mixS);
            check (open.tracks[0].eq[8] > 0.5f, juce::String::formatted ("a dark mix: an open guitar takes some sheen (%+.1f dB)", open.tracks[0].eq[8]));
            check (shut.tracks[0].eq[8] == 0.0f && shut.tracks[0].eq[9] == 0.0f && shut.tracks[0].eq[6] >= 0.0f,
                   juce::String::formatted ("a guitar low-passed at 10.7 kHz gets no sheen or air boost (%+.1f, %+.1f dB)", shut.tracks[0].eq[8], shut.tracks[0].eq[9]));
        }

        // BACKING VOCALS: sit under the lead, SMOOTH, de-essable
        {
            auto withBv = tr;
            mst::TrackInfo bv = tr[2]; bv.id = "bv"; bv.name = "Backing Vox"; bv.role = "Backing Vocals";
            for (auto& v : bv.bandPow) v *= 0.8;   // almost as present as the lead
            for (auto& v : bv.series) v *= 0.8f;
            withBv.push_back (bv);
            const auto rb = mst::planTracks (mix, rock, withBv, {});
            const auto& b = rb.tracks.back();
            bool why = false; for (auto& w : b.why) if (w.startsWith ("TRIM") && w.contains ("under the lead")) why = true;
            check (b.role == "Backing Vocals" && b.trimDb <= rb.tracks[2].trimDb - 2.0f && why,
                   juce::String::formatted ("backing vocals that compete with the lead are pulled under it (TRIM %+.1f dB, lead %+.1f dB)", b.trimDb, rb.tracks[2].trimDb));
            // a punk stack: 16 backing parts, together 4 dB over the lead. Pull them a few dB under,
            // never -6 on every one plus +4 on the lead (that throws the lead out on top)
            auto stack = tr;
            for (int k = 0; k < 16; ++k)
            {
                mst::TrackInfo s = tr[2]; s.id = "bv" + juce::String (k); s.name = "BVox" + juce::String (k); s.role = "Backing Vocals";
                for (auto& v : s.bandPow) v *= 0.157;   // 16 x 0.157 = 2.5x the lead (+4 dB)
                for (auto& v : s.series) v *= 0.157f;
                stack.push_back (s);
            }
            const auto rs = mst::planTracks (mix, rock, stack, {});
            const float lt = rs.tracks[2].trimDb, bt = rs.tracks.back().trimDb;
            check (lt <= 3.0f && bt >= -4.0f && lt - bt >= 3.0f && lt - bt <= 6.5f,
                   juce::String::formatted ("a big stack of backing vocals ends up a little under the lead (lead %+.1f, each backing %+.1f dB)", lt, bt));
            // Flatliner: a lead and its stack, whose level comes and goes -> on a vocal bus
            for (auto& t : stack) if (t.role.startsWith ("Backing")) { auto& sr = t.series; for (size_t k = 0; k < sr.size(); ++k) if ((k / 50) % 2 == 1) sr[k] *= 0.02f; }
            const auto fa = mst::flatlinerAdvice (stack);
            check (fa.size() == 1 && fa[0].backing && fa[0].where == "a backing vocal bus" && fa[0].why.contains ("16 backing"),
                   "Flatliner: backing vocals that swing get their own bus, ridden apart from the lead (" + (fa.empty() ? juce::String() : fa[0].where) + ")");
            auto twoLeads = stack; twoLeads.push_back (tr[2]); twoLeads.back().id = "l2"; twoLeads.back().name = "Lead Vox Dbl";
            for (auto* t : { &twoLeads[2], &twoLeads.back() }) for (size_t k = 0; k < t->series.size(); ++k) if ((k / 40) % 2 == 1) t->series[k] *= 0.03f;
            const auto fb = mst::flatlinerAdvice (twoLeads);
            check (fb.size() == 2 && fb[0].where == "a lead vocal bus" && fb[0].why.contains ("Keep the backing vocals off") && fb[1].backing,
                   "Flatliner: a lead spread over two tracks gets a lead bus, the backing vocals their own");
            {
                mst::HelperInfo h; h.kind = "flatliner"; h.track = "BVox Bus"; h.role = "Lead";
                h.extra = juce::var (new juce::DynamicObject());
                for (auto [k, v] : std::initializer_list<std::pair<const char*, double>> { { "p10", -40.0 }, { "p50", -22.0 }, { "p90", -18.0 }, { "floorDb", -70.0 }, { "seconds", 40.0 } })
                    h.extra.getDynamicObject()->setProperty (k, v);
                const auto fp = mst::planFlatliner (h, rock);
                check (fp.params.at ("range") == 3.0f, juce::String::formatted ("Flatliner on a backing vocal bus rides gently (RANGE %.0f dB)", fp.params.at ("range")));
            }
            {   // a Servant on a drum bus is found from its fingerprint and isn't planned as a track
                auto withBus = tr;
                mst::TrackInfo bus; bus.id = "bus"; bus.name = "Drums"; bus.role = "Drums"; bus.seconds = 40;
                auto kick = tr[3]; kick.id = "k"; kick.name = "01_Kick"; kick.role = "Kick";
                for (size_t k = 0; k < kick.series.size(); ++k) kick.series[k] = (k % 7 == 0 ? 3.0f : 0.05f);
                withBus.push_back (kick);
                bus.series.resize (tr[3].series.size());
                for (size_t k = 0; k < bus.series.size(); ++k) bus.series[k] = 0.8f * tr[3].series[k] + 1.2f * kick.series[k];
                for (int b = 0; b < 10; ++b) bus.bandPow[(size_t) b] = 0.8 * tr[3].bandPow[(size_t) b] + 1.2 * kick.bandPow[(size_t) b];
                withBus.push_back (bus);
                const auto found = mst::findBuses (withBus);
                check (found.size() == 1 && withBus[found[0].index].name == "Drums" && found[0].carries.contains ("Snare") && found[0].carries.contains ("01_Kick"),
                       juce::String::formatted ("a Servant on the drum bus is found as a bus (%d found)", (int) found.size()));
                check (mst::findBuses (tr).empty(), "ordinary tracks aren't taken for buses");
            }
            auto jumpy = tr; for (size_t k = 0; k < jumpy[2].series.size(); ++k) if ((k / 40) % 2 == 1) jumpy[2].series[k] *= 0.05f;
            const auto la = mst::flatlinerAdvice (jumpy);
            const juce::String lw = la.empty() ? juce::String() : la[0].where;
            check (lw == "Lead Vox", "Flatliner: a single lead that swings gets it on its own track (" + lw + ")");

            bool leadUp = rb.tracks[2].trimDb >= 0.0f;
            check (leadUp, "the lead vocal isn't turned down for the backing vocals");
        }

        // TRANSIENTS
        auto withKick = tr;
        mst::TrackInfo kick; kick.id = "k"; kick.name = "Kick In"; kick.role = "Kick"; kick.seconds = 40;
        const double kb[10] = { 0.8, 1.0, 0.2, 0.05, 0.02, 0.01, 0.02, 0.01, 0.0, 0.0 };
        for (int b = 0; b < 10; ++b) kick.bandPow[(size_t) b] = kb[b];
        withKick.push_back (kick);
        for (auto& t : withKick) t.transSeconds = 40;
        withKick[0].transP95 = 18;   // spiky picking
        withKick[2].transP95 = 9;    // lead
        withKick[3].transP95 = 8;    // soft snare
        withKick[4].transP95 = 12;   // kick
        auto flatMix = mix; flatMix.plr = rock.stat (mst::fPlr).mean - 2.0f * rock.stat (mst::fPlr).sd;
        auto peakyMix = mix; peakyMix.plr = rock.stat (mst::fPlr).mean + 2.0f * rock.stat (mst::fPlr).sd;
        auto boomMix = mix; for (int b = 0; b < 3; ++b) boomMix.band[(size_t) b] = rock.stat (mst::fBand0 + b).mean + 2.0f * rock.stat (mst::fBand0 + b).sd;
        boomMix.plr = rock.stat (mst::fPlr).mean;
        const auto rf = mst::planTracks (flatMix, rock, withKick, {});
        check (rf.tracks[3].transOn && rf.tracks[3].transAtk >= 15.0f && rf.tracks[4].transAtk >= 15.0f && rf.tracks[3].params.at ("transOn") == 1.0f
               && rf.tracks[3].summary().contains ("ATTACK +"),
               juce::String::formatted ("flat mix: snare and kick get more attack (%+.0f%%, %+.0f%%)", rf.tracks[3].transAtk, rf.tracks[4].transAtk));
        check (! rf.tracks[0].transOn && ! rf.tracks[1].transOn && ! rf.tracks[2].transOn && rf.tracks[2].params.at ("transOn") == 0.0f, "flat mix: guitar, bass and lead left alone");
        const auto rp = mst::planTracks (peakyMix, rock, withKick, {});
        check (rp.tracks[0].transAtk == -20.0f && rp.tracks[3].transAtk <= 0.0f && rp.tracks[4].transAtk == 0.0f,
               juce::String::formatted ("peaky mix: spiky picking softened (%+.0f%%), no boosts (snare %+.0f%%)", rp.tracks[0].transAtk, rp.tracks[3].transAtk));
        auto hardSnare = withKick; hardSnare[3].transP95 = 18;
        const auto rh = mst::planTracks (flatMix, rock, hardSnare, {});
        check (rh.tracks[3].transAtk == 0.0f, "a snare whose hits are already hard gets no more attack");
        const auto rb = mst::planTracks (boomMix, rock, withKick, {});
        bool susWhy = false; for (auto& w : rb.tracks[4].why) if (w.startsWith ("SUSTAIN") && w.contains ("crowded")) susWhy = true;
        check (rb.tracks[4].transSus == -30.0f && susWhy && rb.tracks[4].params.at ("transSus") == -30.0f, "crowded low end: the kick's boom is shortened (SUSTAIN -30%), with the reason");
        const auto rt = mst::ChainPlan::fromVar ([&] { mst::ChainPlan c; c.valid = true; c.tracks = rb.tracks; return c.toVar(); }());
        check (rt.tracks.size() == rb.tracks.size() && rt.tracks[4].transSus == -30.0f && rt.tracks[4].transOn, "transient plan survives save/load");
    }

    // ---------------- MY PROFILES: a folder of songs becomes a profile
    {
        auto writeWav = [] (const juce::File& f, const Gen& g, double secs)
        {
            f.getParentDirectory().createDirectory(); f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (SR).withNumChannels (2).withBitsPerSample (24));
            juce::AudioBuffer<float> b (2, (int) (secs * SR));
            for (int i = 0; i < b.getNumSamples(); ++i) for (int c = 0; c < 2; ++c) b.setSample (c, i, g (i, c));
            return w != nullptr && w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        };
        mst::SongFolders::ensureRoot();
        check (myRoot.getChildFile ("READ ME.txt").existsAsFile(), "My Profiles folder is made, with a READ ME");
        check (writeWav (myRoot.getChildFile ("My Band").getChildFile ("song one.wav"), drumMix (0.8f), 20.0), "wrote a test song");
        myRoot.getChildFile ("Empty One").createDirectory();
        mst::SongFolders sf;
        sf.setFactory (factory.genres);
        sf.scanNow();
        auto profs = sf.getProfiles();
        auto folders = sf.getFolders();
        check (profs.size() == 1 && profs[0].id == "myMyBand" && profs[0].name == "My Band" && profs[0].tracks == 1 && profs[0].source == "songs",
               "a folder with one song becomes a profile, named after the folder");
        bool emptyListed = false; for (auto& f : folders) if (f.name == "Empty One" && f.songs == 0) emptyListed = true;
        check (emptyListed && folders.size() == 2, "an empty folder is listed but makes no profile yet");
        check (myRoot.getChildFile ("My Band").getChildFile (".castle-danger-cache.json").existsAsFile(), "the measurements are cached in the folder");
        const auto songFx = analyse (drumMix (0.8f), 20.0);
        check (std::abs (profs[0].stat (mst::fLufs).mean - songFx.lufs) < 0.3f, juce::String::formatted ("the profile is the song as The Master hears it (%.1f vs %.1f LUFS)", profs[0].stat (mst::fLufs).mean, songFx.lufs));
        const auto near = mst::detectGenre (songFx, factory.genres);
        check (profs[0].stat (mst::fBand0 + 3).sd >= factory.genres[(size_t) near.front().index].stat (mst::fBand0 + 3).sd - 1e-4f,
               "with one song the allowed spread comes from the closest factory genre");

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        mst::SongFolders sf2; sf2.setFactory (factory.genres); sf2.scanNow();
        check (sf2.getProfiles().size() == 1 && juce::Time::getMillisecondCounterHiRes() - t0 < 2000.0, "another copy reads the cache instead of measuring again");

        writeWav (myRoot.getChildFile ("My Band").getChildFile ("song two.wav"), drumMix (0.4f), 20.0);
        sf.scanNow();
        check (sf.getProfiles().size() == 1 && sf.getProfiles()[0].tracks == 2, "dropping in another song refines the profile");
        myRoot.getChildFile ("My Band").getChildFile ("song two.wav").deleteFile();
        sf.scanNow();
        check (sf.getProfiles()[0].tracks == 1, "removing a song takes it out again");

        // in the plugin: listed after the genres, picked, saved with the project
        MasterProcessor p;
        p.getSongFolders().scanNow();
        p.refreshSongProfiles();
        const int mi = p.getLibrary().indexOf ("myMyBand");
        check (mi >= p.getLibrary().numBuiltIn() && p.getLibrary().isCustom ("myMyBand"), "the plugin lists your profile after the genres");
        p.setGenreChoice ("myMyBand");
        const auto tgt = p.resolveTarget (songFx);
        check (tgt.index == mi, "aiming at your profile");
        juce::File made;
        check (p.createSongProfile ("Album / Sound", made) && made.isDirectory() && ! made.getFileName().containsChar ('/'), "NEW makes a folder with a safe name");
        juce::MemoryBlock mb; p.getStateInformation (mb);
        myRoot.getChildFile ("My Band").deleteRecursively();
        MasterProcessor q;
        q.getSongFolders().scanNow(); q.refreshSongProfiles();
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        check (q.getGenreChoice() == "myMyBand" && q.getLibrary().indexOf ("myMyBand") >= 0, "the project keeps its profile even if the folder is gone");
        juce::File exported = myRoot.getChildFile ("export.json");
        q.getLibrary().exportPack (exported, "t");
        mst::ProfilePack ep; juce::String perr;
        check (mst::ProfilePack::parse (exported.loadFileAsString(), ep, perr) && ep.find ("myMyBand") == nullptr, "exporting a pack leaves your own profiles out");
    }

    // ---------------- The Freak: which ranges jump around, and the bands it gets
    {
        auto byId = [&] (const char* id) -> const mst::GenreProfile& { for (auto& g : factory.genres) if (g.id == id) return g; return factory.genres[0]; };
        const auto& rockG = byId ("rock");
        auto pinkMix = [] (bool bursts)
        {
            auto pk = std::make_shared<std::array<Pink, 2>>();
            return Gen ([pk, bursts] (int64_t n, int ch)
            {
                float s = 0.3f * (*pk)[(size_t) ch].next (noise (n * 2 + ch));
                const double t = (double) n / SR;
                if (bursts && std::fmod (t, 2.0) < 0.3)   // a harsh 2.9 kHz spike, 15 % of the time
                    s += 0.12f * (float) std::sin (juce::MathConstants<double>::twoPi * 2900.0 * t);
                return s;
            });
        };
        const auto steady = analyse (pinkMix (false), 30.0);
        const auto harsh = analyse (pinkMix (true), 30.0);
        float maxSteady = 0; for (auto v : steady.swing) maxSteady = std::max (maxSteady, v);
        check (steady.valid && maxSteady > 0.0f && maxSteady < 3.0f, "steady pink noise: no range jumps around (max swing " + juce::String (maxSteady, 1) + " dB)");
        check (harsh.swing[6] > 8.0f && harsh.swing[3] < 3.0f, "2.9 kHz spikes: PRESENCE swings " + juce::String (harsh.swing[6], 1) + " dB, LOW MID " + juce::String (harsh.swing[3], 1));
        auto f0 = mst::planFreak (steady, rockG);
        auto f1 = mst::planFreak (harsh, rockG);
        check (f0.bands.empty() && f0.params.at ("b1on") == 0.0f, "The Freak: nothing to do on a steady mix");
        check (f1.bands.size() == 1 && f1.bands[0].range == 6 && f1.params.at ("b1on") == 1.0f && std::abs (f1.params.at ("b1f") - 2900.0f) < 1.0f
               && f1.params.at ("b1d") <= -2.0f && f1.params.at ("b1d") >= -6.0f && f1.params.at ("b2on") == 0.0f && f1.notes[0].contains ("PRESENCE"),
               "The Freak: one band on PRESENCE (2.9 kHz, depth " + juce::String (f1.params.count ("b1d") ? f1.params.at ("b1d") : 0.0f, 0) + " dB, threshold "
               + juce::String (f1.params.count ("b1t") ? f1.params.at ("b1t") : 0.0f, 1) + " dB) with a reason");
        auto c0 = mst::planChain (steady, rockG), c1 = mst::planChain (harsh, rockG);
        check (c0.step ("freak") == nullptr && c1.step ("freak") != nullptr && c1.steps[0].kind == "freak" && c1.steps.back().kind == "dominatrix",
               "prescription: The Freak first (only when needed), Dominatrix last before The Master");
        bool why = false; for (auto& c : c1.changes) if (c.plugin == "freak" && c.why.contains ("PRESENCE") && c.why.contains ("Threshold")) why = true;
        check (why, "The Freak's band change explains the range, the jump and the threshold");
        const auto rt = mst::ChainPlan::fromVar (c1.toVar());
        check (rt.step ("freak") != nullptr && rt.step ("freak")->notes.size() == 6 && rt.step ("freak")->notes[0] == c1.step ("freak")->notes[0], "Freak step + notes survive save/load");
        {
            const auto lo = mst::planChain (harsh, rockG, false), hi = mst::planChain (harsh, rockG, true);
            const auto* dl = lo.step ("dominatrix"); const auto* dh = hi.step ("dominatrix");
            check (dl->params.at ("ceiling") == 0.0f && dh->params.at ("ceiling") == 1.0f && dh->params.at ("outputDb") < dl->params.at ("outputDb")
                   && hi.masterFix.ceilingDb == -6.0f && hi.headroom,
                   juce::String::formatted ("OUTPUT -6 dB plan: Dominatrix CEILING -6, OUTPUT %.1f (peaks to -6.3) instead of %.1f; safety limiter -6", dh->params.at ("outputDb"), dl->params.at ("outputDb")));
            const auto quietMix = mst::advise (harsh, rockG, true);
            bool loudFix = false; for (auto& it : quietMix.items) if (it.title.contains ("QUIETER")) loudFix = true;
            check (! loudFix && mst::advise (harsh, rockG, true).score >= mst::advise (harsh, rockG, false).score, "OUTPUT -6 dB: the report doesn't mark a mix down for being quieter than a finished record");
        }
        const auto fv = mst::Features::fromVar (harsh.toVar());
        check (std::abs (fv.swing[6] - harsh.swing[6]) < 0.01f, "swing measurements survive save/load");
    }

    // ---------------- Low Blow: kick/bass clash from the Servants
    {
        auto byId = [&] (const char* id) -> const mst::GenreProfile& { for (auto& g : factory.genres) if (g.id == id) return g; return factory.genres[0]; };
        auto tr = [] (const char* name, const char* role, std::initializer_list<double> bands)
        {
            mst::TrackInfo t; t.id = name; t.name = name; t.role = role; t.seconds = 30;
            int b = 0; for (auto v : bands) t.bandPow[(size_t) b++] = v;
            return t;
        };
        std::vector<mst::TrackInfo> fight { tr ("Kick", "Kick", { 0.010, 0.020, 0.004, 0.001, 0, 0, 0.0005, 0.001, 0, 0 }),
                                            tr ("Bass DI", "Bass", { 0.006, 0.018, 0.010, 0.003, 0.001, 0, 0, 0, 0, 0 }),
                                            tr ("Rhythm Gtr", "Rhythm", { 0, 0.001, 0.02, 0.05, 0.04, 0.02, 0.01, 0, 0, 0 }) };
        std::vector<mst::TrackInfo> apart { tr ("Kick", "Kick", { 0.030, 0.002, 0.0002, 0, 0, 0, 0.001, 0, 0, 0 }),
                                            tr ("Bass", "Bass", { 0.0001, 0.0005, 0.02, 0.01, 0.002, 0, 0, 0, 0, 0 }) };
        const auto c1 = mst::findLowClash (fight), c2 = mst::findLowClash (apart), c0 = mst::findLowClash ({ fight[2] });
        check (c1.measured && c1.found && c1.band == 1 && c1.kickBand == 1 && c1.gapDb < 1.0f && c1.bassName == "Bass DI",
               "kick and bass both in 60-120 Hz: clash found (" + juce::String (c1.gapDb, 1) + " dB apart)");
        check (c2.measured && ! c2.found && c2.kickBand == 0, "sub kick + mid-bass: no clash");
        check (! c0.measured, "no Kick and Bass Servants: nothing measured");

        auto ex = [] (std::initializer_list<std::pair<const char*, juce::var>> kv) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : kv) o->setProperty (k, v); return juce::var (o); };
        mst::HelperInfo lb { "lb1", "lowblow", "Bass DI", "Bass", ex ({ { "sidechain", true }, { "gapMs", 250.0 }, { "hitsPerMin", 240.0 } }) };
        const auto pm = mst::planLowBlow (lb, c1, byId ("metal")), pr = mst::planLowBlow (lb, c1, byId ("rock"));
        check (pm.params.at ("depth") == 7.0f && pr.params.at ("depth") == 5.0f && pm.params.at ("crossover") == 140.0f && pm.params.at ("mode") == 0.0f,
               juce::String::formatted ("Low Blow DEPTH from the genre + a near-even fight (metal %.0f, rock %.0f dB), CROSSOVER %.0f Hz above the kick's thump",
                                        pm.params.at ("depth"), pr.params.at ("depth"), pm.params.at ("crossover")));
        check (std::abs (pm.params.at ("release") - 75.0f) < 0.5f && pm.why.joinIntoString (" ").contains ("250 ms"), "RELEASE from the gap between kicks (250 ms apart -> 75 ms)");
        const auto pa = mst::planLowBlow (lb, c2, byId ("rock"));
        check (pa.params.at ("depth") == 2.0f && pa.params.at ("crossover") == 90.0f, "barely overlapping: lighter DEPTH, crossover 90 Hz for a sub kick");
        mst::HelperInfo noSide = lb; noSide.extra = ex ({ { "sidechain", false } });
        const auto pn = mst::planLowBlow (noSide, c1, byId ("rock"));
        check (pn.note.contains ("NO KICK ON SIDECHAIN") && pn.why.joinIntoString (" ").contains ("sidechain"), "no kick on the sidechain: says how to route it");
        check (mst::plugDisplayName ("lowblow") == "Low Blow" && MasterProcessor::isTrackKind ("lowblow"), "Low Blow counts as a track plugin");
    }

    // ---------------- Dead Air and Flatliner on the tracks
    {
        auto byId = [&] (const char* id) -> const mst::GenreProfile& { for (auto& g : factory.genres) if (g.id == id) return g; return factory.genres[0]; };
        const auto& metal = byId ("metal");
        const auto& rockG = byId ("rock");
        check (metal.id == "metal" && rockG.id == "rock", "factory has metal and rock");
        mst::Features mix = meansOf (metal); mix.humHz = 60; mix.humSeconds = 4.0f;
        auto ex = [] (std::initializer_list<std::pair<const char*, juce::var>> kv) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : kv) o->setProperty (k, v); return juce::var (o); };
        mst::HelperInfo bk { "b1", "buzzkill", "Rhythm Gtr", "Rhythm", ex ({ { "floorDb", -62.0 }, { "playDb", -20.0 }, { "seconds", 30.0 }, { "learned", true } }) };
        auto bm = mst::planBuzzKill (bk, mix, metal);
        auto bmLead = mst::planBuzzKill ({ "b2", "buzzkill", "Lead Gtr", "Lead", bk.extra }, mix, metal);
        auto bp = mst::planBuzzKill (bk, mix, rockG);
        check (bm.params.at ("release") >= 0.75f && bp.params.at ("release") <= 0.5f && bmLead.params.at ("release") < bm.params.at ("release"),
               juce::String::formatted ("Dead Air RELEASE: tight for metal rhythm (%.2f), gentler on a metal lead (%.2f), looser for rock (%.2f)",
                                        bm.params.at ("release"), bmLead.params.at ("release"), bp.params.at ("release")));
        check (bm.params.at ("listen") == 0.0f && bm.why.joinIntoString (" ").contains ("60 Hz hum"), "Dead Air: HEAR REMOVED off, and the mix's hum is the reason");
        auto noisy = bk; noisy.extra = ex ({ { "floorDb", -45.0 }, { "playDb", -20.0 }, { "seconds", 30.0 }, { "learned", true } });
        check (mst::planBuzzKill (noisy, mix, rockG).params.at ("release") > bp.params.at ("release"), "Dead Air closes sooner on a noisy track");
        auto quietGtr = bk; quietGtr.extra = ex ({ { "floorDb", -90.0 }, { "playDb", -120.0 }, { "seconds", 0.0 }, { "learned", false } });
        const auto qp = mst::planBuzzKill (quietGtr, mix, rockG);
        check (qp.note.contains ("CLEAN") && ! qp.note.contains ("NOT LEARNED") && qp.why.joinIntoString (" ").contains ("another track"),
               "Dead Air on a near-silent guitar: CLEAN, nothing to remove (not NOT LEARNED), and the hum is from another track");

        mst::HelperInfo fl { "f1", "flatliner", "Lead Vox", "Lead", ex ({ { "p10", -34.0 }, { "p50", -22.0 }, { "p90", -14.0 }, { "floorDb", -70.0 }, { "seconds", 60.0 } }) };
        auto fp = mst::planFlatliner (fl, rockG);
        auto fm = mst::planFlatliner (fl, byId ("punk"));
        check (std::abs (fp.params.at ("target") + 22.0f) < 0.01f && fp.params.at ("range") >= 10.0f && fm.params.at ("range") < fp.params.at ("range"),
               juce::String::formatted ("Flatliner: TARGET at the lead's average (%.1f), RANGE from its 20 dB swing (rock %.0f, punk %.0f)",
                                        fp.params.at ("target"), fp.params.at ("range"), fm.params.at ("range")));
        check (fp.params.at ("gate") < -34.0f + 0.01f && fp.params.at ("gate") > -70.0f && fp.params.at ("ride") == 1.0f,
               juce::String::formatted ("Flatliner GATE between the gaps and the quietest words (%.0f dB)", fp.params.at ("gate")));
        auto steady = fl; steady.extra = ex ({ { "p10", -24.0 }, { "p50", -21.0 }, { "p90", -19.0 }, { "floorDb", -70.0 }, { "seconds", 60.0 } });
        check (mst::planFlatliner (steady, rockG).params.at ("range") == 3.0f, "Flatliner: a steady lead gets only a gentle ride");
        auto fresh = fl; fresh.extra = ex ({ { "seconds", 1.0 } });
        check (mst::planFlatliner (fresh, rockG).params.empty(), "Flatliner: nothing is set until it has heard the song");
        // CUTTHROAT on a drum track, DOMINATRIX on a thin bass
        {
            mst::TrackInfo snare; snare.name = "Snare"; snare.role = "Snare"; snare.transP95 = 10.0f; snare.transSeconds = 30.0f;
            mst::HelperInfo cs { "c1", "chopshop", "Snare", "Snare", ex ({ { "track", "Snare" }, { "songPeakDb", -6.0 }, { "seconds", 30.0 } }) };
            const auto c = mst::planTrackChop (cs, &snare, byId ("rock"));
            check (std::abs (c.params.at ("ceilingDb") + 9.0f) < 0.01f && c.params.at ("shape") == 0.0f && c.params.at ("mode") == 0.0f && c.params.at ("oversample") == 1.0f,
                   juce::String::formatted ("Cutthroat on the snare: ceiling 3 dB under its loudest hit (%.1f), HARD, MASTER mode, 2x", c.params.at ("ceilingDb")));
            auto kitH = cs; kitH.role = "Drums";
            const auto ck = mst::planTrackChop (kitH, &snare, metal);
            check (ck.params.at ("shape") == 1.0f && ck.params.at ("ceilingDb") < c.params.at ("ceilingDb"), "a full kit gets a SOFT knee; metal shaves a little deeper");
            auto fresh = cs; fresh.extra = ex ({ { "track", "Snare" }, { "seconds", 0.0 } });
            check (mst::planTrackChop (fresh, &snare, byId ("rock")).params.empty(), "Cutthroat: nothing is set until it has heard the song");
            mst::TrackPlan sv; sv.kind = "servant"; sv.role = "Snare"; sv.compOn = true;
            mst::TrackPlan gt = sv; gt.role = "Rhythm Guitar";
            check (mst::wantsDrumClip (sv) && ! mst::wantsDrumClip (gt), "a clipper is wanted on drum tracks The Master would compress for the peaks, not on guitars");

            mst::TrackInfo bass; bass.name = "Bass DI"; bass.role = "Bass";
            const double bb[10] = { 0.6, 1.0, 0.3, 0.1, 0.03, 0.01, 0.005, 0.001, 0.0, 0.0 };
            for (int k = 0; k < 10; ++k) bass.bandPow[(size_t) k] = bb[k];
            mst::HelperInfo dh { "d1", "dominatrix", "Bass DI", "Bass", ex ({ { "track", "Bass DI" } }) };
            const auto dm = mst::planTrackDom (dh, &bass, metal), dr = mst::planTrackDom (dh, &bass, byId ("rock"));
            check (mst::bassHarmonicsDb (bass) < mst::kThinBassDb && dm.params.at ("satType") == 4.0f && dr.params.at ("satType") == 1.0f
                   && dm.params.at ("discipline") == 0.0f && dm.params.at ("ceiling") == 2.0f && dm.params.at ("match") == 1.0f && dm.params.at ("saturation") > 25.0f,
                   juce::String::formatted ("Dominatrix on a thin bass (%.1f dB): WHIP for metal, TUBE for rock, %.0f%% saturation, no comp, no clipper, MATCH on",
                                            mst::bassHarmonicsDb (bass), dm.params.at ("saturation")));
        }
        mst::ChainPlan cp; cp.valid = true; cp.tracks = { bm, fp };
        auto rt = mst::ChainPlan::fromVar (juce::JSON::parse (juce::JSON::toString (cp.toVar())));
        check (rt.tracks.size() == 2 && rt.tracks[0].kind == "buzzkill" && rt.tracks[1].summary() == fp.summary() && rt.tracks[1].params.at ("range") == fp.params.at ("range"),
               "Dead Air / Flatliner plans survive a save/load");
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
        p.apvts.getParameter ("headroom")->setValueNotifyingHost (1.0f);   // these checks are for a finished -1 dB master
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

        // OUTPUT -6 dB: flipping it re-plans the loudness for headroom
        p.apvts.getParameter ("headroom")->setValueNotifyingHost (0.0f);
        p.outputTargetChanged();
        check (p.headroomMode() && std::abs (p.currentFixParams().ceilingDb + 6.0f) < 0.01f, "OUTPUT -6 dB: built-in ceiling moves to -6 dBTP");
        play (30.0);
        const auto hr = p.getAfterFeatures();
        check (hr.truePeak <= -6.0f && hr.truePeak > -8.5f, juce::String::formatted ("OUTPUT -6 dB: peaks land just under -6 dB (%.2f dBTP, %.1f LUFS)", hr.truePeak, hr.lufs));
        MasterProcessor fresh;
        check (fresh.headroomMode(), "a new Master defaults to OUTPUT -6 dB");
        p.apvts.getParameter ("fixOn")->setValueNotifyingHost (0.0f);
        check (p.getLatencySamples() > 0 && ! p.isFixOn(), "FIX off keeps the same latency (A/B doesn't shift timing)");
    }


    // ---------------- MY PLUGINS: prescription, chain detection, APPLY over Castle Danger Link
    {
        MasterProcessor p;
        p.setPlayConfigDetails (2, 2, SR, 512);
        p.prepareToPlay (SR, 512);
        p.setGenreChoice ("rock");
        p.apvts.getParameter ("headroom")->setValueNotifyingHost (1.0f);   // these checks are for a finished -1 dB master
        auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

        // fake Dominatrix + Cutthroat living in "another plugin binary" (a separate node: real socket)
        auto other = cdlink::Node::createIsolatedForTesting();
        juce::var masterEnv;
        struct Fake
        {
            juce::String kind; std::map<juce::String, double> params; juce::var lastSet; int sets = 0;
            std::unique_ptr<cdlink::Endpoint> ep;
        };
        Fake dom { "dominatrix", { { "discipline", 30 }, { "outputDb", 0 }, { "ceiling", 0 }, { "bassMono", 0 }, { "width", 100 }, { "satType", 0 }, { "saturation", 20 } } };
        Fake chop { "chopshop", { { "ceilingDb", 0 }, { "mode", 0 }, { "shape", 0 } } };
        Fake frk { "freak", { { "b1on", 0 }, { "b1d", -6 }, { "amount", 100 } } };
        int freakNotes = 0; juce::String freakNote0;
        Fake gtr { "servant", { { "trim", 0 }, { "comp", 0 }, { "compOn", 0 }, { "toneOn", 0 } } };
        Fake vox { "servant", { { "trim", 0 }, { "comp", 0 }, { "compOn", 0 }, { "toneOn", 0 } } };
        Fake bkz { "buzzkill", { { "release", 0.35 }, { "listen", 1 } } };
        Fake flt { "flatliner", { { "target", -18 }, { "range", 6 }, { "speed", 50 }, { "gate", -55 }, { "ride", 1 }, { "output", 0 } } };
        int finishAsked = 0, finishSeqSeen = 0;
        juce::var watcherEnv;
        cdlink::Endpoint watcher ("watch", [&] (const juce::var& m) { if (m.getProperty ("kind", "").toString() == "master" && m.getProperty ("type", "").toString() == "status") watcherEnv = m.getProperty ("env", {}); }, other);
        for (auto* f : { &dom, &chop, &frk, &gtr, &vox, &bkz, &flt })
            f->ep = std::make_unique<cdlink::Endpoint> (f->kind, [f, &finishAsked, &finishSeqSeen, &freakNotes, &freakNote0] (const juce::var& m)
            {
                if (m.getProperty ("type", "").toString() == "freak-notes" && f->kind == "freak")
                {
                    ++freakNotes;
                    if (auto* a = m.getProperty ("notes", {}).getArray(); a != nullptr && a->size() > 0) freakNote0 = (*a)[0].toString();
                    return;
                }
                if (m.getProperty ("type", "").toString() == "analyze-finish" && f->kind == "servant") { ++finishAsked; finishSeqSeen = m.getProperty ("seq", 0); return; }
                if (m.getProperty ("type", "").toString() != "set") return;
                f->lastSet = m; ++f->sets;
                if (auto* o = m.getProperty ("params", {}).getDynamicObject()) for (auto& pr : o->getProperties()) f->params[pr.name.toString()] = (double) pr.value;
                auto r = cdlink::Endpoint::makeMessage ("applied");
                auto* po = new juce::DynamicObject(); for (auto& [k, v] : f->params) po->setProperty (k, v);
                r->setProperty ("params", juce::var (po));
                f->ep->send (r, m.getProperty ("from", "").toString());
            }, other);
        auto sendStatus = [&] (Fake& f, const juce::var& e, juce::var extra = {})
        {
            auto m = cdlink::Endpoint::makeMessage ("status");
            if (extra.isObject()) m->setProperty ("extra", extra);
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
                    auto shape = [&] (const char* s)
                    {
                        const juce::String k (s);
                        if (k == "same") return v;
                        if (k == "wobble") return v + ((i % 3) - 1) * 2.0;
                        if (k == "zig") return v + ((i % 4) * 2 - 3) * 2.5;
                        return v + ((i % 5) - 2) * 3.0;
                    };
                    in.add (shape (inShape)); out.add (shape (outShape));
                }
            o->setProperty ("in", in); o->setProperty ("out", out);
            return juce::var (o);
        };

        // the Servants on two tracks report all along (their fingerprints: parts of The Master's input)
        auto servantExtra = [&finishSeqSeen] (const char* track, const char* role, std::initializer_list<double> bands)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("track", track); o->setProperty ("role", role); o->setProperty ("seconds", 30.0);
            o->setProperty ("listening", finishSeqSeen == 0); o->setProperty ("finished", finishSeqSeen);
            if (juce::String (role) == "Lead")
            {
                auto* sb = new juce::DynamicObject();
                sb->setProperty ("p50", -28.0); sb->setProperty ("p95", -2.0); sb->setProperty ("perMin", 30.0); sb->setProperty ("seconds", 30.0); sb->setProperty ("hz", 6000.0);
                o->setProperty ("sib", juce::var (sb));
            }
            juce::Array<juce::var> b; double tot = 0; for (auto v : bands) { b.add (v); tot += v; }
            o->setProperty ("bandPow", b); o->setProperty ("pow", tot);
            // what each Servant hears in its track's quiet parts: hum on the vocal mic, the guitar is clean
            auto* nz = new juce::DynamicObject();
            const bool hum = juce::String (role) == "Lead";
            nz->setProperty ("seconds", 30); nz->setProperty ("quietSeconds", 6); nz->setProperty ("floorDb", hum ? -58.0 : -91.0);
            nz->setProperty ("humSeconds", hum ? 5 : 0); nz->setProperty ("humHz", hum ? 60 : 0); nz->setProperty ("humDb", hum ? 22.0 : 0.0);
            o->setProperty ("noise", juce::var (nz));
            return juce::var (o);
        };
        auto gtrX = [&] { return servantExtra ("Rhythm Gtr", "Rhythm", { 0.001, 0.01, 0.05, 0.08, 0.04, 0.02, 0.01, 0.003, 0.001, 0.0005 }); };
        auto voxX = [&] { return servantExtra ("Lead Vox", "Lead", { 0.0, 0.0005, 0.003, 0.006, 0.008, 0.009, 0.006, 0.002, 0.001, 0.0005 }); };
        auto plainExtra = [] (std::initializer_list<std::pair<const char*, juce::var>> kv) { auto* o = new juce::DynamicObject(); for (auto& [k, v] : kv) o->setProperty (k, v); return juce::var (o); };
        const auto bkzX = plainExtra ({ { "track", "Rhythm Gtr" }, { "floorDb", -64.0 }, { "playDb", -18.0 }, { "seconds", 30.0 }, { "learned", true } });
        const auto fltX = plainExtra ({ { "track", "" }, { "p10", -36.0 }, { "p50", -21.0 }, { "p90", -13.0 }, { "floorDb", -72.0 }, { "seconds", 40.0 } });   // no track name: found by its fingerprint
        struct Pinger2 : juce::Timer { std::function<void()> f; void timerCallback() override { f(); } } trackPinger;
        trackPinger.f = [&]
        {
            if (! watcherEnv.isObject()) return;
            sendStatus (gtr, envWith (watcherEnv, "wobble", "wobble"), gtrX());
            sendStatus (vox, envWith (watcherEnv, "spiky", "spiky"), voxX());
            sendStatus (bkz, envWith (watcherEnv, "same", "same"), bkzX);
            sendStatus (flt, envWith (watcherEnv, "same", "spiky"), fltX);   // its output feeds the vocal's Servant
        };
        trackPinger.startTimer (250);

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
        play (18.0, false);
        p.setNonRealtime (false);
        for (int round = 0; round < 70; ++round) { play (0.3, true); pump (20); }   // a little real-time playback so the fingerprints line up
        p.getWorker().waitUntilIdle (8000);
        check (p.applyChain().isNotEmpty(), "APPLY needs a prescription first");
        {
            juce::String perr = "not called";
            p.requestPrescribe ([&perr] (const juce::String& e) { perr = e; });
            check (p.isWaitingForServants() || finishAsked > 0, "PRESCRIBE first asks the listening Servants to finish");
            for (int i = 0; i < 60 && perr == "not called"; ++i) pump (50);
            check (finishAsked == 2 && perr.isEmpty() && p.getChainStage() == 1 && ! p.isWaitingForServants(),
                   juce::String::formatted ("both Servants finished (decided their clean-up), then PRESCRIBE from the analysis (%d asked)", finishAsked));
        }
        const auto& cp = p.getChainPlan();
        const bool hasFreak = cp.step ("freak") != nullptr;
        {
            juce::StringArray kinds; for (auto& s : cp.steps) kinds.add (s.kind);
            check (kinds.joinIntoString (",") == (hasFreak ? "freak,chopshop,dominatrix" : "chopshop,dominatrix"),
                   "prescription: " + kinds.joinIntoString (" > ") + " > The Master");
            juce::StringArray fb;
            if (hasFreak) for (int b = 1; b <= 6; ++b) if (cp.step ("freak")->params.at ("b" + juce::String (b) + "on") > 0.5f)
                fb.add (juce::String (cp.step ("freak")->params.at ("b" + juce::String (b) + "f"), 0) + " Hz " + juce::String (cp.step ("freak")->params.at ("b" + juce::String (b) + "d"), 0) + " dB");
            std::printf ("       (drum mix: The Freak %s)\n", hasFreak ? fb.joinIntoString (", ").toRawUTF8() : "not needed");
        }
        {
            bool voxAdd = false, generic = false;
            for (auto& ad : cp.adds) if (ad.kind == "buzzkill") { if (ad.track == "Lead Vox" && ad.why.contains ("60 Hz hum")) voxAdd = true; if (ad.track.isEmpty()) generic = true; }
            check (voxAdd && ! generic, "the Servants say where the hum is: Dead Air asked for on Lead Vox only, not on every guitar and bass track");
        }
        check (cp.step ("dominatrix")->params.at ("outputDb") > 3.0f && cp.step ("dominatrix")->params.at ("ceiling") == 0.0f, "Dominatrix: OUTPUT drives into the -1 dB ceiling");
        check (cp.step ("chopshop")->params.at ("ceilingDb") < cp.before.samplePeak - 1.0f && cp.step ("chopshop")->params.at ("shape") == 1.0f, "Cutthroat: soft ceiling under the loudest peaks");
        bool allWhy = ! cp.changes.empty(); for (auto& c : cp.changes) allWhy = allWhy && c.why.isNotEmpty();
        check (allWhy, juce::String::formatted ("every planned change has a reason (%d changes)", (int) cp.changes.size()));
        int nServ = 0; const mst::TrackPlan *bkPlan = nullptr, *flPlan = nullptr;
        for (auto& t : cp.tracks) { if (t.kind == "servant") ++nServ; if (t.kind == "buzzkill") bkPlan = &t; if (t.kind == "flatliner") flPlan = &t; }
        check (nServ == 2, juce::String::formatted ("the prescription includes the 2 linked Servants (%d)", nServ));
        check (bkPlan != nullptr && bkPlan->name == "Rhythm Gtr" && bkPlan->role == "Rhythm" && bkPlan->params.count ("release"), "Dead Air planned, on the rhythm guitar's track (by name)");
        check (flPlan != nullptr && flPlan->name == "Lead Vox" && flPlan->role == "Lead" && std::abs (flPlan->params.at ("target") + 21.0f) < 0.01f,
               "Flatliner planned, on the lead vocal's track (found by its level fingerprint)");
        bool genericBuzz = false; for (auto& o : cp.onTracks) if (o.startsWith ("DEAD AIR") || o.startsWith ("FLATLINER")) genericBuzz = true;
        check (! genericBuzz, "no 'put Dead Air / Flatliner on' advice when they're already linked");
        bool voxJumpComp = false; for (auto& t : cp.tracks) if (t.name == "Lead Vox" && t.kind == "servant") for (auto& w : t.why) if (w.contains ("jumps")) voxJumpComp = true;
        check (! voxJumpComp, "the lead a Flatliner rides doesn't also get The Servant's compressor for its jumps");
        check (p.applyChain().contains ("Cutthroat") , "APPLY says which plugins are missing");

        // 2) the plugins appear (and keep reporting 4x a second, like the real ones);
        //    play in real time so The Master can check the order
        struct Pinger : juce::Timer { std::function<void()> f; void timerCallback() override { f(); } } pinger;
        pinger.f = [&]
        {
            if (! watcherEnv.isObject()) return;
            sendStatus (dom, envWith (watcherEnv, "wobble", "same"));     // Dominatrix's output = The Master's input
            sendStatus (chop, envWith (watcherEnv, "spiky", "wobble"));   // Cutthroat's output = Dominatrix's input
            if (hasFreak) sendStatus (frk, envWith (watcherEnv, "zig", "spiky"));   // The Freak's output = Cutthroat's input
        };
        pinger.startTimer (250);
        p.setNonRealtime (false);
        for (int round = 0; round < 30; ++round) { play (0.2, true); pump (30); }
        auto st = p.chainStatus();
        bool allLinked = st.size() == cp.steps.size(), inOrder = p.chainOrderKnown();
        juce::String posTxt;
        for (int i = 0; i < (int) st.size(); ++i) { allLinked = allLinked && st[(size_t) i].linked; inOrder = inOrder && st[(size_t) i].position == i && st[(size_t) i].orderOk; posTxt << st[(size_t) i].kind << " " << st[(size_t) i].position << "  "; }
        check (allLinked, "every prescribed bus plugin is linked");
        check (inOrder, "chain order detected from the level fingerprints (" + posTxt.trim() + ")");

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
        check (gtr.sets == 1 && vox.sets == 1 && gtr.lastSet.getProperty ("params", {}).hasProperty ("trim") && gtr.lastSet.getProperty ("params", {}).hasProperty ("t1g"),
               "both Servants received their tone / comp / trim settings");
        check (std::abs (vox.params["deessOn"] - 1.0) < 0.01 && vox.params["deess"] >= 20.0 && std::abs (gtr.params["deessOn"]) < 0.01,
               "APPLY turns the de-esser on for the hissy lead vocal only");
        check (bkz.sets == 1 && bkz.params["listen"] == 0.0 && std::abs (bkz.params["release"] - bkPlan->params.at ("release")) < 0.01
               && flt.sets == 1 && std::abs (flt.params["range"] - flPlan->params.at ("range")) < 0.01,
               "Dead Air and Flatliner received their settings");
        check (dom.sets == 1 && std::abs (dom.params["outputDb"] - cp.step ("dominatrix")->params.at ("outputDb")) < 0.01 && chop.sets == 1
               && std::abs (chop.params["ceilingDb"] - cp.step ("chopshop")->params.at ("ceilingDb")) < 0.01, "Dominatrix and Cutthroat received their settings");
        if (hasFreak)
            check (frk.sets == 1 && std::abs (frk.params["b1d"] - cp.step ("freak")->params.at ("b1d")) < 0.01 && frk.params.count ("b6on") && freakNotes == 1 && freakNote0.isNotEmpty(),
                   "The Freak received its bands and the reason for each (" + freakNote0 + ")");
        bool confirmed = true; int n = 0;
        int servantEntries = 0, helperEntries = 0;
        for (auto& e : p.getLog())
        {
            if (e.plugin != "The Master") { confirmed = confirmed && e.confirmed; ++n; }
            if (e.plugin == "The Servant") ++servantEntries;
            if ((e.plugin == "Dead Air" || e.plugin == "Flatliner") && e.detail.isNotEmpty()) ++helperEntries;
        }
        check (servantEntries == 2 && helperEntries == 2, "the log has an entry per track plugin, each with its reasons");
        check (n > 5 && confirmed, juce::String::formatted ("the log explains every change and shows both plugins confirmed (%d entries)", n));
        bool fromShown = false;
        for (auto& e : p.getLog()) if (e.plugin == "Dominatrix" && e.title.startsWith ("OUTPUT  0.0 > ")) fromShown = true;
        check (fromShown, "the log shows old > new values");
        check (p.isFixOn() && p.currentFixParams().loudOn && std::abs (p.currentFixParams().gainDb) < 0.01f && ! p.currentFixParams().dynOn && ! p.currentFixParams().stereoOn,
               "The Master's own part: EQ + safety limiter only");
        pump (800);
        pme->refreshNow();
        check (writePng (ped->createComponentSnapshot (ped->getLocalBounds(), true, 2.0f), dir.getChildFile ("gui_plugins_applied.png")), "MY PLUGINS: applied, with the change log");

        // 3b) a track plugin added after APPLY: the main button says UPDATE, which plans it in from the
        //     same analysis (no replay) and sends it its settings, still applied
        Fake bk2 { "buzzkill", { { "release", 0.35 }, { "listen", 0 } } };
        bk2.ep = std::make_unique<cdlink::Endpoint> ("buzzkill", [&bk2] (const juce::var& m)
        {
            if (m.getProperty ("type", "").toString() != "set") return;
            bk2.lastSet = m; ++bk2.sets;
            auto r = cdlink::Endpoint::makeMessage ("applied");
            r->setProperty ("params", m.getProperty ("params", {}));
            bk2.ep->send (r, m.getProperty ("from", "").toString());
        }, other);
        const auto bk2X = plainExtra ({ { "track", "Lead Gtr" }, { "floorDb", -60.0 }, { "playDb", -20.0 }, { "seconds", 30.0 }, { "learned", true } });
        struct Pinger3 : juce::Timer { std::function<void()> f; void timerCallback() override { f(); } } bk2Pinger;
        bk2Pinger.f = [&] { if (watcherEnv.isObject()) sendStatus (bk2, envWith (watcherEnv, "same", "same"), bk2X); };
        bk2Pinger.startTimer (250);
        for (int i = 0; i < 20 && ! p.chainOutdated(); ++i) pump (50);
        pme->refreshNow();
        check (p.chainOutdated() && pme->mainButtonText() == "UPDATE", "a Dead Air added after APPLY: the main button says UPDATE (" + pme->mainButtonText() + ")");
        pme->pressAutoFix();
        for (int i = 0; i < 80 && bk2.sets == 0; ++i) pump (50);
        pme->refreshNow();
        check (p.getChainStage() == 2 && ! p.chainOutdated() && bk2.sets == 1 && pme->mainButtonText() == "REFINE",
               "UPDATE plans the new Dead Air in from the same analysis, sends its settings, and stays applied");

        // 3c) a Cutthroat on a track (its DAW track name matches a Servant) is a track plugin, not the bus one
        Fake chop2 { "chopshop", { { "ceilingDb", 0 }, { "mode", 0 }, { "shape", 0 } } };
        chop2.ep = std::make_unique<cdlink::Endpoint> ("chopshop", [&chop2] (const juce::var& m)
        {
            if (m.getProperty ("type", "").toString() != "set") return;
            chop2.lastSet = m; ++chop2.sets;
            if (auto* o = m.getProperty ("params", {}).getDynamicObject()) for (auto& pr : o->getProperties()) chop2.params[pr.name.toString()] = (double) pr.value;
            auto r = cdlink::Endpoint::makeMessage ("applied");
            r->setProperty ("params", m.getProperty ("params", {}));
            chop2.ep->send (r, m.getProperty ("from", "").toString());
        }, other);
        const auto chop2X = plainExtra ({ { "track", "Rhythm Gtr" }, { "songPeakDb", -4.0 }, { "seconds", 30.0 } });
        struct Pinger4 : juce::Timer { std::function<void()> f; void timerCallback() override { f(); } } chop2Pinger;
        chop2Pinger.f = [&] { auto m = cdlink::Endpoint::makeMessage ("status"); m->setProperty ("extra", chop2X);
                              auto* po = new juce::DynamicObject(); for (auto& [k, v] : chop2.params) po->setProperty (k, v);
                              m->setProperty ("params", juce::var (po)); chop2.ep->send (m); };
        chop2Pinger.startTimer (250);
        juce::String chop2Id;
        for (int i = 0; i < 30 && chop2Id.isEmpty(); ++i) { pump (50); for (auto& pr : p.getPeers()) if (pr.kind == "chopshop" && pr.extra.getProperty ("track", "").toString() == "Rhythm Gtr") chop2Id = pr.id; }
        {
            bool busChopStill = false;
            for (auto& st : p.chainStatus()) if (st.kind == "chopshop") busChopStill = st.linked && st.peerId != chop2Id;
            check (chop2Id.isNotEmpty() && p.isTrackPeer (chop2Id) && busChopStill && p.chainOutdated(),
                   "a Cutthroat on a track is a track plugin: the bus Cutthroat stays the bus one, and the main button says UPDATE");
            pme->refreshNow();
            pme->pressAutoFix();
            for (int i = 0; i < 80 && chop2.sets == 0; ++i) pump (50);
            const mst::TrackPlan* tc = nullptr;
            for (auto& t : p.getChainPlan().tracks) if (t.kind == "chopshop") tc = &t;
            bool rebelNote = false; for (auto& o : p.getChainPlan().onTracks) if (o.startsWith ("REBEL") && o.contains ("Rhythm Gtr")) rebelNote = true;
            // nobody asked for a Cutthroat on the guitar: it's yours, so REBEL goes on and nothing else is sent
            check (tc != nullptr && tc->rebel && chop2.params["rebel"] > 0.5 && chop2.params["ceilingDb"] == 0.0 && rebelNote,
                   "a Cutthroat The Master didn't ask for: it switches its REBEL on, says so, and leaves its settings alone");
            pump (600);
            const int setsRebel = chop2.sets;
            pme->refreshNow(); pme->pressAutoFix();
            for (int i = 0; i < 20; ++i) pump (50);
            check (chop2.sets == setsRebel && chop2.params["ceilingDb"] == 0.0, "while REBEL is on, UPDATE sends it nothing");
            // you turn REBEL off: from now on The Master runs it (and doesn't switch REBEL back on)
            chop2.params["rebel"] = 0.0;
            pump (600);
            pme->refreshNow(); pme->pressAutoFix();
            for (int i = 0; i < 80 && chop2.params["ceilingDb"] == 0.0; ++i) pump (50);
            tc = nullptr;
            for (auto& t : p.getChainPlan().tracks) if (t.kind == "chopshop") tc = &t;
            check (tc != nullptr && ! tc->rebel && tc->name == "Rhythm Gtr" && chop2.params["rebel"] < 0.5 && tc->params.count ("ceilingDb")
                   && std::abs (chop2.params["ceilingDb"] - tc->params.at ("ceilingDb")) < 0.01,
                   juce::String::formatted ("REBEL off: UPDATE plans the track Cutthroat and sends it its ceiling (%.1f dB), and leaves REBEL off", chop2.params["ceilingDb"]));
        }

        {
            const auto d = pme->diagnosticsText();
            dir.getChildFile ("diagnostics.txt").replaceWithText (d);
            check (d.contains ("REPORT (") && d.contains ("PRESCRIPTION") && d.contains ("Dominatrix") && d.contains ("WHAT I CHANGED & WHY")
                   && d.contains ("Lead Gtr") && d.contains ("measured: ") && d.contains ("bandPow"),
                   juce::String::formatted ("EXPORT DIAGNOSTICS: report, prescription, change log and every track's measurements in one file (%d chars)", d.length()));
        }

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
        trackPinger.stopTimer();
        ped.reset();

        juce::MemoryBlock stt; p.getStateInformation (stt);
        MasterProcessor q; q.setStateInformation (stt.getData(), (int) stt.getSize());
        check (q.getChainStage() == 2 && q.getChainPlan().steps.size() == cp.steps.size() && ! q.getLog().empty(), "the prescription and log are saved with the project");

        // OUTPUT -6 dB after APPLY: the bus part is re-planned from the same analysis and sent again
        pinger.startTimer (250);
        const int domSetsBefore = dom.sets;
        p.apvts.getParameter ("headroom")->setValueNotifyingHost (0.0f);
        p.outputTargetChanged();
        for (int i = 0; i < 40 && dom.sets == domSetsBefore; ++i) pump (50);
        check (p.getChainPlan().headroom && dom.sets > domSetsBefore && std::abs (dom.params["ceiling"] - 1.0) < 0.01
               && std::abs (p.currentFixParams().ceilingDb + 6.0f) < 0.01f && p.getChainPlan().tracks.size() == cp.tracks.size(),
               juce::String::formatted ("OUTPUT -6 dB after APPLY: Dominatrix CEILING -6 dB, OUTPUT %.1f dB, safety limiter -6 dBTP, tracks kept", dom.params["outputDb"]));
        {   // the user guide's picture of the fix, with OUTPUT at -6 dB (the recommended setting)
            trackPinger.startTimer (250);   // the track plugins keep reporting while the picture is taken
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            auto* me = dynamic_cast<MasterEditor*> (ed.get());
            me->showPage (1);
            pump (800);
            me->refreshNow();
            pump (200);
            check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), dir.getChildFile ("gui_plugins_applied_6db.png")), "MY PLUGINS: applied at OUTPUT -6 dB (user guide)");
            trackPinger.stopTimer();
        }

        // AUTO keeps judging against the prescription's genre, whatever the processed mix now sounds like
        {
            auto t = p.resolveTarget (p.getChainPlan().before);
            check (t.index >= 0 && p.getLibrary().genres()[(size_t) t.index].name == p.getChainPlan().genreName, "with a prescription in place, AUTO stays on its genre");
        }

        // RESET with a prescription in place asks: start over, or just measure again
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            auto* me = dynamic_cast<MasterEditor*> (ed.get());
            me->showPage (1);
            me->pressReset();
            pump (300);
            check (me->dialogOpen() && p.getChainStage() >= 1, "RESET with a prescription asks START OVER / MEASURE AGAIN (and changes nothing yet)");
            check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f), dir.getChildFile ("gui_reset_dialog.png")), "RESET dialog");
        }

        // START OVER: every Mix System plugin back to neutral, the prescription cleared
        {
            dom.params["outputDb"] = 10.7; chop.params["ceilingDb"] = -17.0; chop.params["mix"] = 100.0;
            const int ds = dom.sets, cs = chop.sets, gs = gtr.sets;
            p.startOver();
            for (int i = 0; i < 60 && (dom.sets == ds || chop.sets == cs || gtr.sets == gs || gtr.params["toneOn"] != 0.0); ++i) pump (50);
            check (p.getChainStage() == 0 && ! p.getChainPlan().valid && ! p.isFixOn(), "START OVER clears the prescription and turns The Master's own fix off");
            check (std::abs (dom.params["outputDb"]) < 0.01 && std::abs (dom.params["ceiling"] - 2.0) < 0.01 && std::abs (dom.params["discipline"]) < 0.01
                   && std::abs (chop.params["mix"]) < 0.01 && std::abs (gtr.params["toneOn"]) < 0.01 && std::abs (gtr.params["compOn"]) < 0.01,
                   juce::String::formatted ("START OVER puts Dominatrix, Cutthroat and The Servants back to pass-through (dom out %.1f ceil %.1f disc %.1f, chop mix %.1f, gtr tone %.1f comp %.1f)",
                                            dom.params["outputDb"], dom.params["ceiling"], dom.params["discipline"], chop.params["mix"], gtr.params["toneOn"], gtr.params["compOn"]));
            // a fresh prescription through a Dominatrix still turned up refuses, and starts over by itself
            p.setNonRealtime (true);
            play (12.0, false);
            p.getWorker().waitUntilIdle (8000);
            pump (300);
            dom.params["outputDb"] = 8.0;
            pump (600);
            const auto err = p.prescribe (false);
            check (err.contains ("back to neutral") && ! p.getChainPlan().valid, "PRESCRIBE through bus plugins still set from before: they go back to neutral first (" + err.substring (0, 40) + ")");
        }
        pinger.stopTimer();
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

        // FIX IT opens on MY PLUGINS; BUILT-IN FIXES is the alternative
        me->showPage (1);
        check (q.isPluginMode(), "FIX IT opens on MY PLUGINS");
        check (writePng (ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f), dir.getChildFile ("gui_fix_plugins.png")), "FIX IT with my plugins (first)");
        // built-in fixes are retired from the UI: the FIX page is always MY PLUGINS
        q.setPluginMode (false);
        me->showPage (1);
        check (q.isPluginMode(), "built-in fixes retired: the FIX page switches back to MY PLUGINS");
        check (q.applyAutoFix().isEmpty() && q.getPlan().valid, "the fix engine still plans (The Master's own EQ + limiter use it)");
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

    // ---- YOUR TRACKS: one row per track, its plugins in order, missing ones marked, and it scrolls
    {
        ChainView cv;
        cv.setBounds (0, 0, 772, 198);
        cv.hasPlan = true; cv.stage = 1;
        auto tr = [&] (const char* kind, const char* name, const char* role, bool linked = true)
        { MasterProcessor::TrackStatus t; t.kind = kind; t.name = name; t.role = role; t.linked = linked; t.planned = true; t.summary = "EQ 3 bands"; cv.tracks.push_back (t); };
        tr ("servant", "Lead Vox", "Lead"); tr ("servant", "Rhythm Gtr L", "Rhythm"); tr ("servant", "Rhythm Gtr R", "Rhythm");
        tr ("servant", "Bass", "Bass"); tr ("servant", "Kick", "Kick"); tr ("servant", "Snare", "Snare"); tr ("servant", "Overheads", "Drums");
        tr ("servant", "Keys", "Keys / Pads"); tr ("buzzkill", "Rhythm Gtr L", "DEAD AIR"); tr ("buzzkill", "Bass", "DEAD AIR");
        std::vector<mst::ChainPlan::TrackAdd> adds { { "flatliner", "Lead Vox", "Its level swings 14 dB." }, { "lowblow", "Bass", "It and Kick fight in BASS." } };
        cv.rebuildTracks (adds);
        auto* vp = dynamic_cast<juce::Viewport*> (cv.getChildComponent (0));
        auto* list = vp != nullptr ? vp->getViewedComponent() : nullptr;
        check (vp != nullptr && vp->isVisible() && list != nullptr && list->getHeight() > vp->getHeight(),
               "YOUR TRACKS: 8 tracks don't fit, so the list scrolls (no '+ more, see the log')");
        check (list != nullptr && list->getHeight() == 9 * TrackChainList::rowH, "YOUR TRACKS: one row per track (Dead Air and Servant share a row) + the Servant reminder");
        juce::Image img (juce::Image::ARGB, 772 * 2, 198 * 2, true);
        { juce::Graphics g (img); g.addTransform (juce::AffineTransform::scale (2.0f)); g.fillAll (juce::Colour (0xff1d1815)); cv.paintEntireComponent (g, true); }
        check (writePng (img, dir.getChildFile ("your_tracks_scroll.png")), "YOUR TRACKS snapshot");
        vp->setViewPosition (0, 1000);
        juce::Image img2 (juce::Image::ARGB, 772 * 2, 198 * 2, true);
        { juce::Graphics g (img2); g.addTransform (juce::AffineTransform::scale (2.0f)); g.fillAll (juce::Colour (0xff1d1815)); cv.paintEntireComponent (g, true); }
        writePng (img2, dir.getChildFile ("your_tracks_scrolled.png"));
    }

    std::printf ("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
