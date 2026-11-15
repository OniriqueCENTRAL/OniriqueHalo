#pragma once

#include "HaloControls.h"
#include "SpectrumSmoother.h"

//==============================================================================
//  L'analyseur de balance tonale de HALO, en bandeau : spectre du vrai signal (avant, après), coloré dans les bandes
//  traitées (MAIN braise, PARALIMIT rose), référence tonale, réduction en cours, drapeaux à glisser et, sous chaque
//  drapeau, son bouton S (solo).
//==============================================================================
namespace ui
{
class Analyser : public juce::Component, private juce::Timer
{
public:
    /** parallelView : la zone de PARALIMIT (ses drapeaux, sa bande, son S, 500 Hz – 20 kHz) ; sinon celle de MAIN */
    Analyser (juce::AudioProcessorValueTreeState& s, const halo::Scope& sc, halo::Meters& m, std::atomic<int>& momentary, bool parallelView = false)
        : state (s), scope (sc), meters (m), solo (momentary), fft (order), par (parallelView),
          viewLo (parallelView ? 500.0f : tonal::minHz), viewHi (tonal::maxHz)
    {
        window.resize ((size_t) size);
        for (int i = 0; i < size; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (size - 1));
        pre.resize ((size_t) size); post.resize ((size_t) size);
        work.resize ((size_t) (2 * size));
        for (auto* v : { &dispPre, &dispPost }) v->assign ((size_t) points, -40.0f);
        mag.resize ((size_t) points); pmag.resize ((size_t) points);
        startTimerHz (30);
    }
    ~Analyser() override { solo.store (-1); }

    void setScale (float sc) { scale = sc; repaint(); }
    std::function<void()> onChip;

    /** la référence Custom : le spectre « après » du moment (thread de l'interface) */
    void captureCustom()
    {
        juce::StringArray v;
        for (int i = 0; i < tonal::customPoints; ++i) v.add (juce::String (sampleDisplay (dispPost, tonal::customHz (i)), 2));
        state.state.setProperty ("customRef", v.joinIntoString (","), nullptr);
        repaint();
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const auto plot = plotArea();

        const auto prm = params::Reader (state).read (false);
        const auto main = halo::BandEdges::make (prm.mainLo, prm.mainHi, prm.mainMode);
        const auto pe = halo::BandEdges::make (prm.parLo, prm.parHi, prm.parMode);
        const double fsView = std::max (44100.0, scope.rate.load());
        const bool defMain = main.mode == halo::BandMode::defined, defPar = pe.mode == halo::BandMode::defined;
        for (int k = 0; k < points; ++k)
        {
            mag[(size_t) k] = (float) halo::bandMagnitude (main, hzAt (k), fsView);
            pmag[(size_t) k] = (float) halo::bandMagnitude (pe, hzAt (k), fsView);
        }

        {
            juce::Graphics::ScopedSaveState keep (g);
            g.reduceClipRegion (plot.toNearestInt());

            // grille
            for (float hz : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
            {
                g.setColour (juce::Colours::white.withAlpha (hz == 1000.f || hz == 10000.f ? 0.06f : 0.035f));
                g.drawVerticalLine ((int) xFor (hz), plot.getY(), plot.getBottom());
            }
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.drawHorizontalLine ((int) yFor (0.0f), plot.getX(), plot.getRight());

            // zones traitées (la vraie courbe de chaque bande)
            auto zone = [&] (const std::vector<float>& m, juce::Colour c, float a)
            {
                juce::Path z;
                z.startNewSubPath (plot.getX(), plot.getBottom());
                for (int k = 0; k < points; ++k) z.lineTo (xAt (k), plot.getBottom() - plot.getHeight() * m[(size_t) k]);
                z.lineTo (plot.getRight(), plot.getBottom()); z.closeSubPath();
                g.setGradientFill (juce::ColourGradient (c.withAlpha (a * 0.35f), 0, plot.getY(), c.withAlpha (a), 0, plot.getBottom(), false));
                g.fillPath (z);
            };
            juce::ignoreUnused (zone);   // pas de forme de fond : la bande se lit à la couleur des barres

            // référence tonale (seulement si elle est choisie dans ⚙)
            if (refIndex() < 4)
            {
                juce::Path line, bandPath;
                for (int k = 0; k < points; ++k) { const float y = yFor (reference (hzAt (k))); if (k == 0) line.startNewSubPath (xAt (k), y); else line.lineTo (xAt (k), y); }
                for (int k = 0; k < points; ++k) { const float y = yFor (reference (hzAt (k)) + 3.0f); if (k == 0) bandPath.startNewSubPath (xAt (k), y); else bandPath.lineTo (xAt (k), y); }
                for (int k = points - 1; k >= 0; --k) bandPath.lineTo (xAt (k), yFor (reference (hzAt (k)) - 3.0f));
                bandPath.closeSubPath();
                g.setColour (side.withAlpha (0.07f));
                g.fillPath (bandPath);
                juce::Path dashed; const float dash[] { 4.0f * scale, 4.0f * scale };
                juce::PathStrokeType (1.0f * scale).createDashedStroke (dashed, line, dash, 2);
                g.setColour (side.withAlpha (0.4f));
                g.fillPath (dashed);
            }

            // spectre « après » : un tapis de barres fines (pas de courbe), gris hors bande, braise dans MAIN,
            // rose dans PARALIMIT
            juce::Path fill;
            {
                const float barW = std::max (1.0f, 1.2f * scale), gap = std::max (1.0f, 0.9f * scale);
                for (float x = plot.getX(); x < plot.getRight(); x += barW + gap)
                {
                    const float hz = hzForX (x + barW * 0.5f);
                    const float y = yFor (sampleDisplay (dispPost, juce::jlimit (tonal::minHz, tonal::maxHz, hz)));
                    if (y < plot.getBottom() - 0.5f) fill.addRectangle (x, y, barW, plot.getBottom() - y);
                }
            }
            g.setGradientFill (juce::ColourGradient (bone.withAlpha (0.42f), 0, plot.getY(), bone.withAlpha (0.12f), 0, plot.getBottom(), false));
            g.fillPath (fill);
            auto tint = [&] (float lo, float hi, juce::Colour c, float a)
            {
                juce::Graphics::ScopedSaveState inner (g);
                g.reduceClipRegion (juce::Rectangle<float> (xFor (lo), plot.getY(), xFor (hi) - xFor (lo), plot.getHeight()).toNearestInt());
                g.setGradientFill (juce::ColourGradient (c.withAlpha (0.95f * a), 0, plot.getY(), c.withAlpha (0.45f * a), 0, plot.getBottom(), false));
                g.fillPath (fill);
            };
            if (! par && prm.mainOn) tint ((float) main.lo, defMain ? (float) main.hi : tonal::maxHz, accentLit, 1.0f);
            if (par && prm.parOn) tint ((float) pe.lo, defPar ? (float) pe.hi : tonal::maxHz, onirique::look::emberPink, 1.0f);

            // la réduction en cours, suspendue au haut : 1 − (1 − g)·|bande|
            const float gr = par ? std::max (grShown[2], grShown[3]) : std::max (grShown[0], grShown[1]);
            const auto& own = par ? pmag : mag;
            if (false && gr > 0.05f && (par ? prm.parOn : prm.mainOn))   // la réduction se lit sur les LED et le fader
            {
                juce::Path red;
                const float span = plot.getHeight() * 0.35f, zoom = par ? 24.0f : grZoom();
                const float gLin = juce::Decibels::decibelsToGain (-gr);
                bool started = false;
                for (int k = 0; k < points; ++k)
                {
                    if (own[(size_t) k] < 0.02f) { started = false; continue; }   // seulement dans la bande
                    const float dB = juce::Decibels::gainToDecibels (std::max (1.0e-4f, 1.0f - (1.0f - gLin) * own[(size_t) k]));
                    const float y = plot.getY() + span * std::min (1.0f, -dB / zoom);
                    if (! started) { red.startNewSubPath (xAt (k), y); started = true; } else red.lineTo (xAt (k), y);
                }
                g.setColour (par ? onirique::look::emberPink : accentLit);
                g.strokePath (red, juce::PathStrokeType (2.0f * scale, juce::PathStrokeType::curved));
            }

            // solo momentané : la zone écoutée s'éclaire
            if (const int m = solo.load(); par ? m == 5 : (m >= 1 && m <= 4)) { g.setColour (bone.withAlpha (0.05f)); g.fillRect (plot); }

            // les drapeaux : un trait, et leur fréquence en haut
            auto flagLine = [&] (int i, float hz, bool active, juce::Colour c)
            {
                const float x = xFor (hz);
                const bool on = hot == i || dragging == i;
                if (active) { g.setColour (c.withAlpha (on ? 1.0f : 0.8f)); g.fillRect (x - (on ? 1.0f : 0.6f) * scale, plot.getY(), (on ? 2.0f : 1.2f) * scale, plot.getHeight()); }
                else
                {
                    juce::Path l; l.startNewSubPath (x, plot.getY()); l.lineTo (x, plot.getBottom());
                    juce::Path d; const float dd[] { 3.0f * scale, 4.0f * scale };
                    juce::PathStrokeType (1.0f * scale).createDashedStroke (d, l, dd, 2);
                    g.setColour (c.withAlpha (on ? 0.7f : 0.35f)); g.fillPath (d);
                }
                if (on || dragging == i)
                {
                    const auto label = params::hzText (hz);
                    const float w = juce::GlyphArrangement::getStringWidth (font (Face::mono, 9.0f * scale), label) + 10 * scale;
                    const bool left = x + 6 * scale + w > plot.getRight();
                    const auto box = juce::Rectangle<float> (left ? x - 6 * scale - w : x + 6 * scale, plot.getY() + 4 * scale, w, 15 * scale);
                    g.setColour (ink.withAlpha (0.85f)); g.fillRoundedRectangle (box, 7 * scale);
                    text (g, label, box, font (Face::mono, 9.0f * scale), c);
                }
            };
            if (par)
            {
                flagLine (3, (float) prm.parHi, defPar && prm.parOn, onirique::look::emberPink);
                flagLine (2, (float) prm.parLo, prm.parOn, onirique::look::emberPink);
            }
            else
            {
                flagLine (1, (float) prm.mainHi, defMain, accentLit);
                flagLine (0, (float) prm.mainLo, prm.mainOn, accentLit);
            }

            // échelle des fréquences, dans le bas de l'écran
            for (float hz : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
            {
                if (hz <= viewLo * 1.05f) continue;
                const auto t = hz >= 1000 ? juce::String ((int) (hz / 1000)) + "k" : juce::String ((int) hz);
                text (g, t, { xFor (hz) + 3 * scale, plot.getBottom() - 13 * scale, 30 * scale, 11 * scale }, font (Face::mono, 8.0f * scale), dim, juce::Justification::centredLeft);
            }
        }

        // la rangée des S, sous les drapeaux
        const int momentary = solo.load();
        auto sButton = [&] (int i, float hz, juce::Colour c, bool lit, bool active)
        {
            const auto r = sArea (hz);
            g.setColour (c.withAlpha (active ? 0.5f : 0.2f));
            g.fillRect (r.getCentreX() - 0.6f * scale, plot.getBottom(), 1.2f * scale, r.getY() - plot.getBottom());
            onirique::da::glass (g, r, r.getHeight() * 0.5f, scale, false);
            if (lit) { onirique::da::seg (g, r, 1.0f, 0.0f, scale, r.getHeight() * 0.5f); }
            else if (sHot == i) { g.setColour (juce::Colours::white.withAlpha (0.08f)); g.fillEllipse (r); }
            text (g, "S", r, font (Face::display, 9.5f * scale), lit ? bone : c.withAlpha (active ? 0.9f : 0.45f));
        };
        const int ms = (int) std::lround (prm.mainSolo);
        if (par)
        {
            sButton (2, (float) prm.parLo, onirique::look::emberPink, prm.parSolo || momentary == 5, prm.parOn);
            sButton (3, (float) prm.parHi, onirique::look::emberPink, prm.parSolo || momentary == 5, defPar && prm.parOn);
            return;
        }
        sButton (0, (float) prm.mainLo, accentLit, ms == 2 || momentary == 2, prm.mainOn);
        sButton (1, (float) prm.mainHi, accentLit, ms == 3 || momentary == 3, defMain && prm.mainOn);

        juce::ignoreUnused (b);
    }

    //==========================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hitFlag (e.position), s = hitS (e.position);
        const bool c = ! par && chipArea().contains (e.position);
        if (h != hot || s != sHot || c != chipHover) { hot = h; sHot = s; chipHover = c; repaint(); }
        setMouseCursor (s >= 0 || c ? juce::MouseCursor::PointingHandCursor : h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hot = sHot = -1; chipHover = false; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! par && chipArea().contains (e.position)) { if (onChip) onChip(); return; }
        const bool momentaryClick = e.mods.isPopupMenu() || e.mods.isCtrlDown();
        if (const int s = hitS (e.position); s >= 0)
        {
            if (momentaryClick) { solo.store (s == 0 ? 2 : s == 1 ? 3 : 5); repaint(); return; }
            // un clic : solo tenu (un second clic le rend)
            if (s >= 2) toggleParam ("parSolo", 1.0f);
            else toggleParam ("mainSolo", s == 0 ? 2.0f : 3.0f);
            return;
        }
        dragging = hitFlag (e.position);
        if (momentaryClick)
        {
            // clic droit maintenu : sur un drapeau, au-dessus de lui ; ailleurs, la bande MAIN (ou PARALIMIT en bas)
            const auto prm = params::Reader (state).read (false);
            int s = par ? 5 : 1;
            if (dragging == 0) s = 2; else if (dragging == 1) s = 3;
            juce::ignoreUnused (prm);
            solo.store (s);
            dragging = -1;
            repaint();
            return;
        }
        if (dragging >= 0) gesture.begin (flagParam (dragging));
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        setFlag (dragging, juce::jlimit (1000.0f, 20000.0f, hzForX (e.position.x)));
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (solo.load() != -1) { solo.store (-1); repaint(); }
        gesture.end();
        dragging = -1;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (auto* p = flagParam (hitFlag (e.position)))
        {
            p->beginChangeGesture(); p->setValueNotifyingHost (p->getDefaultValue()); p->endChangeGesture();
        }
    }

private:
    static constexpr int order = 13, size = 1 << order, points = 520;

    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced (12 * scale, 0).withTrimmedTop (10 * scale).withTrimmedBottom (34 * scale);
    }
    juce::Rectangle<float> sArea (float hz) const
    {
        const auto p = plotArea();
        return juce::Rectangle<float> (20 * scale, 20 * scale).withCentre ({ xFor (hz), p.getBottom() + 19 * scale });
    }
    juce::Rectangle<float> chipArea() const
    {
        return {};   // plus de pastille sur l'écran : la référence et la vitesse sont dans ⚙
    }
    float xFor (float hz) const
    {
        const auto p = plotArea();
        return p.getX() + p.getWidth() * std::log (hz / viewLo) / std::log (viewHi / viewLo);
    }
    float hzForX (float x) const
    {
        const auto p = plotArea();
        return viewLo * std::pow (viewHi / viewLo, (x - p.getX()) / p.getWidth());
    }
    float hzAt (int k) const { return tonal::minHz * std::pow (tonal::maxHz / tonal::minHz, (float) k / (float) (points - 1)); }
    float xAt (int k) const { return xFor (hzAt (k)); }
    float yFor (float db) const
    {
        const auto p = plotArea();
        return juce::jmap (juce::jlimit (-36.0f, 22.0f, db), -36.0f, 22.0f, p.getBottom(), p.getY());
    }

    int refIndex() const { return juce::jlimit (0, 4, (int) state.state.getProperty ("anaRef", 4)); }   // 4 : aucune (par défaut)
    int speedIndex() const { return juce::jlimit (0, 2, (int) state.state.getProperty ("anaSpeed", 1)); }
    float grZoom() const { const int z = (int) state.state.getProperty ("grZoom", 1); return z == 0 ? 3.0f : z == 2 ? 12.0f : 6.0f; }

    float reference (float hz) const
    {
        switch (refIndex())
        {
            case 1: return tonal::interp (tonal::modernMix, hz);
            case 2: return tonal::interp (tonal::vocal, hz);
            case 3:
            {
                const auto src = state.state.getProperty ("customRef").toString();
                if (customCache.size() != (size_t) tonal::customPoints || customSource != src)
                {
                    customSource = src;
                    auto parts = juce::StringArray::fromTokens (src, ",", {});
                    customCache.assign ((size_t) tonal::customPoints, 0.0f);
                    for (int i = 0; i < tonal::customPoints && i < parts.size(); ++i) customCache[(size_t) i] = parts[i].getFloatValue();
                }
                const float pos = std::log (hz / tonal::minHz) / std::log (tonal::maxHz / tonal::minHz) * (float) (tonal::customPoints - 1);
                const int i0 = juce::jlimit (0, tonal::customPoints - 2, (int) pos);
                const float t = juce::jlimit (0.0f, 1.0f, pos - (float) i0);
                return customCache[(size_t) i0] + t * (customCache[(size_t) i0 + 1] - customCache[(size_t) i0]);
            }
            default: return 0.0f;
        }
    }
    float sampleDisplay (const std::vector<float>& d, float hz) const
    {
        const float pos = std::log (hz / tonal::minHz) / std::log (tonal::maxHz / tonal::minHz) * (float) (points - 1);
        const int i0 = juce::jlimit (0, points - 2, (int) pos);
        const float t = juce::jlimit (0.0f, 1.0f, pos - (float) i0);
        return d[(size_t) i0] + t * (d[(size_t) i0 + 1] - d[(size_t) i0]);
    }

    // drapeaux : 0 MAIN bas, 1 MAIN haut, 2 PARALIMIT bas, 3 PARALIMIT haut
    juce::RangedAudioParameter* flagParam (int i) const
    {
        switch (i)
        {
            case 0: return state.getParameter (params::mainLo);
            case 1: return state.getParameter (params::mainHi);
            case 2: return state.getParameter (params::parLo);
            case 3: return state.getParameter (params::parHi);
            default: return nullptr;
        }
    }
    float flagHz (int i) const { auto* p = flagParam (i); return p != nullptr ? plainOf (*p) : 1000.0f; }

    int hitFlag (juce::Point<float> pos) const
    {
        const auto plot = plotArea();
        if (! plot.contains (pos)) return -1;
        const auto prm = params::Reader (state).read (false);
        int best = -1; float bestD = 8.0f * scale;
        juce::ignoreUnused (prm);
        for (int i = par ? 2 : 0; i < (par ? 4 : 2); ++i)
        {
            const float d = std::abs (pos.x - xFor (flagHz (i)));
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }
    int hitS (juce::Point<float> pos) const
    {
        const auto prm = params::Reader (state).read (false);
        const bool defMain = prm.mainMode == 1, defPar = prm.parMode == 1;
        for (int i = par ? 2 : 0; i < (par ? 4 : 2); ++i)
        {
            if (i == 1 && ! defMain) continue;
            if (i >= 2 && ! prm.parOn) continue;
            if (i == 3 && ! defPar) continue;
            if (sArea (flagHz (i)).expanded (2 * scale).contains (pos)) return i;
        }
        return -1;
    }
    void toggleParam (const juce::String& id, float onValue)
    {
        auto* p = state.getParameter (id);
        if (p == nullptr) return;
        const float cur = plainOf (*p);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (std::abs (cur - onValue) < 0.5f ? 0.0f : onValue));
        p->endChangeGesture();
        repaint();
    }
    void setFlag (int i, float hz)
    {
        gesture.set (hz);
        // une demi-octave au moins : le partenaire est repoussé
        const float k = 1.41421356f;
        auto* partner = flagParam (i ^ 1);
        if (partner == nullptr) return;
        const float other = plainOf (*partner);
        float want = other;
        if ((i & 1) == 0 && other < hz * k) want = std::min (20000.0f, hz * k);
        if ((i & 1) == 1 && other > hz / k) want = std::max (1000.0f, hz / k);
        if (want != other) { partner->beginChangeGesture(); partner->setValueNotifyingHost (partner->convertTo0to1 (want)); partner->endChangeGesture(); }
    }

    //==========================================================================
    void timerCallback() override
    {
        const float gr[4] { meters.grA.load(), meters.grB.load(), meters.grPL.load(), meters.grPR.load() };
        for (int i = 0; i < 4; ++i) grShown[i] = gr[i] > grShown[i] ? gr[i] : grShown[i] + 0.25f * (gr[i] - grShown[i]);
        scope.read (pre.data(), post.data(), size);
        const double fs = std::max (8000.0, scope.rate.load());
        const float a = speedIndex() == 0 ? 0.06f : speedIndex() == 2 ? 0.4f : 0.16f;
        spectrum (pre, fs, dispPre, a);
        spectrum (post, fs, dispPost, a);
        repaint();
    }

    /** spectre lissé par une gaussienne d'un sixième d'octave (le lissage de VOCALBOX, SpectrumSmoother.h : pas de
        plateau), incliné de +3 dB/oct (un bruit rose est plat), recentré sur sa moyenne 100 Hz – 10 kHz : l'écran
        montre la balance tonale, pas le niveau */
    void spectrum (const std::vector<float>& x, double fs, std::vector<float>& disp, float a)
    {
        if (fs != smootherRate) { smoother.build (fs, size, points, 1.0 / 24.0); smootherRate = fs; }
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i) work[(size_t) i] = x[(size_t) i] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        // puissance par raie, à l'échelle « un sinus d'amplitude A vaut A² » (fenêtre de Hann : gain 0,5)
        const float k = 2.0f / (0.5f * (float) size);
        for (int i = 0; i <= size / 2; ++i) { const float m = work[(size_t) i] * k; work[(size_t) i] = m * m; }
        smoother.apply (work.data(), fresh);
        double mean = 0; int meanN = 0; float peak = -200.0f;
        for (int i = 0; i < points; ++i)
        {
            const double hz = hzAt (i);
            peak = std::max (peak, fresh[(size_t) i]);
            fresh[(size_t) i] += (float) (10.0 * std::log10 (hz / 1000.0));
            if (hz >= 100 && hz <= 10000) { mean += fresh[(size_t) i]; ++meanN; }
        }
        const bool silent = peak < -100.0f;
        const float offset = meanN > 0 ? (float) (mean / meanN) : 0.0f;
        for (int i = 0; i < points; ++i)
            disp[(size_t) i] += a * ((silent ? -40.0f : fresh[(size_t) i] - offset) - disp[(size_t) i]);
    }

    onirique::ui::SpectrumSmoother smoother;
    double smootherRate = 0;
    juce::AudioProcessorValueTreeState& state;
    const halo::Scope& scope;
    halo::Meters& meters;
    std::atomic<int>& solo;
    juce::dsp::FFT fft;
    std::vector<float> window, pre, post, work, dispPre, dispPost, mag, pmag;
    std::array<float, points> fresh {};
    mutable std::vector<float> customCache;
    mutable juce::String customSource;
    Gesture gesture;
    bool par = false;
    float viewLo = 20.0f, viewHi = 20000.0f;
    float grShown[4] {}, scale = 1.0f;
    int hot = -1, sHot = -1, dragging = -1;
    bool chipHover = false;
};

/** Crêtes (L, R) en deux traits fins, pour la barre d'état : entrée ou sortie. */
class MiniMeter : public juce::Component, private juce::Timer
{
public:
    MiniMeter (halo::Meters& m, bool output) : meters (m), out (output) { startTimerHz (30); }
    void setScale (float sc) { scale = sc; repaint(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0, 2 * scale);
        const float h = (r.getHeight() - 2 * scale) / 2.0f;
        for (int c = 0; c < 2; ++c)
        {
            auto bar = juce::Rectangle<float> (r.getX(), r.getY() + (h + 2 * scale) * (float) c, r.getWidth(), h);
            g.setColour (ink); g.fillRoundedRectangle (bar, 1.5f * scale);
            const float w = bar.getWidth() * juce::jmap (juce::jlimit (-60.0f, 6.0f, level[c]), -60.0f, 6.0f, 0.0f, 1.0f);
            g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.4f), bar.getX(), 0, accentLit, bar.getRight(), 0, false));
            g.fillRect (bar.withWidth (w));
            if (level[c] > 0.0f) { g.setColour (juce::Colour (0xffff5a4a)); g.fillRect (bar.withTrimmedLeft (bar.getWidth() * (60.0f / 66.0f))); }
        }
    }
private:
    void timerCallback() override
    {
        for (int c = 0; c < 2; ++c)
        {
            const float v = juce::Decibels::gainToDecibels (out ? meters.outPeak[c].load() : meters.inPeak[c].load(), -100.0f);
            level[c] = v > level[c] ? v : level[c] - 1.5f;
        }
        repaint();
    }
    halo::Meters& meters;
    bool out;
    float level[2] { -100, -100 }, scale = 1.0f;
};
} // namespace ui
