#pragma once

#include "Controls.h"
#include "../Params.h"

//==============================================================================
//  L'analyseur de balance tonale de HALO, ses drapeaux, et les indicateurs (réduction, bande, entrée / sortie).
//  Tout ce qui est montré vient du moteur : spectre du vrai signal (avant / après), bande dessinée avec la même
//  formule que le DSP (halo::bandMagnitude), réduction réellement calculée.
//==============================================================================
namespace ui
{
namespace tonal
{
struct Pt { float hz, db; };
// Références originales ONIRIQUE, en dB par rapport à un bruit rose (affiché à plat). Pas de données de tiers.
constexpr Pt modernMix[] { { 30, 3.5f }, { 60, 3.0f }, { 120, 1.5f }, { 300, 0.5f }, { 1000, 0.0f }, { 2500, -0.8f },
                           { 5000, -1.8f }, { 8000, -3.0f }, { 12000, -4.6f }, { 16000, -7.0f }, { 20000, -10.0f } };
constexpr Pt vocal[] { { 30, -14.0f }, { 80, -7.0f }, { 150, -2.5f }, { 400, 0.0f }, { 1200, 0.6f }, { 3000, 1.4f },
                       { 5000, 1.0f }, { 8000, -0.6f }, { 12000, -2.8f }, { 16000, -5.5f }, { 20000, -9.0f } };
constexpr int customPoints = 40;
constexpr float minHz = 20.0f, maxHz = 20000.0f;

template <size_t N>
inline float interp (const Pt (&pts)[N], float hz)
{
    if (hz <= pts[0].hz) return pts[0].db;
    for (size_t i = 1; i < N; ++i)
        if (hz <= pts[i].hz)
        {
            const float t = std::log (hz / pts[i - 1].hz) / std::log (pts[i].hz / pts[i - 1].hz);
            return pts[i - 1].db + t * (pts[i].db - pts[i - 1].db);
        }
    return pts[N - 1].db;
}
inline float customHz (int i) { return minHz * std::pow (maxHz / minHz, (float) i / (float) (customPoints - 1)); }
} // namespace tonal

//==============================================================================
/** Crêtes d'entrée et de sortie (L, R), horizontales, avec maintien. */
class IoMeter : public juce::Component, private juce::Timer
{
public:
    explicit IoMeter (halo::Meters& m) : meters (m) { startTimerHz (30); }
    void setScale (float sc) { scale = sc; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { for (auto& h : hold) h = -100.0f; repaint(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float rowH = r.getHeight() / 2.0f;
        const char* names[2] { "IN", "OUT" };
        for (int row = 0; row < 2; ++row)
        {
            auto line = r.removeFromTop (rowH).reduced (0, 3 * scale);
            text (g, names[row], line.removeFromLeft (28 * scale), font (Face::body, 8.5f * scale, 0.16f), haze, juce::Justification::centredLeft);
            auto val = line.removeFromRight (40 * scale);
            const float h = (line.getHeight() - 2 * scale) / 2.0f;
            for (int c = 0; c < 2; ++c)
            {
                auto bar = juce::Rectangle<float> (line.getX(), line.getY() + (h + 2 * scale) * (float) c, line.getWidth(), h);
                g.setColour (ink); g.fillRoundedRectangle (bar, 1.5f * scale);
                const float v = level[row * 2 + c];
                const float w = bar.getWidth() * juce::jmap (juce::jlimit (-60.0f, 6.0f, v), -60.0f, 6.0f, 0.0f, 1.0f);
                g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.4f), bar.getX(), 0, accentLit, bar.getRight(), 0, false));
                g.fillRect (bar.withWidth (w));
                if (v > 0.0f) { g.setColour (juce::Colour (0xffff5a4a)); g.fillRect (bar.withTrimmedLeft (bar.getWidth() * (60.0f / 66.0f))); }
                const float hx = bar.getX() + bar.getWidth() * juce::jmap (juce::jlimit (-60.0f, 6.0f, hold[row * 2 + c]), -60.0f, 6.0f, 0.0f, 1.0f);
                g.setColour (bone.withAlpha (0.8f));
                g.fillRect (hx - 0.5f, bar.getY(), 1.0f * scale, bar.getHeight());
            }
            const float pk = std::max (hold[row * 2], hold[row * 2 + 1]);
            text (g, pk > -99 ? juce::String (pk, 1) : juce::String ("-inf"), val, font (Face::mono, 8.5f * scale), pk > 0 ? juce::Colour (0xffff5a4a) : haze, juce::Justification::centredRight);
        }
    }

private:
    void timerCallback() override
    {
        const float v[4] { meters.inPeak[0].load(), meters.inPeak[1].load(), meters.outPeak[0].load(), meters.outPeak[1].load() };
        for (int i = 0; i < 4; ++i)
        {
            const float db = juce::Decibels::gainToDecibels (v[i], -100.0f);
            level[i] = db > level[i] ? db : level[i] - 1.5f;
            if (db > hold[i]) hold[i] = db;
        }
        repaint();
    }
    halo::Meters& meters;
    float level[4] { -100, -100, -100, -100 }, hold[4] { -100, -100, -100, -100 };
    float scale = 1.0f;
};

//==============================================================================
/** Boutons de la barre du haut que la barre Nuit n'a pas : annuler, rétablir, témoin EXT, réglages. */
class IconButton : public juce::Button
{
public:
    enum Kind { undo, redo, settings };
    IconButton (Kind k, const juce::String& name) : juce::Button (name), kind (k) {}
    void setScale (float sc) { scale = sc; repaint(); }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff1a0c0f));
        g.fillRoundedRectangle (r, 8 * scale);
        g.setColour (juce::Colours::white.withAlpha (over ? 0.2f : 0.09f));
        g.drawRoundedRectangle (r, 8 * scale, 1.0f);
        const auto c = r.getCentre();
        const float u = r.getHeight() * 0.22f;
        g.setColour ((isEnabled() ? bone : dim).withAlpha (down ? 0.6f : 0.9f));
        juce::Path p;
        if (kind == settings)
        {
            for (int i = 0; i < 8; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                p.addRectangle (-u * 0.22f, -u * 1.45f, u * 0.44f, u * 0.6f);
                p.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::twoPi / 8.0f));
                juce::ignoreUnused (a);
            }
            p.addEllipse (-u, -u, 2 * u, 2 * u);
            p.applyTransform (juce::AffineTransform::translation (c.x, c.y));
            g.fillPath (p);
            g.setColour (juce::Colour (0xff1a0c0f));
            g.fillEllipse (juce::Rectangle<float> (u * 0.9f, u * 0.9f).withCentre (c));
            return;
        }
        // flèche courbe (annuler : vers la gauche, rétablir : vers la droite)
        const float dir = kind == undo ? -1.0f : 1.0f;
        p.addCentredArc (c.x, c.y + u * 0.35f, u * 1.15f, u * 1.0f, 0.0f, -juce::MathConstants<float>::halfPi * 1.25f * dir,
                         juce::MathConstants<float>::halfPi * 0.9f * dir, true);
        g.strokePath (p, juce::PathStrokeType (1.6f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const auto tip = juce::Point<float> (c.x + dir * u * 1.15f * std::sin (-juce::MathConstants<float>::halfPi * 1.25f), c.y + u * 0.35f - u * std::cos (-juce::MathConstants<float>::halfPi * 1.25f));
        juce::Path head;
        head.addTriangle (tip.x - dir * u * 0.55f, tip.y - u * 0.15f, tip.x + dir * u * 0.15f, tip.y - u * 0.6f, tip.x + dir * u * 0.25f, tip.y + u * 0.35f);
        g.fillPath (head);
    }
private:
    Kind kind;
    float scale = 1.0f;
};

/** Le témoin EXT : éteint sans sidechain, braise pâle si elle est branchée, allumé quand un moteur l'écoute. */
class ExtLight : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    ExtLight (halo::Meters& m, std::function<bool()> connected) : meters (m), isConnected (std::move (connected))
    {
        startTimerHz (15);
        setTooltip (juce::String::fromUTF8 ("EXT : la sidechain externe. Branchée dans le DAW, puis EXT dans MAIN ou PARALIMIT : "
                                            "c'est elle qui commande la réduction (la bande reste celle de chaque moteur)."));
    }
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    void setScale (float sc) { scale = sc; repaint(); }
    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff1a0c0f));
        g.fillRoundedRectangle (r, 8 * scale);
        g.setColour (juce::Colours::white.withAlpha (0.09f));
        g.drawRoundedRectangle (r, 8 * scale, 1.0f);
        onirique::da::led (g, { r.getX() + r.getHeight() * 0.5f, r.getCentreY() }, 4.5f * scale, lit);
        text (g, "EXT", r.withTrimmedLeft (r.getHeight() * 0.8f), font (Face::body, 10.0f * scale, 0.16f), bone.withAlpha (0.4f + 0.6f * std::max (lit, connected ? 0.4f : 0.0f)));
    }
private:
    void timerCallback() override
    {
        connected = isConnected();
        const bool used = meters.extUsed.load() && connected;
        const float sig = used ? juce::jlimit (0.0f, 1.0f, (meters.extLevel.load() + 60.0f) / 60.0f) : 0.0f;
        const float t = used ? 0.55f + 0.45f * sig : connected ? 0.18f : 0.0f;
        if (std::abs (t - lit) > 0.005f) { lit += (t - lit) * 0.4f; repaint(); }
    }
    halo::Meters& meters;
    std::function<bool()> isConnected;
    bool connected = false;
    float lit = 0.0f, scale = 1.0f;
};
} // namespace ui
