#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"
#include <set>

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
    songs.setFactory ({ library.genres().begin(), library.genres().begin() + library.numBuiltIn() });
    songs.start();
    for (auto& id : fixParamIds()) apvts.addParameterListener (id, this);
    link = std::make_unique<cdlink::Endpoint> ("master", [this] (const juce::var& m) { onLinkMessage (m); });
    linkTimer.fn = [this] { linkTick(); };
    linkTimer.startTimerHz (4);
    link->send (cdlink::Endpoint::makeMessage ("hello"));
}

MasterProcessor::~MasterProcessor()
{
    linkTimer.stopTimer();
    finishTimer.stopTimer();
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

static const float kCeilings[] = { -1.0f, -2.0f, -0.3f, -6.0f };

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
    p.loudOn = v ("loudOn") > 0.5f; p.gainDb = v ("gain"); p.ceilingDb = kCeilings[juce::jlimit (0, 3, (int) v ("ceiling"))];
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
            int ci = 0; for (int i = 0; i < 4; ++i) if (std::abs (kCeilings[i] - f.ceilingDb) < 0.01f) ci = i;
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
    plan = mst::planFixes (in, &after, cur.gainDb, cur.on && cur.loudOn && after.valid, genres[(size_t) target.index], headroomMode());
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
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "ceiling", 1 }, "Ceiling", StringArray { "-1.0 dBTP", "-2.0 dBTP", "-0.3 dBTP", "-6.0 dBTP" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "headroom", 1 }, "Output Target",
                                                        StringArray { "-6 dB (headroom for mastering)", "-1 dB (finished master)" }, 0));
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
    double inPow = 0;
    for (int i = 0; i < n; ++i) inPow += 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
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
    double outPow = 0;
    for (int i = 0; i < n; ++i) outPow += 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
    env.addPowers (inPow / juce::jmax (1, n), outPow / juce::jmax (1, n), n);
}

void MasterProcessor::resetAnalysis()
{
    worker.requestReset();
    postWorker.requestReset();
    myHist.clear();
    peerHist.clear();
    if (link) link->send (cdlink::Endpoint::makeMessage ("measure-reset"));   // linked Servants restart their track measurement too
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
    int chosen = genreChoice == "auto" ? -1 : library.indexOf (genreChoice);
    // AUTO with a prescription in place: keep judging against the genre it aimed at. (Heard through the
    // prescribed plugins, the mix can sound like a different genre, and the score would jump around.)
    if (chosen < 0 && chainPlan.valid && chainStage >= 1)
        for (int i = 0; i < (int) genres.size(); ++i)
            if (genres[(size_t) i].name == chainPlan.genreName) { chosen = i; break; }
    if (chosen >= 0) { t.index = chosen; t.automatic = genreChoice == "auto"; }
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
    state.setProperty ("pluginMode", pluginMode, nullptr);
    state.setProperty ("uiVersion", 2, nullptr);
    if (library.isCustom (genreChoice) && library.indexOf (genreChoice) >= 0)   // your own profile travels with the project
        state.setProperty ("customProfile", juce::JSON::toString (library.genres()[(size_t) library.indexOf (genreChoice)].toVar(), true), nullptr);
    else state.removeProperty ("customProfile", nullptr);
    state.setProperty ("chainStage", chainStage, nullptr);
    if (chainPlan.valid) state.setProperty ("chainPlan", juce::JSON::toString (chainPlan.toVar(), true), nullptr);
    else state.removeProperty ("chainPlan", nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void MasterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            genreChoice = state.getProperty ("genre", "auto").toString();
            {
                mst::GenreProfile g; juce::String err;
                const auto cj = state.getProperty ("customProfile", "").toString();
                if (cj.isNotEmpty() && mst::GenreProfile::fromVar (juce::JSON::parse (cj), g, err)) library.addSessionCustom (g);
                state.removeProperty ("customProfile", nullptr);
            }
            const auto a = state.getProperty ("analysis", "").toString();
            savedFeatures = a.isNotEmpty() ? mst::Features::fromVar (juce::JSON::parse (a)) : mst::Features {};
            const auto af = state.getProperty ("autoFix", "").toString();
            plan = af.isNotEmpty() ? mst::FixPlan::fromVar (juce::JSON::parse (af)) : mst::FixPlan {};
            // from 1.5 MY PLUGINS comes first; older projects keep BUILT-IN FIXES only if they really used AUTO FIX
            pluginMode = (int) state.getProperty ("uiVersion", 1) >= 2 ? (bool) state.getProperty ("pluginMode", true) : ! plan.valid || (bool) state.getProperty ("pluginMode", false);
            chainStage = state.getProperty ("chainStage", 0);
            const auto cp = state.getProperty ("chainPlan", "").toString();
            chainPlan = cp.isNotEmpty() ? mst::ChainPlan::fromVar (juce::JSON::parse (cp)) : mst::ChainPlan {};
            if (! chainPlan.valid) chainStage = 0;
            chainLog.clear();
            for (auto& c : chainPlan.changes)
                chainLog.push_back ({ mst::plugDisplayName (c.plugin), c.label, c.why, chainStage >= 2 });
            for (auto& t : chainPlan.tracks)
                chainLog.push_back ({ mst::plugDisplayName (t.kind), t.name + " (" + t.role + ")  " + t.summary(), t.why.joinIntoString ("\n"), chainStage >= 2, t.id });
            state.removeProperty ("analysis", nullptr);
            state.removeProperty ("autoFix", nullptr);
            state.removeProperty ("chainPlan", nullptr);
            state.removeProperty ("uiVersion", nullptr);
            const bool hadHeadroom = state.getChildWithProperty ("id", "headroom").isValid();
            apvts.replaceState (state);
            if (! hadHeadroom) setParam ("headroom", 1.0f);   // projects from before 1.7 were made for a finished -1 dB master
        }
}

juce::AudioProcessorEditor* MasterProcessor::createEditor() { return new MasterEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MasterProcessor(); }

//==============================================================================
// Castle Danger Link
void MasterProcessor::onLinkMessage (const juce::var& m)
{
    const auto type = m.getProperty ("type", "").toString();
    const auto from = m.getProperty ("from", "").toString();
    const auto kind = m.getProperty ("kind", "").toString();
    if (kind == "master") return;
    if (type == "bye") { peers.erase (from); peerHist.erase (from); ++linkVersion; return; }
    if (type == "status" || type == "applied")
    {
        auto& p = peers[from];
        const bool isNew = p.id.isEmpty();
        p.id = from; p.kind = kind;
        if (type == "status")
        {
            p.name = m.getProperty ("name", kind).toString();
            p.version = m.getProperty ("version", "").toString();
            p.env = m.getProperty ("env", {});
            p.extra = m.getProperty ("extra", {});
            if (kind == "servant") mergeEnv (peerHist[from], p.env, "out");
        }
        p.params = m.getProperty ("params", p.params);
        p.seen = juce::Time::getMillisecondCounter();
        if (type == "applied")
            for (auto& e : chainLog)
                if (e.peerId.isNotEmpty() ? e.peerId == from : e.plugin == mst::plugDisplayName (kind)) e.confirmed = true;
        if (isNew || type == "applied") ++linkVersion;
    }
}

void MasterProcessor::refreshSongProfiles()
{
    if (songs.version.load() == songsVersionSeen) return;
    songsVersionSeen = songs.version.load();
    library.setCustom (songs.getProfiles());
}

bool MasterProcessor::createSongProfile (const juce::String& rawName, juce::File& folderOut)
{
    const auto name = juce::File::createLegalFileName (rawName.trim());
    if (name.isEmpty()) return false;
    mst::SongFolders::ensureRoot();
    folderOut = mst::SongFolders::root().getChildFile (name);
    return folderOut.isDirectory() || folderOut.createDirectory().wasOk();
}

void MasterProcessor::linkTick()
{
    refreshSongProfiles();
    const auto now = juce::Time::getMillisecondCounter();
    for (auto it = peers.begin(); it != peers.end();)
        if (now - it->second.seen > 3000) { it = peers.erase (it); ++linkVersion; }
        else ++it;
    auto m = cdlink::Endpoint::makeMessage ("status");
    m->setProperty ("name", "The Master");
    const auto e = env.toVar();
    m->setProperty ("env", e);
    link->send (m);
    if (isListening()) mergeEnv (myHist, e, "in");
}

void MasterProcessor::mergeEnv (std::map<juce::int64, float>& hist, const juce::var& env, const char* key)
{
    const auto* a = env.getProperty (key, {}).getArray();
    if (a == nullptr) return;
    const auto first = (juce::int64) (double) env.getProperty ("slot", 0.0);
    for (int i = 0; i < a->size(); ++i)
    {
        const double db = (*a)[i];
        if (db <= -99.0) continue;
        hist[first + i] = (float) std::pow (10.0, db / 10.0);
    }
    while (hist.size() > 12000) hist.erase (hist.begin());   // keep the last 20 minutes
}

std::vector<MasterProcessor::TrackStatus> MasterProcessor::trackStatus() const
{
    std::vector<TrackStatus> out;
    std::set<juce::String> seen;
    for (auto& t : chainPlan.tracks)
    {
        TrackStatus s;
        s.peerId = t.id; s.kind = t.kind; s.name = t.name; s.role = t.role; s.share = t.share; s.summary = t.summary(); s.planned = true;
        s.linked = peers.count (t.id) > 0;
        if (s.linked && t.kind == "servant") { const auto& ex = peers.at (t.id).extra; s.role = ex.getProperty ("role", s.role).toString(); }
        seen.insert (t.id);
        out.push_back (s);
    }
    for (auto& [id, p] : peers)
    {
        if (! isTrackPeer (id) || seen.count (id)) continue;
        TrackStatus s;
        s.peerId = id; s.kind = p.kind; s.linked = true;
        s.name = p.extra.getProperty ("track", "").toString();
        if (s.name.isEmpty()) s.name = "(track name not given by the DAW)";
        s.role = p.kind == "servant" ? p.extra.getProperty ("role", "Other").toString() : mst::plugDisplayName (p.kind).toUpperCase();
        s.summary = chainStage >= 1 ? "linked: press UPDATE to include it" : "linked";
        out.push_back (s);
    }
    return out;
}

std::vector<MasterProcessor::Peer> MasterProcessor::getPeers() const
{
    std::vector<Peer> v;
    for (auto& [id, p] : peers) v.push_back (p);
    return v;
}

std::vector<juce::String> MasterProcessor::detectChain() const
{
    std::vector<juce::String> order;
    const auto myEnv = env.toVar();
    juce::var cur = myEnv;
    juce::String curKey = "in";
    std::set<juce::String> used;
    for (int depth = 0; depth < 8; ++depth)
    {
        juce::String best; float bestD = 0.6f;
        for (auto& [id, p] : peers)
        {
            if (used.count (id) || ! p.env.isObject() || isTrackPeer (id)) continue;   // Servants, BuzzKill, Flatliner sit on tracks, not on the bus
            int shared = 0;
            const float d = cdlink::envDistance (p.env, "out", cur, curKey, shared);
            if (shared >= 12 && d < bestD) { bestD = d; best = id; }
        }
        if (best.isEmpty()) break;
        used.insert (best);
        order.insert (order.begin(), best);
        cur = peers.at (best).env;
        curKey = "in";
    }
    return order;
}

bool MasterProcessor::chainOrderKnown() const
{
    const auto e = env.toVar();
    int loud = 0;
    if (auto* a = e.getProperty ("in", {}).getArray()) for (auto& v : *a) if ((double) v > -70.0) ++loud;
    return loud >= 12;
}

bool MasterProcessor::isTrackPeer (const juce::String& id) const
{
    const auto it = peers.find (id);
    if (it == peers.end()) return false;
    const auto& p = it->second;
    if (isTrackKind (p.kind)) return true;
    if (p.kind != "chopshop" && p.kind != "dominatrix") return false;
    const auto track = p.extra.getProperty ("track", "").toString();
    if (track.isEmpty()) return false;
    for (auto& [sid, sp] : peers)
        if (sp.kind == "servant" && sp.extra.getProperty ("track", "").toString().equalsIgnoreCase (track)) return true;
    return false;
}

juce::String MasterProcessor::resolvePeer (const juce::String& kind) const
{
    const auto order = detectChain();
    for (auto& id : order) if (peers.at (id).kind == kind) return id;
    juce::String only; int count = 0;
    for (auto& [id, p] : peers) if (p.kind == kind && ! isTrackPeer (id)) { only = id; ++count; }
    return count == 1 ? only : juce::String();
}

std::vector<MasterProcessor::StepStatus> MasterProcessor::chainStatus() const
{
    std::vector<StepStatus> out;
    if (! chainPlan.valid) return out;
    const auto order = detectChain();
    int lastPos = -1;
    for (auto& st : chainPlan.steps)
    {
        StepStatus s;
        s.kind = st.kind; s.name = st.name; s.role = st.role;
        s.peerId = resolvePeer (st.kind);
        s.linked = s.peerId.isNotEmpty();
        for (int i = 0; i < (int) order.size(); ++i) if (order[(size_t) i] == s.peerId) s.position = i;
        s.orderOk = s.position >= 0 && s.position > lastPos;
        if (s.position >= 0) lastPos = s.position;
        out.push_back (s);
    }
    return out;
}

void MasterProcessor::sendSet (const juce::String& peerId, const std::map<juce::String, float>& params, const juce::String& note)
{
    auto m = cdlink::Endpoint::makeMessage ("set");
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : params) o->setProperty (k, (double) v);
    m->setProperty ("params", juce::var (o));
    m->setProperty ("note", note);
    link->send (m, peerId);
}

void MasterProcessor::startOver()
{
    // pass-through settings: nothing these plugins do changes the level or tone while The Master listens again
    static const std::map<juce::String, std::map<juce::String, float>> neutral {
        { "chopshop",   { { "inputDb", 0.0f }, { "outputDb", 0.0f }, { "mix", 0.0f }, { "color", 0.0f }, { "gainMatch", 0.0f } } },
        { "dominatrix", { { "discipline", 0.0f }, { "saturation", 0.0f }, { "analog", 0.0f }, { "width", 100.0f }, { "bassMono", 0.0f },
                          { "outputDb", 0.0f }, { "ceiling", 2.0f }, { "match", 0.0f }, { "mix", 100.0f } } },
        { "freak",      { { "amount", 0.0f }, { "output", 0.0f } } },
        { "servant",    { { "toneOn", 0.0f }, { "compOn", 0.0f }, { "transOn", 0.0f }, { "deessOn", 0.0f }, { "trim", 0.0f } } },
        { "lowblow",    { { "depth", 0.0f } } },
        { "flatliner",  { { "ride", 0.0f }, { "output", 0.0f } } } };
    if (link)
        for (auto& [id, p] : peers)
            if (auto it = neutral.find (p.kind); it != neutral.end())
                sendSet (id, it->second, "The Master: START OVER (back to neutral)");
    setParam ("fixOn", 0.0f);
    chainPlan = {};
    chainStage = 0;
    chainLog.clear();
    resetAnalysis();
    ++linkVersion;
}

juce::String MasterProcessor::prescribe (bool fromFrozen)
{
    bool saved = false;
    auto in = fromFrozen && chainPlan.valid ? chainPlan.before : getDisplayFeatures (saved);
    if (! in.valid) return "Play the whole song first (with no plugins on the master bus before The Master), then press PRESCRIBE.";
    if (! fromFrozen)
    {
        // a fresh prescription has to hear the mix itself, not the mix through an earlier prescription
        bool tainted = false;
        for (auto& [id, p] : peers)
        {
            if (isTrackPeer (id)) continue;
            if (p.kind == "dominatrix" && std::abs ((double) p.params.getProperty ("outputDb", 0.0)) > 1.0) tainted = true;
            if (p.kind == "chopshop" && (double) p.extra.getProperty ("seconds", 0.0) >= 5.0 && (double) p.params.getProperty ("mix", 0.0) > 0.0
                && (double) p.extra.getProperty ("songPeakDb", -100.0) > in.samplePeak + 2.0) tainted = true;
        }
        if (tainted)
        {
            startOver();
            return "The plugins on the master bus were still set from an earlier prescription, so The Master put every Mix System plugin back to neutral. "
                   "Play the song again from the top, then press GO TO FIX.";
        }
    }
    const auto target = resolveTarget (in);
    const auto& genres = library.genres();
    if (target.index < 0 || target.index >= (int) genres.size()) return "No genre profile to aim for.";
    chainPlan = mst::planChain (in, genres[(size_t) target.index], headroomMode());

    // The Servants on the tracks: tone, compression and balance shared out to the tracks that cause them
    std::vector<mst::TrackInfo> tracks;
    std::vector<juce::int64> slots;
    for (auto& [slot, v] : myHist) if (v > 1e-9f) slots.push_back (slot);
    std::vector<float> mixSeries;
    std::vector<juce::String> servantIds;
    for (auto& [id, p] : peers)
    {
        if (p.kind != "servant") continue;
        const auto* bp = p.extra.getProperty ("bandPow", {}).getArray();
        if (bp == nullptr || bp->size() != mst::kNumBands) continue;
        mst::TrackInfo t;
        t.id = id;
        t.name = p.extra.getProperty ("track", "").toString();
        t.role = p.extra.getProperty ("role", "Other").toString();
        for (int b = 0; b < mst::kNumBands; ++b) t.bandPow[(size_t) b] = (double) (*bp)[b];
        t.pow = p.extra.getProperty ("pow", 0.0);
        t.seconds = p.extra.getProperty ("seconds", 0.0);
        t.lpf = (float) (double) p.params.getProperty ("lpf", 20000.0);
        if (auto sib = p.extra.getProperty ("sib", {}); sib.isObject())
        {
            t.sibP50 = (float) (double) sib.getProperty ("p50", -45.0); t.sibP95 = (float) (double) sib.getProperty ("p95", -45.0);
            t.sibPerMin = (float) (double) sib.getProperty ("perMin", 0.0); t.sibSeconds = (float) (double) sib.getProperty ("seconds", 0.0);
            t.sibHz = (float) (double) sib.getProperty ("hz", 6500.0);
        }
        if (auto tr = p.extra.getProperty ("trans", {}); tr.isObject())
        {
            t.transP95 = (float) (double) tr.getProperty ("p95", 0.0);
            t.transSeconds = (float) (double) tr.getProperty ("seconds", 0.0);
        }
        if (t.seconds < 1.0) continue;   // silent track (or nothing measured yet)
        tracks.push_back (t);
    }
    // series on the slots everyone has
    std::vector<juce::int64> common;
    for (auto s : slots)
    {
        bool ok = true;
        for (auto& t : tracks) { auto it = peerHist.find (t.id); if (it == peerHist.end() || it->second.count (s) == 0) { ok = false; break; } }
        if (ok) common.push_back (s);
    }
    for (auto s : common) mixSeries.push_back (myHist.at (s));
    for (auto& t : tracks) for (auto s : common) t.series.push_back (peerHist.at (t.id).at (s));
    // BuzzKill and Flatliner on the tracks: which track each one is on (its Servant), what it heard
    std::vector<mst::HelperInfo> helpers;
    std::set<juce::String> ridden;
    for (auto& [id, p] : peers)
    {
        if (p.kind != "buzzkill" && p.kind != "flatliner" && p.kind != "lowblow" && ! ((p.kind == "chopshop" || p.kind == "dominatrix") && isTrackPeer (id))) continue;
        mst::HelperInfo h;
        h.id = id; h.kind = p.kind; h.extra = p.extra;
        h.track = p.extra.getProperty ("track", "").toString();
        const auto sv = servantOnSameTrack (p);
        if (sv.isNotEmpty())
        {
            const auto& sp = peers.at (sv);
            h.role = sp.extra.getProperty ("role", "Other").toString();
            if (h.track.isEmpty()) h.track = sp.extra.getProperty ("track", "").toString();
            if (p.kind == "flatliner") ridden.insert (sv);
        }
        else h.role = p.kind == "flatliner" ? "Lead" : (p.kind == "lowblow" || p.kind == "dominatrix" ? "Bass" : (p.kind == "chopshop" ? "Drums" : "Rhythm"));
        helpers.push_back (h);
    }

    if (! tracks.empty())
    {
        auto tr = mst::planTracks (in, genres[(size_t) target.index], tracks, mixSeries, ridden);
        chainPlan.tracks = tr.tracks;
        chainPlan.tracksNote = tr.note;
        // The Master's own EQ only does what the tracks couldn't
        chainPlan.masterFix.eq = tr.residualEq;
        bool any = false; for (auto v : tr.residualEq) if (std::abs (v) >= 0.5f) any = true;
        chainPlan.masterFix.eqOn = any;
        chainPlan.masterEq = any;
        for (auto it = chainPlan.changes.begin(); it != chainPlan.changes.end();)
            if (it->plugin == "master" && it->param == "eq") it = chainPlan.changes.erase (it); else ++it;
        if (any) chainPlan.changes.push_back ({ "master", "eq", "TONE MATCH EQ", 1.0f, 1.0f, "ON",
                                                "Only what the tracks couldn't fix within 6 dB each.", false });
        if (tr.note.isNotEmpty()) chainPlan.onTracks.add (tr.note);
    }

    const auto& G = genres[(size_t) target.index];
    bool anyBuzz = false, anyFlat = false, anyLowBlow = false;
    const auto clash = mst::findLowClash (tracks);
    for (auto& h : helpers)
    {
        auto trackInfo = [&] () -> const mst::TrackInfo* { for (auto& t : tracks) if (t.name.equalsIgnoreCase (h.track)) return &t; return nullptr; };
        if (h.kind == "chopshop")
        {
            chainPlan.tracks.push_back (mst::planTrackChop (h, trackInfo(), G));
            // the clipper takes the peaks: The Servant on the same track doesn't compress them as well
            for (auto& sv : chainPlan.tracks)
                if (sv.kind == "servant" && sv.name.equalsIgnoreCase (h.track) && sv.compOn)
                {
                    sv.compOn = false; sv.comp = 0; sv.params["compOn"] = 0.0f;
                    for (int w = sv.why.size(); --w >= 0;) if (sv.why[w].startsWith ("COMP")) sv.why.remove (w);
                    sv.why.add ("COMP off: Chop Shop on this track shaves its peaks instead (more punch than compressing them).");
                }
        }
        else if (h.kind == "dominatrix") chainPlan.tracks.push_back (mst::planTrackDom (h, trackInfo(), G));
        else if (h.kind == "lowblow") { chainPlan.tracks.push_back (mst::planLowBlow (h, clash, G)); anyLowBlow = true; }
        else if (h.kind == "buzzkill") { chainPlan.tracks.push_back (mst::planBuzzKill (h, in, G)); anyBuzz = true; }
        else                      { chainPlan.tracks.push_back (mst::planFlatliner (h, G)); anyFlat = true; }
    }
    // the advice for the tracks: what's linked now does the job; ask for what's missing
    for (int i = chainPlan.onTracks.size(); --i >= 0;)
    {
        if (anyBuzz && chainPlan.onTracks[i].startsWith ("BUZZKILL")) { chainPlan.onTracks.remove (i); chainPlan.removeAdds ("buzzkill"); }
        else if (anyFlat && chainPlan.onTracks[i].startsWith ("FLATLINER")) chainPlan.onTracks.remove (i);
    }
    if (anyBuzz) chainPlan.removeAdds ("buzzkill");
    // drum tracks that make the mix's peaks: a clipper on the track beats compressing them
    auto hasOn = [&] (const juce::String& kind, const juce::String& track)
    {
        for (auto& t : chainPlan.tracks) if (t.kind == kind && t.name.equalsIgnoreCase (track)) return true;
        return false;
    };
    for (auto& sv : chainPlan.tracks)
        if (mst::wantsDrumClip (sv) && ! hasOn ("chopshop", sv.name))
            chainPlan.adds.push_back ({ "chopshop", sv.name, "Its hits make the mix's peaks. Chop Shop shaves them at the source with more punch than "
                                        "compressing them (The Servant's compressor does the job until you add it). Put it after The Servant, then press UPDATE." });
    // a bass that's thin on small speakers: Dominatrix's saturation gives it harmonics
    for (auto& t : tracks)
        if (mst::roleIs (t.role, { "Bass" }) && mst::bassHarmonicsDb (t) < mst::kThinBassDb && ! hasOn ("dominatrix", t.name))
            chainPlan.adds.push_back ({ "dominatrix", t.name, "Its 250 Hz - 2 kHz sits " + juce::String (-mst::bassHarmonicsDb (t), 0)
                                        + " dB under its lows, so it all but disappears on phones and small speakers. Dominatrix's saturation adds the harmonics "
                                        "that let it be heard there, without changing its level. Put it after The Servant, then press UPDATE." });
    if (! anyLowBlow && clash.found)
    {
        chainPlan.adds.push_back ({ "lowblow", clash.bassName, "It and " + clash.kickName + " fight in " + juce::String (mst::bandDefs()[(size_t) clash.band].label)
                                    + " (" + mst::bandDefs()[(size_t) clash.band].range + " Hz). Send the kick to Low Blow's sidechain, then press UPDATE." });
        chainPlan.onTracks.add ("LOW BLOW on " + clash.bassName + ": it and " + clash.kickName + " fight "
                                + juce::String (mst::bandDefs()[(size_t) clash.band].label) + " (" + mst::bandDefs()[(size_t) clash.band].range
                                + " Hz), " + juce::String (clash.gapDb, 1) + " dB apart. Low Blow ducks the bass under each kick; send the kick to its sidechain, then press UPDATE.");
    }
    if (! anyFlat)
    {
        // a lead that jumps around: from its Servant's fingerprint
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            if (! mst::roleIs (tracks[i].role, { "Lead" })) continue;
            const float swing = mst::leadSwingDb (tracks[i].series);
            if (swing > 10.0f)
            {
                for (int k = chainPlan.onTracks.size(); --k >= 0;) if (chainPlan.onTracks[k].startsWith ("FLATLINER")) chainPlan.onTracks.remove (k);
                chainPlan.removeAdds ("flatliner");
                chainPlan.adds.push_back ({ "flatliner", tracks[i].name, "Its level swings " + juce::String (swing, 0) + " dB. Put Flatliner first on the track, then press UPDATE." });
                chainPlan.onTracks.insert (0, "FLATLINER on " + (tracks[i].name.isNotEmpty() ? tracks[i].name : juce::String ("the lead"))
                                              + ": its level swings " + juce::String (swing, 0) + " dB. Put it first on that track, then press UPDATE.");
                break;
            }
        }
    }
    chainStage = 1;
    chainLog.clear();
    ++linkVersion;
    link->send (cdlink::Endpoint::makeMessage ("hello"));   // ask everyone to report right away
    return {};
}

juce::String MasterProcessor::applyChain()
{
    if (! chainPlan.valid) return "Press PRESCRIBE first.";
    const auto st = chainStatus();
    juce::StringArray missing;
    for (auto& s : st) if (! s.linked) missing.add (s.name);
    if (! missing.isEmpty())
        return "Put " + missing.joinIntoString (" and ") + " on the master bus (before The Master) first. If more than one copy is open, play a few seconds so The Master can tell which one is on the bus.";

    chainLog.clear();
    for (auto& s : st)
    {
        const auto* step = chainPlan.step (s.kind);
        const auto& peer = peers.at (s.peerId);
        sendSet (s.peerId, step->params, "The Master: " + chainPlan.genreName + " prescription");
        if (! step->notes.isEmpty())
        {
            auto m = cdlink::Endpoint::makeMessage ("freak-notes");   // The Freak shows why each band is there
            juce::Array<juce::var> a;
            for (auto& nt : step->notes) a.add (nt);
            m->setProperty ("notes", a);
            link->send (m, s.peerId);
        }
        for (auto& c : chainPlan.changes)
            if (c.plugin == s.kind)
            {
                c.hasFrom = peer.params.hasProperty (c.param);
                if (c.hasFrom) c.from = (float) (double) peer.params.getProperty (c.param, 0.0);
            }
    }
    // The Servants, BuzzKills and Flatliners on the tracks
    int tracksSet = 0;
    for (auto& t : chainPlan.tracks)
    {
        if (peers.count (t.id) == 0 || t.params.empty()) continue;
        sendSet (t.id, t.params, "The Master: " + chainPlan.genreName + " mix balance");
        ++tracksSet;
    }

    // The Master's own part: tone EQ + safety limiter, nothing else
    const auto& f = chainPlan.masterFix;
    plan = {};
    plan.valid = true; plan.p = f; plan.genreName = chainPlan.genreName; plan.seconds = chainPlan.seconds;
    plan.whyEq = "Part of the MY PLUGINS prescription.";
    for (auto sct : { secEq, secDyn, secStereo, secHum, secLoud }) applySection (sct, f);
    setParam ("amount", 100.0f);
    setParam ("match", 0.0f);
    setParam ("fixOn", 1.0f);

    for (auto& c : chainPlan.changes)
    {
        juce::String title = c.label;
        if (c.plugin != "master")
        {
            const bool choice = c.unit != "dB" && c.unit != "%";   // named settings: MASTER, SOFT, 4x, TAPE, ON, -1 dB...
            auto fmt = [&] (float v) { return choice ? c.unit : juce::String (v, 1) + (c.unit == "%" ? "%" : " " + c.unit); };
            title << "  " << (c.hasFrom && ! choice ? juce::String (c.from, 1) + " > " : juce::String()) << fmt (c.to);
        }
        else title << "  " << c.unit;
        chainLog.push_back ({ mst::plugDisplayName (c.plugin), title, c.why, c.plugin == "master" });
    }
    for (auto& t : chainPlan.tracks)
    {
        const bool there = peers.count (t.id) > 0;
        chainLog.push_back ({ mst::plugDisplayName (t.kind), t.name + " (" + t.role + ")  " + (there ? t.summary() : juce::String ("NOT LINKED, SKIPPED")),
                              t.why.joinIntoString ("\n"), false, t.id });
    }
    chainStage = 2;
    resetAnalysis();
    ++linkVersion;
    return {};
}

void MasterProcessor::outputTargetChanged()
{
    const bool hr = headroomMode();
    const auto& genres = library.genres();
    // MY PLUGINS: rebuild the bus part of the prescription from the analysis it was made from
    if (chainPlan.valid && chainStage >= 1 && chainPlan.headroom != hr)
    {
        const auto target = resolveTarget (chainPlan.before);
        if (target.index >= 0 && target.index < (int) genres.size())
        {
            auto fresh = mst::planChain (chainPlan.before, genres[(size_t) target.index], hr);
            fresh.tracks = chainPlan.tracks; fresh.tracksNote = chainPlan.tracksNote; fresh.onTracks = chainPlan.onTracks; fresh.adds = chainPlan.adds;
            fresh.masterFix.eq = chainPlan.masterFix.eq; fresh.masterFix.eqOn = chainPlan.masterFix.eqOn; fresh.masterEq = chainPlan.masterEq;
            fresh.changes.erase (std::remove_if (fresh.changes.begin(), fresh.changes.end(), [] (const mst::ChainChange& c) { return c.plugin == "master" && c.param == "eq"; }), fresh.changes.end());
            for (auto& c : chainPlan.changes) if (c.plugin == "master" && c.param == "eq") fresh.changes.push_back (c);
            chainPlan = fresh;
            if (chainStage >= 2 && applyChain().isNotEmpty()) chainStage = 1;   // plugins gone: back to "place them"
            ++linkVersion;
        }
    }
    // BUILT-IN FIXES: redo only the loudness + ceiling
    if (plan.valid && isFixOn() && ! (chainPlan.valid && chainStage >= 2))
    {
        mst::Features in;
        worker.getSnapshot (in);
        bool saved = false;
        if (! in.valid) in = getDisplayFeatures (saved);
        const auto target = resolveTarget (in);
        if (in.valid && target.index >= 0 && target.index < (int) genres.size())
        {
            const auto fresh = mst::planFixes (in, nullptr, 0.0f, false, genres[(size_t) target.index], hr);
            plan.p.gainDb = fresh.p.gainDb; plan.p.ceilingDb = fresh.p.ceilingDb; plan.p.loudOn = true; plan.whyLoud = fresh.whyLoud;
            applySection (secLoud, plan.p);
            postWorker.requestReset();
        }
    }
}

juce::String MasterProcessor::refineChain()
{
    if (chainStage < 2) return "APPLY the prescription first.";
    const auto after = getAfterFeatures();
    if (! after.valid || after.seconds < 20.0) return "Play at least 20 seconds (best the whole song) after APPLY, then press REFINE.";
    const auto& genres = library.genres();
    const auto target = resolveTarget (chainPlan.before);
    if (target.index < 0) return "No genre profile.";
    const auto& g = genres[(size_t) target.index];
    std::vector<LogEntry> added;

    const auto dom = resolvePeer ("dominatrix");
    if (dom.isNotEmpty())
    {
        const float cur = (float) (double) peers.at (dom).params.getProperty ("outputDb", 0.0);
        float want = g.stat (mst::fLufs).mean - after.lufs;
        juce::String why = "It came out at " + juce::String (after.lufs, 1) + " LUFS; the target for " + g.name + " is " + juce::String (g.stat (mst::fLufs).mean, 1) + ".";
        if (chainPlan.headroom)
        {
            // -6 dB mode: peaks just under -6 dBFS, and no squashing (the mastering engineer gets the dynamics)
            const float squash = chainPlan.before.plr - after.plr;
            want = 0.0f;
            if (squash > 2.0f) { want = -(squash - 1.0f); why = "The peaks came out " + juce::String (squash, 1) + " dB more squashed than your mix; backing off so the mastering step keeps its dynamics."; }
            else if (after.truePeak < -7.5f) { want = -6.3f - after.truePeak; why = "Peaks came out at " + juce::String (after.truePeak, 1) + " dBTP; bringing them up to just under the -6 dB headroom line."; }
        }
        const float next = juce::jlimit (-12.0f, 12.0f, std::round ((cur + want) * 10.0f) / 10.0f);
        if (std::abs (next - cur) >= 0.2f)
        {
            sendSet (dom, { { "outputDb", next } }, "The Master: refine loudness");
            added.push_back ({ "Dominatrix", "OUTPUT  " + juce::String (cur, 1) + " > " + juce::String (next, 1) + " dB",
                               why });
        }
        if (after.lowCorrelation < 0.9f && (double) peers.at (dom).params.getProperty ("bassMono", 0.0) < 0.5)
        {
            sendSet (dom, { { "bassMono", 1.0f } }, "The Master: refine");
            added.push_back ({ "Dominatrix", "BASS MONO  ON", "The low end still isn't centred after the chain." });
        }
    }
    const auto chop = resolvePeer ("chopshop");
    if (chop.isNotEmpty() && ! chainPlan.headroom && g.z (mst::fPlr, after.plr) > 1.0f)   // with -6 dB headroom, peaks are the mastering step's job
    {
        const float cur = (float) (double) peers.at (chop).params.getProperty ("ceilingDb", 0.0);
        const float next = juce::jmax (-24.0f, cur - 1.0f);
        sendSet (chop, { { "ceilingDb", next } }, "The Master: refine peaks");
        added.push_back ({ "Chop Shop", "CEILING  " + juce::String (cur, 1) + " > " + juce::String (next, 1) + " dB",
                           "Peaks still stick out " + juce::String (after.plr, 1) + " dB over the average after the chain; shaving another dB." });
    }
    if (added.empty()) added.push_back ({ "The Master", "NOTHING TO REFINE", "Loudness, peaks and low end landed where " + g.name + " records sit." });
    for (auto& a : added) chainLog.push_back (a);
    resetAnalysis();
    ++linkVersion;
    return {};
}

//==============================================================================
juce::String MasterProcessor::servantOnSameTrack (const Peer& h) const
{
    const auto track = h.extra.getProperty ("track", "").toString();
    if (track.isNotEmpty())
        for (auto& [id, p] : peers)
            if (p.kind == "servant" && p.extra.getProperty ("track", "").toString().equalsIgnoreCase (track)) return id;
    // no names from the DAW: the Servant whose level fingerprint matches (before or after it on the track)
    juce::String best; float bestD = 0.6f;
    for (auto& [id, p] : peers)
    {
        if (p.kind != "servant" || ! p.env.isObject() || ! h.env.isObject()) continue;
        for (auto [a, ak, b, bk] : { std::tuple<const juce::var*, const char*, const juce::var*, const char*> { &h.env, "out", &p.env, "in" },
                                     { &p.env, "out", &h.env, "in" } })
        {
            int shared = 0;
            const float d = cdlink::envDistance (*a, ak, *b, bk, shared);
            if (shared >= 12 && d < bestD) { bestD = d; best = id; }
        }
    }
    return best;
}

bool MasterProcessor::chainOutdated() const
{
    if (! chainPlan.valid || chainStage < 1) return false;
    std::set<juce::String> planned;
    for (auto& t : chainPlan.tracks) { planned.insert (t.id); if (peers.count (t.id) == 0) return true; }   // one went away
    for (auto& [id, p] : peers)
    {
        if (! isTrackPeer (id) || planned.count (id)) continue;
        // a Servant added later can only be planned once it has heard its track
        if (p.kind == "servant" && (double) p.extra.getProperty ("seconds", 0.0) < 1.0) continue;
        return true;
    }
    return false;
}

void MasterProcessor::requestUpdate (std::function<void (const juce::String&)> done)
{
    const bool wasApplied = chainStage >= 2;
    requestPrescribe ([this, wasApplied, done] (const juce::String& err)
    {
        juce::String e = err;
        if (e.isEmpty() && wasApplied) e = applyChain();   // keep it applied: send the new settings straight away
        if (done) done (e);
    }, true);
}

void MasterProcessor::requestPrescribe (std::function<void (const juce::String&)> done, bool fromFrozen)
{
    finishWaiting.clear();
    for (auto& [id, p] : peers)
        if (p.kind == "servant" && (bool) p.extra.getProperty ("listening", false)) finishWaiting.insert (id);
    if (finishWaiting.empty() || ! link) { const auto err = prescribe (fromFrozen); if (done) done (err); return; }

    ++finishSeq;
    auto m = cdlink::Endpoint::makeMessage ("analyze-finish");   // each Servant decides its resonances + clean-up now
    m->setProperty ("seq", finishSeq);
    link->send (m);
    finishSentMs = juce::Time::getMillisecondCounter();
    pendingPrescribe = std::move (done);
    finishTimer.fn = [this, fromFrozen]
    {
        for (auto it = finishWaiting.begin(); it != finishWaiting.end();)
        {
            auto p = peers.find (*it);
            if (p == peers.end() || (int) p->second.extra.getProperty ("finished", 0) >= finishSeq) it = finishWaiting.erase (it);
            else ++it;
        }
        if (! finishWaiting.empty() && juce::Time::getMillisecondCounter() - finishSentMs < 2500) return;
        finishTimer.stopTimer();
        auto cb = std::move (pendingPrescribe);
        pendingPrescribe = nullptr;
        const auto err = prescribe (fromFrozen);
        if (cb) cb (err);
    };
    finishTimer.startTimer (50);
}
