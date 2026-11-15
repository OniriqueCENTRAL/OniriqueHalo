#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "ui/OniriquePresets.h"

//==============================================================================
//  Presets d'usine de HALO, rangés par usage (VOCALS, INSTRUMENTS, MIX BUS, MASTERING) pour le navigateur de la barre
//  du haut. Des points de départ : un réglage non cité reprend sa valeur par défaut. Les seuils sont des niveaux de
//  crête de la bande (dBFS) : ils se règlent ensuite à l'oreille, l'indicateur de bande sous les yeux.
//==============================================================================
namespace halopresets
{
enum Tags { vocals = 1, instruments = 2, mixbus = 4, mastering = 8, parallel = 16, midSide = 32 };

inline juce::StringArray tagNames() { return { "Vocals", "Instruments", "Mix bus", "Mastering", "Paralimit", "M/S" }; }

inline std::vector<onirique::PresetBank::Stock> stockPresets()
{
    using namespace params;
    // choix : 0 SPLIT, 1 DEFINED, 2 FULL HF | 0 SLOW, 1 FAST | 0 L/R, 1 M/S | canal 0 MID, 1 SIDE, 2 L/R | qualité 0 LINEAR, 1 DYNAMIC, 2 LIVE
    return {
        { "INIT", 0, {} },

        // ---- VOCALS
        { "VOCAL POLISH", vocals,
          { { mainLo, 5000.0f }, { mainMode, 0.0f }, { thrA, -22.0f }, { thrB, -22.0f }, { attack, 0.0f }, { release, 0.0f } } },
        { "SMOOTH PRESENCE", vocals,
          { { mainLo, 2500.0f }, { mainHi, 6000.0f }, { mainMode, 1.0f }, { thrA, -24.0f }, { thrB, -24.0f }, { mainTrim, -1.0f },
            { attack, 0.0f }, { release, 0.0f } } },
        { "DE-ESS & SHINE", vocals | parallel,
          { { mainLo, 5000.0f }, { mainHi, 10000.0f }, { mainMode, 1.0f }, { thrA, -28.0f }, { thrB, -28.0f }, { attack, 1.0f }, { release, 1.0f },
            { parOn, 1.0f }, { parLo, 10000.0f }, { parMode, 0.0f }, { parThr, -30.0f }, { parAmount, -10.0f }, { parChannel, 0.0f } } },
        { "AIR CONTROL", vocals,
          { { mainLo, 10000.0f }, { mainMode, 0.0f }, { thrA, -26.0f }, { thrB, -26.0f }, { mainTrim, 1.0f } } },
        { "BRIGHT VOCAL DENSITY", vocals | parallel,
          { { mainLo, 4500.0f }, { thrA, -20.0f }, { thrB, -20.0f },
            { parOn, 1.0f }, { parLo, 7000.0f }, { parThr, -36.0f }, { parAmount, -8.0f }, { parTrim, 1.0f }, { parChannel, 0.0f } } },
        { "VOCAL TOP END", vocals | parallel,
          { { mainLo, 7000.0f }, { mainMode, 2.0f }, { thrA, -22.0f }, { thrB, -22.0f }, { mainTrim, 1.5f },
            { parOn, 1.0f }, { parLo, 12000.0f }, { parThr, -34.0f }, { parAmount, -12.0f }, { parChannel, 0.0f } } },

        // ---- INSTRUMENTS
        { "SYNTH CLARITY", instruments | parallel,
          { { mainLo, 3000.0f }, { mainHi, 9000.0f }, { mainMode, 1.0f }, { thrA, -20.0f }, { thrB, -20.0f }, { attack, 1.0f }, { release, 0.0f },
            { parOn, 1.0f }, { parLo, 8000.0f }, { parThr, -32.0f }, { parAmount, -12.0f }, { parChannel, 2.0f } } },
        { "GUITAR SMOOTHNESS", instruments,
          { { mainLo, 2500.0f }, { mainHi, 7000.0f }, { mainMode, 1.0f }, { thrA, -22.0f }, { thrB, -22.0f }, { mainTrim, -1.0f } } },
        { "DRUM BRIGHTNESS", instruments | parallel,
          { { mainLo, 6000.0f }, { thrA, -18.0f }, { thrB, -18.0f }, { attack, 1.0f }, { release, 1.0f },
            { parOn, 1.0f }, { parLo, 8000.0f }, { parThr, -34.0f }, { parAmount, -6.0f }, { parChannel, 2.0f } } },
        { "PIANO POLISH", instruments,
          { { mainLo, 4000.0f }, { mainMode, 2.0f }, { thrA, -24.0f }, { thrB, -24.0f }, { quality, 0.0f } } },
        { "HI-HAT CONTROL", instruments,
          { { mainLo, 7000.0f }, { thrA, -26.0f }, { thrB, -26.0f }, { mainTrim, -1.5f }, { attack, 1.0f }, { release, 1.0f } } },

        // ---- MIX BUS
        { "GENTLE MASTER POLISH", mixbus,
          { { mainLo, 6000.0f }, { thrA, -16.0f }, { thrB, -16.0f }, { quality, 0.0f } } },
        { "MODERN HIGH END", mixbus | parallel,
          { { mainLo, 5000.0f }, { thrA, -18.0f }, { thrB, -18.0f }, { mainTrim, 1.0f },
            { parOn, 1.0f }, { parLo, 9000.0f }, { parThr, -36.0f }, { parAmount, -10.0f }, { parChannel, 2.0f } } },
        { "SMOOTH TOP", mixbus,
          { { mainLo, 3500.0f }, { mainMode, 2.0f }, { thrA, -20.0f }, { thrB, -20.0f }, { mainTrim, -1.0f } } },
        { "OPEN & CONTROLLED", mixbus | parallel | midSide,
          { { mainLo, 4500.0f }, { stereo, 1.0f }, { thrLink, 0.0f }, { thrA, -16.0f }, { thrB, -22.0f }, { mainLink, 50.0f },
            { parOn, 1.0f }, { parLo, 8000.0f }, { parThr, -34.0f }, { parAmount, -12.0f }, { parChannel, 1.0f } } },
        { "ANALOG INSPIRED", mixbus | parallel,
          { { mainLo, 3000.0f }, { mainMode, 2.0f }, { thrA, -18.0f }, { thrB, -18.0f }, { mainTrim, -0.5f }, { mix, 80.0f },
            { parOn, 1.0f }, { parLo, 6000.0f }, { parThr, -32.0f }, { parAmount, -14.0f }, { parChannel, 0.0f } } },

        // ---- MASTERING
        { "TRANSPARENT LIMITING", mastering,
          { { mainLo, 6000.0f }, { thrA, -14.0f }, { thrB, -14.0f }, { quality, 0.0f }, { osOffline, 3.0f } } },
        { "HIGH FREQUENCY CONTROL", mastering,
          { { mainLo, 4000.0f }, { thrA, -18.0f }, { thrB, -18.0f }, { attack, 1.0f }, { release, 0.0f }, { quality, 0.0f } } },
        { "HIGH END DENSITY", mastering | parallel,
          { { mainLo, 5000.0f }, { thrA, -16.0f }, { thrB, -16.0f }, { quality, 0.0f },
            { parOn, 1.0f }, { parLo, 8000.0f }, { parThr, -34.0f }, { parAmount, -10.0f }, { parChannel, 2.0f } } },
        { "SUBTLE STEREO SHINE", mastering | parallel | midSide,
          { { mainLo, 6000.0f }, { stereo, 1.0f }, { thrLink, 0.0f }, { thrA, -14.0f }, { thrB, -18.0f }, { quality, 0.0f },
            { parOn, 1.0f }, { parLo, 9000.0f }, { parThr, -36.0f }, { parAmount, -9.0f }, { parChannel, 1.0f } } },
    };
}
} // namespace halopresets
