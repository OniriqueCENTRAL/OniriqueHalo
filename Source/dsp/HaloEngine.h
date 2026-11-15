#pragma once

#include "HaloDsp.h"
#include <array>
#include <mutex>

//==============================================================================
//  ONIRIQUE HALO : la chaîne complète (voir Documentation/DSP.md).
//
//    entrée (gain) ─┬─ séparation MAIN  (FIR à phase linéaire | LR IIR) → bande bm, et a(x)
//                   ├─ séparation PARALIMIT (ses propres drapeaux)        → bande bp
//    sidechain ─────┴─ mêmes séparations                                   → clés km, kp (EXT)
//      → suréchantillonnage 1×/2×/4×/8× des bandes et des clés
//      → MAIN : détection (LR ou MS), anticipation, gain adaptatif, liaison  → écart (g·trim − 1)·bm
//      → PARALIMIT : limitation en vrai L/R, rattrapage, extraction M/S/LR   → bande dense ajoutée
//      → sous-échantillonnage des écarts → y = a(x) + mix·écart + parallèle
//    → écoute (DELTA, SOLO), compensation de niveau, sortie, bypass en fondu.
//
//  Les trois qualités sont trois chaînes différentes :
//    LINEAR   séparation FIR à phase linéaire, suréchantillonnage FIR à phase linéaire, anticipation
//    DYNAMIC  séparation IIR Linkwitz-Riley (phase minimale), suréchantillonnage FIR, anticipation
//    LIVE     séparation IIR, suréchantillonnage IIR polyphase (le plus court), sans anticipation
//  La latence de chaque chaîne est MESURÉE au prepare (impulsion et corrélation sur les vrais étages) et c'est elle
//  qui est déclarée.
//
//  Thread audio : aucune allocation (tout est réservé dans prepare), aucun verrou.
//==============================================================================
namespace halo
{
enum Quality { linear = 0, dynamic = 1, live = 2 };

struct EngineParams
{
    // MAIN
    bool mainOn = true;
    double mainLo = 4000, mainHi = 20000; int mainMode = 0;
    double thrA = -18, thrB = -18;     // dB : L / R, ou M / S
    double mainTrim = 0;               // dB
    bool attackFast = false, releaseFast = false;
    int stereo = 0;                    // 0 LR, 1 MS
    double mainLink = 1.0;             // 0 … 1
    int mainSolo = 0;                  // 0 off, 1 bande traitée, 2 au-dessus du drapeau bas, 3 au-dessus du drapeau haut, 4 reste (hors bande)
    bool scMain = false;
    double mix = 1.0;                  // 0 … 1
    // PARALIMIT
    bool parOn = false;
    double parLo = 6000, parHi = 20000; int parMode = 0;
    double parThr = -30, parAmountDb = -12, parTrim = 0;
    bool parAttackFast = true, parReleaseFast = false;
    int parChannel = 2;                // 0 MID, 1 SIDE, 2 LR
    double parLink = 1.0;
    bool parSolo = false, scPar = false;
    // général
    double inDb = 0, outDb = 0;
    bool levelComp = false, delta = false, bypass = false;
    int quality = dynamic;
    int os = 1;                        // 0 1×, 1 2×, 2 4×, 3 8× (déjà choisi : temps réel ou rendu)
};

/** Ce que l'interface lit (écrit par le thread audio). */
struct Meters
{
    std::atomic<float> inPeak[2] {}, outPeak[2] {};
    std::atomic<float> bandA { -120 }, bandB { -120 };   // niveau de la bande détectée (dBFS)
    std::atomic<float> grA {}, grB {}, grPL {}, grPR {}; // réduction, dB (≥ 0)
    std::atomic<float> parMakeup {}, compDb {};
    std::atomic<float> extLevel { -120 };
    std::atomic<bool> extUsed { false };
};

/** Les derniers échantillons (mono) avant et après, pour l'analyseur. */
struct Scope
{
    static constexpr int size = 1 << 15, mask = size - 1;
    std::vector<float> pre, post;
    std::atomic<int> write { 0 };
    std::atomic<double> rate { 48000 };
    void prepare (double fs) { pre.assign ((size_t) size, 0.0f); post.assign ((size_t) size, 0.0f); write.store (0); rate.store (fs); }
    /** thread de l'interface : les n derniers (n ≤ size / 2) */
    void read (float* outPre, float* outPost, int n) const
    {
        if (pre.empty()) { std::fill (outPre, outPre + n, 0.0f); std::fill (outPost, outPost + n, 0.0f); return; }
        const int w = write.load();
        for (int i = 0; i < n; ++i)
        {
            const int idx = (w - n + i) & mask;
            outPre[i] = pre[(size_t) idx]; outPost[i] = post[(size_t) idx];
        }
    }
};

class Engine
{
public:
    static constexpr int sub = 32;          // pas de contrôle (échantillons à la fréquence de base)
    static constexpr int osChannels = 12;
    static constexpr int factors[4] { 1, 2, 4, 8 };

    //==========================================================================
    void prepare (double sampleRate, int maxBlock, const EngineParams& p)
    {
        // tout le prepare sous le verrou du dessin des FIR : la minuterie (updateFir) ne touche jamais un convolueur
        // en cours de réallocation
        std::lock_guard<std::mutex> lock (designLock);
        fs = sampleRate;
        maxN = std::max (1, maxBlock);
        base.setSize (osChannels, sub);
        aux.setSize (8, sub);

        // ---- séparations IIR
        ctlMain.prepare (fs, BandEdges::make (p.mainLo, p.mainHi, p.mainMode));
        ctlPar.prepare (fs, BandEdges::make (p.parLo, p.parHi, p.parMode));
        for (auto& b : iirMain) { b.reset(); b.set (ctlMain.gl, ctlMain.gh); }
        for (auto& b : iirPar) { b.reset(); b.set (ctlPar.gl, ctlPar.gh); }
        for (auto& b : soloBand) { b.reset(); b.set (ctlMain.gl, ctlMain.gh); }
        ctrlA = 1.0 - std::exp (-(double) sub / (0.03 * fs));

        // ---- séparations FIR (longueur fixe pour cette fréquence : la latence ne dépend pas des drapeaux)
        firN = firLengthFor (fs);
        partition = fs <= 50000 ? 128 : fs <= 100000 ? 256 : 512;
        convMain.prepare (partition, firN, 4);
        convPar.prepare (partition, firN, 4);
        {
            int order = 0; while ((1 << order) < 4 * firN) ++order;
            designFft = std::make_unique<juce::dsp::FFT> (order);
            designScratch.assign ((size_t) (2 << order), 0.0f);
            designTaps.assign ((size_t) firN, 0.0f);
            loadedMain = BandEdges::make (p.mainLo, p.mainHi, p.mainMode);
            loadedPar = BandEdges::make (p.parLo, p.parHi, p.parMode);
            designBandFir (loadedMain, fs, designTaps, *designFft, designScratch);
            convMain.loadNow (designTaps.data(), firN);
            designBandFir (loadedPar, fs, designTaps, *designFft, designScratch);
            convPar.loadNow (designTaps.data(), firN);
        }

        // ---- suréchantillonnage : FIR à phase linéaire (latence entière) et IIR polyphase (le plus court)
        for (int i = 1; i < 4; ++i)
        {
            osFir[i] = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) osChannels, (size_t) i,
                           juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
            osIir[i] = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) osChannels, (size_t) i,
                           juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
            osFir[i]->initProcessing ((size_t) sub);
            osIir[i]->initProcessing ((size_t) sub);
        }

        // ---- anticipation (LINEAR, DYNAMIC) : réservée pour 8×
        lookahead = std::max (1, (int) std::lround (0.0015 * fs));
        const int maxOs = 8;
        for (auto& d : laDelay) d.prepare (lookahead * maxOs + 16);
        const int holdMax = (int) std::ceil (0.5 / BandEdges::minFreq * fs * maxOs) + 4;
        for (auto& m : maxMain) m.prepare (std::max (lookahead * maxOs + 18, holdMax));
        for (auto& m : maxPar) m.prepare (std::max (lookahead * maxOs + 18, holdMax));

        // ---- mesures : la latence réelle de chaque étage
        measureLatencies();

        const int maxLat = *std::max_element (&latencyTable[0][0], &latencyTable[0][0] + 12) + 8;
        for (auto& d : axDelay) d.prepare (maxLat);
        for (auto& d : bDelay) d.prepare (maxLat);
        for (auto& d : dryDelay) d.prepare (maxLat);
        for (auto& d : soloLoDelay) d.prepare (maxLat);
        for (auto& d : soloHiDelay) d.prepare (maxLat);

        scope.prepare (fs);
        setBaseSmoothing();
        activeQuality = std::clamp (p.quality, 0, 2);
        activeOs = std::clamp (p.os, 0, 3);
        configure();
        structGain = 1.0; structTarget = 1.0;
        firstBlock = true;
    }

    /** La latence déclarée de la chaîne en service (mesurée). */
    int getLatency() const { return latencyTable[activeQuality][activeOs]; }
    /** La même, lisible depuis un autre thread (minuterie du processeur). */
    int getLatencyShared() const { return latencyNow.load(); }
    /** La latence mesurée d'une chaîne (qualité, facteur 0 … 3). */
    int latencyFor (int quality, int os) const { return latencyTable[std::clamp (quality, 0, 2)][std::clamp (os, 0, 3)]; }
    /** La chaîne qui va être en service (la latence à déclarer pendant le fondu de changement). */
    int pendingLatency() const { return latencyTable[wantQuality][wantOs]; }

    Meters meters;
    Scope scope;

    //==========================================================================
    /** Hors du thread audio (minuterie de l'interface ou du processeur) : recalcule les réponses FIR quand les
        drapeaux ou la forme ont changé. Les nouvelles réponses sont fondues par le thread audio. */
    void updateFir (const BandEdges& m, const BandEdges& p)
    {
        std::lock_guard<std::mutex> lock (designLock);
        if (designFft == nullptr) return;
        if (m != loadedMain)
        {
            designBandFir (m, fs, designTaps, *designFft, designScratch);
            if (convMain.load (designTaps.data(), firN)) loadedMain = m;
        }
        if (p != loadedPar)
        {
            designBandFir (p, fs, designTaps, *designFft, designScratch);
            if (convPar.load (designTaps.data(), firN)) loadedPar = p;
        }
    }

    //==========================================================================
    /** io : 1 ou 2 canaux, en place. sc : la sidechain (0, 1 ou 2 canaux), ou nullptr. */
    void process (juce::AudioBuffer<float>& io, const float* const* sc, int scChannels, const EngineParams& p)
    {
        const int n = io.getNumSamples();
        const int nch = std::min (2, io.getNumChannels());
        if (n <= 0 || nch <= 0) return;

        wantQuality = std::clamp (p.quality, 0, 2);
        wantOs = std::clamp (p.os, 0, 3);
        if (firstBlock) { snapSmoothers (p); firstBlock = false; }

        float* ch[2] { io.getWritePointer (0), io.getWritePointer (nch > 1 ? 1 : 0) };
        for (int start = 0; start < n; start += sub)
        {
            const int len = std::min (sub, n - start);
            const float* s[2] { nullptr, nullptr };
            if (sc != nullptr && scChannels > 0)
            {
                s[0] = sc[0] + start;
                s[1] = (scChannels > 1 ? sc[1] : sc[0]) + start;
            }
            block (ch[0] + start, nch > 1 ? ch[1] + start : nullptr, s[0], s[1], len, p);
        }

        // relevés du bloc
        auto put = [] (std::atomic<float>& a, float v) { a.store (v); };
        put (meters.inPeak[0], (float) inPk[0]); put (meters.inPeak[1], (float) inPk[1]);
        put (meters.outPeak[0], (float) outPk[0]); put (meters.outPeak[1], (float) outPk[1]);
        put (meters.bandA, (float) gainToDb (bandPk[0])); put (meters.bandB, (float) gainToDb (bandPk[1]));
        put (meters.grA, (float) grPk[0]); put (meters.grB, (float) grPk[1]);
        put (meters.grPL, (float) grPk[2]); put (meters.grPR, (float) grPk[3]);
        put (meters.parMakeup, (float) gainToDb (makeup.v));
        put (meters.compDb, (float) gainToDb (compGain.v));
        put (meters.extLevel, (float) gainToDb (extPk));
        meters.extUsed.store (sc != nullptr && (p.scMain || (p.parOn && p.scPar)));
        inPk[0] = inPk[1] = outPk[0] = outPk[1] = bandPk[0] = bandPk[1] = extPk = 0;
        for (auto& g : grPk) g = 0;
    }

private:
    //==========================================================================
    void measureLatencies()
    {
        // FIR de séparation : une impulsion dans le convolueur, la position du maximum de la réponse
        {
            PartitionedConvolver probe;
            probe.prepare (partition, firN, 1);
            std::vector<float> h ((size_t) firN);
            designBandFir (BandEdges::make (4000, 20000, 0), fs, h, *designFft, designScratch);
            probe.loadNow (h.data(), firN);
            const int total = firN + 4 * partition;
            std::vector<float> v ((size_t) total, 0.0f);
            v[0] = 1.0f;
            float* d[1] { v.data() };
            const bool on[1] { true };
            probe.process (d, total, on);
            int best = 0;
            for (int i = 1; i < total; ++i) if (std::abs (v[(size_t) i]) > std::abs (v[(size_t) best])) best = i;
            firLatency = best;
        }
        // suréchantillonnage FIR (phase linéaire) : son retard exact, souvent fractionnaire ; on le complète jusqu'à
        // l'échantillon entier suivant par un retard dans le domaine suréchantillonné (osPad), l'alignement est exact.
        // LIVE : la chaîne réelle (passe-tout IIR + suréchantillonnage IIR), retard au pic de corrélation.
        osLatFir[0] = osLatIir[0] = osPad[0] = 0;
        for (int i = 1; i < 4; ++i)
        {
            const double frac = measureOsFraction (*osFir[i], factors[i]);
            osLatFir[i] = (int) std::ceil (frac - 1.0e-9);
            osPad[i] = (int) std::lround ((osLatFir[i] - frac) * factors[i]);
            osLatIir[i] = measureOs (*osIir[i], true);
        }
        for (int o = 0; o < 4; ++o)
        {
            latencyTable[linear][o] = firLatency + osLatFir[o] + lookahead;
            latencyTable[dynamic][o] = osLatFir[o] + lookahead;
            latencyTable[live][o] = osLatIir[o];
        }
    }

    /** Fait passer un signal par la montée et la descente d'un suréchantillonneur (et, pour LIVE, d'abord par le
        passe-tout de la séparation IIR : la chaîne réelle). */
    std::vector<float> throughOs (juce::dsp::Oversampling<float>& os, const std::vector<float>& in, bool withIirRest)
    {
        os.reset();
        IirBand band; band.reset(); band.set (ctlMain.gl, ctlMain.gh);
        const int total = (int) in.size();
        std::vector<float> out ((size_t) total, 0.0f);
        juce::AudioBuffer<float> b (1, sub);
        for (int start = 0; start + sub <= total; start += sub)
        {
            for (int i = 0; i < sub; ++i)
            {
                double v = in[(size_t) (start + i)];
                if (withIirRest) { band.tick (v, ctlMain.w); v = band.ax; }
                b.setSample (0, i, (float) v);
            }
            juce::dsp::AudioBlock<float> blk (b);
            os.processSamplesUp (blk);
            os.processSamplesDown (blk);
            for (int i = 0; i < sub; ++i) out[(size_t) (start + i)] = b.getSample (0, i);
        }
        os.reset();
        return out;
    }

    /** Retard au pic de corrélation sur du bruit blanc (le retard « du programme », large bande). */
    int measureOs (juce::dsp::Oversampling<float>& os, bool withIirRest)
    {
        const int total = 8192;
        std::vector<float> ref ((size_t) total);
        uint32_t seed = 12345u;
        for (auto& v : ref) { seed = seed * 1664525u + 1013904223u; v = (float) ((double) (seed >> 8) / 16777216.0 - 0.5); }
        return measureLag (ref, throughOs (os, ref, withIirRest), 512);
    }

    /** Retard exact (fractionnaire) d'un suréchantillonnage FIR à phase linéaire : la phase d'un sinus à fs / 32,
        déroulée autour du pic de corrélation, arrondie au pas du domaine suréchantillonné (1 / facteur). */
    double measureOsFraction (juce::dsp::Oversampling<float>& os, int factor)
    {
        const int coarse = measureOs (os, false);
        const int total = 8192;
        const double f = fs / 32.0, w = 2 * pi * f / fs;
        std::vector<float> x ((size_t) total);
        for (int i = 0; i < total; ++i) x[(size_t) i] = (float) (0.5 * std::sin (w * i));
        const auto y = throughOs (os, x, false);
        double re = 0, im = 0, rx = 0, ix = 0;
        for (int i = total / 2; i < total; ++i)
        {
            re += y[(size_t) i] * std::cos (w * i); im += y[(size_t) i] * std::sin (w * i);
            rx += x[(size_t) i] * std::cos (w * i); ix += x[(size_t) i] * std::sin (w * i);
        }
        // atan2(Σ y·sin, Σ y·cos) = π/2 − φ : un retard d donne +ω·d d'écart
        double d = (std::atan2 (im, re) - std::atan2 (ix, rx)) / w;
        while (d < coarse - 16.0) d += 32.0;
        while (d > coarse + 16.0) d -= 32.0;
        return std::round (d * factor) / factor;
    }

    /** Met en place la chaîne choisie (thread audio, pendant le silence du fondu, ou au prepare). */
    void configure()
    {
        const int q = activeQuality, o = activeOs, f = factors[o];
        fsO = fs * f;
        osObj = o == 0 ? nullptr : (q == live ? osIir[o].get() : osFir[o].get());
        if (osObj != nullptr) osObj->reset();
        const int osLat = q == live ? osLatIir[o] : osLatFir[o];
        const int la = q == live ? 0 : lookahead;
        laOs = la * f + (q == live ? 0 : osPad[o]);
        latencyNow.store (latencyTable[q][o]);
        for (auto& d : laDelay) { d.reset(); d.setDelay (laOs); }
        for (auto& m : maxMain) m.reset();
        for (auto& m : maxPar) m.reset();
        const int total = latencyTable[q][o];
        for (int c = 0; c < 2; ++c)
        {
            axDelay[c].reset(); bDelay[c].reset(); dryDelay[c].reset();
            axDelay[c].setDelay (q == linear ? firLatency + osLat + la : q == dynamic ? osLat + la : 0);
            bDelay[c].setDelay (q == live ? 0 : osLat + la);
            dryDelay[c].setDelay (total);
            soloLoDelay[c].reset(); soloHiDelay[c].reset(); soloBand[c].reset();
            soloLoDelay[c].setDelay (total); soloHiDelay[c].setDelay (total);
        }
        for (auto& b : iirMain) b.reset();
        for (auto& b : iirPar) b.reset();
        convMain.reset(); convPar.reset();
        for (auto& l : limMain) l.reset();
        for (auto& l : limPar) l.reset();
        tabMain.fs = tabPar.fs = 0;   // à refaire à la nouvelle fréquence
        for (auto* s : { &thrAS, &thrBS, &trimS, &onS, &linkS, &pThrS, &pGainS, &pLinkS, &pOnS })
            s->setTime (0.02, fsO);
        makeup.setTime (0.3, fsO);
        pinPow = poutPow = 0;
        powA = 1.0 - std::exp (-1.0 / (0.3 * fsO));
    }

    void snapSmoothers (const EngineParams& p)
    {
        thrAS.snap (dbToGain (p.thrA)); thrBS.snap (dbToGain (p.thrB));
        trimS.snap (dbToGain (p.mainTrim)); onS.snap (p.mainOn ? 1 : 0); linkS.snap (p.mainLink);
        pThrS.snap (dbToGain (p.parThr)); pGainS.snap (parGain (p)); pLinkS.snap (p.parLink); pOnS.snap (p.parOn ? 1 : 0);
        makeup.snap (1.0);
        inG.snap (dbToGain (p.inDb)); outG.snap (dbToGain (p.outDb)); mixS.snap (p.mix);
        bypassS.snap (p.bypass ? 1 : 0); compGain.snap (1.0);
        const auto w = monitorWeights (p);
        for (int i = 0; i < 3; ++i) monS[i].snap (w[i]);
        const auto st = soloTargets (p);
        for (int k = 0; k < 5; ++k) soloKindS[k].snap (st[(size_t) k]);
    }

    static double parGain (const EngineParams& p)
    {
        return (p.parAmountDb <= -59.9 ? 0.0 : dbToGain (p.parAmountDb)) * dbToGain (p.parTrim);
    }
    /** les solos : bande traitée, au-dessus du bas, au-dessus du haut, reste, PARALIMIT */
    static std::array<double, 5> soloTargets (const EngineParams& p)
    {
        return { p.mainSolo == 1 ? 1.0 : 0.0, p.mainSolo == 2 ? 1.0 : 0.0, p.mainSolo == 3 ? 1.0 : 0.0, p.mainSolo == 4 ? 1.0 : 0.0,
                 p.parSolo && p.parOn ? 1.0 : 0.0 };
    }
    /** poids de l'écoute : normal, DELTA, SOLO */
    static std::array<double, 3> monitorWeights (const EngineParams& p)
    {
        const bool solo = p.mainSolo != 0 || (p.parSolo && p.parOn);
        if (solo) return { 0, 0, 1 };
        if (p.delta) return { 0, 1, 0 };
        return { 1, 0, 0 };
    }

    //==========================================================================
    void block (float* L, float* R, const float* sL, const float* sR, int len, const EngineParams& p)
    {
        // ---- changement de chaîne : fondu au silence, mise en place, retour
        if ((wantQuality != activeQuality || wantOs != activeOs) && structTarget > 0.5) structTarget = 0.0;
        if (structTarget < 0.5 && structGain <= 0.0)
        {
            activeQuality = wantQuality; activeOs = wantOs;
            configure();
            structTarget = 1.0;
            // silence le temps que les nouvelles lignes de retard se remplissent, puis retour en fondu
            structHold = getLatency() + (int) (0.004 * fs);
        }
        const double structStep = 1.0 / (0.006 * fs);

        const int q = activeQuality, f = factors[activeOs];
        const bool scOn = sL != nullptr;
        const bool extM = scOn && p.scMain, extP = scOn && p.parOn && p.scPar;

        // ---- contrôle : drapeaux IIR, tables du limiteur, fenêtres de détection
        ctlMain.target = BandEdges::make (p.mainLo, p.mainHi, p.mainMode);
        ctlPar.target = BandEdges::make (p.parLo, p.parHi, p.parMode);
        if (ctlMain.control (ctrlA)) { for (auto& b : iirMain) b.set (ctlMain.gl, ctlMain.gh); for (auto& b : soloBand) b.set (ctlMain.gl, ctlMain.gh); }
        if (ctlPar.control (ctrlA)) for (auto& b : iirPar) b.set (ctlPar.gl, ctlPar.gh);
        tabMain.build (fsO, p.attackFast, p.releaseFast);
        tabPar.build (fsO, p.parAttackFast, p.parReleaseFast);
        // la détection tient au moins une demi-période du bas de la bande (l'énergie commande, pas l'oscillation)
        const int holdM = (int) std::ceil (0.5 / ctlMain.lo * fsO), holdP = (int) std::ceil (0.5 / ctlPar.lo * fsO);
        for (auto& m : maxMain) m.setWindow (std::max (laOs + 1, holdM));
        for (auto& m : maxPar) m.setWindow (std::max (laOs + 1, holdP));

        float* const* B = base.getArrayOfWritePointers();
        float* const* A = aux.getArrayOfWritePointers();

        // ---- 1. entrée
        for (int i = 0; i < len; ++i)
        {
            const float l = L[i], r = R != nullptr ? R[i] : l;
            const double g = inG.next (dbToGain (p.inDb));
            A[4][i] = l; A[5][i] = r;
            A[0][i] = (float) (l * g); A[1][i] = (float) (r * g);
            inPk[0] = std::max (inPk[0], (double) std::abs (l)); inPk[1] = std::max (inPk[1], (double) std::abs (r));
            if (scOn) extPk = std::max (extPk, (double) std::max (std::abs (sL[i]), std::abs (sR[i])));
        }

        // ---- 2. séparations
        if (q == linear)
        {
            for (int i = 0; i < len; ++i)
            {
                B[0][i] = B[2][i] = A[0][i];
                B[1][i] = B[3][i] = A[1][i];
                B[8][i] = B[10][i] = scOn ? sL[i] : 0.0f;
                B[9][i] = B[11][i] = scOn ? sR[i] : 0.0f;
                A[2][i] = A[0][i]; A[3][i] = A[1][i];   // a(x) = x (retardé plus bas)
            }
            float* m[4] { B[0], B[1], B[8], B[9] };
            float* pp[4] { B[2], B[3], B[10], B[11] };
            const bool onM[4] { true, true, extM, extM };
            const bool onP[4] { p.parOn, p.parOn, extP, extP };
            convMain.process (m, len, onM);
            convPar.process (pp, len, onP);
        }
        else
        {
            for (int i = 0; i < len; ++i)
            {
                for (int c = 0; c < 2; ++c)
                {
                    iirMain[c].tick (A[c][i], ctlMain.w);
                    B[c][i] = (float) iirMain[c].band;
                    A[2 + c][i] = (float) iirMain[c].ax;
                    iirPar[c].tick (A[c][i], ctlPar.w);
                    B[2 + c][i] = (float) iirPar[c].band;
                }
                if (extM)
                {
                    iirMain[2].tick (sL[i], ctlMain.w); iirMain[3].tick (sR[i], ctlMain.w);
                    B[8][i] = (float) iirMain[2].band; B[9][i] = (float) iirMain[3].band;
                }
                if (extP)
                {
                    iirPar[2].tick (sL[i], ctlPar.w); iirPar[3].tick (sR[i], ctlPar.w);
                    B[10][i] = (float) iirPar[2].band; B[11][i] = (float) iirPar[3].band;
                }
            }
        }

        // ---- 3. références alignées : a(x) et la bande d'origine (pour SOLO), au retard de l'écart
        if (q == live)
        {
            for (int i = 0; i < len; ++i)
            {
                B[4][i] = A[2][i]; B[5][i] = A[3][i];   // passent par le suréchantillonnage avec les écarts
                B[6][i] = B[0][i]; B[7][i] = B[1][i];
            }
        }
        else
        {
            for (int i = 0; i < len; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    A[2 + c][i] = axDelay[c].tick (A[2 + c][i]);
                    A[6 + c][i] = bDelay[c].tick (B[c][i]);
                }
        }

        // ---- 4. suréchantillonnage, limiteurs, retour
        const int upCh = (extM || extP) ? 12 : (q == live ? 8 : 4);
        const int downCh = q == live ? 8 : 4;
        juce::dsp::AudioBlock<float> baseBlock (B, (size_t) osChannels, (size_t) len);
        float* O[osChannels];
        int nO = len;
        if (osObj != nullptr)
        {
            auto up = osObj->processSamplesUp (baseBlock.getSubsetChannelBlock (0, (size_t) upCh));
            for (int c = 0; c < osChannels; ++c) O[c] = c < (int) up.getNumChannels() ? up.getChannelPointer ((size_t) c) : B[c];
            nO = (int) up.getNumSamples();
        }
        else
        {
            for (int c = 0; c < osChannels; ++c) O[c] = B[c];
        }
        limit (O, nO, p, extM, extP);
        if (osObj != nullptr)
        {
            auto down = baseBlock.getSubsetChannelBlock (0, (size_t) downCh);
            osObj->processSamplesDown (down);
        }
        (void) f;

        // ---- 5. sortie
        const auto mw = monitorWeights (p);
        const auto soloT = soloTargets (p);
        // les solos « au-dessus d'un drapeau » : le vrai signal filtré (croisements LR4 aux drapeaux), au retard total
        const bool needAbove = p.mainSolo == 2 || p.mainSolo == 3 || soloKindS[1].v > 1.0e-4 || soloKindS[2].v > 1.0e-4;
        const float* axd[2] { q == live ? B[4] : A[2], q == live ? B[5] : A[3] };
        const float* bd[2] { q == live ? B[6] : A[6], q == live ? B[7] : A[7] };
        for (int i = 0; i < len; ++i)
        {
            const double mix = mixS.next (p.mix), og = outG.next (dbToGain (p.outDb)), byp = bypassS.next (p.bypass ? 1.0 : 0.0);
            double w[3]; for (int k = 0; k < 3; ++k) w[k] = monS[k].next (mw[(size_t) k]);
            double sk[5]; for (int k = 0; k < 5; ++k) sk[k] = soloKindS[k].next (soloT[(size_t) k]);
            double above[2][2] {};
            if (needAbove)
                for (int c = 0; c < 2; ++c)
                {
                    soloBand[c].tick (A[c][i], ctlMain.w);
                    above[c][0] = soloLoDelay[c].tick ((float) soloBand[c].H);
                    above[c][1] = soloHiDelay[c].tick ((float) soloBand[c].H2);
                }
            if (structHold > 0) --structHold;
            else structGain = std::clamp (structGain + (structTarget > 0.5 ? structStep : -structStep), 0.0, 1.0);

            double y[2], dl[2], so[2];
            for (int c = 0; c < 2; ++c)
            {
                const double dev = mix * B[c][i], par = B[2 + c][i];
                y[c] = axd[c][i] + dev + par;
                dl[c] = dev + par;
                so[c] = sk[0] * (bd[c][i] + dev) + sk[1] * above[c][0] + sk[2] * above[c][1] + sk[3] * (axd[c][i] - bd[c][i]) + sk[4] * par;
            }
            // compensation de niveau : puissance lente de a(x) contre celle de y
            {
                const double pi_ = 0.5 * ((double) axd[0][i] * axd[0][i] + (double) axd[1][i] * axd[1][i]);
                const double po_ = 0.5 * (y[0] * y[0] + y[1] * y[1]);
                cmpIn += cmpA * (pi_ - cmpIn); cmpOut += cmpA * (po_ - cmpOut);
                double target = 1.0;
                if (p.levelComp && cmpIn > 1.0e-8 && cmpOut > 1.0e-12)
                    target = std::clamp (std::sqrt (cmpIn / cmpOut), 0.25, 4.0);
                else if (p.levelComp) target = compGain.v;
                compGain.next (target);
            }
            for (int c = 0; c < 2; ++c)
            {
                const double mon = w[0] * y[c] * compGain.v + w[1] * dl[c] + w[2] * so[c];
                const double wet = mon * og * structGain;
                const double dry = dryDelay[c].tick (A[4 + c][i]);
                const double out = dry + (wet - dry) * (1.0 - byp);
                if (c == 0) L[i] = (float) out; else if (R != nullptr) R[i] = (float) out;
                outPk[c] = std::max (outPk[c], std::abs (out));
                if (c == 0) tmpPost = out; else tmpPost = 0.5 * (tmpPost + out);
            }
            if (R == nullptr) tmpPost = L[i];
            const int wi = scopeW;
            scope.pre[(size_t) wi] = 0.5f * (A[4][i] + A[5][i]);
            scope.post[(size_t) wi] = (float) tmpPost;
            scopeW = (wi + 1) & Scope::mask;
        }
        scope.write.store (scopeW);
    }

    //==========================================================================
    /** Le cœur, au rythme suréchantillonné : O[0..1] bandes MAIN → écarts, O[2..3] bandes PARALIMIT → ajout dense,
        O[8..11] clés externes. */
    void limit (float** O, int n, const EngineParams& p, bool extM, bool extP)
    {
        const bool ms = p.stereo == 1;
        const double tA = dbToGain (p.thrA), tB = dbToGain (p.thrB), tr = dbToGain (p.mainTrim);
        const double ptT = dbToGain (p.parThr), pg = parGain (p);
        const int pmode = std::clamp (p.parChannel, 0, 2);
        for (int j = 0; j < n; ++j)
        {
            // ---- MAIN
            const double bL = O[0][j], bR = O[1][j];
            const double kL = extM ? O[8][j] : bL, kR = extM ? O[9][j] : bR;
            const double dL = laDelay[0].tick ((float) bL), dR = laDelay[1].tick ((float) bR);
            double kA, kB, dA, dB;
            if (ms) { kA = 0.5 * (kL + kR); kB = 0.5 * (kL - kR); dA = 0.5 * (dL + dR); dB = 0.5 * (dL - dR); }
            else    { kA = kL; kB = kR; dA = dL; dB = dR; }
            const double lA = maxMain[0].push ((float) std::abs (kA)), lB = maxMain[1].push ((float) std::abs (kB));
            bandPk[0] = std::max (bandPk[0], lA); bandPk[1] = std::max (bandPk[1], lB);
            const double thA = thrAS.next (tA), thB = thrBS.next (tB);
            double wA = lA > thA ? gainToDb (thA / lA) : 0.0;
            double wB = lB > thB ? gainToDb (thB / lB) : 0.0;
            const double lk = linkS.next (p.mainLink), mn = std::min (wA, wB);
            wA += lk * (mn - wA); wB += lk * (mn - wB);
            const double gA = limMain[0].tick (wA, tabMain), gB = limMain[1].tick (wB, tabMain);
            const double on = onS.next (p.mainOn ? 1.0 : 0.0), trim = trimS.next (tr);
            grPk[0] = std::max (grPk[0], -gA * on); grPk[1] = std::max (grPk[1], -gB * on);
            const double devA = on * (dbToGain (gA) * trim - 1.0) * dA;
            const double devB = on * (dbToGain (gB) * trim - 1.0) * dB;
            if (ms) { O[0][j] = (float) (devA + devB); O[1][j] = (float) (devA - devB); }
            else    { O[0][j] = (float) devA; O[1][j] = (float) devB; }

            // ---- PARALIMIT : limité en vrai L/R, puis extrait
            const double pon = pOnS.next (p.parOn ? 1.0 : 0.0);
            if (pon > 1.0e-6 || p.parOn)
            {
                const double pL = O[2][j], pR = O[3][j];
                const double qL = extP ? O[10][j] : pL, qR = extP ? O[11][j] : pR;
                const double eL = laDelay[2].tick ((float) pL), eR = laDelay[3].tick ((float) pR);
                const double lL = maxPar[0].push ((float) std::abs (qL)), lR = maxPar[1].push ((float) std::abs (qR));
                const double th = pThrS.next (ptT);
                double wL = lL > th ? gainToDb (th / lL) : 0.0, wR = lR > th ? gainToDb (th / lR) : 0.0;
                const double plk = pLinkS.next (p.parLink), pm = std::min (wL, wR);
                wL += plk * (pm - wL); wR += plk * (pm - wR);
                const double hL = limPar[0].tick (wL, tabPar), hR = limPar[1].tick (wR, tabPar);
                grPk[2] = std::max (grPk[2], -hL * pon); grPk[3] = std::max (grPk[3], -hR * pon);
                const double oL = dbToGain (hL) * eL, oR = dbToGain (hR) * eR;
                // rattrapage : la bande dense revient au niveau moyen de la bande d'origine (0 … +24 dB)
                pinPow += powA * (eL * eL + eR * eR - pinPow);
                poutPow += powA * (oL * oL + oR * oR - poutPow);
                double mk = makeup.v;
                if (pinPow > 1.0e-10 && poutPow > 1.0e-14) mk = std::clamp (std::sqrt (pinPow / poutPow), 1.0, 15.85);
                makeup.next (mk);
                double xL, xR;
                switch (pmode)
                {
                    case 0:  { const double m = 0.5 * (oL + oR); xL = xR = m; break; }
                    case 1:  { const double s = 0.5 * (oL - oR); xL = s; xR = -s; break; }
                    default: xL = oL; xR = oR; break;
                }
                const double gain = pon * pGainS.next (pg) * makeup.v;
                O[2][j] = (float) (gain * xL); O[3][j] = (float) (gain * xR);
            }
            else
            {
                laDelay[2].tick (0.0f); laDelay[3].tick (0.0f);
                O[2][j] = O[3][j] = 0.0f;
            }
        }
    }

    //==========================================================================
    double fs = 48000, fsO = 48000, ctrlA = 0.1;
    int maxN = 512;
    juce::AudioBuffer<float> base, aux;

    IirControl ctlMain, ctlPar;
    std::array<IirBand, 4> iirMain, iirPar;           // x L, x R, sc L, sc R

    int firN = 1023, partition = 128, firLatency = 0;
    PartitionedConvolver convMain, convPar;
    std::mutex designLock;                              // seulement entre prepare() et updateFir() (hors audio)
    std::unique_ptr<juce::dsp::FFT> designFft;
    std::vector<float> designScratch, designTaps;
    BandEdges loadedMain, loadedPar;

    std::unique_ptr<juce::dsp::Oversampling<float>> osFir[4], osIir[4];
    juce::dsp::Oversampling<float>* osObj = nullptr;
    int osLatFir[4] {}, osLatIir[4] {}, osPad[4] {};
    int lookahead = 72, laOs = 0;
    int latencyTable[3][4] {};
    int activeQuality = dynamic, activeOs = 1, wantQuality = dynamic, wantOs = 1;
    double structGain = 1.0, structTarget = 1.0;
    int structHold = 0;
    std::atomic<int> latencyNow { 0 };
    bool firstBlock = true;

    std::array<Delay, 4> laDelay;                       // bandes MAIN L R, PARALIMIT L R (anticipation)
    std::array<SlidingMax, 2> maxMain, maxPar;
    std::array<AccelLimiter, 2> limMain, limPar;
    AccelLimiter::Tables tabMain, tabPar;
    std::array<Delay, 2> axDelay, bDelay, dryDelay;

    Smooth thrAS, thrBS, trimS, onS, linkS, pThrS, pGainS, pLinkS, pOnS, makeup;
    Smooth inG, outG, mixS, bypassS, compGain, monS[3], soloKindS[5];
    std::array<IirBand, 2> soloBand;
    std::array<Delay, 2> soloLoDelay, soloHiDelay;
    double pinPow = 0, poutPow = 0, powA = 0.001;
    double cmpIn = 0, cmpOut = 0, cmpA = 1.0e-5;
    double inPk[2] {}, outPk[2] {}, bandPk[2] {}, grPk[4] {}, extPk = 0, tmpPost = 0;
    int scopeW = 0;

public:
    /** à appeler après prepare : les temps de lissage à la fréquence de base */
    void setBaseSmoothing()
    {
        for (auto* s : { &inG, &outG, &mixS }) s->setTime (0.02, fs);
        bypassS.setTime (0.012, fs);
        for (auto& s : monS) s.setTime (0.01, fs);
        for (auto& s : soloKindS) s.setTime (0.01, fs);
        compGain.setTime (0.5, fs);
        cmpA = 1.0 - std::exp (-1.0 / (1.5 * fs));
    }
};
} // namespace halo
