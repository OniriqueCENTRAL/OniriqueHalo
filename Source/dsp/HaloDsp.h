#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
//  ONIRIQUE HALO : les briques du traitement (voir Documentation/DSP.md).
//
//  Principe commun aux deux moteurs (MAIN et PARALIMIT) : un filtre sépare le signal x en « reste » et en « bande » b,
//  de sorte que reste + b = a(x), où a est soit un simple retard (phase linéaire), soit un passe-tout (phase minimale),
//  soit x lui-même. Le traitement rend
//          y = a(x) + (g · trim − 1) · b
//  c'est-à-dire a(x) dont seule la bande a changé : au neutre (g = 1, trim = 0 dB), y = a(x) — amplitude rigoureusement
//  plate, et sortie identique à l'entrée (retardée) en LINEAR.
//==============================================================================
namespace halo
{
constexpr double pi = 3.14159265358979323846;
inline double dbToGain (double db) { return std::exp (db * 0.11512925464970228); }   // ln(10) / 20
inline double gainToDb (double g) { return 8.685889638065035 * std::log (std::max (1.0e-12, g)); }

//==============================================================================
/** SVF TPT (Simper) : passe-bas, passe-bande, passe-haut et passe-tout de 2e ordre, coefficients modifiables sans clic. */
struct Svf
{
    double a1 = 0, a2 = 0, a3 = 0, k = 1.41421356, ic1 = 0, ic2 = 0, v1 = 0, v2 = 0, in = 0;
    void set (double g, double q) { k = 1.0 / q; a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2; }
    void reset() { ic1 = ic2 = v1 = v2 = in = 0; }
    inline void tick (double x)
    {
        in = x;
        const double v3 = x - ic2;
        v1 = a1 * ic1 + a2 * v3;
        v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
    }
    double lp() const { return v2; }
    double hp() const { return in - k * v1 - v2; }
    double ap() const { return in - 2.0 * k * v1; }
};

/** La forme de la bande traitée. */
enum class BandMode { split = 0, defined = 1, fullHigh = 2 };

/** Les bords de la bande, corrigés : en DEFINED, une demi-octave au moins entre les deux drapeaux. */
struct BandEdges
{
    double lo = 2500, hi = 20000;
    BandMode mode = BandMode::split;

    static constexpr double minFreq = 50.0, maxFreq = 20000.0;
    static double halfOctave() { return 1.4142135623730951; }

    static BandEdges make (double lo, double hi, int mode)
    {
        BandEdges e;
        e.mode = (BandMode) std::clamp (mode, 0, 2);
        e.lo = std::clamp (lo, minFreq, maxFreq);
        e.hi = std::clamp (hi, minFreq, maxFreq);
        if (e.hi < e.lo * halfOctave())
        {
            // le drapeau du haut est repoussé ; s'il ne peut plus monter, c'est celui du bas qui descend
            e.hi = std::min (maxFreq, e.lo * halfOctave());
            e.lo = std::min (e.lo, e.hi / halfOctave());
        }
        return e;
    }
    bool operator== (const BandEdges& o) const { return lo == o.lo && hi == o.hi && mode == o.mode; }
    bool operator!= (const BandEdges& o) const { return ! (*this == o); }
};

/** L'amplitude de la bande à la fréquence f : la même cible pour les trois qualités (le FIR la copie à phase linéaire).
      SPLIT    passe-haut Linkwitz-Riley 24 dB/oct au drapeau du bas : w⁴ / (1 + w⁴)
      DEFINED  le même, multiplié par le passe-bas LR 24 dB/oct au drapeau du haut
      FULL HF  passe-haut doux 12 dB/oct (deux pôles réels confondus) : w² / (1 + w²) */
inline double bandMagnitude (const BandEdges& e, double f, double fs)
{
    // fréquences « déformées » de la transformée bilinéaire : la réponse numérique exacte des filtres IIR
    const double top = 0.45 * fs;
    auto warp = [fs] (double x) { return std::tan (pi * std::min (x, 0.4999 * fs) / fs); };
    const double W = warp (f), w = W / warp (std::min (e.lo, top));
    switch (e.mode)
    {
        case BandMode::split:   { const double w4 = w * w * w * w; return w4 / (1 + w4); }
        case BandMode::defined: { const double w4 = w * w * w * w, v = W / warp (std::min (e.hi, top)), v4 = v * v * v * v; return (w4 / (1 + w4)) * (1 / (1 + v4)); }
        case BandMode::fullHigh:
        default:                { const double w2 = w * w; return w2 / (1 + w2); }
    }
}

//==============================================================================
/** La bande en IIR à phase minimale (qualités DYNAMIC et LIVE), pour un canal.
      SPLIT    croisement Linkwitz-Riley 4e ordre au drapeau du bas : bas + haut = passe-tout ; la bande est le haut
      DEFINED  le haut recoupé au drapeau du haut (même croisement), le bas passé dans le passe-tout du haut pour rester
               aligné : la somme est le produit des deux passe-tout
      FULL HF  croisement LR2 (12 dB/oct, pôles réels confondus) : bas − haut est un passe-tout, −haut y est en phase
    Dans les trois formes la bande est en phase avec la somme : l'amplitude rendue est exactement 1 − (1 − g) · |bande|,
    la même courbe qu'en LINEAR (et que celle de l'interface).
    Les trois formes tournent en permanence ; changer de forme fond les sorties (sans clic). Les fréquences glissent
    en logarithme à la cadence de contrôle. */
struct IirBand
{
    Svf loA, loLp, loHp;            // croisement du bas : BW2, puis BW2 de chaque côté (LR4)
    Svf hiA, hiLp, hiHp, hiAp;      // croisement du haut sur la partie haute, passe-tout du haut sur la partie basse
    Svf full;                        // FULL HF : Q 0,5
    double ax = 0, band = 0, H = 0, H2 = 0;   // H : au-dessus du drapeau bas, H2 : au-dessus du drapeau haut (solos)

    void reset() { for (auto* s : { &loA, &loLp, &loHp, &hiA, &hiLp, &hiHp, &hiAp, &full }) s->reset(); ax = band = 0; }

    /** gl, gh : tan(π f / fs) des deux drapeaux */
    void set (double gl, double gh)
    {
        const double q = 0.7071067811865476;
        loA.set (gl, q); loLp.set (gl, q); loHp.set (gl, q);
        hiA.set (gh, q); hiLp.set (gh, q); hiHp.set (gh, q); hiAp.set (gh, q);
        full.set (gl, 0.5);
    }

    /** w : poids des trois formes (somme 1) */
    inline void tick (double x, const double* w)
    {
        loA.tick (x);
        loLp.tick (loA.lp());
        loHp.tick (loA.hp());
        const double L = loLp.lp();
        H = loHp.hp();
        hiA.tick (H);
        hiLp.tick (hiA.lp());
        hiHp.tick (hiA.hp());
        hiAp.tick (L);
        const double M = hiLp.lp();
        H2 = hiHp.hp();
        full.tick (x);

        // FULL HF : croisement LR2 (pôles réels confondus) ; bas − haut est un passe-tout du 1er ordre, et −haut
        // y est en phase : la bande garde la même courbe d'amplitude qu'en LINEAR
        const double axSplit = L + H, axDef = hiAp.ap() + M + H2, axFull = full.lp() - full.hp();
        const double bSplit = H, bDef = M, bFull = -full.hp();
        ax = w[0] * axSplit + w[1] * axDef + w[2] * axFull;
        band = w[0] * bSplit + w[1] * bDef + w[2] * bFull;
    }
};

/** Les réglages lissés d'un jeu de bandes IIR (partagés par ses canaux). */
struct IirControl
{
    double lo = 2500, hi = 20000, w[3] { 1, 0, 0 }, gl = 0, gh = 0, fs = 48000;
    BandEdges target;
    void prepare (double sampleRate, const BandEdges& e) { fs = sampleRate; snap (e); }
    void snap (const BandEdges& e)
    {
        target = e; lo = e.lo; hi = e.hi;
        for (int i = 0; i < 3; ++i) w[i] = (int) e.mode == i ? 1.0 : 0.0;
        update();
    }
    /** a : coefficient de lissage pour un pas de contrôle */
    bool control (double a)
    {
        const double lo0 = lo, hi0 = hi;
        lo *= std::exp (a * std::log (target.lo / lo));
        hi *= std::exp (a * std::log (target.hi / hi));
        if (std::abs (std::log (target.lo / lo)) < 1e-6) lo = target.lo;
        if (std::abs (std::log (target.hi / hi)) < 1e-6) hi = target.hi;
        bool moved = lo != lo0 || hi != hi0;
        for (int i = 0; i < 3; ++i)
        {
            const double t = (int) target.mode == i ? 1.0 : 0.0, old = w[i];
            w[i] += a * (t - w[i]);
            if (std::abs (t - w[i]) < 1e-5) w[i] = t;
            moved |= w[i] != old;
        }
        if (moved) update();
        return moved;
    }
    void update()
    {
        const double top = 0.45 * fs;
        gl = std::tan (pi * std::min (lo, top) / fs);
        gh = std::tan (pi * std::min (hi, top) / fs);
    }
};

//==============================================================================
/** La bande à phase linéaire (qualité LINEAR) : FIR symétrique de longueur fixe (pour une fréquence d'échantillonnage)
    — la latence ne bouge donc pas quand on déplace les drapeaux. Conçu par échantillonnage fréquentiel de
    bandMagnitude() (même amplitude que l'IIR, sans rotation de phase), fenêtre de Kaiser. */
inline int firLengthFor (double fs)
{
    int n = (int) std::ceil (fs * 0.17);   // ≈ 170 ms : de quoi suivre une pente 24 dB/oct dès 50 Hz
    return std::clamp (n, 255, 32767) | 1;
}

inline double besselI0 (double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}

/** Remplit h (taille impaire) ; scratch : au moins 2 × la taille de FFT (puissance de 2 ≥ 4 × la longueur). */
inline void designBandFir (const BandEdges& e, double fs, std::vector<float>& h, juce::dsp::FFT& fft, std::vector<float>& scratch)
{
    const int N = (int) h.size(), K = fft.getSize(), half = (N - 1) / 2;
    // l'échelle de l'inverse : le premier échantillon de l'impulsion d'un spectre plat (les moteurs FFT ne
    // normalisent pas tous de la même façon)
    std::fill (scratch.begin(), scratch.end(), 0.0f);
    for (int k = 0; k <= K / 2; ++k) scratch[(size_t) (2 * k)] = 1.0f;
    fft.performRealOnlyInverseTransform (scratch.data());
    const double norm = 1.0 / std::max (1.0e-12, (double) scratch[0]);
    // spectre réel à phase nulle : partie réelle de chaque raie, imaginaire nulle
    std::fill (scratch.begin(), scratch.end(), 0.0f);
    for (int k = 0; k <= K / 2; ++k)
        scratch[(size_t) (2 * k)] = (float) bandMagnitude (e, fs * k / K, fs);
    fft.performRealOnlyInverseTransform (scratch.data());
    const double beta = 7.0, i0b = besselI0 (beta);
    for (int n = 0; n < N; ++n)
    {
        const int m = n - half;                       // réponse à phase nulle centrée sur 0 (circulaire)
        const float v = scratch[(size_t) ((m + K) % K)];
        const double r = (double) m / (double) half;
        const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
        h[(size_t) n] = (float) (v * norm * w);
    }
}

//==============================================================================
/** Convolution FFT par partitions uniformes (overlap-save), synchrone, plusieurs canaux sur la même réponse.
    Latence : la taille de partition P (mesurée au prepare par l'engine). La réponse se change sans clic : la
    nouvelle est préparée hors du thread audio, prise au début d'un bloc, puis les deux sorties sont fondues sur
    quatre partitions. Aucune allocation ni verrou dans process(). */
class PartitionedConvolver
{
public:
    void prepare (int partition, int maxTaps, int channels)
    {
        P = partition;
        int order = 0; while ((1 << order) < 2 * P) ++order;
        fft = std::make_unique<juce::dsp::FFT> (order);
        F = 1 << order;
        bins = F / 2 + 1;
        K = (maxTaps + P - 1) / P;
        C = channels;
        for (auto& s : slots) s.assign ((size_t) (K * bins * 2), 0.0f);
        ch.assign ((size_t) C, Channel {});
        for (auto& c : ch)
        {
            c.frame.assign ((size_t) (2 * F), 0.0f);
            c.fdl.assign ((size_t) (K * bins * 2), 0.0f);
            c.out.assign ((size_t) P, 0.0f);
        }
        work.assign ((size_t) (2 * F), 0.0f);
        workB.assign ((size_t) (2 * F), 0.0f);
        irPad.assign ((size_t) (2 * F), 0.0f);
        // échelle de l'inverse
        std::fill (work.begin(), work.end(), 0.0f);
        work[0] = 1.0f;
        fft->performRealOnlyForwardTransform (work.data(), true);
        fft->performRealOnlyInverseTransform (work.data());
        invScale = 1.0f / std::max (1.0e-12f, work[0]);
        cur.store (0); prev = -1; pending.store (-1); fade = 0;
        reset();
    }

    void reset()
    {
        for (auto& c : ch)
        {
            std::fill (c.frame.begin(), c.frame.end(), 0.0f);
            std::fill (c.fdl.begin(), c.fdl.end(), 0.0f);
            std::fill (c.out.begin(), c.out.end(), 0.0f);
        }
        head = 0; fill = 0;
    }

    int partition() const { return P; }
    int maxTaps() const { return K * P; }

    /** Hors du thread audio : prépare une réponse. Rend false si la précédente n'a pas encore été prise (réessayer). */
    bool load (const float* ir, int taps)
    {
        if (pending.load() != -1) return false;
        const int c = cur.load(), p = prevShared.load();
        int slot = 0;
        while (slot == c || slot == p) ++slot;
        spectra (ir, taps, slots[(size_t) slot]);
        pending.store (slot);
        return true;
    }

    /** Au prepare (thread audio arrêté) : la réponse tout de suite, sans fondu. */
    void loadNow (const float* ir, int taps)
    {
        spectra (ir, taps, slots[0]);
        cur.store (0); prev = -1; prevShared.store (-1); pending.store (-1); fade = 0;
    }

    /** Un bloc, en place, sur les canaux 0 … C−1 ; active : quels canaux tournent (les autres rendent 0). */
    void process (float* const* data, int n, const bool* active)
    {
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < C; ++c)
            {
                auto& s = ch[(size_t) c];
                s.frame[(size_t) (P + fill)] = active[c] ? data[c][i] : 0.0f;
                data[c][i] = s.out[(size_t) fill];
            }
            if (++fill == P) { block (active); fill = 0; }
        }
    }

private:
    struct Channel { std::vector<float> frame, fdl, out; };

    void spectra (const float* ir, int taps, std::vector<float>& dest)
    {
        taps = std::min (taps, K * P);
        for (int k = 0; k < K; ++k)
        {
            std::fill (irPad.begin(), irPad.end(), 0.0f);
            for (int i = 0; i < P; ++i)
            {
                const int n = k * P + i;
                if (n < taps) irPad[(size_t) i] = ir[n];
            }
            fft->performRealOnlyForwardTransform (irPad.data(), true);
            std::copy (irPad.begin(), irPad.begin() + bins * 2, dest.begin() + (size_t) (k * bins * 2));
        }
    }

    void accumulate (const Channel& s, const std::vector<float>& H, std::vector<float>& acc)
    {
        std::fill (acc.begin(), acc.begin() + bins * 2, 0.0f);
        for (int k = 0; k < K; ++k)
        {
            const int slot = (head - k + K) % K;
            const float* X = s.fdl.data() + (size_t) (slot * bins * 2);
            const float* Hk = H.data() + (size_t) (k * bins * 2);
            float* a = acc.data();
            for (int b = 0; b < bins; ++b)
            {
                const float xr = X[2 * b], xi = X[2 * b + 1], hr = Hk[2 * b], hi = Hk[2 * b + 1];
                a[2 * b] += xr * hr - xi * hi;
                a[2 * b + 1] += xr * hi + xi * hr;
            }
        }
    }

    void block (const bool* active)
    {
        // une nouvelle réponse : prise ici, fondue sur les blocs suivants
        if (fade == 0)
        {
            const int p = pending.load();
            if (p != -1)
            {
                prev = cur.load();
                prevShared.store (prev);
                cur.store (p);
                pending.store (-1);
                fade = fadeBlocks;
            }
        }
        head = (head + 1) % K;
        const auto& Hc = slots[(size_t) cur.load()];
        for (int c = 0; c < C; ++c)
        {
            auto& s = ch[(size_t) c];
            if (! active[c])
            {
                // à l'arrêt : silence, et une ligne de spectres vide pour repartir propre
                std::fill (s.out.begin(), s.out.end(), 0.0f);
                std::fill (s.fdl.begin() + (size_t) (head * bins * 2), s.fdl.begin() + (size_t) ((head + 1) * bins * 2), 0.0f);
                std::copy (s.frame.begin() + P, s.frame.begin() + 2 * P, s.frame.begin());
                continue;
            }
            // la trame : P anciens + P nouveaux
            std::copy (s.frame.begin(), s.frame.begin() + 2 * P, work.begin());
            std::fill (work.begin() + 2 * P, work.end(), 0.0f);
            fft->performRealOnlyForwardTransform (work.data(), true);
            std::copy (work.begin(), work.begin() + bins * 2, s.fdl.begin() + (size_t) (head * bins * 2));
            std::copy (s.frame.begin() + P, s.frame.begin() + 2 * P, s.frame.begin());

            accumulate (s, Hc, work);
            fft->performRealOnlyInverseTransform (work.data());
            if (fade > 0 && prev >= 0)
            {
                accumulate (s, slots[(size_t) prev], workB);
                fft->performRealOnlyInverseTransform (workB.data());
                const float a0 = (float) (fadeBlocks - fade) / (float) fadeBlocks, a1 = (float) (fadeBlocks - fade + 1) / (float) fadeBlocks;
                for (int i = 0; i < P; ++i)
                {
                    const float t = a0 + (a1 - a0) * (float) i / (float) P;
                    s.out[(size_t) i] = invScale * (t * work[(size_t) (P + i)] + (1.0f - t) * workB[(size_t) (P + i)]);
                }
            }
            else
            {
                for (int i = 0; i < P; ++i) s.out[(size_t) i] = invScale * work[(size_t) (P + i)];
            }
        }
        if (fade > 0 && --fade == 0) { prev = -1; prevShared.store (-1); }
    }

    static constexpr int fadeBlocks = 4;
    std::unique_ptr<juce::dsp::FFT> fft;
    int P = 128, F = 256, bins = 129, K = 1, C = 1, head = 0, fill = 0, prev = -1, fade = 0;
    float invScale = 1.0f;
    std::vector<float> slots[3];
    std::atomic<int> cur { 0 }, pending { -1 }, prevShared { -1 };
    std::vector<Channel> ch;
    std::vector<float> work, workB, irPad;
};

//==============================================================================
/** Le limiteur d'accélération d'un canal : gain calculé échantillon par échantillon. Pas de temps d'attaque ni de
    relâchement fixes : leur constante de temps se raccourcit à mesure que la réduction de gain déjà en cours grandit
    (une crête isolée est prise vite et rendue vite, un passage dense est tenu plus calmement).

        Gd(n)  = min(0, T − L(n))                    gain voulu, dB (L : niveau détecté, T : seuil)
        a      = −G(n)                               réduction en cours, dB
        τ      = τ0 / (1 + k · a)   (bornée par τmin) τ0, k de l'ATTACK si Gd < G, de la RELEASE sinon
        G(n+1) = G(n) + (Gd − G(n)) · (1 − e^(−1/(τ fs)))

    ATTACK et RELEASE ont chacune deux profils, SLOW et FAST, indépendants. Les coefficients par pas de 0,1 dB de
    réduction sont tabulés (aucun exp() par échantillon pour la constante de temps). */
struct AccelLimiter
{
    struct Law { double t0Ms, k, minMs; };
    // constantes internes du modèle adaptatif (pas des temps annoncés : le temps effectif dépend du programme)
    static constexpr Law attackSlow { 4.0, 0.35, 0.35 }, attackFast { 0.5, 0.6, 0.05 };
    static constexpr Law releaseSlow { 240.0, 0.16, 30.0 }, releaseFast { 70.0, 0.28, 10.0 };

    static constexpr int tableSize = 481;   // 0 … 48 dB par 0,1 dB

    struct Tables
    {
        float att[tableSize] {}, rel[tableSize] {};
        double fs = 0; int attSel = -1, relSel = -1;
        void build (double sampleRate, bool attackFast_, bool releaseFast_)
        {
            const int a = attackFast_ ? 1 : 0, r = releaseFast_ ? 1 : 0;
            if (fs == sampleRate && a == attSel && r == relSel) return;
            fs = sampleRate; attSel = a; relSel = r;
            const Law& la = a ? attackFast : attackSlow;
            const Law& lr = r ? releaseFast : releaseSlow;
            for (int i = 0; i < tableSize; ++i)
            {
                const double gr = 0.1 * i;
                const double ta = std::max (la.minMs, la.t0Ms / (1.0 + la.k * gr));
                const double tr = std::max (lr.minMs, lr.t0Ms / (1.0 + lr.k * gr));
                att[i] = (float) (1.0 - std::exp (-1000.0 / (ta * fs)));
                rel[i] = (float) (1.0 - std::exp (-1000.0 / (tr * fs)));
            }
        }
    };

    double gDb = 0;
    void reset() { gDb = 0; }

    inline double tick (double wantDb, const Tables& t)
    {
        const int idx = std::min (tableSize - 1, (int) (-gDb * 10.0));
        const double c = wantDb < gDb ? t.att[idx] : t.rel[idx];
        gDb += (wantDb - gDb) * c;
        if (gDb > -1.0e-7) gDb = 0.0;
        return gDb;
    }
};

/** Maximum glissant sur une fenêtre de W échantillons (file monotone, O(1) amorti, pré-réservée). */
struct SlidingMax
{
    std::vector<float> val; std::vector<int64_t> at;
    int cap = 1, headI = 0, tailI = 0, size = 0, W = 1;
    int64_t n = 0;
    void prepare (int maxWindow) { cap = std::max (2, maxWindow + 2); val.assign ((size_t) cap, 0.0f); at.assign ((size_t) cap, 0); reset(); }
    void setWindow (int w) { W = std::clamp (w, 1, cap - 2); }
    void reset() { headI = tailI = size = 0; n = 0; }
    inline float push (float v)
    {
        // retire de la queue les valeurs plus petites
        while (size > 0)
        {
            const int last = (tailI - 1 + cap) % cap;
            if (val[(size_t) last] > v) break;
            tailI = last; --size;
        }
        val[(size_t) tailI] = v; at[(size_t) tailI] = n; tailI = (tailI + 1) % cap; ++size;
        // retire de la tête ce qui est sorti de la fenêtre
        while (size > 0 && at[(size_t) headI] <= n - W) { headI = (headI + 1) % cap; --size; }
        ++n;
        return val[(size_t) headI];
    }
};

/** Ligne de retard (pré-réservée), retard entier. */
struct Delay
{
    std::vector<float> buf; int pos = 0, d = 0, len = 1;
    void prepare (int maxDelay) { len = std::max (1, maxDelay + 1); buf.assign ((size_t) len, 0.0f); pos = 0; }
    void setDelay (int samples) { d = std::clamp (samples, 0, len - 1); }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
    inline float tick (float x)
    {
        buf[(size_t) pos] = x;
        int r = pos - d; if (r < 0) r += len;
        if (++pos == len) pos = 0;
        return buf[(size_t) r];
    }
};

/** Lissage d'un réglage, un pôle. */
struct Smooth
{
    double v = 0, a = 1;
    void setTime (double seconds, double rate) { a = 1.0 - std::exp (-1.0 / std::max (1.0e-6, seconds * rate)); }
    void snap (double x) { v = x; }
    inline double next (double target) { v += (target - v) * a; return v; }
};

//==============================================================================
/** Retard mesuré entre deux signaux : le décalage (0 … maxLag) où leur corrélation est la plus forte. Sert au prepare
    à mesurer la latence réelle de chaque étage (FIR, suréchantillonnage). */
inline int measureLag (const std::vector<float>& ref, const std::vector<float>& out, int maxLag)
{
    int best = 0; double bestV = -1.0e300;
    const int n = (int) std::min (ref.size(), out.size());
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double s = 0;
        for (int i = 0; i + lag < n; ++i) s += (double) ref[(size_t) i] * (double) out[(size_t) (i + lag)];
        if (s > bestV) { bestV = s; best = lag; }
    }
    return best;
}

/** Une salve de test : sinus fenêtré (Hann) à f, de longueur len, suivie de silence. */
inline std::vector<float> testBurst (double f, double fs, int len, int total)
{
    std::vector<float> v ((size_t) total, 0.0f);
    for (int i = 0; i < len && i < total; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2 * pi * i / (len - 1));
        v[(size_t) i] = (float) (0.5 * w * std::sin (2 * pi * f * i / fs));
    }
    return v;
}
} // namespace halo
