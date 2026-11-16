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
    for (auto& id : fixParamIds()) apvts.addParameterListener (id, this);
}

MasterProcessor::~MasterProcessor()
{
    for (auto& id : fixParamIds()) apvts.removeParameterListener (id, this);
}

juce::StringArray MasterProcessor::fixParamIds()
{
    juce::StringArray ids { "fixOn", "amount", "eqOn", "dynOn", "glue", "leveler", "stereoOn", "width", "bassMono", "humOn", "humHz", "loudOn", "gain", "ceiling" };
    for (int b = 0; b < mst::kNumBands; ++b) ids.add ("eq" + juce::String (b));
    return ids;
}

// any fix change: the 'after' analysis starts over so it always matches the settings
void MasterProcessor::parameterChanged (const juce::String&, float) { postWorker.requestReset(); }

static const float kCeilings[] = { -1.0f, -2.0f, -0.3f };

mst::FixParams MasterProcessor::currentFixParams() const
{
    auto v = [this] (const juce::String& id) { return apvts.getRawParameterValue (id)->load(); };
    mst::FixParams p;
    p.on = v ("fixOn") > 0.5f;
    p.amount = v ("amount") * 0.01f;
    p.match = v ("match") > 0.5f;
    p.eqOn = v ("eqOn") > 0.5f;
    for (int b = 0; b < mst::kNumBands; ++b) p.eq[(size_t) b] = v ("eq" + juce::String (b));
    p.dynOn = v ("dynOn") > 0.5f; p.glue = v ("glue") * 0.01f; p.leveler = v ("leveler") * 0.01f;
    p.stereoOn = v ("stereoOn") > 0.5f; p.width = v ("width") * 0.01f; p.bassMono = v ("bassMono") > 0.5f;
    p.humOn = v ("humOn") > 0.5f; p.humHz = (int) v ("humHz") == 0 ? 50 : 60;
    p.loudOn = v ("loudOn") > 0.5f; p.gainDb = v ("gain"); p.ceilingDb = kCeilings[juce::jlimit (0, 2, (int) v ("ceiling"))];
    return p;
}

void MasterProcessor::setParam (const juce::String& id, float value)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

void MasterProcessor::applySection (Section s, const mst::FixParams& f)
{
    switch (s)
    {
        case secEq:
            setParam ("eqOn", f.eqOn ? 1.0f : 0.0f);
            for (int b = 0; b < mst::kNumBands; ++b) setParam ("eq" + juce::String (b), f.eq[(size_t) b]);
            break;
        case secDyn: setParam ("dynOn", f.dynOn ? 1.0f : 0.0f); setParam ("glue", f.glue * 100.0f); setParam ("leveler", f.leveler * 100.0f); break;
        case secStereo: setParam ("stereoOn", f.stereoOn ? 1.0f : 0.0f); setParam ("width", f.width * 100.0f); setParam ("bassMono", f.bassMono ? 1.0f : 0.0f); break;
        case secHum: setParam ("humOn", f.humOn ? 1.0f : 0.0f); setParam ("humHz", f.humHz == 50 ? 0.0f : 1.0f); break;
        case secLoud:
        {
            setParam ("loudOn", f.loudOn ? 1.0f : 0.0f); setParam ("gain", f.gainDb);
            int ci = 0; for (int i = 0; i < 3; ++i) if (std::abs (kCeilings[i] - f.ceilingDb) < 0.01f) ci = i;
            setParam ("ceiling", (float) ci);
            break;
        }
    }
}

juce::String MasterProcessor::applyAutoFix()
{
    mst::Features in;
    worker.getSnapshot (in);
    bool saved = false;
    if (! in.valid) in = getDisplayFeatures (saved);   // the report saved with the project works too
    if (! in.valid) return "Play the song first: AUTO FIX works from what The Master has heard.";
    const auto target = resolveTarget (in);
    const auto& genres = library.genres();
    if (target.index < 0 || target.index >= (int) genres.size()) return "No genre profile to aim for.";
    const auto cur = currentFixParams();
    const auto after = getAfterFeatures();
    plan = mst::planFixes (in, &after, cur.gainDb, cur.on && cur.loudOn && after.valid, genres[(size_t) target.index]);
    if (! plan.valid) return "Nothing analysed yet.";
    for (auto s : { secEq, secDyn, secStereo, secHum, secLoud }) applySection (s, plan.p);
    setParam ("amount", 100.0f);
    setParam ("fixOn", 1.0f);
    postWorker.requestReset();
    return {};
}

void MasterProcessor::revertToAuto (Section s)
{
    if (plan.valid) applySection (s, plan.p);
}

bool MasterProcessor::isOverridden (Section s) const
{
    if (! plan.valid) return false;
    const auto c = currentFixParams();
    const auto& a = plan.p;
    auto near = [] (float x, float y, float tol) { return std::abs (x - y) <= tol; };
    switch (s)
    {
        case secEq:
        {
            if (c.eqOn != a.eqOn) return true;
            for (int b = 0; b < mst::kNumBands; ++b) if (! near (c.eq[(size_t) b], a.eq[(size_t) b], 0.05f)) return true;
            return false;
        }
        case secDyn: return c.dynOn != a.dynOn || ! near (c.glue, a.glue, 0.006f) || ! near (c.leveler, a.leveler, 0.006f);
        case secStereo: return c.stereoOn != a.stereoOn || ! near (c.width, a.width, 0.006f) || c.bassMono != a.bassMono;
        case secHum: return c.humOn != a.humOn || c.humHz != a.humHz;
        case secLoud: return c.loudOn != a.loudOn || ! near (c.gainDb, a.gainDb, 0.05f) || ! near (c.ceilingDb, a.ceilingDb, 0.01f);
    }
    return false;
}

juce::AudioProcessorValueTreeState::ParameterLayout MasterProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "listen", 1 }, "Listen", true));

    // ---- FIX IT: set by AUTO FIX, every one can be overridden
    auto dbAttr = AudioParameterFloatAttributes().withLabel ("dB")
        .withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; });
    auto pctAttr = AudioParameterFloatAttributes().withLabel ("%")
        .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + "%"; });
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "fixOn", 1 }, "Fix On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "Fix Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "match", 1 }, "Match Levels (A/B)", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "eqOn", 1 }, "Tone EQ On", false));
    for (int b = 0; b < mst::kNumBands; ++b)
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "eq" + String (b), 1 }, "EQ " + String (mst::bandDefs()[(size_t) b].label).toLowerCase(),
                                                             NormalisableRange<float> (-8.0f, 8.0f, 0.1f), 0.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "dynOn", 1 }, "Dynamics On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "glue", 1 }, "Glue", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "leveler", 1 }, "Leveler", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "stereoOn", 1 }, "Stereo On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Width", NormalisableRange<float> (50.0f, 160.0f, 0.1f), 100.0f, pctAttr));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "bassMono", 1 }, "Bass Mono", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "humOn", 1 }, "Hum Remover On", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "humHz", 1 }, "Hum Frequency", StringArray { "50 Hz", "60 Hz" }, 1));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "loudOn", 1 }, "Loudness On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Loudness Gain", NormalisableRange<float> (-6.0f, 12.0f, 0.1f), 0.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "ceiling", 1 }, "Ceiling", StringArray { "-1.0 dBTP", "-2.0 dBTP", "-0.3 dBTP" }, 0));
    return { p.begin(), p.end() };
}

bool MasterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    const auto in = layouts.getMainInputChannelSet();
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

void MasterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    maxBlock = juce::jmax (64, samplesPerBlock);
    worker.configure (sampleRate);
    postWorker.configure (sampleRate);
    chain.prepare (sampleRate, maxBlock);
    monoR.assign ((size_t) maxBlock * 4, 0.0f);
    setLatencySamples (chain.latency());
}

void MasterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int inCh = getTotalNumInputChannels();
    if (inCh == 1 && buffer.getNumChannels() > 1) buffer.copyFrom (1, 0, buffer, 0, 0, n);   // mono in -> stereo out
    for (int c = juce::jmax (inCh, 2); c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    if (n == 0 || buffer.getNumChannels() == 0) return;

    const bool stereo = buffer.getNumChannels() > 1;
    float* l = buffer.getWritePointer (0);
    float* r = stereo ? buffer.getWritePointer (1) : nullptr;
    if (! stereo)
    {
        if ((int) monoR.size() < n) monoR.resize ((size_t) n);
        std::copy (l, l + n, monoR.data());
        r = monoR.data();
    }
    inPeak.store (std::max (inPeak.load(), std::max (buffer.getMagnitude (0, 0, n), stereo ? buffer.getMagnitude (1, 0, n) : 0.0f)));
    const bool listen = isListening(), offline = isNonRealtime();
    if (listen) worker.push (l, r, n, offline);

    chain.setParams (currentFixParams());
    for (int start = 0; start < n; start += maxBlock)
    {
        const int len = juce::jmin (maxBlock, n - start);
        chain.process (l + start, r + start, len, [&] (const float* pl, const float* pr, int m)
        {
            if (listen) postWorker.push (pl, pr, m, offline);
        });
    }
}

void MasterProcessor::resetAnalysis()
{
    worker.requestReset();
    postWorker.requestReset();
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
    if (plan.valid) state.setProperty ("autoFix", juce::JSON::toString (plan.toVar(), true), nullptr);
    else state.removeProperty ("autoFix", nullptr);
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
            const auto af = state.getProperty ("autoFix", "").toString();
            plan = af.isNotEmpty() ? mst::FixPlan::fromVar (juce::JSON::parse (af)) : mst::FixPlan {};
            state.removeProperty ("analysis", nullptr);
            state.removeProperty ("autoFix", nullptr);
            apvts.replaceState (state);
        }
}

juce::AudioProcessorEditor* MasterProcessor::createEditor() { return new MasterEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MasterProcessor(); }
