#pragma once
// THE MASTER - your own profiles: a folder of songs per profile.
//
//   macOS    ~/Library/Application Support/Castle Danger/The Master/Profiles/My Profiles/<name>/
//   Windows  %APPDATA%\Castle Danger\The Master\Profiles\My Profiles\<name>\
//
// Drop finished, mixed songs into a folder (one is enough). A background thread measures each song
// once, with the same analyser The Master uses on your mix, and keeps the numbers in a hidden cache
// file in that folder (.castle-danger-cache.json); no audio is copied. With one or two songs the
// allowed spread comes from the closest factory genre; with three or more, from how much your songs
// really vary.
#include <juce_audio_formats/juce_audio_formats.h>
#include "Profiles.h"

namespace mst
{
inline juce::String customIdFor (const juce::String& folderName)
{
    auto words = juce::StringArray::fromTokens (folderName.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789 -_"), " -_", "");
    words.removeEmptyStrings();
    juce::String id ("my");
    for (auto& w : words) id << w.substring (0, 1).toUpperCase() << w.substring (1);
    return id == "my" ? juce::String ("myProfile") + juce::String (folderName.hashCode() & 0xffff) : id;
}

inline bool isCustomId (const juce::String& id) { return id.startsWith ("my") && id.length() > 2 && juce::CharacterFunctions::isUpperCase (id[2]); }

// one song, measured the way The Master measures your mix
inline bool analyseSong (juce::AudioFormatManager& fm, const juce::File& f, Features& result, juce::String& err, std::function<bool()> shouldStop = {})
{
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) { err = "can't read this file type"; return false; }
    if (r->sampleRate < 8000 || r->lengthInSamples <= 0) { err = "no audio"; return false; }
    MixAnalysis a;
    a.prepare (r->sampleRate);
    const int chunk = 1 << 16;
    juce::AudioBuffer<float> buf ((int) juce::jmax (1u, juce::jmin (2u, r->numChannels)), chunk);
    for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += chunk)
    {
        if (shouldStop && shouldStop()) { err = "stopped"; return false; }
        const int n = (int) std::min<juce::int64> ((juce::int64) chunk, r->lengthInSamples - pos);
        r->read (&buf, 0, n, pos, true, buf.getNumChannels() > 1);
        a.process (buf.getReadPointer (0), buf.getNumChannels() > 1 ? buf.getReadPointer (1) : nullptr, n);
    }
    result = a.compute();
    if (! result.valid) { err = "too short or silent"; return false; }
    return true;
}

// Build a profile from your songs. Few songs can't tell how much a genre varies, so the spread is
// borrowed from the closest factory genre until there are three.
inline GenreProfile buildCustomProfile (const juce::String& name, const std::vector<Features>& songs, const std::vector<GenreProfile>& factory)
{
    auto g = buildProfile (customIdFor (name), name, songs);
    g.source = "songs";
    if (songs.empty()) return g;
    // the closest factory genre to the songs' average
    Features avg; avg.valid = true;
    for (int f = 0; f < numFeatures; ++f) avg.set (f, g.s[(size_t) f].mean);
    const auto guesses = detectGenre (avg, factory);
    if (guesses.empty()) return g;
    const auto& near = factory[(size_t) guesses.front().index];
    for (int f = 0; f < numFeatures; ++f)
    {
        auto& st = g.s[(size_t) f];
        if (songs.size() < 3) st.sd = juce::jmax (st.sd, near.s[(size_t) f].sd);
        else st.sd = juce::jmax (st.sd, 0.5f * near.s[(size_t) f].sd);   // never stricter than half a real genre's spread
    }
    return g;
}

class SongFolders : private juce::Thread
{
public:
    SongFolders() : juce::Thread ("The Master song profiles") { fm.registerBasicFormats(); }
    ~SongFolders() override { stopThread (8000); }

    static juce::File& rootOverride() { static juce::File f; return f; }   // tests
    static juce::File root()
    {
        if (rootOverride() != juce::File()) return rootOverride();
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        base = base.getChildFile ("Application Support");
       #endif
        return base.getChildFile ("Castle Danger").getChildFile ("The Master").getChildFile ("Profiles").getChildFile ("My Profiles");
    }
    static void ensureRoot()
    {
        auto r = root();
        if (r.isDirectory()) return;
        r.createDirectory();
        r.getChildFile ("READ ME.txt").replaceWithText (
            "THE MASTER - MY PROFILES\n\n"
            "Each folder in here is one profile, named after the folder (\"My Band\", \"90s Grunge\", \"Album Sound\").\n"
            "Put finished, mixed songs that sound the way you want your mixes to sound into a folder.\n"
            "One song is enough; three or more make it more reliable.\n\n"
            "WAV, AIFF, FLAC, MP3 and OGG work everywhere (M4A / AAC too on a Mac).\n"
            "The Master measures each song once, in the background, and keeps only the numbers in a hidden\n"
            "file in the folder. Your audio is never copied or changed.\n\n"
            "Pick the profile in The Master: REFERENCE GENRE > MY PROFILES.\n");
    }

    void setFactory (std::vector<GenreProfile> f) { const juce::ScopedLock sl (lock); factory = std::move (f); }
    void start() { ensureRoot(); startThread (juce::Thread::Priority::low); }

    // message thread
    std::vector<GenreProfile> getProfiles() const { const juce::ScopedLock sl (lock); return profiles; }
    struct FolderInfo { juce::String name, id; int songs = 0, failed = 0; };
    std::vector<FolderInfo> getFolders() const { const juce::ScopedLock sl (lock); return folders; }
    juce::String getMeasuring() const { const juce::ScopedLock sl (lock); return measuring; }
    std::atomic<int> version { 0 };

    // measure now, on this thread (tests, and "new profile" right after creating the folder)
    void scanNow() { scan(); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            scan();
            wait (3000);
        }
    }

    void scan()
    {
        const juce::ScopedLock scanLock (scanning);
        std::vector<GenreProfile> fac;
        { const juce::ScopedLock sl (lock); fac = factory; }
        std::vector<GenreProfile> found;
        std::vector<FolderInfo> infos;
        juce::String sig;
        for (auto& dir : root().findChildFiles (juce::File::findDirectories, false))
        {
            if (dir.getFileName().startsWith (".")) continue;
            const auto cacheFile = dir.getChildFile (".castle-danger-cache.json");
            juce::var cache = juce::JSON::parse (cacheFile.loadFileAsString());
            if (! cache.isObject() || (int) cache.getProperty ("analyzerVersion", 0) != kAnalyzerVersion)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("analyzerVersion", kAnalyzerVersion);
                o->setProperty ("songs", juce::var (new juce::DynamicObject()));
                cache = juce::var (o);
            }
            auto* songsObj = cache.getProperty ("songs", {}).getDynamicObject();
            if (songsObj == nullptr) continue;
            bool changed = false;
            std::vector<Features> feats;
            FolderInfo info; info.name = dir.getFileName(); info.id = customIdFor (info.name);
            juce::StringArray present;
            auto files = dir.findChildFiles (juce::File::findFiles, true);
            files.sort();
            for (auto& f : files)
            {
                if (threadShouldExit()) return;
                if (f.getFileName().startsWith (".") || fm.findFormatForFileExtension (f.getFileExtension()) == nullptr) continue;
                const auto key = f.getRelativePathFrom (dir);
                present.add (key);
                const auto stamp = juce::String (f.getSize()) + ":" + juce::String (f.getLastModificationTime().toMilliseconds());
                sig << key << "@" << stamp << ",";
                const auto entry = songsObj->getProperty (key);
                if (entry.isObject() && entry.getProperty ("stamp", "").toString() == stamp)
                {
                    if (entry.getProperty ("error", "").toString().isNotEmpty()) { ++info.failed; continue; }
                    auto fx = Features::fromVar (entry.getProperty ("features", {}));
                    if (fx.valid) { feats.push_back (fx); continue; }
                }
                { const juce::ScopedLock sl (lock); measuring = dir.getFileName() + ": " + f.getFileNameWithoutExtension(); }
                ++version;
                Features fx; juce::String err;
                const bool ok = analyseSong (fm, f, fx, err, [this] { return threadShouldExit(); });
                if (threadShouldExit()) return;
                auto* e = new juce::DynamicObject();
                e->setProperty ("stamp", stamp);
                if (ok) { e->setProperty ("features", fx.toVar()); feats.push_back (fx); }
                else { e->setProperty ("error", err); ++info.failed; }
                songsObj->setProperty (key, juce::var (e));
                changed = true;
            }
            // forget songs that were removed
            juce::StringArray gone;
            for (auto& p : songsObj->getProperties()) if (! present.contains (p.name.toString())) gone.add (p.name.toString());
            for (auto& k : gone) { songsObj->removeProperty (k); changed = true; }
            if (changed) cacheFile.replaceWithText (juce::JSON::toString (cache, false));

            info.songs = (int) feats.size();
            infos.push_back (info);
            sig << info.id << ":" << info.songs << ":" << info.failed << ";";
            if (! feats.empty()) found.push_back (buildCustomProfile (info.name, feats, fac));
        }
        bool bump = false;
        {
            const juce::ScopedLock sl (lock);
            if (sig != lastSig || measuring.isNotEmpty()) { profiles = std::move (found); folders = std::move (infos); lastSig = sig; bump = true; }
            measuring = {};
        }
        if (bump) ++version;
    }

    juce::AudioFormatManager fm;
    juce::CriticalSection lock, scanning;
    std::vector<GenreProfile> factory, profiles;
    std::vector<FolderInfo> folders;
    juce::String measuring, lastSig;
};
} // namespace mst
