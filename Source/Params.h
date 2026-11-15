#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/HaloEngine.h"

//==============================================================================
//  Paramètres de HALO : identifiants stables (ne jamais les renommer, les projets les retrouvent par là).
//==============================================================================
namespace params
{
// MAIN
inline const juce::String mainOn = "mainOn", mainLo = "mainLo", mainHi = "mainHi", mainMode = "mainMode",
                          thrA = "thrA", thrB = "thrB", thrLink = "thrLink", mainTrim = "mainTrim",
                          attack = "attack", release = "release", stereo = "stereo", mainLink = "mainLink",
                          mainSolo = "mainSolo", scMain = "scMain", mix = "mix";
// PARALIMIT
inline const juce::String parOn = "parOn", parLo = "parLo", parHi = "parHi", parMode = "parMode", parThr = "parThr",
                          parAmount = "parAmount", parTrim = "parTrim", parAttack = "parAttack", parRelease = "parRelease",
                          parChannel = "parChannel", parLink = "parLink", parSolo = "parSolo", scPar = "scPar";
// général
inline const juce::String inGain = "inGain", outGain = "outGain", levelComp = "levelComp", delta = "delta",
                          quality = "quality", osRealtime = "osRealtime", osOffline = "osOffline", bypass = "bypass";

inline juce::String dbText (float v, int decimals = 1)
{
    if (std::abs (v) < 0.05f) v = 0.0f;
    return (v > 0.0f ? "+" : "") + juce::String (v, decimals) + " dB";
}

inline juce::String hzText (float v)
{
    if (v >= 1000.0f) return juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz";
    return juce::String (juce::roundToInt (v)) + " Hz";
}

inline float parseNumber (const juce::String& t)
{
    const auto lower = t.toLowerCase();
    float v = lower.retainCharacters ("+-.0123456789").getFloatValue();
    if (lower.contains ("k")) v *= 1000.0f;
    return v;
}

inline juce::AudioProcessorValueTreeState::ParameterLayout layout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto freq = [&] (const String& id, const String& name, float def)
    {
        NormalisableRange<float> r (50.0f, 20000.0f,
                                    [] (float a, float b, float t) { return a * std::pow (b / a, t); },
                                    [] (float a, float b, float v) { return std::log (v / a) / std::log (b / a); },
                                    [] (float, float, float v) { return v; });
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
                   AudioParameterFloatAttributes().withLabel ("Hz")
                       .withStringFromValueFunction ([] (float v, int) { return hzText (v); })
                       .withValueFromStringFunction ([] (const String& t) { return parseNumber (t); })));
    };
    auto db = [&] (const String& id, const String& name, float lo, float hi, float def)
    {
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (lo, hi, 0.01f), def,
                   AudioParameterFloatAttributes().withLabel ("dB")
                       .withStringFromValueFunction ([] (float v, int) { return dbText (v); })
                       .withValueFromStringFunction ([] (const String& t) { return parseNumber (t); })));
    };
    auto pct = [&] (const String& id, const String& name, float def)
    {
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 100.0f, 0.1f), def,
                   AudioParameterFloatAttributes().withLabel ("%")
                       .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })
                       .withValueFromStringFunction ([] (const String& t) { return parseNumber (t); })));
    };
    auto toggle = [&] (const String& id, const String& name, bool def)
    {
        l.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def,
                   AudioParameterBoolAttributes().withStringFromValueFunction ([] (bool b, int) { return b ? String ("ON") : String ("OFF"); })));
    };
    auto choice = [&] (const String& id, const String& name, const StringArray& items, int def)
    {
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, items, def));
    };

   #if ONIRIQUE_UI_TEST
    const bool demo = SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_TEST", {}).contains ("demo");
   #else
    const bool demo = false;
   #endif

    // MAIN
    toggle (mainOn, "Main Power", true);
    freq (mainLo, "Main Low Flag", demo ? 4200.0f : 4000.0f);
    freq (mainHi, "Main High Flag", demo ? 12500.0f : 16000.0f);
    choice (mainMode, "Main Band", { "SPLIT", "DEFINED", "FULL HF" }, 0);
    db (thrA, "Threshold L/M", -40.0f, 0.0f, demo ? -22.0f : -18.0f);
    db (thrB, "Threshold R/S", -40.0f, 0.0f, demo ? -22.0f : -18.0f);
    toggle (thrLink, "Thresholds Linked", true);
    db (mainTrim, "Main Trim", -6.0f, 6.0f, 0.0f);
    choice (attack, "Attack", { "SLOW", "FAST" }, 0);
    choice (release, "Release", { "SLOW", "FAST" }, 0);
    choice (stereo, "Stereo Mode", { "L/R", "M/S" }, 0);
    pct (mainLink, "Main Stereo Link", 100.0f);
    choice (mainSolo, "Main Solo", { "OFF", "BAND", "ABOVE LOW", "ABOVE HIGH", "REST" }, 0);
    toggle (scMain, "Main Sidechain", false);
    pct (mix, "Main Dry/Wet", 100.0f);

    // PARALIMIT
    toggle (parOn, "Paralimit Power", demo);
    freq (parLo, "Paralimit Low Flag", demo ? 7000.0f : 6000.0f);
    freq (parHi, "Paralimit High Flag", 18000.0f);
    choice (parMode, "Paralimit Band", { "SPLIT", "DEFINED", "FULL HF" }, 0);
    db (parThr, "Paralimit Threshold", -60.0f, 0.0f, demo ? -34.0f : -30.0f);
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { parAmount, 1 }, "Paralimit Amount", NormalisableRange<float> (-60.0f, 12.0f, 0.01f, 2.2f),
               demo ? -10.0f : -12.0f,
               AudioParameterFloatAttributes().withLabel ("dB")
                   .withStringFromValueFunction ([] (float v, int) { return v <= -59.9f ? String (CharPointer_UTF8 ("-\xe2\x88\x9e dB")) : dbText (v); })
                   .withValueFromStringFunction ([] (const String& t) { return t.containsIgnoreCase ("inf") || t.contains (CharPointer_UTF8 ("\xe2\x88\x9e")) ? -60.0f : parseNumber (t); })));
    db (parTrim, "Paralimit Trim", -6.0f, 6.0f, 0.0f);
    choice (parAttack, "Paralimit Attack", { "SLOW", "FAST" }, 1);
    choice (parRelease, "Paralimit Release", { "SLOW", "FAST" }, 0);
    choice (parChannel, "Paralimit Channel", { "MID", "SIDE", "L/R" }, 2);
    pct (parLink, "Paralimit Stereo Link", 100.0f);
    toggle (parSolo, "Paralimit Solo", false);
    toggle (scPar, "Paralimit Sidechain", false);

    // général
    db (inGain, "Input", -18.0f, 18.0f, 0.0f);
    db (outGain, "Output", -18.0f, 18.0f, 0.0f);
    toggle (levelComp, "Level Compensation", false);
    toggle (delta, "Delta", false);
    choice (quality, "Processing Mode", { "LINEAR", "DYNAMIC", "LIVE" }, 1);
    choice (osRealtime, "Oversampling", { "1x", "2x", "4x", "8x" }, 1);
    choice (osOffline, "Oversampling Offline", { "1x", "2x", "4x", "8x" }, 2);
    l.add (std::make_unique<AudioParameterBool> (ParameterID { bypass, 1 }, "Bypass", false,
               AudioParameterBoolAttributes().withStringFromValueFunction ([] (bool b, int) { return b ? String ("BYPASSED") : String ("IN"); })));
    return l;
}

/** Lecture des paramètres (thread audio : seulement des chargements atomiques). */
struct Reader
{
    explicit Reader (juce::AudioProcessorValueTreeState& s)
    {
        for (const auto* id : { &mainOn, &mainLo, &mainHi, &mainMode, &thrA, &thrB, &thrLink, &mainTrim, &attack, &release,
                                &stereo, &mainLink, &mainSolo, &scMain, &mix,
                                &parOn, &parLo, &parHi, &parMode, &parThr, &parAmount, &parTrim, &parAttack, &parRelease,
                                &parChannel, &parLink, &parSolo, &scPar,
                                &inGain, &outGain, &levelComp, &delta, &quality, &osRealtime, &osOffline, &bypass })
            v.push_back (s.getRawParameterValue (*id));
    }

    /** offline : le rendu hors ligne (son propre suréchantillonnage) */
    halo::EngineParams read (bool offline) const
    {
        int i = 0;
        auto f = [&] { return (double) v[(size_t) i++]->load(); };
        auto b = [&] { return v[(size_t) i++]->load() > 0.5f; };
        auto c = [&] { return (int) std::lround (v[(size_t) i++]->load()); };
        halo::EngineParams p;
        p.mainOn = b(); p.mainLo = f(); p.mainHi = f(); p.mainMode = c();
        p.thrA = f(); p.thrB = f();
        if (b()) p.thrB = p.thrA;     // seuils liés : B suit A
        p.mainTrim = f(); p.attackFast = c() == 1; p.releaseFast = c() == 1;
        p.stereo = c(); p.mainLink = f() / 100.0; p.mainSolo = c(); p.scMain = b(); p.mix = f() / 100.0;
        p.parOn = b(); p.parLo = f(); p.parHi = f(); p.parMode = c(); p.parThr = f(); p.parAmountDb = f(); p.parTrim = f();
        p.parAttackFast = c() == 1; p.parReleaseFast = c() == 1; p.parChannel = c(); p.parLink = f() / 100.0;
        p.parSolo = b(); p.scPar = b();
        p.inDb = f(); p.outDb = f(); p.levelComp = b(); p.delta = b(); p.quality = c();
        const int rt = c(), off = c();
        p.os = offline ? off : rt;
        p.bypass = b();
        return p;
    }

    std::vector<std::atomic<float>*> v;
};

/** Ce qu'un preset ou la mémoire A / B laissent tel quel : l'écoute (bypass, solos, delta). */
inline juce::StringArray notStored() { return { bypass, mainSolo, parSolo, delta }; }
} // namespace params
