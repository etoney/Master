#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Advisor.h"

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

class MasterProcessor : public juce::AudioProcessor
{
public:
    MasterProcessor();
    ~MasterProcessor() override = default;

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

    // ---- analysis
    AnalysisWorker& getWorker() { return worker; }
    void resetAnalysis();
    // what the report should show: the live analysis, or the one saved with the project
    // until enough new audio has come in
    mst::Features getDisplayFeatures (bool& fromSavedProject) const;
    bool isListening() const { return apvts.getRawParameterValue ("listen")->load() > 0.5f; }
    std::atomic<float> inPeak { 0.0f };

    // ---- genres
    mst::ProfileLibrary& getLibrary() { return library; }
    juce::String getGenreChoice() const { return genreChoice; }   // "auto" or a genre id
    void setGenreChoice (const juce::String& id) { genreChoice = id; }

    // Which profile the report compares against, plus the detector's guesses
    struct Target { int index = -1; bool automatic = true; std::vector<mst::GenreGuess> guesses; };
    Target resolveTarget (const mst::Features&) const;

private:
    AnalysisWorker worker;
    mst::ProfileLibrary library;
    juce::String genreChoice { "auto" };
    mst::Features savedFeatures;   // restored from the project

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterProcessor)
};
