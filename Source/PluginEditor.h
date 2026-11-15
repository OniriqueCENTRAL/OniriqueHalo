#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "ui/Controls.h"
#include "ui/Analyser.h"
#include "ui/OniriqueBar.h"
#if HALO_LICENCE
 #include "License/LicenseOverlay.h"
#endif

//==============================================================================
//  L'interface de HALO (1100 × 600 points, de 75 à 200 % sans déformation), dans la DA de VOCALBOX, organisée comme
//  un limiteur d'aigus de studio :
//    barre du haut   presets, A/B, IN (bypass) | annuler, rétablir, EXT (vers MAIN / PARALIMIT), réglages
//    bandeau         balance tonale, drapeaux MAIN et PARALIMIT à glisser, un S (solo) sous chaque drapeau
//    MAIN            fader des seuils (L/M, R/S, liés), CHANNEL, ATTACK, RELEASE, CH LINK, LED de réduction et
//                    écouteur (solo de bande), TRIM, POWER
//    PARALIMIT       fader AMOUNT et son S, canal, seuil, POWER, TRIM, LINK
//    barre d'état    in, os, render, mode (latence mesurée), formes de bande, mix, delta, out, match
//==============================================================================
class HaloEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HaloEditor (HaloProcessor&);
    ~HaloEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int baseWidth = 1100, baseHeight = 600;

private:
    juce::RangedAudioParameter& p (const juce::String& id) { return *processor.apvts.getParameter (id); }
    void renderBackground();
    void paintTitle (juce::Graphics&, juce::Rectangle<float> area) const;
    void timerCallback() override;
    void showSettings();
    void showExt();
    void updateTips();
    float scale() const { return (float) getWidth() / (float) baseWidth; }

    HaloProcessor& processor;
    ui::FontScope fonts;
    ui::KnobLook knobLook;
    juce::Image background, logo;
    juce::TooltipWindow tooltips { this, 700 };

    ui::Analyser analyser;
    // MAIN
    ui::ThresholdFader thresholds;
    ui::Segments channel, attack, release;
    ui::Knob chLink, trim;
    ui::LedLadder ladder;
    ui::LedButton mainPower;
    // PARALIMIT
    ui::AmountFader amount;
    ui::SoloButton parSolo;
    ui::Segments parChannel;
    ui::Knob parThr, parTrim, parLink;
    ui::LedButton parPower;
    // barre d'état
    ui::StatusItem inItem, osItem, renderItem, modeItem, bandItem, paraItem, mixItem, deltaItem, outItem, matchItem;
    ui::MiniMeter inMeter, outMeter;
    // haut
    ui::IconButton undoButton, redoButton, settingsButton;
    ui::ExtLight ext;

    juce::ParameterAttachment parSoloWatch;

    onirique::nuit::Bar bar;
    bool sizeReady = false;
    int themeSeen = -1, tipTicks = 0;

#if HALO_LICENCE
    std::unique_ptr<onirique::license::Overlay> licenseOverlay;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HaloEditor)
};
