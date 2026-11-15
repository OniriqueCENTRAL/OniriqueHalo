// Banc de mesure du moteur de HALO : chaque essai mesure, compare à une tolérance et s'affiche OK / ÉCHEC.
// Le code de sortie est le nombre d'échecs (la CI s'arrête si un essai échoue).
#include <juce_dsp/juce_dsp.h>
#include <cstdio>
#include <random>
#include <chrono>
#include "dsp/HaloEngine.h"

using namespace halo;

namespace
{
int failures = 0;
void check (bool ok, const char* what, const juce::String& detail)
{
    std::printf ("%s  %-58s %s\n", ok ? "OK    " : "ECHEC ", what, detail.toRawUTF8());
    if (! ok) ++failures;
}

using Signal = std::vector<float>;
struct Stereo { Signal l, r; };

Stereo silence (int n) { return { Signal ((size_t) n, 0.0f), Signal ((size_t) n, 0.0f) }; }

Signal sine (double f, double amp, double fs, int n, double phase = 0)
{
    Signal s ((size_t) n);
    for (int i = 0; i < n; ++i) s[(size_t) i] = (float) (amp * std::sin (2 * pi * f * i / fs + phase));
    return s;
}
Signal noise (double amp, int n, unsigned seed)
{
    std::mt19937 g (seed); std::normal_distribution<float> d (0.0f, (float) amp);
    Signal s ((size_t) n); for (auto& v : s) v = d (g);
    return s;
}
void add (Signal& a, const Signal& b) { for (size_t i = 0; i < a.size(); ++i) a[i] += b[i]; }

/** Fait passer un signal dans un moteur, par blocs ; onBlock peut changer les réglages en route. */
Stereo run (Engine& e, const Stereo& in, EngineParams p, int blockSize = 256, const Stereo* sc = nullptr,
            std::function<void (int, EngineParams&)> onBlock = {})
{
    Stereo out = in;
    const int n = (int) in.l.size();
    for (int start = 0; start < n; start += blockSize)
    {
        const int len = std::min (blockSize, n - start);
        if (onBlock) onBlock (start, p);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        juce::AudioBuffer<float> b (ch, 2, len);
        const float* s[2] { nullptr, nullptr };
        if (sc != nullptr) { s[0] = sc->l.data() + start; s[1] = sc->r.data() + start; }
        e.process (b, sc != nullptr ? s : nullptr, sc != nullptr ? 2 : 0, p);
    }
    return out;
}

double rms (const Signal& s, int from, int to)
{
    double a = 0; for (int i = from; i < to; ++i) a += (double) s[(size_t) i] * s[(size_t) i];
    return std::sqrt (a / std::max (1, to - from));
}
/** amplitude d'une composante (Goertzel normalisé) */
double tone (const Signal& s, double f, double fs, int from, int to)
{
    double re = 0, im = 0;
    for (int i = from; i < to; ++i) { const double w = 2 * pi * f * i / fs; re += s[(size_t) i] * std::cos (w); im += s[(size_t) i] * std::sin (w); }
    return 2.0 * std::sqrt (re * re + im * im) / (to - from);
}
double db (double v) { return 20.0 * std::log10 (std::max (1.0e-12, v)); }

int xcorrLag (const Signal& ref, const Signal& out, int maxLag)
{
    std::vector<float> a (ref.begin(), ref.end()), b (out.begin(), out.end());
    return measureLag (a, b, maxLag);
}

EngineParams neutral (int quality, int os)
{
    EngineParams p;
    p.quality = quality; p.os = os;
    p.thrA = p.thrB = 0.0;  // rien n'est limité sous 0 dBFS de bande
    p.parOn = false;
    return p;
}

const char* qname (int q) { return q == linear ? "LINEAR" : q == dynamic ? "DYNAMIC" : "LIVE"; }
} // namespace

int main()
{
    const double fs = 48000;
    const int block = 512;

    std::printf ("ONIRIQUE HALO - banc de mesure (%.0f Hz)\n\n", fs);

    // ---------------------------------------------------------------- latences mesurées
    std::printf ("Latences declarees (mesurees au prepare), en echantillons :\n");
    {
        Engine e; e.prepare (fs, block, neutral (dynamic, 1));
        for (int q = 0; q < 3; ++q)
            std::printf ("   %-8s 1x %5d   2x %5d   4x %5d   8x %5d   (2x = %.2f ms)\n", qname (q), e.latencyFor (q, 0), e.latencyFor (q, 1),
                         e.latencyFor (q, 2), e.latencyFor (q, 3), 1000.0 * e.latencyFor (q, 1) / fs);
        std::printf ("\n");
    }

    // ---------------------------------------------------------------- 1. latence réelle = latence déclarée
    for (int q = 0; q < 3; ++q)
        for (int o = 0; o < 4; ++o)
        {
            Engine e; auto p = neutral (q, o); e.prepare (fs, block, p);
            const int n = 24000;
            Stereo in { noise (0.1, n, 7), noise (0.1, n, 8) };
            auto out = run (e, in, p);
            const int declared = e.getLatency();
            // pic de corrélation cherché de 0 à 128 échantillons au-delà de la latence déclarée (et autant avant)
            int lag = 0; double bestV = -1.0e300;
            for (int L = std::max (0, declared - 128); L <= declared + 128; ++L)
            {
                double s = 0;
                for (int i = 0; i + L < n; ++i) s += (double) in.l[(size_t) i] * out.l[(size_t) (i + L)];
                if (s > bestV) { bestV = s; lag = L; }
            }
            const juce::String what = juce::String ("latence reelle = declaree, ") + qname (q) + " " + juce::String (Engine::factors[o]) + "x";
            check (std::abs (lag - declared) <= (q == live ? 1 : 0), what.toRawUTF8(), "mesuree " + juce::String (lag) + ", declaree " + juce::String (declared));
        }

    // ---------------------------------------------------------------- 2. neutre : LINEAR nul exact, IIR amplitude plate
    for (int o : { 0, 1, 3 })
    {
        Engine e; auto p = neutral (linear, o); e.prepare (fs, block, p);
        const int n = 30000;
        Stereo in { noise (0.1, n, 1), noise (0.1, n, 2) };
        auto out = run (e, in, p);
        const int L = e.getLatency();
        double err = 0;
        for (int i = L + 2000; i < n; ++i) err = std::max (err, (double) std::abs (out.l[(size_t) i] - in.l[(size_t) (i - L)]));
        const juce::String what = "LINEAR au repos : sortie = entree retardee, " + juce::String (Engine::factors[o]) + "x";
        check (db (err) < -100.0, what.toRawUTF8(), "ecart max " + juce::String (db (err), 1) + " dBFS");
    }
    for (int q : { dynamic, live })
    {
        Engine e; auto p = neutral (q, 1); e.prepare (fs, block, p);
        double worst = 0;
        for (double f : { 60.0, 500.0, 1500.0, 3000.0, 6000.0, 12000.0, 18000.0 })
        {
            const int n = 24000;
            Stereo in { sine (f, 0.1, fs, n), sine (f, 0.1, fs, n) };
            e.prepare (fs, block, p);
            auto out = run (e, in, p);
            worst = std::max (worst, std::abs (db (rms (out.l, 8000, n) / rms (in.l, 8000, n))));
        }
        const juce::String what = juce::String (qname (q)) + " au repos : amplitude plate 60 Hz - 18 kHz";
        check (worst < 0.05, what.toRawUTF8(), "ecart max " + juce::String (worst, 3) + " dB");
    }

    // ---------------------------------------------------------------- 3. limitation : la bande descend au seuil, le reste ne bouge pas
    for (int q = 0; q < 3; ++q)
        for (int o : { 0, 1, 2 })
        {
            Engine e; auto p = neutral (q, o);
            p.mainLo = 3000; p.mainMode = 0; p.thrA = p.thrB = -20; p.attackFast = true;
            e.prepare (fs, block, p);
            const int n = 48000;
            Signal x = sine (10000, 0.5, fs, n);           // -6 dBFS dans la bande
            add (x, sine (200, 0.3, fs, n));              // hors bande
            Stereo in { x, x };
            auto out = run (e, in, p);
            const double hf = tone (out.l, 10000, fs, 24000, n), lf = tone (out.l, 200, fs, 24000, n);
            const double hfIn = 0.5 * bandMagnitude (BandEdges::make (3000, 20000, 0), 10000, fs);
            const juce::String w1 = juce::String (qname (q)) + " " + juce::String (Engine::factors[o]) + "x : bande -6 dB ramenee vers -20 dB";
            check (db (hf) < -17.0 && db (hf) > -23.0, w1.toRawUTF8(), "10 kHz sort a " + juce::String (db (hf), 2) + " dB (entree " + juce::String (db (hfIn), 1) + ")");
            const juce::String w2 = juce::String (qname (q)) + " " + juce::String (Engine::factors[o]) + "x : 200 Hz intact";
            check (std::abs (db (lf / 0.3)) < 0.2, w2.toRawUTF8(), "ecart " + juce::String (db (lf / 0.3), 3) + " dB");
        }

    // ---------------------------------------------------------------- 3b. forme exacte : TRIM -6 dB seul, rendu = 1 − (1 − t)·|bande|
    //   (la courbe de l'interface), pour les trois formes, les trois qualités, tous les facteurs
    for (int q = 0; q < 3; ++q)
        for (int o = 0; o < 4; ++o)
        {
            double worst = 0;
            // drapeaux dans les aigus (2 – 8 kHz) et dans les graves (80 – 400 Hz)
            struct Cfg { double lo, hi; std::vector<double> freqs; };
            const Cfg cfgs[] { { 2000, 8000, { 1500.0, 3000.0, 5000.0, 9000.0, 14000.0 } }, { 80, 400, { 50.0, 100.0, 200.0, 350.0, 800.0, 3000.0 } } };
            for (const auto& cfg : cfgs)
                for (int mode = 0; mode < 3; ++mode)
                {
                    Engine e; auto p = neutral (q, o); p.mainLo = cfg.lo; p.mainHi = cfg.hi; p.mainMode = mode; p.mainTrim = -6;
                    const int n = 48000;
                    for (double f : cfg.freqs)
                    {
                        e.prepare (fs, block, p);
                        Stereo in { sine (f, 0.1, fs, n), sine (f, 0.1, fs, n) };
                        auto out = run (e, in, p);
                        const double expect = db (1.0 - (1.0 - 0.5011872) * bandMagnitude (BandEdges::make (cfg.lo, cfg.hi, mode), f, fs));
                        worst = std::max (worst, std::abs (db (tone (out.l, f, fs, 18000, n) / 0.1) - expect));
                    }
                }
            const juce::String what = juce::String ("forme de bande = courbe affichee, ") + qname (q) + " " + juce::String (Engine::factors[o]) + "x";
            check (worst < 0.2, what.toRawUTF8(), "ecart max " + juce::String (worst, 3) + " dB (SPLIT, DEFINED, FULL HF)");
        }

    // ---------------------------------------------------------------- 4. DELTA : sortie normale − delta = a(x) (polarité et retard)
    {
        Engine a, b; auto p = neutral (linear, 1);
        p.thrA = p.thrB = -24; p.mainLo = 3000;
        p.parOn = true; p.parThr = -30; p.parAmountDb = -6;
        a.prepare (fs, block, p); b.prepare (fs, block, p);
        const int n = 30000;
        Stereo in { noise (0.2, n, 3), noise (0.2, n, 4) };
        auto normal = run (a, in, p);
        auto pd = p; pd.delta = true;
        auto delta = run (b, in, pd);
        const int L = a.getLatency();
        double err = 0, dl = 0;
        for (int i = L + 4000; i < n; ++i)
        {
            err = std::max (err, (double) std::abs (normal.l[(size_t) i] - delta.l[(size_t) i] - in.l[(size_t) (i - L)]));
            dl = std::max (dl, (double) std::abs (delta.l[(size_t) i]));
        }
        check (db (err) < -90.0 && db (dl) > -40.0, "DELTA + entree alignee = sortie (LINEAR)", "ecart " + juce::String (db (err), 1) + " dB, delta " + juce::String (db (dl), 1) + " dBFS");
    }
    {
        // au repos, DELTA est silencieux dans toutes les qualités
        double worst = -300;
        for (int q = 0; q < 3; ++q)
        {
            Engine e; auto p = neutral (q, 1); p.delta = true; e.prepare (fs, block, p);
            const int n = 20000;
            Stereo in { noise (0.1, n, 5), noise (0.1, n, 6) };
            auto out = run (e, in, p);
            double m = 0; for (int i = 6000; i < n; ++i) m = std::max (m, (double) std::abs (out.l[(size_t) i]));
            worst = std::max (worst, db (m));
        }
        check (worst < -100.0, "DELTA silencieux sans reduction (3 qualites)", "max " + juce::String (worst, 1) + " dBFS");
    }

    // ---------------------------------------------------------------- 5. temps adaptatifs : FAST plus vif que SLOW, plus vif quand la réduction grandit
    {
        auto attackTime = [&] (bool fast, double amp)
        {
            Engine e; auto p = neutral (live, 0);
            p.thrA = p.thrB = -30; p.mainLo = 4000; p.attackFast = fast; p.mainLink = 0;
            e.prepare (fs, block, p);
            const int n = 24000, on = 4800;
            Signal x ((size_t) n, 0.0f);
            for (int i = on; i < n; ++i) x[(size_t) i] = (float) (amp * std::sin (2 * pi * 9000 * i / fs));
            Stereo in { x, x };
            // la réduction finale, puis le temps pour en atteindre 90 %
            double grEnd = 0;
            std::vector<double> gr;
            run (e, in, p, 32, nullptr, [&] (int, EngineParams&) { gr.push_back (e.meters.grA.load()); });
            grEnd = gr.back();
            for (size_t k = 0; k < gr.size(); ++k)
                if ((int) (k * 32) > on && gr[k] >= 0.9 * grEnd) return 1000.0 * ((double) k * 32 - on) / fs;
            return 1.0e9;
        };
        const double slow = attackTime (false, 0.25), fast = attackTime (true, 0.25);
        check (fast < slow, "ATTACK FAST plus vif que SLOW", "90 % de la reduction : FAST " + juce::String (fast, 2) + " ms, SLOW " + juce::String (slow, 2) + " ms");
        const double small = attackTime (false, 0.05), big = attackTime (false, 0.9);
        check (big < small, "attaque plus vive quand la reduction grandit (SLOW)", "4 dB : " + juce::String (small, 2) + " ms, 29 dB : " + juce::String (big, 2) + " ms");

        auto releaseTime = [&] (bool fast)
        {
            Engine e; auto p = neutral (live, 0);
            p.thrA = p.thrB = -30; p.mainLo = 4000; p.releaseFast = fast; p.attackFast = true;
            e.prepare (fs, block, p);
            const int n = 96000, off = 24000;
            Signal x ((size_t) n, 0.0f);
            for (int i = 0; i < off; ++i) x[(size_t) i] = (float) (0.25 * std::sin (2 * pi * 9000 * i / fs));
            Stereo in { x, x };
            std::vector<double> gr;
            run (e, in, p, 32, nullptr, [&] (int, EngineParams&) { gr.push_back (e.meters.grA.load()); });
            const double start = gr[(size_t) (off / 32)];
            for (size_t k = (size_t) (off / 32); k < gr.size(); ++k)
                if (gr[k] <= 0.1 * start) return 1000.0 * ((double) k * 32 - off) / fs;
            return 1.0e9;
        };
        const double rs = releaseTime (false), rf = releaseTime (true);
        check (rf < rs, "RELEASE FAST plus vif que SLOW", "retour a 10 % : FAST " + juce::String (rf, 1) + " ms, SLOW " + juce::String (rs, 1) + " ms");
    }

    // ---------------------------------------------------------------- 6. stéréo : MS et liaison
    {
        Engine e; auto p = neutral (dynamic, 1);
        p.stereo = 1; p.thrA = 0; p.thrB = -30; p.mainLink = 0; p.mainLo = 4000;
        e.prepare (fs, block, p);
        const int n = 48000;
        // Mid : 6 kHz fort mais sous son seuil ; Side : 9 kHz
        Signal m = sine (6000, 0.3, fs, n), s = sine (9000, 0.3, fs, n);
        Stereo in { m, m };
        for (int i = 0; i < n; ++i) { in.l[(size_t) i] = m[(size_t) i] + s[(size_t) i]; in.r[(size_t) i] = m[(size_t) i] - s[(size_t) i]; }
        auto out = run (e, in, p);
        Signal om ((size_t) n), os ((size_t) n);
        for (int i = 0; i < n; ++i) { om[(size_t) i] = 0.5f * (out.l[(size_t) i] + out.r[(size_t) i]); os[(size_t) i] = 0.5f * (out.l[(size_t) i] - out.r[(size_t) i]); }
        const double mid = db (tone (om, 6000, fs, 24000, n) / 0.3), side = db (tone (os, 9000, fs, 24000, n) / 0.3);
        check (std::abs (mid) < 0.3 && side < -6.0, "MS : seuils separes (Mid libre, Side limite)", "Mid " + juce::String (mid, 2) + " dB, Side " + juce::String (side, 2) + " dB");

        Engine e2; auto p2 = neutral (dynamic, 1); p2.thrA = p2.thrB = -30; p2.mainLink = 1.0; p2.mainLo = 4000;
        e2.prepare (fs, block, p2);
        Stereo in2 { sine (9000, 0.3, fs, n), Signal ((size_t) n, 0.0f) };
        add (in2.r, sine (300, 0.3, fs, n));
        run (e2, in2, p2);
        check (std::abs (e2.meters.grA.load() - e2.meters.grB.load()) < 0.1 && e2.meters.grB.load() > 3.0f,
               "liaison 100 % : meme reduction des deux cotes", "L " + juce::String (e2.meters.grA.load(), 2) + " dB, R " + juce::String (e2.meters.grB.load(), 2) + " dB");
    }

    // ---------------------------------------------------------------- 7. formes de bande
    {
        auto bandGain = [&] (int mode, double f)
        {
            Engine e; auto p = neutral (linear, 0);
            p.mainLo = 2000; p.mainHi = 14000; p.mainMode = mode; p.mainTrim = -6; p.thrA = p.thrB = 0;
            e.prepare (fs, block, p);
            const int n = 24000;
            Stereo in { sine (f, 0.1, fs, n), sine (f, 0.1, fs, n) };
            auto out = run (e, in, p);
            return db (tone (out.l, f, fs, 8000, n) / 0.1);
        };
        const double fEdge = bandGain (2, 1500), sEdge = bandGain (0, 1500);
        check (fEdge < sEdge - 0.3, "FULL HF : pente douce (prend plus sous le drapeau)", "1,5 kHz FULL " + juce::String (fEdge, 2) + " dB, SPLIT " + juce::String (sEdge, 2) + " dB");
        const auto e = BandEdges::make (8000, 9000, 1);
        check (e.hi / e.lo >= std::sqrt (2.0) - 1e-9, "drapeaux : demi-octave minimum", juce::String (e.lo, 0) + " / " + juce::String (e.hi, 0) + " Hz");
    }

    // ---------------------------------------------------------------- 8. sidechain EXT
    {
        Engine e; auto p = neutral (dynamic, 1); p.thrA = p.thrB = -30; p.scMain = true; p.mainLo = 4000;
        e.prepare (fs, block, p);
        const int n = 48000;
        Stereo in { sine (9000, 0.02, fs, n), sine (9000, 0.02, fs, n) };    // -34 dB : sous le seuil
        Stereo sc { sine (9000, 0.5, fs, n), sine (9000, 0.5, fs, n) };
        auto out = run (e, in, p, 256, &sc);
        const double g = db (tone (out.l, 9000, fs, 24000, n) / 0.02);
        check (g < -6.0 && e.meters.extUsed.load(), "EXT : la sidechain commande la reduction", "bande " + juce::String (g, 2) + " dB");
        Engine e2; auto p2 = p; p2.scMain = false; e2.prepare (fs, block, p2);
        auto out2 = run (e2, in, p2, 256, &sc);
        check (std::abs (db (tone (out2.l, 9000, fs, 24000, n) / 0.02)) < 0.2, "EXT coupe sur MAIN : la sidechain est ignoree", "");
    }

    // ---------------------------------------------------------------- 9. PARALIMIT
    {
        const int n = 48000;
        Stereo in { sine (9000, 0.3, fs, n), sine (11000, 0.3, fs, n) };
        auto parOut = [&] (int mode)
        {
            Engine e; auto p = neutral (dynamic, 1); p.parOn = true; p.parThr = -30; p.parAmountDb = 0; p.parChannel = mode; p.parSolo = true; p.parLo = 4000;
            e.prepare (fs, block, p);
            return run (e, in, p);
        };
        auto m = parOut (0), s = parOut (1), lr = parOut (2);
        double dm = 0, ds = 0;
        for (int i = 24000; i < n; ++i) { dm = std::max (dm, (double) std::abs (m.l[(size_t) i] - m.r[(size_t) i])); ds = std::max (ds, (double) std::abs (s.l[(size_t) i] + s.r[(size_t) i])); }
        check (db (dm) < -100 && db (ds) < -100, "PARALIMIT : extraction MID (L = R) et SIDE (L = -R)", "");
        const double lrL9 = tone (lr.l, 9000, fs, 24000, n), lrL11 = tone (lr.l, 11000, fs, 24000, n);
        check (lrL9 > 0.05 && lrL11 < 0.003, "PARALIMIT LR : chaque canal garde le sien (vrai L/R)", "L 9k " + juce::String (db (lrL9), 1) + " dB, L 11k " + juce::String (db (lrL11), 1) + " dB");

        // le rattrapage ramène la bande dense au niveau moyen de la bande d'origine
        Engine e; auto p = neutral (dynamic, 1); p.parOn = true; p.parThr = -24; p.parAmountDb = 0; p.parSolo = true; p.parLo = 4000;
        e.prepare (fs, block, p);
        auto o = run (e, in, p);
        const double lvl = db (rms (o.l,  36000, n) / rms (in.l, 36000, n));
        check (std::abs (lvl) < 1.5, "PARALIMIT : rattrapage au niveau de la bande", "niveau " + juce::String (lvl, 2) + " dB, rattrapage " + juce::String (e.meters.parMakeup.load(), 1) + " dB");

        Engine off; auto po = neutral (linear, 1); po.parOn = true; po.parAmountDb = -60; off.prepare (fs, block, po);
        auto oo = run (off, in, po);
        double d = 0; const int L = off.getLatency();
        for (int i = 20000; i < n; ++i) d = std::max (d, (double) std::abs (oo.l[(size_t) i] - in.l[(size_t) (i - L)]));
        check (db (d) < -60, "PARALIMIT AMOUNT au minimum : rien n'est ajoute", juce::String (db (d), 1) + " dB");
    }

    // ---------------------------------------------------------------- 10. sans clic : drapeaux balayés, formes changées, bypass, solo, qualité
    {
        auto maxJump = [&] (std::function<void (int, EngineParams&)> change, int q)
        {
            Engine e; auto p = neutral (q, 1); p.thrA = p.thrB = -20; p.mainLo = 3000;
            e.prepare (fs, block, p);
            const int n = 96000;
            Stereo in { sine (440, 0.4, fs, n), sine (440, 0.4, fs, n) };
            add (in.l, sine (7000, 0.2, fs, n)); add (in.r, sine (7000, 0.2, fs, n));
            auto out = run (e, in, p, 256, nullptr, change);
            // dérivée seconde : un clic s'y voit, une sinusoïde à 7 kHz y est bornée
            double worst = 0;
            for (int i = 2; i < n; ++i)
            {
                const double d2 = out.l[(size_t) i] - 2.0 * out.l[(size_t) (i - 1)] + out.l[(size_t) (i - 2)];
                worst = std::max (worst, std::abs (d2));
            }
            return worst;
        };
        const double ref = maxJump ({}, dynamic);
        const double sweep = maxJump ([] (int s, EngineParams& p) { p.mainLo = 1000.0 * std::pow (16.0, (s % 48000) / 48000.0); }, dynamic);
        check (sweep < ref * 1.25, "drapeau balaye 1 -> 16 kHz sans clic (DYNAMIC)", "pic " + juce::String (sweep / ref, 2) + " x la reference");
        const double sweepL = maxJump ([] (int s, EngineParams& p) { p.mainLo = 1000.0 * std::pow (16.0, (s % 48000) / 48000.0); }, linear);
        check (sweepL < ref * 1.25, "drapeau balaye sans clic (LINEAR, reponses fondues)", "pic " + juce::String (sweepL / ref, 2) + " x");
        const double modes = maxJump ([] (int s, EngineParams& p) { p.mainMode = (s / 12000) % 3; }, dynamic);
        check (modes < ref * 1.25, "SPLIT / DEFINED / FULL HF changes sans clic", "pic " + juce::String (modes / ref, 2) + " x");
        const double byp = maxJump ([] (int s, EngineParams& p) { p.bypass = (s / 9000) % 2 == 1; }, linear);
        check (byp < ref * 1.25, "bypass sans clic", "pic " + juce::String (byp / ref, 2) + " x");
        const double solo = maxJump ([] (int s, EngineParams& p) { p.mainSolo = (s / 9000) % 3; p.delta = (s / 15000) % 2 == 1; }, dynamic);
        check (solo < ref * 1.6, "SOLO / DELTA en fondu", "pic " + juce::String (solo / ref, 2) + " x");
        const double ql = maxJump ([] (int s, EngineParams& p) { p.quality = (s / 24000) % 3; }, dynamic);
        check (ql < ref * 1.25, "changement de qualite : fondu au silence", "pic " + juce::String (ql / ref, 2) + " x");
    }

    // ---------------------------------------------------------------- 11. stabilité
    {
        Engine e; auto p = neutral (dynamic, 3); e.prepare (fs, block, p);
        const int n = 96000;
        Stereo in { noise (1.5, n, 11), noise (1.5, n, 12) };
        std::mt19937 g (5); std::uniform_real_distribution<double> u (0, 1);
        auto out = run (e, in, p, 97, nullptr, [&] (int, EngineParams& q)
        {
            q.thrA = -60 * u (g); q.thrB = -60 * u (g); q.mainLo = 1000 + 19000 * u (g); q.mainHi = 1000 + 19000 * u (g);
            q.mainMode = (int) (3 * u (g)); q.stereo = u (g) > 0.5; q.mainLink = u (g); q.mainTrim = -6 + 12 * u (g);
            q.parOn = u (g) > 0.3; q.parThr = -60 * u (g); q.parAmountDb = -60 + 72 * u (g); q.parChannel = (int) (3 * u (g));
            q.attackFast = u (g) > 0.5; q.releaseFast = u (g) > 0.5; q.mix = u (g); q.levelComp = u (g) > 0.5;
        });
        bool finite = true; double peak = 0;
        for (int i = 0; i < n; ++i) { finite &= std::isfinite (out.l[(size_t) i]) && std::isfinite (out.r[(size_t) i]); peak = std::max (peak, (double) std::abs (out.l[(size_t) i])); }
        check (finite && peak < 40.0, "automation aleatoire extreme : sortie finie et bornee", "crete " + juce::String (db (peak), 1) + " dBFS");

        Engine m; auto pm = neutral (linear, 1); pm.thrA = -20; m.prepare (fs, block, pm);
        Signal mono = sine (9000, 0.5, fs, 24000);
        juce::AudioBuffer<float> b (1, 24000);
        b.copyFrom (0, 0, mono.data(), 24000);
        for (int s = 0; s < 24000; s += 256)
        {
            juce::AudioBuffer<float> part (b.getArrayOfWritePointers(), 1, s, std::min (256, 24000 - s));
            m.process (part, nullptr, 0, pm);
        }
        check (std::isfinite (b.getSample (0, 23999)) && m.meters.grA.load() > 3.0f, "mono : traite sans erreur", "");
    }

    // ---------------------------------------------------------------- 12. CPU
    {
        std::printf ("\nCPU (1 s de stereo, MAIN + PARALIMIT + sidechain) :\n");
        const int n = 48000;
        Stereo in { noise (0.2, n, 21), noise (0.2, n, 22) }, sc { noise (0.2, n, 23), noise (0.2, n, 24) };
        for (int q = 0; q < 3; ++q)
            for (int o = 0; o < 4; ++o)
            {
                Engine e; auto p = neutral (q, o); p.thrA = p.thrB = -24; p.parOn = true; p.scMain = true; p.scPar = true;
                e.prepare (fs, block, p);
                const auto t0 = std::chrono::high_resolution_clock::now();
                run (e, in, p, 512, &sc);
                const double ms = std::chrono::duration<double, std::milli> (std::chrono::high_resolution_clock::now() - t0).count();
                std::printf ("   %-8s %dx : %6.1f ms  (%.1f %% d'un coeur)\n", qname (q), Engine::factors[o], ms, ms / 10.0);
            }
    }

    std::printf ("\n%s : %d echec(s)\n", failures == 0 ? "TOUT EST BON" : "A CORRIGER", failures);
    return failures;
}
