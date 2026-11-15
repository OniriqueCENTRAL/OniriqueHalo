#include "PluginEditor.h"
#include "OniriqueWindowSize.h"
#include "BinaryData.h"

namespace
{
const juce::String pluginName = "ONIRIQUE HALO";
using R = juce::Rectangle<float>;
juce::String fr (const char* s) { return juce::String::fromUTF8 (s); }

const juce::Point<float> big { 132, 162 }, medium { 104, 128 }, tiny { 58, 74 };

namespace layout
{
const R strip { 14, 58, 806, 200 }, parStrip { 828, 58, 258, 200 };
const R mainPanel { 14, 266, 806, 276 }, parPanel { 828, 266, 258, 276 };
const R status { 14, 550, 1072, 40 };
}
} // namespace

//==============================================================================
HaloEditor::HaloEditor (HaloProcessor& proc)
    : AudioProcessorEditor (proc), processor (proc),
      analyser (proc.apvts, proc.scope(), proc.meters(), proc.momentarySolo),
      parAnalyser (proc.apvts, proc.scope(), proc.meters(), proc.momentarySolo, true),
      thresholds (proc.apvts, proc.meters()),
      channel (p (params::stereo), { "L/R", "M/S" }),
      attack (p (params::attack), { "SLOW", "FAST" }), release (p (params::release), { "SLOW", "FAST" }),
      chLink (p (params::mainLink), "CH LINK", knobLook), trim (p (params::mainTrim), "TRIM", knobLook, true),
      ladder (proc.apvts, proc.meters()),
      mainPower (p (params::mainOn), "MAIN"),
      amount (proc.apvts, proc.meters()),
      parSolo ("S"),
      parChannel (p (params::parChannel), { "M", "S", "LR" }),
      parThr (p (params::parThr), "PARALIMIT", knobLook), parTrim (p (params::parTrim), "TRIM", knobLook, true),
      parLink (p (params::parLink), "LINK", knobLook),
      parPower (p (params::parOn), "PARALIMIT"),
      inItem (p (params::inGain), "in", ui::StatusItem::value),
      osItem (p (params::osRealtime), "os", ui::StatusItem::choice, { "1x", "2x", "4x", "8x" }),
      renderItem (p (params::osOffline), "render", ui::StatusItem::choice, { "1x", "2x", "4x", "8x" }),
      modeItem (p (params::quality), "mode", ui::StatusItem::choice, { "LINEAR", "DYNAMIC", "LIVE" }),
      bandItem (p (params::mainMode), "band", ui::StatusItem::choice, { "SPLIT", "DEFINED", "FULL HF" }),
      paraItem (p (params::parMode), "para", ui::StatusItem::choice, { "SPLIT", "DEFINED", "FULL HF" }),
      mixItem (p (params::mix), "mix", ui::StatusItem::value),
      deltaItem (p (params::delta), "DELTA", ui::StatusItem::toggle),
      outItem (p (params::outGain), "out", ui::StatusItem::value),
      matchItem (p (params::levelComp), "MATCH", ui::StatusItem::toggle),
      inMeter (proc.meters(), false), outMeter (proc.meters(), true),
      undoButton (ui::IconButton::undo, fr ("Annuler")), redoButton (ui::IconButton::redo, fr ("Rétablir")),
      settingsButton (ui::IconButton::settings, fr ("Réglages")),
      ext (proc.meters(), [&proc] { return proc.sidechainConnected(); }),
      parSoloWatch (p (params::parSolo), [this] (float v) { parSolo.setLit (v > 0.5f); }),
      bar (proc.presets, proc.apvts, "HALO", {}, baseWidth, baseWidth)
{
    onirique::look::load();
    setLookAndFeel (&menuLook);   // menus et infobulles dans la DA
    bar.onThemeChanged = [this]
    {
        renderBackground();
        std::function<void (juce::Component&)> all = [&all] (juce::Component& k) { k.repaint(); for (auto* ch : k.getChildren()) all (*ch); };
        all (*this);
    };
    {
        int size = 0;
        if (const char* data = BinaryData::getNamedResource ("logo_png", size))
            logo = juce::ImageFileFormat::loadFrom (data, (size_t) size);
    }

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &analyser, &parAnalyser, &thresholds, &channel, &attack, &release, &chLink, &trim, &ladder, &mainPower,
             &amount, &parSolo, &parChannel, &parThr, &parTrim, &parLink, &parPower,
             &inItem, &osItem, &renderItem, &modeItem, &bandItem, &paraItem, &mixItem, &deltaItem, &outItem, &matchItem,
             &inMeter, &outMeter, &undoButton, &redoButton, &settingsButton, &ext })
        addAndMakeVisible (c);
    parSoloWatch.sendInitialUpdate();

    parSolo.onClick = [this]
    {
        auto& s = p (params::parSolo);
        s.beginChangeGesture(); s.setValueNotifyingHost (s.getValue() > 0.5f ? 0.0f : 1.0f); s.endChangeGesture();
    };
    undoButton.onClick = [this] { processor.undo.undo(); };
    redoButton.onClick = [this] { processor.undo.redo(); };
    settingsButton.onClick = [this] { showSettings(); };
    analyser.onChip = [this] { showSettings(); };
    ext.onClick = [this] { showExt(); };
    modeItem.suffix = [this]
    {
        const double fs = processor.getSampleRate() > 0 ? processor.getSampleRate() : 48000.0;
        return "  " + juce::String (1000.0 * processor.currentLatency() / fs, 2) + " ms";
    };
    // chaque geste à la souris ouvre une nouvelle étape d'annulation
    addMouseListener (this, true);

    addAndMakeVisible (bar);
    bar.setBrand (300, [this] (juce::Graphics& g, juce::Rectangle<float> area) { paintTitle (g, area); });

#if HALO_LICENCE
    licenseOverlay = std::make_unique<onirique::license::Overlay> (pluginName, processor.license);
    addAndMakeVisible (*licenseOverlay);
   #if ONIRIQUE_UI_TEST
    if (juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_TEST", {}).contains ("nolicense"))
        licenseOverlay->setVisible (false);
   #endif
#endif

    // la taille gardée se lit AVANT de poser les limites (setResizeLimits redimensionne déjà l'éditeur)
    auto heightFor = [] (int w) { return juce::roundToInt ((float) w * (float) baseHeight / (float) baseWidth); };
    const int minW = juce::roundToInt (baseWidth * 0.75f), maxW = baseWidth * 2;
    const auto size = onirique::window::openingSize (processor.apvts.state, pluginName, { baseWidth, heightFor (baseWidth) },
                                                     { minW, heightFor (minW) }, { maxW, heightFor (maxW) });
    setResizable (true, true);
    setResizeLimits (minW, heightFor (minW), maxW, heightFor (maxW));
    getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
    setSize (size.x, heightFor (size.x));
    sizeReady = true;
    onirique::window::remember (processor.apvts.state, getWidth(), getHeight());
   #if ONIRIQUE_UI_TEST
    if (juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_TEST", {}).contains ("demo"))
        setSize (baseWidth, baseHeight);
   #endif

    updateTips();
    startTimerHz (30);

    // essais locaux : ONIRIQUE_UI_PNG=fichier → l'interface s'écrit elle-même en PNG (rendu logiciel)
    if (auto* png = std::getenv ("ONIRIQUE_UI_PNG"))
    {
        const juce::String path (png);
        juce::Component::SafePointer<HaloEditor> safe (this);
        if (juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_TEST", {}).contains ("settings"))
            juce::Timer::callAfterDelay (900, [safe] { if (safe != nullptr) safe->showSettings(); });
        if (juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_TEST", {}).contains ("browser"))
            juce::Timer::callAfterDelay (500, [safe] { if (safe != nullptr) safe->bar.openBrowser(); });
        juce::Timer::callAfterDelay (2500, [safe, path]
        {
            if (safe == nullptr) return;
            juce::Image img (juce::Image::ARGB, safe->getWidth(), safe->getHeight(), true, juce::SoftwareImageType());
            { juce::Graphics g (img); safe->paintEntireComponent (g, true); }
            juce::File f (path); f.deleteFile();
            if (auto os = f.createOutputStream()) juce::PNGImageFormat().writeImageToStream (img, *os);
            // et un menu dessiné par le style de la DA (les menus ont leur propre fenêtre, hors de la capture)
            if (juce::SystemStats::getEnvironmentVariable ("ONIRIQUE_UI_TEST", {}).contains ("menupng"))
            {
                auto& lf = safe->menuLook;
                const char* rows[] { "#HALO", "Analyseur : vitesse>", "Analyseur : référence tonale>", "Zoom des réductions>", "-",
                                     "*Modern Mix", " Neutral", "!Vocal" };
                juce::Image m (juce::Image::ARGB, 300, 8 * 28 + 10, true, juce::SoftwareImageType());
                juce::Graphics mg (m);
                lf.drawPopupMenuBackground (mg, m.getWidth(), m.getHeight());
                int y = 5;
                for (auto* rowText : rows)
                {
                    const juce::String t = juce::String::fromUTF8 (rowText);
                    const juce::Rectangle<int> area (5, y, 290, t == "-" ? 9 : 28);
                    if (t.startsWith ("#")) lf.drawPopupMenuSectionHeader (mg, area, t.substring (1));
                    else if (t == "-") lf.drawPopupMenuItem (mg, area, true, true, false, false, false, {}, {}, nullptr, nullptr);
                    else lf.drawPopupMenuItem (mg, area, false, true, t.startsWith ("!"), t.startsWith ("*"), t.endsWith (">"),
                                               t.substring (1).upToLastOccurrenceOf (">", false, false).trim().isEmpty() ? t : t.trimCharactersAtStart ("*!").trimCharactersAtEnd (">").trim(),
                                               {}, nullptr, nullptr);
                    y += area.getHeight();
                }
                juce::File mf (path.upToLastOccurrenceOf (".", false, false) + "-menu.png"); mf.deleteFile();
                if (auto os2 = mf.createOutputStream()) juce::PNGImageFormat().writeImageToStream (m, *os2);
            }
        });
    }
}

HaloEditor::~HaloEditor()
{
    removeMouseListener (this);
    setLookAndFeel (nullptr);
    processor.momentarySolo.store (-1);
    onirique::window::rememberOnThisComputer (pluginName, getWidth(), getHeight());
}

void HaloEditor::mouseDown (const juce::MouseEvent&)
{
    processor.undo.beginNewTransaction();
}

//==============================================================================
void HaloEditor::timerCallback()
{
    // la couleur d'interface choisie dans une autre fenêtre de ce plugin
    if (onirique::look::changedSince (themeSeen) && bar.onThemeChanged) bar.onThemeChanged();

    // les sections s'estompent en douceur quand leur moteur est éteint
    auto fade = [] (std::initializer_list<juce::Component*> cs, float target)
    {
        for (auto* c : cs)
        {
            const float a = c->getAlpha();
            if (std::abs (a - target) > 0.01f) c->setAlpha (a + (target - a) * 0.25f);
        }
    };
    const auto prm = processor.readParams();
    fade ({ &thresholds, &channel, &attack, &release, &chLink, &trim, &ladder }, prm.mainOn ? 1.0f : 0.4f);
    fade ({ &amount, &parSolo, &parChannel, &parThr, &parTrim, &parLink }, prm.parOn ? 1.0f : 0.4f);

    undoButton.setEnabled (processor.undo.canUndo());
    redoButton.setEnabled (processor.undo.canRedo());
    if (++tipTicks % 30 == 0) { updateTips(); modeItem.repaint(); }
}

void HaloEditor::updateTips()
{
    const auto prm = processor.readParams();
    auto ms = [&] (int q) { return juce::String (processor.latencyMs (q, prm.os), 2) + " ms"; };
    modeItem.setTooltip (fr ("LINEAR : séparation à phase linéaire (FIR), la plus fidèle, pour le mastering. Latence mesurée : ") + ms (0) + ".\n"
                         + fr ("DYNAMIC : séparation à phase minimale et anticipation courte : les transitoires des aigus restent entiers. Latence mesurée : ") + ms (1) + ".\n"
                         + fr ("LIVE : sans anticipation, suréchantillonnage le plus court, pour jouer ou enregistrer à travers. Latence mesurée : ") + ms (2) + ".");
    osItem.setTooltip (fr ("Suréchantillonnage pendant la lecture : la détection voit les crêtes entre les échantillons et la réduction ne replie pas."));
    renderItem.setTooltip (fr ("Suréchantillonnage du rendu hors ligne (export, bounce)."));
    bandItem.setTooltip (fr ("Bande de MAIN. SPLIT : tout au-dessus du drapeau bas, pente raide (24 dB/oct). DEFINED : seulement entre les deux drapeaux. "
                             "FULL HF : au-dessus du drapeau bas, pente douce (12 dB/oct), plus large et plus naturel."));
    paraItem.setTooltip (fr ("Bande de PARALIMIT (mêmes formes que MAIN)."));
    inItem.setTooltip (fr ("Gain d'entrée. À tirer ; Maj : réglage fin ; double-clic : 0 dB."));
    outItem.setTooltip (fr ("Gain de sortie. À tirer ; Maj : réglage fin ; double-clic : 0 dB."));
    mixItem.setTooltip (fr ("DRY/WET de MAIN : le signal d'origine mêlé au signal traité par MAIN. PARALIMIT reste une voie à part."));
    deltaItem.setTooltip (fr ("DELTA : seulement ce que HALO enlève ou ajoute (bonne polarité, bon alignement)."));
    matchItem.setTooltip (fr ("MATCH : la sortie garde le niveau moyen de l'entrée, pour comparer à l'oreille sans être trompé par le volume."));
    channel.setTooltip (fr ("CHANNEL : L/R (gauche et droite) ou M/S (centre et côtés), chacun avec son seuil."));
    attack.setTooltip (fr ("ATTACK : SLOW laisse respirer les transitoires des aigus, FAST les saisit tout de suite. Dans les deux cas l'attaque accélère à mesure que la réduction grandit."));
    release.setTooltip (fr ("RELEASE : SLOW rend la réduction en douceur, FAST récupère vite. Le relâchement s'adapte lui aussi à la réduction en cours."));
    chLink.setTip (fr ("CH LINK : 100 % = la même réduction des deux canaux (image stable), 0 % = deux canaux indépendants."));
    trim.setTip (fr ("TRIM : le niveau de la bande après limitation, de -6 à +6 dB, indépendant du seuil. La balance tonale d'un côté, la dynamique de l'autre."));
    mainPower.setTooltip (fr ("MAIN : en service ou non."));
    parPower.setTooltip (fr ("PARALIMIT : une bande parallèle, limitée fort puis ramenée à son niveau, ajoutée au signal : densité et présence dans les aigus."));
    parThr.setTip (fr ("Seuil de PARALIMIT : plus il est bas, plus la bande parallèle est dense."));
    parTrim.setTip (fr ("TRIM de la bande parallèle, de -6 à +6 dB."));
    parLink.setTip (fr ("Liaison stéréo de PARALIMIT."));
    parChannel.setTooltip (fr ("M : le centre. S : les côtés. LR : chaque canal. La limitation se fait toujours sur le vrai gauche / droite, avant l'extraction."));
    parSolo.setTooltip (fr ("S : écouter seulement ce que PARALIMIT ajoute."));
    undoButton.setTooltip (fr ("Annuler le dernier réglage."));
    redoButton.setTooltip (fr ("Rétablir le réglage annulé."));
    settingsButton.setTooltip (fr ("Réglages : analyseur, zoom des réductions, temps de PARALIMIT, latences mesurées."));
}

//==============================================================================
void HaloEditor::showExt()
{
    juce::PopupMenu m;
    const bool connected = processor.sidechainConnected();
    m.addSectionHeader (connected ? fr ("SIDECHAIN BRANCHÉE") : fr ("SIDECHAIN ABSENTE (à brancher dans le DAW)"));
    m.addItem (1, fr ("EXT commande MAIN"), true, p (params::scMain).getValue() > 0.5f);
    m.addItem (2, fr ("EXT commande PARALIMIT"), true, p (params::scPar).getValue() > 0.5f);
    juce::Component::SafePointer<HaloEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&ext), [safe] (int id)
    {
        if (safe == nullptr || id == 0) return;
        auto& prm = safe->p (id == 1 ? params::scMain : params::scPar);
        prm.beginChangeGesture(); prm.setValueNotifyingHost (prm.getValue() > 0.5f ? 0.0f : 1.0f); prm.endChangeGesture();
    });
}

void HaloEditor::showSettings()
{
    auto& st = processor.apvts.state;
    juce::PopupMenu menu, speed, ref, zoom, parTimes, soloMenu, lat;
    const int sp = (int) st.getProperty ("anaSpeed", 1), rf = (int) st.getProperty ("anaRef", 4), zm = (int) st.getProperty ("grZoom", 1);
    speed.addItem (101, "Lente", true, sp == 0);
    speed.addItem (102, "Moyenne", true, sp == 1);
    speed.addItem (103, "Rapide", true, sp == 2);
    ref.addItem (206, fr ("Aucune (spectre seul)"), true, rf == 4);
    ref.addItem (201, "Neutral", true, rf == 0);
    ref.addItem (202, "Modern Mix", true, rf == 1);
    ref.addItem (203, "Vocal", true, rf == 2);
    ref.addItem (204, "Custom", st.hasProperty ("customRef"), rf == 3);
    ref.addSeparator();
    ref.addItem (205, fr ("Capturer Custom (le spectre du moment)"));
    zoom.addItem (301, "3 dB", true, zm == 0);
    zoom.addItem (302, "6 dB", true, zm == 1);
    zoom.addItem (303, "12 dB", true, zm == 2);
    const bool pa = p (params::parAttack).getValue() > 0.5f, pr = p (params::parRelease).getValue() > 0.5f;
    parTimes.addItem (401, "ATTACK SLOW", true, ! pa);
    parTimes.addItem (402, "ATTACK FAST", true, pa);
    parTimes.addSeparator();
    parTimes.addItem (403, "RELEASE SLOW", true, ! pr);
    parTimes.addItem (404, "RELEASE FAST", true, pr);
    const int so = (int) std::lround (ui::plainOf (p (params::mainSolo)));
    const char* soloNames[] { "OFF", "BANDE TRAITÉE", "AU-DESSUS DU DRAPEAU BAS", "AU-DESSUS DU DRAPEAU HAUT", "RESTE (HORS BANDE)" };
    for (int i = 0; i < 5; ++i) soloMenu.addItem (601 + i, fr (soloNames[i]), true, so == i);
    const char* qn[] { "LINEAR", "DYNAMIC", "LIVE" };
    for (int q = 0; q < 3; ++q)
    {
        juce::String line = juce::String (qn[q]) + "   ";
        for (int o = 0; o < 4; ++o) line << (1 << o) << "x " << juce::String (processor.latencyMs (q, o), 2) << " ms   ";
        lat.addItem (500 + q, line, false);
    }
    menu.addSectionHeader ("HALO");
    menu.addSubMenu ("Analyseur : vitesse", speed);
    menu.addSubMenu (fr ("Analyseur : référence tonale"), ref);
    menu.addSubMenu (fr ("Zoom des réductions"), zoom);
    menu.addSubMenu (fr ("Solo de MAIN"), soloMenu);
    menu.addSubMenu ("PARALIMIT : attack / release", parTimes);
    menu.addSeparator();
    menu.addSubMenu (fr ("Latences mesurées"), lat);

    juce::Component::SafePointer<HaloEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&settingsButton), [safe] (int id)
    {
        if (safe == nullptr || id == 0) return;
        auto& s = safe->processor.apvts.state;
        auto setParam = [safe] (const juce::String& pid, float plain)
        {
            auto& prm = safe->p (pid);
            prm.beginChangeGesture(); prm.setValueNotifyingHost (prm.convertTo0to1 (plain)); prm.endChangeGesture();
        };
        if (id >= 101 && id <= 103) s.setProperty ("anaSpeed", id - 101, nullptr);
        else if (id >= 201 && id <= 204) s.setProperty ("anaRef", id - 201, nullptr);
        else if (id == 206) s.setProperty ("anaRef", 4, nullptr);
        else if (id == 205) { safe->analyser.captureCustom(); s.setProperty ("anaRef", 3, nullptr); }
        else if (id >= 301 && id <= 303) s.setProperty ("grZoom", id - 301, nullptr);
        else if (id == 401 || id == 402) setParam (params::parAttack, (float) (id - 401));
        else if (id == 403 || id == 404) setParam (params::parRelease, (float) (id - 403));
        else if (id >= 601 && id <= 605) setParam (params::mainSolo, (float) (id - 601));
        safe->repaint();
    });
}

//==============================================================================
void HaloEditor::resized()
{
    const float s = scale();
    auto place = [s] (juce::Component& c, R r) { c.setBounds (R (r.getX() * s, r.getY() * s, r.getWidth() * s, r.getHeight() * s).toNearestInt()); };
    auto knob = [&] (ui::Knob& k, float cx, float cy, juce::Point<float> sz) { place (k, R (sz.x, sz.y).withCentre ({ cx, cy })); k.setScale (s); };
    auto seg = [&] (ui::Segments& g, R r) { place (g, r); g.setScale (s); };
    auto item = [&] (ui::StatusItem& i, R r) { place (i, r); i.setScale (s); };

    place (analyser, layout::strip); analyser.setScale (s);
    place (parAnalyser, layout::parStrip); parAnalyser.setScale (s);

    // MAIN
    {
        const float x = layout::mainPanel.getX(), y = layout::mainPanel.getY();
        place (thresholds, R (x + 16, y + 12, 150, 254)); thresholds.setScale (s);
        seg (channel, R (x + 196, y + 58, 112, 24));
        seg (attack, R (x + 196, y + 124, 112, 24));
        seg (release, R (x + 196, y + 190, 112, 24));
        knob (chLink, x + 414, y + 132, big);
        place (ladder, R (x + 526, y + 16, 120, 244)); ladder.setScale (s);
        knob (trim, x + 724, y + 108, medium);
        place (mainPower, R (36, 36).withCentre ({ x + 724, y + 214 })); mainPower.setScale (s);
    }
    // PARALIMIT
    {
        const float x = layout::parPanel.getX(), y = layout::parPanel.getY();
        place (amount, R (x + 10, y + 10, 70, 226)); amount.setScale (s);
        place (parSolo, R (x + 22, y + 242, 32, 22)); parSolo.setScale (s);
        seg (parChannel, R (x + 106, y + 40, 128, 22));
        knob (parThr, x + 170, y + 132, medium);
        place (parPower, R (30, 30).withCentre ({ x + 170, y + 222 })); parPower.setScale (s);
        knob (parTrim, x + 114, y + 236, tiny);
        knob (parLink, x + 226, y + 236, tiny);
    }
    // barre d'état
    {
        const float y = layout::status.getY() + 5;
        item (inItem, R (24, y, 92, 30));
        place (inMeter, R (120, y + 7, 54, 16)); inMeter.setScale (s);
        item (osItem, R (190, y, 70, 30));
        item (renderItem, R (266, y, 94, 30));
        item (modeItem, R (370, y, 172, 30));
        item (bandItem, R (550, y, 104, 30));
        item (paraItem, R (660, y, 104, 30));
        item (mixItem, R (772, y, 72, 30));
        item (deltaItem, R (850, y, 56, 30));
        item (outItem, R (912, y, 82, 30));
        place (outMeter, R (998, y + 7, 30, 16)); outMeter.setScale (s);
        item (matchItem, R (1032, y, 52, 30));
    }

    // la barre du haut et la pastille de licence
#if HALO_LICENCE
    const int pillW = licenseOverlay != nullptr ? licenseOverlay->pillWidth() : 0;
#else
    const int pillW = 0;
#endif
    bar.setPillWidth (pillW);
    bar.place (s, getLocalBounds());
    {
        // annuler, rétablir, EXT, réglages : à gauche de la pastille de licence
        const auto pill = bar.getPillArea().toFloat();
        float right = (pillW > 0 ? pill.getX() : pill.getRight()) - 10;
        const float cy = pill.getHeight() > 0 ? pill.getCentreY() : 25;
        auto put = [&] (juce::Component& c, float w) { place (c, R (right - w, cy - 15, w, 30)); right -= w + 6; };
        put (settingsButton, 30); settingsButton.setScale (s);
        put (ext, 64); ext.setScale (s);
        put (redoButton, 30); redoButton.setScale (s);
        put (undoButton, 30); undoButton.setScale (s);
    }
#if HALO_LICENCE
    if (licenseOverlay != nullptr)
        licenseOverlay->fit (getLocalBounds(), s, bar.getPillArea(), juce::Justification::centredRight);
#endif
    renderBackground();
    if (sizeReady) onirique::window::remember (processor.apvts.state, getWidth(), getHeight());
}

//==============================================================================
void HaloEditor::renderBackground()
{
    const float k = juce::jmax (1.0f, (float) juce::Component::getApproximateScaleFactorForComponent (this));
    background = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) getWidth() * k)),
                              juce::jmax (1, juce::roundToInt ((float) getHeight() * k)), true);
    juce::Graphics g (background);
    {
        const float u = scale() * k;
        const auto night = onirique::da::backdrop (background.getWidth(), background.getHeight(), u);
        g.drawImageAt (night, 0, 0);
    }
    g.addTransform (juce::AffineTransform::scale (scale() * k));
    using namespace ui;

    for (const auto& r : { layout::mainPanel, layout::parPanel, layout::status })
        onirique::da::glass (g, r, 14.0f, 1.0f);

    // MAIN : légendes de la colonne, icônes du CH LINK, séparation
    {
        const float x = layout::mainPanel.getX(), y = layout::mainPanel.getY();
        const auto lf = font (Face::display, 11.5f, 0.16f);
        text (g, "CHANNEL", R (x + 186, y + 34, 132, 18), lf, bone);
        text (g, "ATTACK", R (x + 186, y + 100, 132, 18), lf, bone);
        text (g, "RELEASE", R (x + 186, y + 166, 132, 18), lf, bone);
        // à gauche : deux canaux séparés (0 %), à droite : liés (100 %)
        const juce::Point<float> l (x + 330, y + 160), r (x + 498, y + 160);
        g.setColour (haze.withAlpha (0.7f));
        g.drawEllipse (R (11, 11).withCentre (l.translated (-6, 0)), 1.3f);
        g.drawEllipse (R (11, 11).withCentre (l.translated (6, 0)), 1.3f);
        g.drawEllipse (R (11, 11).withCentre (r.translated (-2.5f, 0)), 1.3f);
        g.drawEllipse (R (11, 11).withCentre (r.translated (2.5f, 0)), 1.3f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        for (float sx : { x + 178, x + 330 - 22, x + 512, x + 660 })
            g.fillRect (sx, y + 20, 1.0f, layout::mainPanel.getHeight() - 40);
        text (g, "MAIN", R (x + 674, y + 236, 100, 14), font (Face::body, 8.5f, 0.2f), dim);
    }
    // PARALIMIT : titre, flèches de densité
    {
        const float x = layout::parPanel.getX(), y = layout::parPanel.getY();
        text (g, "AMOUNT", R (x + 4, y + 262, 80, 12), font (Face::body, 7.5f, 0.2f), dim);
        g.setColour (onirique::look::emberPink.withAlpha (0.55f));
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < 4; ++i)
            {
                const float cx = (side == 0 ? x + 98 : x + 226) + (float) i * 5.0f, cy = y + 140;
                juce::Path a; a.addTriangle (cx, cy - 3.5f, cx, cy + 3.5f, cx + 4, cy);
                g.fillPath (a);
            }
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRect (x + 88, y + 20, 1.0f, layout::parPanel.getHeight() - 40);
    }
}

// Le titre, dans la barre : le sigil, un halo (anneau de braise et sa lueur), ONIRIQUE, HALO
void HaloEditor::paintTitle (juce::Graphics& g, juce::Rectangle<float> area) const
{
    using namespace ui;
    const float cy = 25.0f;
    float x = area.getX();
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    if (logo.isValid()) g.drawImage (logo, R (x, cy - 13, 28, 26), juce::RectanglePlacement::centred);
    x += 36;
    {
        const juce::Point<float> c (x + 12, cy);
        juce::ColourGradient glow (accent.withAlpha (0.0f), c.x, c.y, accent.withAlpha (0.0f), c.x + 15, c.y, true);
        glow.addColour (0.55, accent.withAlpha (0.55f));
        g.setGradientFill (glow);
        g.fillEllipse (R (30, 30).withCentre (c));
        g.setColour (accentLit);
        g.drawEllipse (R (17, 17).withCentre (c), 1.6f);
        g.setColour (bone.withAlpha (0.9f));
        g.fillEllipse (R (3, 3).withCentre (c.translated (0, -8.5f)));
    }
    x += 32;
    text (g, "ONIRIQUE", R (x, cy - 16, 120, 14), font (Face::display, 11.0f, 0.14f), bone.withAlpha (0.75f), juce::Justification::centredLeft);
    text (g, "HALO", R (x - 1, cy - 4, 160, 22), font (Face::display, 20.0f, 0.1f), accent, juce::Justification::centredLeft);
    const float w = juce::GlyphArrangement::getStringWidth (font (Face::display, 20.0f, 0.1f), "HALO");
    text (g, "HF ACCELERATION LIMITER", R (x + w + 10, cy + 1, 170, 14), font (Face::body, 8.5f, 0.14f), dim, juce::Justification::centredLeft);
}

void HaloEditor::paint (juce::Graphics& g)
{
    if (background.isValid())
        g.drawImage (background, getLocalBounds().toFloat());
    else
        g.fillAll (ui::ink);
}
