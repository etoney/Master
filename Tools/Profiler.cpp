// TheMasterProfiler - admin tool that builds The Master's genre profiles from reference songs.
//
//   TheMasterProfiler build <references folder> <out.json> [--merge <pack.json>] [--version <name>]
//       <references folder> holds one sub-folder per genre ("Rock", "Metal", "Hard Rock", ...),
//       each full of mastered reference songs (WAV, AIFF, FLAC, OGG, MP3; M4A/AAC on macOS).
//       Every song is measured with the same analyser the plugin uses; each genre gets the mean
//       and spread of every measurement. --merge keeps genres from an existing pack that have
//       no folder (so you can update one genre at a time). Also writes <out>.tracks.csv so you
//       can see every song's numbers and spot an odd one out.
//
//   TheMasterProfiler analyze <song> [<song> ...]
//       Prints what The Master measures for each song.
//
//   TheMasterProfiler check <pack.json> <song>
//       Prints the genre guess and the full report for one song against a pack.
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/Advisor.h"
#include "BinaryData.h"
#include <cstdio>

static void out (const juce::String& s) { std::printf ("%s\n", s.toRawUTF8()); std::fflush (stdout); }

static bool analyseFile (juce::AudioFormatManager& fm, const juce::File& f, mst::Features& result, juce::String& err)
{
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) { err = "can't read this file type"; return false; }
    if (r->sampleRate < 8000 || r->lengthInSamples <= 0) { err = "no audio"; return false; }
    mst::MixAnalysis a;
    a.prepare (r->sampleRate);
    const int chunk = 1 << 16;
    juce::AudioBuffer<float> buf ((int) juce::jmax (1u, juce::jmin (2u, r->numChannels)), chunk);
    for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += chunk)
    {
        const int n = (int) std::min<juce::int64> ((juce::int64) chunk, r->lengthInSamples - pos);
        r->read (&buf, 0, n, pos, true, buf.getNumChannels() > 1);
        a.process (buf.getReadPointer (0), buf.getNumChannels() > 1 ? buf.getReadPointer (1) : nullptr, n);
    }
    result = a.compute();
    if (! result.valid) { err = "too short or silent"; return false; }
    return true;
}

static juce::String idFor (const juce::String& name)
{
    auto words = juce::StringArray::fromTokens (name.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789 -_"), " -_", "");
    words.removeEmptyStrings();
    juce::String id;
    for (int i = 0; i < words.size(); ++i) id << (i == 0 ? words[i] : words[i].substring (0, 1).toUpperCase() + words[i].substring (1));
    return id.isEmpty() ? juce::String ("genre") : id;
}

static void printFeatures (const mst::Features& f)
{
    out (juce::String::formatted ("  loudness %.1f LUFS   true peak %.1f dBTP   PLR %.1f dB   LRA %.1f LU", f.lufs, f.truePeak, f.plr, f.lra));
    out (juce::String::formatted ("  width %.1f dB   correlation %.2f   low-end correlation %.2f   clipping %d   hum %.0f s",
                                  f.width, f.correlation, f.lowCorrelation, f.clipEvents, f.humSeconds));
    juce::String b ("  bands:");
    for (int i = 0; i < mst::kNumBands; ++i) b << " " << mst::bandDefs()[(size_t) i].key << " " << juce::String (f.band[(size_t) i], 1);
    out (b);
}

static juce::Array<juce::File> audioFilesIn (const juce::File& dir, juce::AudioFormatManager& fm)
{
    juce::Array<juce::File> res;
    for (auto& f : dir.findChildFiles (juce::File::findFiles, true))
        if (! f.getFileName().startsWith (".") && fm.findFormatForFileExtension (f.getFileExtension()) != nullptr) res.add (f);
    res.sort();
    return res;
}

int main (int argc, char** argv)
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add (juce::String::fromUTF8 (argv[i]));
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (args.size() >= 2 && args[0] == "analyze")
    {
        for (int i = 1; i < args.size(); ++i)
        {
            mst::Features f; juce::String err;
            const auto file = cwd.getChildFile (args[i]);
            if (! analyseFile (fm, file, f, err)) { out (file.getFileName() + ": " + err); continue; }
            out (file.getFileName() + "  (" + juce::String (f.seconds, 0) + " s)");
            printFeatures (f);
        }
        return 0;
    }

    if (args.size() >= 3 && args[0] == "check")
    {
        mst::ProfilePack pack; juce::String err;
        if (! mst::ProfilePack::parse (cwd.getChildFile (args[1]).loadFileAsString(), pack, err)) { out ("pack: " + err); return 1; }
        mst::Features f;
        if (! analyseFile (fm, cwd.getChildFile (args[2]), f, err)) { out ("song: " + err); return 1; }
        printFeatures (f);
        auto guesses = mst::detectGenre (f, pack.genres);
        for (auto& g : guesses) out (juce::String::formatted ("  sounds like %-12s %3d%%", pack.genres[(size_t) g.index].name.toRawUTF8(), juce::roundToInt (g.probability * 100)));
        auto rep = mst::advise (f, pack.genres[(size_t) guesses[0].index]);
        out ("\nScore " + juce::String (rep.score) + "/100 vs " + rep.genreName);
        for (auto& s : rep.items)
        {
            const char* tag = s.sev == mst::Sev::fix ? "FIX  " : s.sev == mst::Sev::check ? "CHECK" : s.sev == mst::Sev::good ? "GOOD " : "NOTE ";
            out (juce::String (tag) + " " + s.title + ": " + s.body + (s.plugin.isNotEmpty() ? "  -> " + s.plugin + ": " + s.tip : juce::String()));
        }
        return 0;
    }

    if (args.size() >= 3 && args[0] == "build")
    {
        const auto root = cwd.getChildFile (args[1]);
        const auto dest = cwd.getChildFile (args[2]);
        if (! root.isDirectory()) { out ("not a folder: " + root.getFullPathName()); return 1; }
        mst::ProfilePack base;
        bool merging = false;
        juce::String version = juce::Time::getCurrentTime().formatted ("%Y.%m.%d");
        for (int i = 3; i < args.size(); ++i)
        {
            if (args[i] == "--merge" && i + 1 < args.size())
            {
                juce::String err;
                const auto mf = cwd.getChildFile (args[++i]);
                const auto json = mf.getFileName() == "factory" ? juce::String::fromUTF8 (BinaryData::factory_json, BinaryData::factory_jsonSize) : mf.loadFileAsString();
                if (! mst::ProfilePack::parse (json, base, err)) { out ("--merge: " + err); return 1; }
                merging = true;
            }
            else if (args[i] == "--version" && i + 1 < args.size()) version = args[++i];
        }

        mst::ProfilePack pack;
        pack.packVersion = version;
        juce::String csv ("genre,file,seconds,lufs,truePeak,plr,lra,width,correlation,lowCorrelation,clipEvents,humSeconds");
        for (auto& b : mst::bandDefs()) csv << "," << b.key;
        csv << "\n";
        int totalSongs = 0;
        auto dirs = root.findChildFiles (juce::File::findDirectories, false);
        dirs.sort();
        for (auto& d : dirs)
        {
            if (d.getFileName().startsWith (".")) continue;
            const auto name = d.getFileName();
            juce::String id = idFor (name);
            if (const auto* existing = [&]() -> const mst::GenreProfile* { for (auto& g : base.genres) if (g.name.equalsIgnoreCase (name) || g.id == id) return &g; return nullptr; }())
                id = existing->id;
            std::vector<mst::Features> songs;
            out ("== " + name + " (id " + id + ")");
            for (auto& f : audioFilesIn (d, fm))
            {
                mst::Features fx; juce::String err;
                if (! analyseFile (fm, f, fx, err)) { out ("   skip " + f.getFileName() + ": " + err); continue; }
                songs.push_back (fx);
                out (juce::String::formatted ("   %-50s %6.1f LUFS  PLR %4.1f", f.getFileName().substring (0, 50).toRawUTF8(), fx.lufs, fx.plr));
                csv << name.quoted() << "," << f.getFileName().quoted() << "," << fx.seconds << "," << fx.lufs << "," << fx.truePeak << "," << fx.plr << "," << fx.lra
                    << "," << fx.width << "," << fx.correlation << "," << fx.lowCorrelation << "," << fx.clipEvents << "," << fx.humSeconds;
                for (auto v : fx.band) csv << "," << v;
                csv << "\n";
            }
            if (songs.empty()) { out ("   no usable songs, genre left out"); continue; }
            if (songs.size() < 10) out ("   note: only " + juce::String ((int) songs.size()) + " songs; 20+ per genre gives a much steadier profile");
            pack.genres.push_back (mst::buildProfile (id, name, songs));
            totalSongs += (int) songs.size();
        }
        if (merging)
            for (auto& g : base.genres)
                if (pack.find (g.id) == nullptr) { out ("== keeping " + g.name + " from the merged pack"); pack.genres.push_back (g); }
        if (pack.genres.empty()) { out ("no genres built"); return 1; }
        pack.notes = "Built by TheMasterProfiler from " + juce::String (totalSongs) + " reference songs.";
        if (! dest.replaceWithText (pack.toJson())) { out ("couldn't write " + dest.getFullPathName()); return 1; }
        dest.withFileExtension (".tracks.csv").replaceWithText (csv);
        out ("\nwrote " + dest.getFullPathName() + " (" + juce::String ((int) pack.genres.size()) + " genres, " + juce::String (totalSongs) + " songs)");
        out ("wrote " + dest.withFileExtension (".tracks.csv").getFullPathName());
        return 0;
    }

    out ("TheMasterProfiler - builds genre profiles for The Master (Castle Danger Music Company)\n");
    out ("  TheMasterProfiler build <references folder> <out.json> [--merge <pack.json>|factory] [--version <name>]");
    out ("  TheMasterProfiler analyze <song> [<song> ...]");
    out ("  TheMasterProfiler check <pack.json> <song>");
    out ("\nSee ADMIN.txt.");
    return args.isEmpty() ? 0 : 1;
}
