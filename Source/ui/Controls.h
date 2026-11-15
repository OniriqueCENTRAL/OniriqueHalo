#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"
#include "../dsp/EclipseEngine.h"

//==============================================================================
//  Les commandes d'ECLIPSE, dans la DA ONIRIQUE : potard à graduations (OniriqueKnob.h), gélules et segments en verre
//  (OniriqueDA.h). Toutes se dessinent à partir de leur taille (l'éditeur les place en points × échelle).
//==============================================================================
namespace ui
{
//==============================================================================
/** Le potard ONIRIQUE « GRADUATIONS », le même que dans VOCALBOX. */
class KnobLook : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s) override
    {
        const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        onirique::ui::drawKnob (g, area, pos, start, end, accent, (bool) s.getProperties()["bipolar"], s.isMouseOverOrDragging());
        if (s.hasKeyboardFocus (false))
        {
            const float d = std::min (area.getWidth(), area.getHeight());
            g.setColour (accent.withAlpha (0.4f));
            g.drawEllipse (juce::Rectangle<float> (d * 0.72f, d * 0.72f).withCentre (area.getCentre()), 1.0f);
        }
    }
};

//==============================================================================
/** Un potard lié à un paramètre : étiquette au-dessus, valeur en dessous (en braise pendant le réglage). */
class Knob : public juce::Component
{
public:
    Knob (juce::RangedAudioParameter& p, const juce::String& labelText, KnobLook& look, bool bipolar = false)
        : param (p), label (labelText), attachment (p, slider)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::degreesToRadians (-135.0f), juce::degreesToRadians (135.0f), true);
        slider.setLookAndFeel (&look);
        slider.getProperties().set ("bipolar", bipolar);
        slider.setDoubleClickReturnValue (true, (double) p.convertFrom0to1 (p.getDefaultValue()));
        slider.setMouseDragSensitivity (220);
        slider.setPopupMenuEnabled (false);
        slider.setTitle (labelText);
        slider.onValueChange = [this] { repaint(); };
        slider.onDragStart = [this] { repaint(); };
        slider.onDragEnd = [this] { repaint(); };
        addAndMakeVisible (slider);
        setRepaintsOnMouseActivity (true);
    }
    ~Knob() override { slider.setLookAndFeel (nullptr); }

    void setScale (float s) { scale = s; resized(); }
    void setLabel (const juce::String& t) { if (label != t) { label = t; slider.setTitle (t); repaint(); } }

    void resized() override
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromTop (labelH());
        r.removeFromBottom (valueH());
        slider.setBounds (r.toNearestInt());
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        text (g, label, b.withHeight (labelH()), font (Face::body, 10.5f * scale, 0.16f), haze);
        const bool live = slider.isMouseOverOrDragging();
        text (g, param.getCurrentValueAsText(), b.withTrimmedTop (b.getHeight() - valueH()), font (Face::mono, 10.5f * scale),
              live ? onirique::look::emberPink : bone.withAlpha (0.85f));
    }

    juce::Slider slider;

private:
    float labelH() const { return 16.0f * scale; }
    float valueH() const { return 16.0f * scale; }
    juce::RangedAudioParameter& param;
    juce::String label;
    juce::SliderParameterAttachment attachment;
    float scale = 1.0f;
};

//==============================================================================
/** Bouton gélule de la DA (verre, allumé en dégradé braise, liseré rose), avec son témoin ; s'allume en fondu. */
class Switch : public juce::Button, private juce::Timer
{
public:
    Switch (juce::RangedAudioParameter& p, const juce::String& t) : juce::Button (t), attachment (p, *this)
    {
        setClickingTogglesState (true);
        setTitle (t);
        lit = getToggleState() ? 1.0f : 0.0f;
        onStateChange = [this] { startTimerHz (60); };
    }
    void setScale (float s) { scale = s; repaint(); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        hover += ((over ? 1.0f : 0.0f) - hover) * 0.5f;
        fullPill (g, r, lit, over ? 1.0f : 0.0f, scale);
        if (down) { g.setColour (juce::Colours::black.withAlpha (0.2f)); g.fillRoundedRectangle (r, r.getHeight() * 0.5f); }
        onirique::da::led (g, { r.getX() + r.getHeight() * 0.55f, r.getCentreY() }, 5.0f * scale, lit);
        ui::text (g, getButtonText(), r.withTrimmedLeft (r.getHeight() * 0.9f).withTrimmedRight (r.getHeight() * 0.3f),
                  font (Face::body, 10.5f * scale, 0.14f), bone.withAlpha (0.6f + 0.4f * lit));
    }

private:
    void timerCallback() override
    {
        const float target = getToggleState() ? 1.0f : 0.0f;
        lit += (target - lit) * 0.3f;
        if (std::abs (target - lit) < 0.01f) { lit = target; stopTimer(); }
        repaint();
    }
    juce::ButtonParameterAttachment attachment;
    float lit = 0.0f, hover = 0.0f, scale = 1.0f;
};

//==============================================================================
/** Sélecteur à segments (onglets de la DA) pour un paramètre à choix : SOLO, cible de l'OFFSET. */
class Segments : public juce::Component
{
public:
    Segments (juce::RangedAudioParameter& p, juce::StringArray names)
        : items (std::move (names)), attachment (p, [this] (float v) { index = (int) std::lround (v); repaint(); })
    {
        attachment.sendInitialUpdate();
        setTitle (p.getName (64));
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    void setScale (float s) { scale = s; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        onirique::da::glass (g, r, r.getHeight() * 0.5f, scale, false);
        const float w = (r.getWidth() - 1.0f * scale) / (float) items.size();
        for (int i = 0; i < items.size(); ++i)
        {
            const auto cell = juce::Rectangle<float> (r.getX() + 0.5f * scale + w * (float) i, r.getY() + 0.5f * scale, w, r.getHeight() - 1.0f * scale);
            onirique::da::seg (g, cell, i == index ? 1.0f : 0.0f, i == hover ? 1.0f : 0.0f, scale, cell.getHeight() * 0.5f);
            text (g, items[i], cell, font (Face::body, 10.0f * scale, 0.12f), i == index ? bone : haze);
        }
    }
    void mouseMove (const juce::MouseEvent& e) override { const int h = at (e.position.x); if (h != hover) { hover = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override { attachment.setValueAsCompleteGesture ((float) at (e.position.x)); }

private:
    int at (float x) const { return juce::jlimit (0, items.size() - 1, (int) (x / ((float) getWidth() / (float) items.size()))); }
    juce::StringArray items;
    juce::ParameterAttachment attachment;
    int index = 0, hover = -1;
    float scale = 1.0f;
};
} // namespace ui
