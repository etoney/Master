#pragma once
// THE MASTER - genre profiles, profile packs and the genre detector.
//
// A genre profile is the mean and spread (standard deviation) of every measured number
// (loudness, PLR, LRA, width, correlation and the 10 tonal bands) across reference songs.
//
// Profile packs are plain JSON (see Profiles/factory.json):
//   * the factory pack is compiled into the plugin
//   * a user pack (profiles.json in the profiles folder) overrides factory genres with the
//     same id and can add new genres. It is written by IMPORT PROFILE PACK and by the
//     admin capture ("add this analysis as a reference").
//   * TheMasterProfiler (admin tool) builds packs from folders of reference songs.
#include "Analyzer.h"
#include <juce_core/juce_core.h>
#include <map>

namespace mst
{
struct Stat { float mean = 0.0f, sd = 1.0f; };

// Smallest spread a profile may have, so a handful of near-identical references doesn't
// turn every tiny difference into a "fix".
inline float minSd (int f)
{
    switch (f)
    {
        case fLufs: return 0.8f;
        case fPlr: return 0.7f;
        case fLra: return 1.0f;
        case fWidth: return 1.5f;
        case fCorr: return 0.05f;
        case fLowCorr: return 0.03f;
        default: return 1.0f;
    }
}

constexpr float kSeedWeight = 5.0f;   // a seed profile counts like 5 songs when references are captured on top of it

struct GenreProfile
{
    juce::String id, name, source { "seed" };
    int tracks = 0;
    std::array<Stat, numFeatures> s {};

    Stat stat (int f) const { return s[(size_t) f]; }
    float z (int f, float v) const { const auto st = stat (f); return (v - st.mean) / juce::jmax (1e-4f, st.sd); }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", id);
        o->setProperty ("name", name);
        o->setProperty ("tracks", tracks);
        o->setProperty ("source", source);
        auto* fo = new juce::DynamicObject();
        for (int f = 0; f < numFeatures; ++f)
        {
            auto* so = new juce::DynamicObject();
            so->setProperty ("mean", std::round (s[(size_t) f].mean * 1000.0f) / 1000.0f);
            so->setProperty ("sd", std::round (s[(size_t) f].sd * 1000.0f) / 1000.0f);
            fo->setProperty (featureKey (f), juce::var (so));
        }
        o->setProperty ("features", juce::var (fo));
        return juce::var (o);
    }

    static bool fromVar (const juce::var& v, GenreProfile& out, juce::String& err)
    {
        out = {};
        out.id = v.getProperty ("id", "").toString().trim();
        out.name = v.getProperty ("name", "").toString().trim();
        if (out.id.isEmpty() || out.name.isEmpty()) { err = "a genre is missing its id or name"; return false; }
        out.tracks = (int) v.getProperty ("tracks", 0);
        out.source = v.getProperty ("source", "seed").toString();
        const auto feats = v.getProperty ("features", {});
        if (! feats.isObject()) { err = out.name + " has no features"; return false; }
        for (int f = 0; f < numFeatures; ++f)
        {
            const auto st = feats.getProperty (featureKey (f), {});
            if (! st.isObject()) { err = out.name + " is missing " + featureKey (f); return false; }
            const double m = st.getProperty ("mean", 0.0), sd = st.getProperty ("sd", 1.0);
            if (! std::isfinite (m) || ! std::isfinite (sd) || sd <= 0.0) { err = out.name + ": bad numbers for " + featureKey (f); return false; }
            out.s[(size_t) f] = { (float) m, juce::jmax (minSd (f), (float) sd) };
        }
        return true;
    }
};

struct ProfilePack
{
    juce::String packVersion, notes;
    int analyzerVersion = kAnalyzerVersion;
    std::vector<GenreProfile> genres;

    const GenreProfile* find (const juce::String& id) const
    {
        for (auto& g : genres) if (g.id == id) return &g;
        return nullptr;
    }
    GenreProfile* find (const juce::String& id)
    {
        for (auto& g : genres) if (g.id == id) return &g;
        return nullptr;
    }

    juce::String toJson() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("format", "the-master-profiles");
        o->setProperty ("formatVersion", 1);
        o->setProperty ("analyzerVersion", analyzerVersion);
        o->setProperty ("packVersion", packVersion);
        o->setProperty ("notes", notes);
        juce::Array<juce::var> arr;
        for (auto& g : genres) arr.add (g.toVar());
        o->setProperty ("genres", arr);
        return juce::JSON::toString (juce::var (o), false, 4);
    }

    static bool parse (const juce::String& json, ProfilePack& out, juce::String& err)
    {
        out = {};
        juce::var v;
        const auto res = juce::JSON::parse (json, v);
        if (res.failed()) { err = "not valid JSON (" + res.getErrorMessage() + ")"; return false; }
        if (v.getProperty ("format", "").toString() != "the-master-profiles") { err = "not a The Master profile pack"; return false; }
        out.analyzerVersion = (int) v.getProperty ("analyzerVersion", kAnalyzerVersion);
        if (out.analyzerVersion > kAnalyzerVersion) { err = "made for a newer version of The Master; update the plugin first"; return false; }
        out.packVersion = v.getProperty ("packVersion", "").toString();
        out.notes = v.getProperty ("notes", "").toString();
        const auto* arr = v.getProperty ("genres", {}).getArray();
        if (arr == nullptr || arr->isEmpty()) { err = "the pack has no genres"; return false; }
        for (auto& gv : *arr)
        {
            GenreProfile g;
            if (! GenreProfile::fromVar (gv, g, err)) return false;
            if (out.find (g.id) != nullptr) { err = "genre id '" + g.id + "' appears twice"; return false; }
            out.genres.push_back (g);
        }
        return true;
    }
};

//==============================================================================
// Build a profile from a set of analysed reference songs.
inline GenreProfile buildProfile (const juce::String& id, const juce::String& name, const std::vector<Features>& tracks)
{
    GenreProfile g;
    g.id = id; g.name = name; g.tracks = (int) tracks.size(); g.source = "references";
    const double n = (double) std::max<size_t> (1, tracks.size());
    for (int f = 0; f < numFeatures; ++f)
    {
        double m = 0;
        for (auto& t : tracks) m += t.get (f);
        m /= n;
        double v = 0;
        for (auto& t : tracks) v += juce::square (t.get (f) - m);
        v = tracks.size() > 1 ? v / (n - 1.0) : 0.0;
        g.s[(size_t) f] = { (float) m, juce::jmax (minSd (f), (float) std::sqrt (v)) };
    }
    return g;
}

// Pool one more reference song into a profile (seed profiles count as kSeedWeight songs).
inline void addReference (GenreProfile& g, const Features& t)
{
    const double na = g.tracks > 0 ? (double) g.tracks : (double) kSeedWeight, nb = 1.0, n = na + nb;
    for (int f = 0; f < numFeatures; ++f)
    {
        auto& st = g.s[(size_t) f];
        const double ma = st.mean, mb = t.get (f), m = (na * ma + nb * mb) / n;
        const double var = (na * (juce::square ((double) st.sd) + juce::square (ma - m)) + nb * juce::square (mb - m)) / n;
        st = { (float) m, juce::jmax (minSd (f), (float) std::sqrt (var)) };
    }
    g.tracks = g.tracks > 0 ? g.tracks + 1 : 1;
    g.source = "references";
}

//==============================================================================
// Genre detector: which profile is this mix most likely drawn from? A per-feature Gaussian
// model (naive Bayes) over the tonal bands, loudness, dynamics and stereo picture.
struct GenreGuess { int index = -1; float probability = 0.0f; };

inline float detectWeight (int f)
{
    switch (f)
    {
        case fLufs: return 0.6f;
        case fPlr: return 0.8f;
        case fLra: return 0.5f;
        case fWidth: return 0.6f;
        case fCorr: return 0.3f;
        case fLowCorr: return 0.0f;    // a mono low end is a mixing choice, not a genre
        default: return 1.0f;
    }
}

inline std::vector<GenreGuess> detectGenre (const Features& x, const std::vector<GenreProfile>& genres)
{
    std::vector<GenreGuess> out;
    if (! x.valid || genres.empty()) return out;
    std::vector<double> ll;
    double best = -1e300;
    for (auto& g : genres)
    {
        double l = 0;
        for (int f = 0; f < numFeatures; ++f)
        {
            const double w = detectWeight (f);
            if (w <= 0) continue;
            const double z = juce::jlimit (-6.0, 6.0, (double) g.z (f, x.get (f)));
            l += w * (-0.5 * z * z - std::log ((double) g.stat (f).sd));
        }
        l *= 0.5;    // temperature: genres overlap, so don't overclaim
        ll.push_back (l);
        best = std::max (best, l);
    }
    double sum = 0;
    for (auto& l : ll) { l = std::exp (l - best); sum += l; }
    for (int i = 0; i < (int) genres.size(); ++i) out.push_back ({ i, (float) (ll[(size_t) i] / sum) });
    std::sort (out.begin(), out.end(), [] (const GenreGuess& a, const GenreGuess& b) { return a.probability > b.probability; });
    return out;
}

//==============================================================================
// Factory pack + the user's pack layered on top.
class ProfileLibrary
{
public:
    static juce::File getProfilesFolder()
    {
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);   // ~/Library on macOS, %APPDATA% on Windows
       #if JUCE_MAC
        base = base.getChildFile ("Application Support");
       #endif
        return base.getChildFile ("Castle Danger").getChildFile ("The Master").getChildFile ("Profiles");
    }
    static juce::File getUserPackFile() { return getProfilesFolder().getChildFile ("profiles.json"); }

    // factoryJson: the compiled-in pack. userFile: normally getUserPackFile() (tests pass a temp file).
    void load (const juce::String& factoryJson, const juce::File& userFile)
    {
        userPackFile = userFile;
        juce::String err;
        factory = {};
        if (! ProfilePack::parse (factoryJson, factory, err)) jassertfalse;
        user = {}; userError = {}; hasUser = false;
        if (userFile.existsAsFile())
        {
            if (ProfilePack::parse (userFile.loadFileAsString(), user, err)) hasUser = true;
            else userError = err;
        }
        rebuild();
    }

    const std::vector<GenreProfile>& genres() const { return merged; }
    int indexOf (const juce::String& id) const
    {
        for (int i = 0; i < (int) merged.size(); ++i) if (merged[(size_t) i].id == id) return i;
        return -1;
    }
    bool usingUserPack() const { return hasUser; }
    juce::String getUserError() const { return userError; }
    const ProfilePack& getFactory() const { return factory; }
    const ProfilePack& getUser() const { return user; }
    juce::String describe() const
    {
        if (hasUser) return "PACK " + (user.packVersion.isNotEmpty() ? user.packVersion : juce::String ("(custom)"));
        return "FACTORY " + factory.packVersion;
    }
    std::atomic<int> version { 0 };

    // Import a pack file the admin made; replaces the user pack. Returns an error or {}.
    juce::String importPack (const juce::File& f)
    {
        ProfilePack p; juce::String err;
        if (! ProfilePack::parse (f.loadFileAsString(), p, err)) return err;
        return writeUser (p);
    }

    juce::String restoreFactory()
    {
        if (userPackFile.existsAsFile() && ! userPackFile.deleteFile()) return "couldn't delete " + userPackFile.getFullPathName();
        user = {}; hasUser = false; userError = {};
        rebuild();
        return {};
    }

    // Admin: pool a measured mix into a genre (creates the genre in the user pack if needed).
    juce::String addReference (const juce::String& genreId, const Features& fx)
    {
        if (! fx.valid) return "nothing analysed yet";
        ProfilePack p = hasUser ? user : ProfilePack {};
        if (! hasUser)
        {
            p.packVersion = juce::Time::getCurrentTime().formatted ("%Y.%m.%d") + "-custom";
            p.notes = "Built on top of factory " + factory.packVersion + " with captured references.";
        }
        auto* g = p.find (genreId);
        if (g == nullptr)
        {
            const int mi = indexOf (genreId);
            if (mi < 0) return "unknown genre";
            p.genres.push_back (merged[(size_t) mi]);
            g = &p.genres.back();
        }
        mst::addReference (*g, fx);
        return writeUser (p);
    }

    juce::String exportPack (const juce::File& dest, const juce::String& packVersion) const
    {
        ProfilePack p;
        p.packVersion = packVersion;
        p.notes = "Exported from The Master.";
        p.genres = merged;
        return dest.replaceWithText (p.toJson()) ? juce::String() : "couldn't write " + dest.getFullPathName();
    }

private:
    juce::String writeUser (const ProfilePack& p)
    {
        userPackFile.getParentDirectory().createDirectory();
        if (! userPackFile.replaceWithText (p.toJson())) return "couldn't write " + userPackFile.getFullPathName();
        user = p; hasUser = true; userError = {};
        rebuild();
        return {};
    }

    void rebuild()
    {
        merged = factory.genres;
        if (hasUser)
            for (auto& g : user.genres)
            {
                bool replaced = false;
                for (auto& m : merged) if (m.id == g.id) { m = g; replaced = true; }
                if (! replaced) merged.push_back (g);
            }
        ++version;
    }

    ProfilePack factory, user;
    bool hasUser = false;
    juce::String userError;
    juce::File userPackFile;
    std::vector<GenreProfile> merged;
};
} // namespace mst
