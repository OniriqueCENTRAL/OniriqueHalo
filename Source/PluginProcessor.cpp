#include "PluginProcessor.h"
#include "PluginEditor.h"
#if ONIRIQUE_UI_TEST
 #include <juce_audio_formats/juce_audio_formats.h>
#endif

HaloProcessor::HaloProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, &undo, "HALO", params::layout())
{
    apvts.state.setProperty ("preset", "INIT", nullptr);   // l'état de départ : les valeurs par défaut
    startTimerHz (25);
}

HaloProcessor::~HaloProcessor() { stopTimer(); }

bool HaloProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out || (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

void HaloProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
#if HALO_LICENCE
    license.refresh();
#endif
    // la chaîne du rendu hors ligne si l'hôte l'annonce déjà : pas de changement (ni de fondu) au début du rendu
    engine.prepare (sampleRate, samplesPerBlock, reader.read (isNonRealtime()));
    setLatencySamples (engine.getLatency());
    prepared.store (true);
   #if ONIRIQUE_UI_TEST
    primeDemo();
   #endif
}

#if ONIRIQUE_UI_TEST
// Build de démo (jamais publié) : ONIRIQUE_DEMO_WAV=prise → quelques secondes passent une fois dans le vrai moteur au
// prepare, pour que l'analyseur et les indicateurs de la démo du site montrent un vrai traitement.
void HaloProcessor::primeDemo()
{
    const auto path = juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_DEMO_WAV", {});
    if (path.isEmpty()) return;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (path)));
    if (r == nullptr) return;
    const juce::int64 total = r->lengthInSamples;
    const int n = (int) std::min<juce::int64> (total, (juce::int64) (r->sampleRate * 6.0));
    const juce::int64 start = std::max<juce::int64> (0, std::min<juce::int64> (total / 3, total - n));
    juce::AudioBuffer<float> b (2, n);
    r->read (&b, 0, n, start, true, true);
    const auto p = reader.read (false);
    for (int s = 0; s < n; s += 512)
    {
        juce::AudioBuffer<float> part (b.getArrayOfWritePointers(), 2, s, std::min (512, n - s));
        engine.process (part, nullptr, 0, p);
    }
}
#endif

void HaloProcessor::timerCallback()
{
    if (! prepared.load()) return;
    // FIR à phase linéaire : nouvelles réponses quand les drapeaux bougent (calculées ici, jamais dans l'audio)
    const auto p = reader.read (isNonRealtime());
    engine.updateFir (halo::BandEdges::make (p.mainLo, p.mainHi, p.mainMode), halo::BandEdges::make (p.parLo, p.parHi, p.parMode));
    // la latence de la chaîne en service (mesurée), redéclarée quand elle change
    const int lat = engine.getLatencyShared();
    if (lat != getLatencySamples()) setLatencySamples (lat);
}

void HaloProcessor::process (juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    const int n = buffer.getNumSamples();
    for (int c = getMainBusNumInputChannels(); c < main.getNumChannels(); ++c) main.clear (c, 0, n);
    // un hôte envoie parfois des NaN / inf : ils ne doivent jamais entrer dans les mémoires des filtres
    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        auto* s = buffer.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (s[i])) s[i] = 0.0f;
    }

    auto p = reader.read (isNonRealtime());
    if (bypassed) p.bypass = true;
    const int m = momentarySolo.load();
    if (m >= 1 && m <= 4) p.mainSolo = m;
    if (m == 5) p.parSolo = true;

    // la sidechain (EXT) : seulement si l'hôte l'a branchée
    const float* sc[2] { nullptr, nullptr };
    int scCh = 0;
    if (getBusCount (true) > 1)
    {
        auto* bus = getBus (true, 1);
        if (bus != nullptr && bus->isEnabled())
        {
            auto side = getBusBuffer (buffer, true, 1);
            scCh = std::min (2, side.getNumChannels());
            for (int c = 0; c < scCh; ++c) sc[c] = side.getReadPointer (c);
        }
    }
    scConnected.store (scCh > 0);
    engine.process (main, scCh > 0 ? sc : nullptr, scCh, p);
#if HALO_LICENCE
    license.process (main, getSampleRate(), isNonRealtime());
#endif
}

void HaloProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, false); }
void HaloProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { process (buffer, true); }

void HaloProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    presets.writeState (state);   // le côté A/B écouté, et l'autre
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void HaloProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto state = juce::ValueTree::fromXml (*xml);
    presets.readState (state);
    apvts.replaceState (state);
    undo.clearUndoHistory();
}

juce::AudioProcessorEditor* HaloProcessor::createEditor() { return new HaloEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HaloProcessor(); }
