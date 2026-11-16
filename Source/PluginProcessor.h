#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Advisor.h"
#include "Cure.h"
#include "Chain.h"
#include "CDLink.h"
#include "SongProfiles.h"

// Lock-free hand-off from the audio thread to a low-priority analysis thread, so the
// master bus never waits on an FFT. Audio passes through untouched (zero latency).
class AnalysisWorker : private juce::Thread
{
public:
    AnalysisWorker();
    ~AnalysisWorker() override;

    void configure (double sampleRate);              // from prepareToPlay
    // audio thread. Live playback never waits (if the analyser fell behind, audio is skipped);
    // an offline bounce (mayWait) waits for room, so a faster-than-real-time render is analysed in full.
    void push (const float* l, const float* r, int n, bool mayWait);
    void requestReset() { resetRequested.store (true); }
    void getSnapshot (mst::Features& out) const { const juce::SpinLock::ScopedLockType sl (snapLock); out = snapshot; }
    int getSnapshotVersion() const { return snapVersion.load(); }
    double getSampleRate() const { return sr.load(); }
    // for tests: wait until everything pushed so far is analysed and a fresh snapshot is out
    bool waitUntilIdle (int timeoutMs);

private:
    void run() override;
    void drain();

    juce::AbstractFifo fifo { 1 << 18 };
    std::vector<float> bufL, bufR;
    mst::MixAnalysis analysis;
    std::atomic<double> sr { 0.0 }, pendingSr { 0.0 };
    std::atomic<bool> resetRequested { false };
    std::atomic<juce::int64> pushed { 0 }, consumed { 0 };
    mst::Features snapshot;
    mutable juce::SpinLock snapLock;
    std::atomic<int> snapVersion { 0 };
    std::atomic<juce::int64> snapConsumed { 0 };
    std::vector<float> scratchL, scratchR;
};

class MasterProcessor : public juce::AudioProcessor,
                        private juce::AudioProcessorValueTreeState::Listener
{
public:
    MasterProcessor();
    ~MasterProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "The Master"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    // ---- analysis: 'worker' hears your mix as it comes in, 'postWorker' hears the result
    // after the fixes (reset whenever a fix setting changes, so it always matches them)
    AnalysisWorker& getWorker() { return worker; }
    AnalysisWorker& getPostWorker() { return postWorker; }
    void resetAnalysis();
    // what the report should show: the live analysis, or the one saved with the project
    // until enough new audio has come in. after = the corrected output.
    mst::Features getDisplayFeatures (bool& fromSavedProject) const;
    mst::Features getAfterFeatures() const { mst::Features f; postWorker.getSnapshot (f); return f; }
    bool isFixOn() const { return apvts.getRawParameterValue ("fixOn")->load() > 0.5f; }

    // ---- AUTO FIX
    // Works out the corrections from what's been heard and sets every fix control.
    // Returns an empty string or the reason it couldn't.
    juce::String applyAutoFix();
    enum Section { secEq = 0, secDyn, secStereo, secHum, secLoud };
    void revertToAuto (Section);              // undo your overrides in one section
    bool isOverridden (Section) const;        // differs from what AUTO FIX set
    const mst::FixPlan& getPlan() const { return plan; }
    mst::FixParams currentFixParams() const;
    mst::FixChain& getChain() { return chain; }
    static juce::StringArray fixParamIds();

    // ---- MY PLUGINS mode: The Master prescribes Castle Danger plugins for the master bus,
    //      then sets their controls over Castle Danger Link when you press APPLY.
    struct Peer
    {
        juce::String id, kind, name, version;
        juce::var params, env, extra;
        juce::uint32 seen = 0;
    };
    struct StepStatus
    {
        juce::String kind, name, role, peerId;
        bool linked = false;         // a plugin of this kind is in the session
        int position = -1;           // where it actually sits in The Master's chain (0 = first), -1 = not detected
        bool orderOk = false;        // detected and in the prescribed order
    };
    struct LogEntry { juce::String plugin, title, detail; bool confirmed = false; juce::String peerId; };
    struct TrackStatus { juce::String peerId, kind { "servant" }, name, role, summary; float share = -1; bool linked = false, planned = false; };
    std::vector<TrackStatus> trackStatus() const;   // linked Servants, BuzzKills and Flatliners + what the prescription does to each
    static bool isTrackKind (const juce::String& k) { return k == "servant" || k == "buzzkill" || k == "flatliner"; }

    bool isPluginMode() const { return pluginMode; }
    void setPluginMode (bool b) { pluginMode = b; }
    std::vector<Peer> getPeers() const;
    std::vector<StepStatus> chainStatus() const;   // the prescription vs what's actually on the bus
    bool chainOrderKnown() const;                  // enough audio has played to check the order
    juce::String prescribe();                      // freezes the analysis, makes the prescription
    // PRESCRIBE as the button does it: the Servants that are listening finish first (each decides
    // its own resonances and clean-up), then The Master prescribes. 'done' gets prescribe()'s answer.
    void requestPrescribe (std::function<void (const juce::String&)> done);
    bool isWaitingForServants() const { return pendingPrescribe != nullptr; }
    juce::String applyChain();                     // sends the settings (needs every prescribed plugin linked)
    juce::String refineChain();                    // after a play-through: fine-tune from what came out
    int getChainStage() const { return chainStage; }   // 0 nothing, 1 prescribed, 2 applied
    const mst::ChainPlan& getChainPlan() const { return chainPlan; }
    const std::vector<LogEntry>& getLog() const { return chainLog; }
    std::atomic<int> linkVersion { 0 };
    bool isListening() const { return apvts.getRawParameterValue ("listen")->load() > 0.5f; }
    std::atomic<float> inPeak { 0.0f };

    // ---- genres
    mst::ProfileLibrary& getLibrary() { return library; }
    // your own profiles: a folder of songs each (Profiles/My Profiles/<name>)
    mst::SongFolders& getSongFolders() { return songs; }
    void refreshSongProfiles();                       // pick up new / changed folders now (also done 4x a second)
    bool createSongProfile (const juce::String& name, juce::File& folderOut);
    juce::String getGenreChoice() const { return genreChoice; }   // "auto" or a genre id
    void setGenreChoice (const juce::String& id) { genreChoice = id; }

    // Which profile the report compares against, plus the detector's guesses
    struct Target { int index = -1; bool automatic = true; std::vector<mst::GenreGuess> guesses; };
    Target resolveTarget (const mst::Features&) const;

private:
    void parameterChanged (const juce::String&, float) override;
    void onLinkMessage (const juce::var&);
    void linkTick();
    juce::String resolvePeer (const juce::String& kind) const;
    std::vector<juce::String> detectChain() const;   // peer ids feeding The Master, first .. last
    void sendSet (const juce::String& peerId, const std::map<juce::String, float>& params, const juce::String& note);
    struct LinkTimer : juce::Timer { std::function<void()> fn; void timerCallback() override { fn(); } };
    juce::String servantOnSameTrack (const Peer& helper) const;   // by track name, else by level fingerprint
    std::function<void (const juce::String&)> pendingPrescribe;
    int finishSeq = 0;
    juce::uint32 finishSentMs = 0;
    std::set<juce::String> finishWaiting;
    LinkTimer finishTimer;

    bool pluginMode = true;   // MY PLUGINS first; BUILT-IN FIXES is the alternative
    int chainStage = 0;
    mst::ChainPlan chainPlan;
    std::vector<LogEntry> chainLog;
    std::map<juce::String, Peer> peers;
    // level fingerprints over the whole analysis (100 ms steps, linear power): The Master's input
    // and every linked plugin's output, to work out each track's real gain into the mix
    std::map<juce::int64, float> myHist;
    std::map<juce::String, std::map<juce::int64, float>> peerHist;
    static void mergeEnv (std::map<juce::int64, float>& hist, const juce::var& env, const char* key);
    cdlink::EnvTap env;
    std::unique_ptr<cdlink::Endpoint> link;
    LinkTimer linkTimer;
    void setParam (const juce::String& id, float value);
    void applySection (Section, const mst::FixParams&);

    AnalysisWorker worker, postWorker;
    mst::FixChain chain;
    mst::FixPlan plan;
    int maxBlock = 512;
    std::vector<float> monoR;
    mst::ProfileLibrary library;
    mst::SongFolders songs;
    int songsVersionSeen = -1;
    juce::String genreChoice { "auto" };
    mst::Features savedFeatures;   // restored from the project

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterProcessor)
};
