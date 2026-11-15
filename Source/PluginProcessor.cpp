#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

//==============================================================================
AnalysisWorker::AnalysisWorker() : juce::Thread ("The Master analysis")
{
    bufL.assign ((size_t) fifo.getTotalSize(), 0.0f);
    bufR.assign ((size_t) fifo.getTotalSize(), 0.0f);
    scratchL.assign (4096, 0.0f);
    scratchR.assign (4096, 0.0f);
    startThread (juce::Thread::Priority::low);
}

AnalysisWorker::~AnalysisWorker() { stopThread (2000); }

void AnalysisWorker::configure (double sampleRate)
{
    if (sampleRate > 0 && sampleRate != sr.load()) pendingSr.store (sampleRate);
    notify();
}

void AnalysisWorker::push (const float* l, const float* r, int n, bool mayWait)
{
    if (sr.load() <= 0 && pendingSr.load() <= 0) return;
    if (mayWait)
        for (int tries = 0; fifo.getFreeSpace() < n && tries < 2000; ++tries) { notify(); juce::Thread::sleep (1); }
    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);     // drops what doesn't fit rather than wait
    if (n1 > 0) { std::copy (l, l + n1, bufL.data() + s1); std::copy (r, r + n1, bufR.data() + s1); }
    if (n2 > 0) { std::copy (l + n1, l + n1 + n2, bufL.data() + s2); std::copy (r + n1, r + n1 + n2, bufR.data() + s2); }
    fifo.finishedWrite (n1 + n2);
    pushed.fetch_add (n1 + n2);
}

void AnalysisWorker::drain()
{
    if (sr.load() <= 0) return;    // not prepared yet: leave the audio waiting in the FIFO
    for (;;)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead ((int) scratchL.size(), s1, n1, s2, n2);
        const int n = n1 + n2;
        if (n == 0) return;
        std::copy (bufL.data() + s1, bufL.data() + s1 + n1, scratchL.data());
        std::copy (bufR.data() + s1, bufR.data() + s1 + n1, scratchR.data());
        if (n2 > 0)
        {
            std::copy (bufL.data() + s2, bufL.data() + s2 + n2, scratchL.data() + n1);
            std::copy (bufR.data() + s2, bufR.data() + s2 + n2, scratchR.data() + n1);
        }
        fifo.finishedRead (n);
        analysis.process (scratchL.data(), scratchR.data(), n);
        consumed.fetch_add (n);
    }
}

void AnalysisWorker::run()
{
    auto lastSnap = juce::Time::getMillisecondCounter();
    bool dirty = true;
    while (! threadShouldExit())
    {
        double want = pendingSr.load();
        if (want > 0)
        {
            analysis.prepare (want);
            sr.store (want);
            pendingSr.compare_exchange_strong (want, 0.0);   // keep a newer request if one came in meanwhile
            dirty = true;
        }
        if (resetRequested.exchange (false)) { analysis.reset(); dirty = true; }

        const auto before = consumed.load();
        drain();
        if (consumed.load() != before) dirty = true;

        const auto now = juce::Time::getMillisecondCounter();
        if (dirty && (now - lastSnap > 250 || pushed.load() == consumed.load()))
        {
            const auto done = consumed.load();
            auto f = analysis.compute();
            {
                const juce::SpinLock::ScopedLockType sl (snapLock);
                snapshot = f;
            }
            snapConsumed.store (done);
            ++snapVersion;
            lastSnap = now;
            dirty = false;
        }
        wait (30);
    }
}

bool AnalysisWorker::waitUntilIdle (int timeoutMs)
{
    const auto start = juce::Time::getMillisecondCounter();
    const auto target = pushed.load();
    while (consumed.load() < target || pendingSr.load() > 0)
    {
        notify();
        juce::Thread::sleep (5);
        if ((int) (juce::Time::getMillisecondCounter() - start) > timeoutMs) return false;
    }
    while (snapConsumed.load() < target)
    {
        notify();
        juce::Thread::sleep (5);
        if ((int) (juce::Time::getMillisecondCounter() - start) > timeoutMs) return false;
    }
    return true;
}

//==============================================================================
MasterProcessor::MasterProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TheMaster", createLayout())
{
    library.load (juce::String::fromUTF8 (BinaryData::factory_json, BinaryData::factory_jsonSize), mst::ProfileLibrary::getUserPackFile());
}

juce::AudioProcessorValueTreeState::ParameterLayout MasterProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "listen", 1 }, "Listen", true));
    return { p.begin(), p.end() };
}

bool MasterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    const auto in = layouts.getMainInputChannelSet();
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

void MasterProcessor::prepareToPlay (double sampleRate, int)
{
    worker.configure (sampleRate);
    setLatencySamples (0);
}

void MasterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int inCh = getTotalNumInputChannels();
    if (inCh == 1 && buffer.getNumChannels() > 1) buffer.copyFrom (1, 0, buffer, 0, 0, n);   // mono in -> stereo out
    for (int c = juce::jmax (inCh, 2); c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    if (n == 0 || buffer.getNumChannels() == 0) return;

    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : l;
    inPeak.store (std::max (inPeak.load(), std::max (buffer.getMagnitude (0, 0, n), buffer.getNumChannels() > 1 ? buffer.getMagnitude (1, 0, n) : 0.0f)));
    if (isListening()) worker.push (l, r, n, isNonRealtime());
}

void MasterProcessor::resetAnalysis()
{
    worker.requestReset();
    savedFeatures = {};
}

mst::Features MasterProcessor::getDisplayFeatures (bool& fromSaved) const
{
    mst::Features live;
    worker.getSnapshot (live);
    fromSaved = savedFeatures.valid && live.seconds < 2.0;
    return fromSaved ? savedFeatures : live;
}

MasterProcessor::Target MasterProcessor::resolveTarget (const mst::Features& fx) const
{
    Target t;
    const auto& genres = library.genres();
    t.guesses = mst::detectGenre (fx, genres);
    const int chosen = genreChoice == "auto" ? -1 : library.indexOf (genreChoice);
    if (chosen >= 0) { t.index = chosen; t.automatic = false; }
    else if (! t.guesses.empty()) t.index = t.guesses.front().index;
    else t.index = genres.empty() ? -1 : 0;
    return t;
}

void MasterProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("genre", genreChoice, nullptr);
    bool fromSaved = false;
    const auto fx = getDisplayFeatures (fromSaved);
    if (fx.valid) state.setProperty ("analysis", juce::JSON::toString (fx.toVar(), true), nullptr);
    else state.removeProperty ("analysis", nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void MasterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            genreChoice = state.getProperty ("genre", "auto").toString();
            const auto a = state.getProperty ("analysis", "").toString();
            savedFeatures = a.isNotEmpty() ? mst::Features::fromVar (juce::JSON::parse (a)) : mst::Features {};
            state.removeProperty ("analysis", nullptr);
            apvts.replaceState (state);
        }
}

juce::AudioProcessorEditor* MasterProcessor::createEditor() { return new MasterEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MasterProcessor(); }
