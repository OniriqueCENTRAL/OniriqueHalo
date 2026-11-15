#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "Presets.h"
#if HALO_LICENCE
 #include "License/LicenseGuard.h"
#endif

//==============================================================================
//  ONIRIQUE HALO : le processeur. Bus principal mono ou stéréo, sidechain stéréo facultative (EXT). La latence déclarée
//  est celle que le moteur a mesurée pour la chaîne en service ; quand elle change (qualité, suréchantillonnage), la
//  minuterie la redéclare depuis le thread des messages.
//==============================================================================
class HaloProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    HaloProcessor();
    ~HaloProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter (params::bypass); }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ONIRIQUE HALO"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    halo::EngineParams readParams() const { return reader.read (isNonRealtime()); }
    halo::Meters& meters() { return engine.meters; }
    const halo::Scope& scope() const { return engine.scope; }
    /** latence mesurée d'une chaîne (pour l'interface), en ms */
    double latencyMs (int quality, int os) const
    {
        const double fs = getSampleRate() > 0 ? getSampleRate() : 48000.0;
        return 1000.0 * engine.latencyFor (quality, os) / fs;
    }
    int currentLatency() const { return engine.getLatencyShared(); }
    bool sidechainConnected() const { return scConnected.load(); }

    /** Solo momentané (clic droit sur l'analyseur) : −1 rien, sinon le solo MAIN 1 … 4, ou 5 pour PARALIMIT. */
    std::atomic<int> momentarySolo { -1 };

    juce::UndoManager undo { 30000, 60 };
    juce::AudioProcessorValueTreeState apvts;
    onirique::PresetBank presets { apvts, "HALO", halopresets::stockPresets(), halopresets::tagNames(), params::notStored() };
#if HALO_LICENCE
    onirique::license::Guard license { "onirique-halo" };
#endif

private:
    void process (juce::AudioBuffer<float>&, bool bypassed);
    void timerCallback() override;

    params::Reader reader { apvts };
    halo::Engine engine;
    std::atomic<bool> prepared { false }, scConnected { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HaloProcessor)
};
