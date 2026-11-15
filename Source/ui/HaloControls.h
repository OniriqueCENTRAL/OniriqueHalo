#pragma once

#include "Display.h"

//==============================================================================
//  Les commandes propres à la façade de HALO (DA ONIRIQUE) : fader de seuil à deux poignées, échelle de LED de
//  réduction, fader AMOUNT de PARALIMIT, éléments de la barre d'état (valeurs à tirer, choix, interrupteurs).
//==============================================================================
namespace ui
{
/** Un geste complet sur un paramètre (pour le glisser à la souris). */
struct Gesture
{
    juce::RangedAudioParameter* p = nullptr;
    void begin (juce::RangedAudioParameter* param) { end(); p = param; if (p) p->beginChangeGesture(); }
    void set (float plain) { if (p) p->setValueNotifyingHost (p->convertTo0to1 (p->getNormalisableRange().snapToLegalValue (plain))); }
    void end() { if (p) p->endChangeGesture(); p = nullptr; }
};

inline float plainOf (juce::RangedAudioParameter& p) { return p.convertFrom0to1 (p.getValue()); }

//==============================================================================
/** Le fader de seuil de MAIN : échelle 0 … −48 dBFS, au centre le niveau de crête de la bande détectée (deux canaux),
    une poignée à gauche (L ou M) et une à droite (R ou S). En tête, l'icône de liaison des seuils (un clic). Seuils
    liés : les deux poignées bougent ensemble. Double-clic : valeur d'origine. */
class ThresholdFader : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    ThresholdFader (juce::AudioProcessorValueTreeState& s, halo::Meters& m)
        : state (s), meters (m), a (*s.getParameter (params::thrA)), b (*s.getParameter (params::thrB)),
          link (*s.getParameter (params::thrLink)), stereo (*s.getParameter (params::stereo))
    {
        startTimerHz (30);
        setTooltip (juce::String::fromUTF8 ("Seuils de MAIN (crête de la bande, dBFS). Poignée gauche : L ou Mid, droite : R ou Side. "
                                            "Au centre, la crête de la bande détectée. L'icône du haut lie les deux seuils."));
    }
    void setScale (float sc) { scale = sc; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const bool ms = plainOf (stereo) > 0.5f, linked = plainOf (link) > 0.5f;
        // en tête : L ⛓ R
        const auto head = headArea();
        text (g, ms ? "M" : "L", head.withWidth (head.getWidth() * 0.33f), font (Face::display, 12.0f * scale), bone);
        text (g, ms ? "S" : "R", head.withTrimmedLeft (head.getWidth() * 0.67f), font (Face::display, 12.0f * scale), bone);
        drawChain (g, linkArea(), linked);

        const auto track = trackArea();
        // échelle
        for (int db = 0; db >= -48; db -= 6)
        {
            const float y = yFor ((float) db);
            const bool label = db % 12 == 0;
            g.setColour (juce::Colours::white.withAlpha (label ? 0.25f : 0.12f));
            g.fillRect (track.getX() - (label ? 9.0f : 6.0f) * scale, y - 0.5f, (label ? 5.0f : 3.0f) * scale, 1.0f);
            g.fillRect (track.getRight() + 4.0f * scale, y - 0.5f, (label ? 5.0f : 3.0f) * scale, 1.0f);
            if (label) text (g, juce::String (db), { track.getX() - 44 * scale, y - 6 * scale, 30 * scale, 12 * scale }, font (Face::mono, 8.5f * scale), dim, juce::Justification::centredRight);
        }
        // la colonne : niveaux de bande
        g.setColour (ink);
        g.fillRoundedRectangle (track, 3 * scale);
        const float half = track.getWidth() / 2.0f;
        for (int c = 0; c < 2; ++c)
        {
            auto col = juce::Rectangle<float> (track.getX() + half * (float) c, track.getY(), half, track.getHeight()).reduced (1.5f * scale, 1.0f);
            const float yb = yFor (band[c]);
            const bool over = band[c] > (c == 0 ? plainOf (a) : (linked ? plainOf (a) : plainOf (b)));
            g.setGradientFill (juce::ColourGradient ((over ? accentLit : haze).withAlpha (0.85f), 0, col.getY(), (over ? accent : haze).withAlpha (0.25f), 0, col.getBottom(), false));
            g.fillRect (col.withTop (yb));
        }
        // les poignées
        auto handle = [&] (float db, bool left, bool hotNow)
        {
            const float y = yFor (db);
            const float x = left ? track.getX() - 26 * scale : track.getRight() + 26 * scale;
            g.setColour (accentLit.withAlpha (0.8f));
            g.fillRect (std::min (x, left ? track.getX() : track.getRight()), y - 0.75f * scale, std::abs (x - (left ? track.getX() : track.getRight())), 1.5f * scale);
            const auto c = juce::Rectangle<float> (16 * scale, 16 * scale).withCentre ({ x, y });
            if (hotNow) { g.setColour (accent.withAlpha (0.3f)); g.fillEllipse (c.expanded (5 * scale)); }
            g.setColour (ink); g.fillEllipse (c);
            g.setColour (accentLit); g.drawEllipse (c, 1.6f * scale);
            g.fillEllipse (c.reduced (5 * scale));
        };
        handle (plainOf (a), true, hot == 0 || drag == 0);
        handle (linked ? plainOf (a) : plainOf (b), false, hot == 1 || drag == 1);
        // valeurs
        const auto vals = juce::Rectangle<float> (r.getX(), r.getBottom() - 16 * scale, r.getWidth(), 16 * scale);
        text (g, juce::String (plainOf (a), 1), vals.withWidth (vals.getWidth() / 2), font (Face::mono, 9.5f * scale), drag == 0 ? onirique::look::emberPink : bone.withAlpha (0.85f));
        text (g, juce::String (linked ? plainOf (a) : plainOf (b), 1), vals.withTrimmedLeft (vals.getWidth() / 2), font (Face::mono, 9.5f * scale),
              drag == 1 ? onirique::look::emberPink : bone.withAlpha (linked ? 0.45f : 0.85f));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h != hot) { hot = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::UpDownResizeCursor : linkArea().contains (e.position) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { hot = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (linkArea().contains (e.position))
        {
            gesture.begin (&link); gesture.set (plainOf (link) > 0.5f ? 0.0f : 1.0f); gesture.end();
            repaint(); return;
        }
        drag = hit (e.position);
        if (drag < 0) drag = e.position.x < trackArea().getCentreX() ? 0 : 1;
        const bool linked = plainOf (link) > 0.5f;
        gesture.begin (drag == 0 || linked ? &a : &b);
        grabDb = plainOf (drag == 0 || linked ? a : b);
        grabY = e.position.y;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag < 0) return;
        const float perDb = trackArea().getHeight() / 48.0f;
        const float fine = e.mods.isShiftDown() ? 0.25f : 1.0f;
        gesture.set (juce::jlimit (-40.0f, 0.0f, grabDb - (e.position.y - grabY) / perDb * fine));
        if (e.mods.isShiftDown()) { grabDb = plainOf (*gesture.p); grabY = e.position.y; }
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override { gesture.end(); drag = -1; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h < 0) return;
        auto& p = (h == 0 || plainOf (link) > 0.5f) ? a : b;
        p.beginChangeGesture(); p.setValueNotifyingHost (p.getDefaultValue()); p.endChangeGesture();
    }

private:
    juce::Rectangle<float> headArea() const { return getLocalBounds().toFloat().removeFromTop (22 * scale); }
    juce::Rectangle<float> linkArea() const { return juce::Rectangle<float> (26 * scale, 20 * scale).withCentre (headArea().getCentre()); }
    juce::Rectangle<float> trackArea() const
    {
        const auto r = getLocalBounds().toFloat();
        return { r.getCentreX() - 9 * scale, r.getY() + 34 * scale, 18 * scale, r.getHeight() - 34 * scale - 30 * scale };
    }
    float yFor (float db) const
    {
        const auto t = trackArea();
        return juce::jmap (juce::jlimit (-48.0f, 0.0f, db), 0.0f, -48.0f, t.getY(), t.getBottom());
    }
    int hit (juce::Point<float> pos) const
    {
        const auto t = trackArea();
        const bool linked = plainOf (link) > 0.5f;
        const float ya = yFor (plainOf (a)), yb = yFor (linked ? plainOf (a) : plainOf (b));
        if (std::abs (pos.y - ya) < 11 * scale && pos.x < t.getCentreX()) return 0;
        if (std::abs (pos.y - yb) < 11 * scale && pos.x >= t.getCentreX()) return 1;
        return -1;
    }
    static void drawChain (juce::Graphics& g, juce::Rectangle<float> r, bool linked)
    {
        const float u = r.getHeight() / 20.0f;
        g.setColour (linked ? accentLit : dim);
        const auto c = r.getCentre();
        const float dx = linked ? 4.5f * u : 7.0f * u;
        g.drawRoundedRectangle (juce::Rectangle<float> (11 * u, 7 * u).withCentre (c.translated (-dx, 0)), 3.5f * u, 1.6f * u);
        g.drawRoundedRectangle (juce::Rectangle<float> (11 * u, 7 * u).withCentre (c.translated (dx, 0)), 3.5f * u, 1.6f * u);
    }

    void timerCallback() override
    {
        const float v[2] { meters.bandA.load(), meters.bandB.load() };
        for (int i = 0; i < 2; ++i) band[i] = v[i] > band[i] ? v[i] : band[i] - 1.0f;
        repaint();
    }

    juce::AudioProcessorValueTreeState& state;
    halo::Meters& meters;
    juce::RangedAudioParameter &a, &b, &link, &stereo;
    Gesture gesture;
    float band[2] { -60, -60 }, grabDb = 0, grabY = 0, scale = 1.0f;
    int hot = -1, drag = -1;
};

//==============================================================================
/** L'échelle de LED de réduction de MAIN : deux colonnes (L/R ou M/S), graduées selon le zoom (3, 6 ou 12 dB : un clic
    sur l'échelle). Au centre, l'écouteur : solo de la bande traitée (Alt + clic : le reste, hors bande). */
class LedLadder : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    LedLadder (juce::AudioProcessorValueTreeState& s, halo::Meters& m)
        : state (s), meters (m), solo (*s.getParameter (params::mainSolo)), stereo (*s.getParameter (params::stereo))
    {
        startTimerHz (30);
        setTooltip (juce::String::fromUTF8 ("Réduction de gain de MAIN, canal par canal. Un clic sur l'échelle : zoom 3 / 6 / 12 dB. "
                                            "L'écouteur : écouter la bande traitée (Alt + clic : le reste)."));
    }
    void setScale (float sc) { scale = sc; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const bool ms = plainOf (stereo) > 0.5f;
        const auto head = getLocalBounds().toFloat().removeFromTop (22 * scale);
        const float colA = colX (0), colB = colX (1), mid = (colA + colB) / 2.0f;
        text (g, ms ? "M" : "L", { colA - 15 * scale, head.getY(), 30 * scale, head.getHeight() }, font (Face::display, 12.0f * scale), bone);
        text (g, ms ? "S" : "R", { colB - 15 * scale, head.getY(), 30 * scale, head.getHeight() }, font (Face::display, 12.0f * scale), bone);
        drawPhones (g, phonesArea(), (int) std::lround (plainOf (solo)));

        const float* steps = levels();
        for (int k = 0; k < 6; ++k)
        {
            const float y = rowY (k);
            const float v = steps[k];
            text (g, juce::String (v, v == std::floor (v) ? 0 : 1), { mid - 15 * scale, y - 7 * scale, 30 * scale, 14 * scale }, font (Face::mono, 10.0f * scale), haze);
            for (int c = 0; c < 2; ++c)
            {
                const float lit = k == 0 ? (gr[c] > 0.05f ? 1.0f : 0.0f) : juce::jlimit (0.0f, 1.0f, (gr[c] - steps[k - 1]) / (steps[k] - steps[k - 1]));
                const float held = hold[c] >= v && k > 0 ? 1.0f : 0.0f;
                const auto led = juce::Rectangle<float> (12 * scale, 9 * scale).withCentre ({ colX (c), y });
                g.setColour (ink); g.fillRoundedRectangle (led.expanded (1.5f * scale), 3 * scale);
                g.setColour (juce::Colours::white.withAlpha (0.06f)); g.fillRoundedRectangle (led, 2.5f * scale);
                const float l = std::max (lit, 0.35f * held);
                if (l > 0.0f)
                {
                    g.setColour (accent.withAlpha (0.25f * l)); g.fillRoundedRectangle (led.expanded (3 * scale), 4 * scale);
                    g.setColour ((k >= 4 ? juce::Colour (0xffff5a4a) : accentLit).withAlpha (l)); g.fillRoundedRectangle (led, 2.5f * scale);
                }
            }
        }
        const float big = std::max (gr[0], gr[1]);
        text (g, juce::String (big, 1) + " dB", { 0, (float) getHeight() - 16 * scale, (float) getWidth(), 16 * scale }, font (Face::mono, 9.5f * scale), big > 0.05f ? accentLit : dim);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (phonesArea().contains (e.position))
        {
            const int cur = (int) std::lround (plainOf (solo));
            const int want = e.mods.isAltDown() ? 4 : 1;
            gesture.begin (&solo); gesture.set ((float) (cur == want ? 0 : want)); gesture.end();
            repaint();
            return;
        }
        const int z = ((int) state.state.getProperty ("grZoom", 1) + 1) % 3;
        state.state.setProperty ("grZoom", z, nullptr);
        repaint();
    }

private:
    const float* levels() const
    {
        static constexpr float z3[] { 0, 0.5f, 1, 1.5f, 2, 3 }, z6[] { 0, 1, 2, 3, 4, 6 }, z12[] { 0, 2, 4, 6, 8, 12 };
        const int z = (int) state.state.getProperty ("grZoom", 1);
        return z == 0 ? z3 : z == 2 ? z12 : z6;
    }
    float colX (int c) const { return getWidth() * (c == 0 ? 0.22f : 0.78f); }
    float rowY (int k) const
    {
        const float top = 40 * scale, bottom = (float) getHeight() - 30 * scale;
        return top + (bottom - top) * (float) k / 5.0f;
    }
    juce::Rectangle<float> phonesArea() const { return juce::Rectangle<float> (24 * scale, 22 * scale).withCentre ({ getWidth() * 0.5f, 11 * scale }); }
    void drawPhones (juce::Graphics& g, juce::Rectangle<float> r, int soloMode) const
    {
        const bool on = soloMode == 1 || soloMode == 4;
        const float u = r.getHeight() / 22.0f;
        const auto c = r.getCentre();
        if (on) { g.setColour (accent.withAlpha (0.3f)); g.fillEllipse (juce::Rectangle<float> (26 * u, 26 * u).withCentre (c)); }
        g.setColour (on ? bone : haze);
        juce::Path arc;
        arc.addCentredArc (c.x, c.y + 2 * u, 7 * u, 7 * u, 0, -juce::MathConstants<float>::halfPi * 1.1f, juce::MathConstants<float>::halfPi * 1.1f, true);
        g.strokePath (arc, juce::PathStrokeType (1.6f * u));
        g.fillRoundedRectangle (c.x - 8.5f * u, c.y + 1 * u, 3.5f * u, 6.5f * u, 1.2f * u);
        g.fillRoundedRectangle (c.x + 5.0f * u, c.y + 1 * u, 3.5f * u, 6.5f * u, 1.2f * u);
        if (soloMode == 4) text (g, "REST", { c.x - 20 * u, c.y + 8 * u, 40 * u, 9 * u }, font (Face::mono, 7.0f * u), bone);
    }
    void timerCallback() override
    {
        const float v[2] { meters.grA.load(), meters.grB.load() };
        for (int i = 0; i < 2; ++i)
        {
            gr[i] = v[i] > gr[i] ? v[i] : gr[i] + 0.3f * (v[i] - gr[i]);
            if (v[i] >= hold[i]) { hold[i] = v[i]; age[i] = 0; } else if (++age[i] > 40) hold[i] = std::max (0.0f, hold[i] - 0.25f);
        }
        repaint();
    }
    juce::AudioProcessorValueTreeState& state;
    halo::Meters& meters;
    juce::RangedAudioParameter &solo, &stereo;
    Gesture gesture;
    float gr[2] {}, hold[2] {}, scale = 1.0f;
    int age[2] {};
};

//==============================================================================
/** Le fader AMOUNT de PARALIMIT (−∞ … +12 dB) : la piste montre aussi la réduction de la bande parallèle. */
class AmountFader : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    AmountFader (juce::AudioProcessorValueTreeState& s, halo::Meters& m) : meters (m), amount (*s.getParameter (params::parAmount))
    {
        startTimerHz (30);
        setTooltip (juce::String::fromUTF8 ("AMOUNT : ce que PARALIMIT ajoute au signal, de -inf à +12 dB. Dans la piste, la réduction de la bande parallèle."));
    }
    void setScale (float sc) { scale = sc; repaint(); }
    void paint (juce::Graphics& g) override
    {
        const auto t = track();
        g.setColour (ink); g.fillRoundedRectangle (t, 3 * scale);
        // réduction de la bande parallèle, de haut en bas (0 … 24 dB)
        const float grv = std::max (gr[0], gr[1]);
        g.setGradientFill (juce::ColourGradient (onirique::look::emberPink.withAlpha (0.7f), 0, t.getY(), onirique::look::emberPink.withAlpha (0.15f), 0, t.getBottom(), false));
        g.fillRect (t.reduced (2 * scale, 1).withHeight (t.getHeight() * juce::jlimit (0.0f, 1.0f, grv / 24.0f)));
        for (float db : { 12.0f, 0.0f, -12.0f, -24.0f, -60.0f })
        {
            const float y = yFor (db);
            g.setColour (juce::Colours::white.withAlpha (0.2f));
            g.fillRect (t.getRight() + 4 * scale, y - 0.5f, 4 * scale, 1.0f);
            text (g, db <= -59 ? juce::String::fromUTF8 ("-\xe2\x88\x9e") : (db > 0 ? "+" : "") + juce::String ((int) db),
                  { t.getRight() + 9 * scale, y - 6 * scale, 26 * scale, 12 * scale }, font (Face::mono, 8.0f * scale), dim, juce::Justification::centredLeft);
        }
        // le chapeau du fader : verre strié
        const float y = yFor (plainOf (amount));
        const auto cap = juce::Rectangle<float> (t.getWidth() + 18 * scale, 26 * scale).withCentre ({ t.getCentreX(), y });
        onirique::da::glass (g, cap, 5 * scale, scale, false);
        g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillRoundedRectangle (cap.reduced (1.0f), 5 * scale);
        for (int i = -3; i <= 3; ++i)
        {
            g.setColour ((i == 0 ? accentLit : juce::Colours::white.withAlpha (0.22f)));
            g.fillRect (cap.getX() + 4 * scale, cap.getCentreY() + (float) i * 3.2f * scale - (i == 0 ? 0.75f : 0.5f) * scale, cap.getWidth() - 8 * scale, (i == 0 ? 1.5f : 1.0f) * scale);
        }
        text (g, amount.getCurrentValueAsText(), { 0, (float) getHeight() - 16 * scale, (float) getWidth(), 16 * scale }, font (Face::mono, 9.0f * scale), dragging ? onirique::look::emberPink : bone.withAlpha (0.85f));
    }
    void mouseDown (const juce::MouseEvent& e) override { gesture.begin (&amount); dragging = true; grabN = amount.getValue(); grabY = e.position.y; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float k = (e.mods.isShiftDown() ? 0.25f : 1.0f) / track().getHeight();
        gesture.p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, grabN - (e.position.y - grabY) * k));
        if (e.mods.isShiftDown()) { grabN = amount.getValue(); grabY = e.position.y; }
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override { gesture.end(); dragging = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { amount.beginChangeGesture(); amount.setValueNotifyingHost (amount.getDefaultValue()); amount.endChangeGesture(); }
private:
    juce::Rectangle<float> track() const { return { getWidth() * 0.36f - 6 * scale, 14 * scale, 12 * scale, (float) getHeight() - 14 * scale - 34 * scale }; }
    float yFor (float db) const
    {
        const auto t = track();
        return t.getBottom() - t.getHeight() * amount.convertTo0to1 (juce::jlimit (-60.0f, 12.0f, db));
    }
    void timerCallback() override
    {
        const float v[2] { meters.grPL.load(), meters.grPR.load() };
        for (int i = 0; i < 2; ++i) gr[i] = v[i] > gr[i] ? v[i] : gr[i] + 0.3f * (v[i] - gr[i]);
        repaint();
    }
    halo::Meters& meters;
    juce::RangedAudioParameter& amount;
    Gesture gesture;
    float gr[2] {}, grabN = 0, grabY = 0, scale = 1.0f;
    bool dragging = false;
};

//==============================================================================
/** Bouton rond « S » (solo) : verre, s'allume en braise. */
class SoloButton : public juce::Button
{
public:
    explicit SoloButton (const juce::String& t = "S") : juce::Button (t) {}
    void setScale (float sc) { scale = sc; repaint(); }
    void setLit (bool b) { if (b != lit) { lit = b; repaint(); } }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        fullPill (g, r, lit ? 1.0f : 0.0f, over ? 1.0f : 0.0f, scale);
        if (down) { g.setColour (juce::Colours::black.withAlpha (0.2f)); g.fillRoundedRectangle (r, r.getHeight() * 0.5f); }
        ui::text (g, getButtonText(), r, font (Face::display, 10.5f * scale), lit ? bone : haze);
    }
private:
    bool lit = false;
    float scale = 1.0f;
};

//==============================================================================
/** Barre d'état : « étiquette  valeur ⌃ ». Valeur numérique : à tirer (Maj : fin), double-clic : origine.
    Choix : un clic ouvre la liste. Interrupteur : un clic. */
class StatusItem : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    enum Kind { value, choice, toggle };
    StatusItem (juce::RangedAudioParameter& p, const juce::String& labelText, Kind k, juce::StringArray names = {})
        : param (p), label (labelText), kind (k), items (std::move (names)),
          attachment (p, [this] (float) { repaint(); })
    {
        attachment.sendInitialUpdate();
        setMouseCursor (kind == value ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::PointingHandCursor);
        if (kind == toggle) startTimerHz (30);
    }
    void setScale (float sc) { scale = sc; repaint(); }
    /** texte ajouté après la valeur (la latence mesurée, par exemple) */
    std::function<juce::String()> suffix;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool over = isMouseOverOrDragging();
        if (kind == toggle)
        {
            const bool on = param.getValue() > 0.5f;
            fullPill (g, r.reduced (1.0f, 4 * scale), lit, over ? 1.0f : 0.0f, scale);
            text (g, label, r, font (Face::body, 10.0f * scale, 0.14f), on ? bone : haze);
            return;
        }
        const auto lf = font (Face::body, 10.0f * scale, 0.1f), vf = font (Face::mono, 10.0f * scale);
        const float lw = juce::GlyphArrangement::getStringWidth (lf, label) + 7 * scale;
        text (g, label, r.removeFromLeft (lw), lf, dim, juce::Justification::centredLeft);
        juce::String v = kind == choice ? items[(int) std::lround (plainOf (param))] : param.getCurrentValueAsText();
        if (suffix) v << suffix();
        const auto box = r.withTrimmedRight (kind == choice ? 12 * scale : 0).withSizeKeepingCentre (r.getWidth() - (kind == choice ? 12 * scale : 0), 20 * scale);
        g.setColour (ink.withAlpha (over ? 0.95f : 0.75f));
        g.fillRoundedRectangle (box.withX (r.getX()), 5 * scale);
        text (g, v, box.withX (r.getX()).reduced (5 * scale, 0), vf, dragging ? onirique::look::emberPink : bone.withAlpha (0.9f), juce::Justification::centredLeft);
        if (kind == choice)
        {
            juce::Path ch;
            const float cx = r.getRight() - 6 * scale, cy = r.getCentreY();
            ch.startNewSubPath (cx - 3.5f * scale, cy + 2 * scale); ch.lineTo (cx, cy - 2 * scale); ch.lineTo (cx + 3.5f * scale, cy + 2 * scale);
            g.setColour (haze); g.strokePath (ch, juce::PathStrokeType (1.3f * scale));
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (kind == toggle) { attachment.setValueAsCompleteGesture (param.getValue() > 0.5f ? 0.0f : 1.0f); return; }
        if (kind == choice)
        {
            juce::PopupMenu m;
            const int cur = (int) std::lround (plainOf (param));
            for (int i = 0; i < items.size(); ++i) m.addItem (i + 1, items[i], true, i == cur);
            juce::Component::SafePointer<StatusItem> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int id)
            {
                if (safe != nullptr && id > 0) safe->attachment.setValueAsCompleteGesture ((float) (id - 1));
            });
            return;
        }
        dragging = true; grabN = param.getValue(); grabY = e.position.y;
        attachment.beginGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (kind != value) return;
        const float k = e.mods.isShiftDown() ? 0.0008f : 0.004f;
        attachment.setValueAsPartOfGesture (param.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, grabN - (e.position.y - grabY) * k / scale)));
        if (e.mods.isShiftDown()) { grabN = param.getValue(); grabY = e.position.y; }
    }
    void mouseUp (const juce::MouseEvent&) override { if (kind == value && dragging) attachment.endGesture(); dragging = false; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (kind == value) attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

private:
    void timerCallback() override
    {
        const float t = param.getValue() > 0.5f ? 1.0f : 0.0f;
        if (std::abs (t - lit) > 0.01f) { lit += (t - lit) * 0.3f; repaint(); }
        else if (lit != t) { lit = t; repaint(); }
    }
    juce::RangedAudioParameter& param;
    juce::String label;
    Kind kind;
    juce::StringArray items;
    juce::ParameterAttachment attachment;
    float scale = 1.0f, grabN = 0, grabY = 0, lit = 0;
    bool dragging = false;
};
} // namespace ui
