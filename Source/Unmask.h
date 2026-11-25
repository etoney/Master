#pragma once
// THE MASTER - UNMASKING
// Two tracks that play at the same time in the same range hide each other. Whoever matters more there
// keeps the range; the other one makes a pocket for it. The Master decides who wins each range from the
// ROLES (set by the track names), measures the overlap from what the Servants report (each track's
// share of each band, after its fader), and how often the two actually play together (their level
// fingerprints), then:
//   - DYNAMIC: The Freak on the loser (or on its bus), keyed by the winner on its sidechain, cuts the
//     winner's range only while the winner plays. The loser keeps its full tone the rest of the time.
//   - STATIC (until The Freak is there and hears its key): a smaller, fixed cut on the loser's Servant.
// The winner is left alone in its own range.
// Included by Tracks.h once the track types are defined (planTracks uses it).

namespace mst
{
// who wins a range: the lead vocal owns 500 Hz - 4 kHz against everything but the other vocals and the
// drums; the lead guitar owns 1 - 4 kHz against the rhythm guitars and keys.
enum class Owner { none, leadVocal, leadGuitar };

inline Owner ownerOf (const juce::String& role)
{
    if (isLeadVocal (role)) return Owner::leadVocal;
    if (roleIs (role, { "Lead Guitar" })) return Owner::leadGuitar;
    return Owner::none;
}

inline bool yieldsTo (Owner o, const juce::String& role)
{
    if (o == Owner::leadVocal)
        return ! (isLeadVocal (role) || roleIs (role, { "Backing", "Kick", "Snare", "Drums", "Bass" }));
    if (o == Owner::leadGuitar)
        return roleIs (role, { "Rhythm", "Keys", "Other" });
    return false;
}

// a Freak on a track: where it is, and whether it hears its key (the vocal) on the sidechain
struct KeyedFreak { juce::String track; bool keyHeard = false; };

inline const char* ownerName (Owner o) { return o == Owner::leadVocal ? "the lead vocal" : "the lead guitar"; }

inline std::vector<int> ownerBands (Owner o)
{
    if (o == Owner::leadVocal) return { 4, 5, 6 };   // 500 Hz - 4 kHz: the words
    return { 5, 6 };                                  // 1 - 4 kHz: the solo's bite
}

// the middle of an analyzer band, where a carve goes
inline float bandCentreHz (int b) { return std::sqrt (bandDefs()[(size_t) b].lo * bandDefs()[(size_t) b].hi); }

struct UnmaskPair
{
    Owner owner = Owner::none;
    juce::String ownerTracks;   // "25_LeadVox, 26_LeadVoxDT1"
    size_t loser = 0;           // index into the tracks
    int band = 0;
    float overlapDb = 0;        // the loser's level in the band against the owner's (0 = as loud, + = louder)
    float together = 0;         // of the time the owner plays, how much the loser plays too
    float depth = 0;            // the dynamic cut (dB, positive), while the owner plays
    float staticDepth = 0;      // the fixed cut on the Servant, until a keyed Freak takes over
};

// gain-weighted band powers (c), the gains and each track's fingerprint are what planTracks has
inline std::vector<UnmaskPair> findMasking (const std::vector<TrackInfo>& tracks, const std::vector<std::array<double, kNumBands>>& c,
                                            const std::vector<double>& gains, bool seriesOk)
{
    std::vector<UnmaskPair> out;
    const size_t n = tracks.size();
    for (auto o : { Owner::leadVocal, Owner::leadGuitar })
    {
        // the owner: every track with that role together (a lead and its doubles)
        std::array<double, kNumBands> own {};
        std::vector<float> ownSer;
        juce::StringArray names;
        for (size_t i = 0; i < n; ++i)
        {
            if (ownerOf (tracks[i].role) != o) continue;
            names.add (tracks[i].name);
            for (int b = 0; b < kNumBands; ++b) own[(size_t) b] += c[i][(size_t) b];
            if (seriesOk)
            {
                if (ownSer.empty()) ownSer.assign (tracks[i].series.size(), 0.0f);
                for (size_t k = 0; k < ownSer.size() && k < tracks[i].series.size(); ++k) ownSer[k] += (float) gains[i] * tracks[i].series[k];
            }
        }
        if (names.isEmpty()) continue;
        // the owner's ranges: the two biggest of its bands, if they carry a real part of it
        auto bands = ownerBands (o);
        double ownTot = 0; for (auto b : bands) ownTot += own[(size_t) b];
        if (ownTot <= 0) continue;
        std::sort (bands.begin(), bands.end(), [&] (int a, int b) { return own[(size_t) a] > own[(size_t) b]; });
        if (bands.size() > 2) bands.resize (2);
        bands.erase (std::remove_if (bands.begin(), bands.end(), [&] (int b) { return own[(size_t) b] / ownTot < 0.2; }), bands.end());

        // when the owner plays: slots within 35 dB of its loudest
        auto active = [] (const std::vector<float>& s)
        {
            std::vector<bool> a (s.size(), false);
            float mx = 0; for (auto v : s) mx = std::max (mx, v);
            for (size_t k = 0; k < s.size(); ++k) a[k] = mx > 0 && s[k] > mx * 3.2e-4f;
            return a;
        };
        const auto ownOn = active (ownSer);
        size_t ownOnCount = 0; for (auto x : ownOn) ownOnCount += x ? 1 : 0;

        for (size_t i = 0; i < n; ++i)
        {
            if (! yieldsTo (o, tracks[i].role)) continue;
            float together = 1.0f;   // without fingerprints: assume they play together
            if (seriesOk && ownOnCount >= 30 && tracks[i].series.size() == ownOn.size())
            {
                const auto on = active (tracks[i].series);
                size_t both = 0; for (size_t k = 0; k < on.size(); ++k) if (on[k] && ownOn[k]) ++both;
                together = (float) both / (float) ownOnCount;
            }
            if (together < 0.3f) continue;   // they hardly ever play at the same time (a solo between the verses)
            for (auto b : bands)
            {
                if (own[(size_t) b] <= 0 || c[i][(size_t) b] <= 0) continue;
                const float overlap = (float) (10.0 * std::log10 (c[i][(size_t) b] / own[(size_t) b]));
                if (overlap < -6.0f) continue;   // far enough under the owner already
                UnmaskPair p;
                p.owner = o; p.ownerTracks = names.joinIntoString (", "); p.loser = i; p.band = b;
                p.overlapDb = overlap; p.together = together;
                const float full = o == Owner::leadVocal ? 6.0f : 4.0f;
                p.depth = std::round (juce::jlimit (2.0f, full, 3.0f + 0.5f * overlap));
                p.staticDepth = std::round (juce::jlimit (1.0f, o == Owner::leadVocal ? 3.0f : 2.0f, p.depth * 0.5f) * 2.0f) / 2.0f;
                out.push_back (p);
            }
        }
    }
    return out;
}

inline juce::String overlapText (const UnmaskPair& p)
{
    const auto& d = bandDefs()[(size_t) p.band];
    const auto rel = std::abs (p.overlapDb) < 0.5f ? juce::String ("as loud as")
                   : juce::String (std::abs (p.overlapDb), 1) + " dB " + (p.overlapDb > 0 ? "louder than" : "under");
    return juce::String ("in ") + d.range + " Hz it sits " + rel + " " + ownerName (p.owner) + " (" + p.ownerTracks + "), and plays during "
         + juce::String (juce::roundToInt (p.together * 100.0f)) + "% of it";
}

// A Freak on a track, keyed by the lead vocal: which of the losers does it cover? Its own track, or the
// tracks a bus named for them carries ("Rhythm GTR Bus", "Guitar Bus", "Keys Bus").
inline bool freakCovers (const juce::String& freakTrack, const TrackInfo& loser)
{
    if (freakTrack.equalsIgnoreCase (loser.name)) return true;
    const auto t = freakTrack.toLowerCase();
    if (! t.contains ("bus")) return false;
    const bool gtrBus = t.contains ("gtr") || t.contains ("guitar"), keysBus = t.contains ("key") || t.contains ("synth") || t.contains ("piano");
    if (gtrBus && loser.role.containsIgnoreCase ("Guitar"))
    {
        if (t.contains ("rhythm") || t.contains ("rhy")) return roleIs (loser.role, { "Rhythm" });
        if (t.contains ("lead")) return roleIs (loser.role, { "Lead Guitar" });
        return true;
    }
    return keysBus && roleIs (loser.role, { "Keys" });
}

// The Freak on the loser(s): one KEY band per range it has to make room in, cutting while the vocal sings
inline TrackPlan planUnmaskFreak (const juce::String& id, const juce::String& track, const std::vector<UnmaskPair>& pairs,
                                  const std::vector<TrackInfo>& tracks, bool keyHeard)
{
    TrackPlan p;
    p.kind = "freak"; p.id = id; p.name = track; p.role = "Unmask";
    std::map<int, float> depthByBand;
    std::map<int, juce::String> whyByBand;
    for (auto& u : pairs)
    {
        if (u.owner != Owner::leadVocal || ! freakCovers (track, tracks[u.loser])) continue;
        if (u.depth > depthByBand[u.band]) { depthByBand[u.band] = u.depth; whyByBand[u.band] = tracks[u.loser].name + ": " + overlapText (u); }
    }
    for (int k = 0; k < 6; ++k)
    {
        const juce::String b = "b" + juce::String (k + 1);
        p.params[b + "on"] = 0.0f; p.params[b + "k"] = 0.0f; p.params[b + "g"] = 0.0f;
    }
    int k = 0;
    juce::StringArray what;
    for (auto& [band, depth] : depthByBand)
    {
        if (k >= 6) break;
        const juce::String b = "b" + juce::String (k + 1);
        const auto& d = bandDefs()[(size_t) band];
        p.params[b + "on"] = 1.0f; p.params[b + "type"] = 0.0f; p.params[b + "f"] = bandCentreHz (band); p.params[b + "q"] = 1.4f;
        p.params[b + "g"] = 0.0f; p.params[b + "d"] = -depth; p.params[b + "t"] = 8.0f; p.params[b + "k"] = 1.0f;
        p.bandNotes.set (k, "Room for the vocal: -" + juce::String (juce::roundToInt (depth)) + " dB at " + d.range + " Hz while it sings");
        p.why.add ("BAND " + juce::String (k + 1) + " KEY " + d.label + " -" + juce::String (juce::roundToInt (depth)) + " dB: " + whyByBand[band]
                   + ". It cuts only while the vocal sings, so between the lines this track keeps its full tone.");
        what.add (juce::String (d.label) + " -" + juce::String (juce::roundToInt (depth)));
        ++k;
    }
    p.params["amount"] = 100.0f; p.params["speed"] = 65.0f; p.params["output"] = 0.0f;
    p.why.add ("THRESHOLD 8 dB on the KEY bands: the cut holds through the vocal's quieter words and lets go about half a second after a line ends.");
    if (! keyHeard)
        p.why.add ("Nothing on The Freak's sidechain yet: send the lead vocal (or the lead vocal bus) to it (in Studio Pro: the sidechain button in The Freak's window), "
                   "then press UPDATE. Until then the Servants make a smaller fixed pocket instead.");
    p.note = (what.isEmpty() ? juce::String ("NOTHING TO UNMASK HERE") : "UNMASK " + what.joinIntoString (", ") + " dB")
           + (keyHeard ? juce::String() : juce::String ("  //  NO VOCAL ON SIDECHAIN"));
    if (what.isEmpty()) p.why.add ("No track it carries hides the lead vocal: all KEY bands off.");
    return p;
}
} // namespace mst
