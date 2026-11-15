# ONIRIQUE HALO — le traitement

HALO limite l'énergie des aigus (« l'accélération » du signal, qui croît avec la fréquence) sans toucher au reste du
spectre. Deux moteurs travaillent en même temps : **MAIN**, un limiteur de bande, et **PARALIMIT**, une bande parallèle
dense ajoutée au signal. Les fichiers : `Source/dsp/HaloDsp.h` (les briques) et `Source/dsp/HaloEngine.h` (la chaîne).

## 1. Le principe : seule la bande change

Un filtre sépare le signal `x` en un « reste » et une bande `b`, de sorte que `reste + b = a(x)`. Selon la qualité,
`a` est un simple retard (phase linéaire), un passe-tout (phase minimale), ou `x` lui-même. La sortie de MAIN vaut :

    y = a(x) + mix · (g · trim − 1) · b

- `g ≤ 1` est le gain calculé par le limiteur ;
- `trim` est le TRIM de la bande (−6 … +6 dB) ;
- `mix` est le DRY/WET de MAIN.

Au repos (`g = 1`, `trim = 0 dB`), `y = a(x)` :

- en LINEAR, la sortie est l'entrée retardée, à l'échantillon près (le banc mesure −240 dBFS d'écart) ;
- en DYNAMIC et LIVE, l'amplitude est plate à ±0,005 dB.

Dans les trois formes de bande, `b` est **en phase** avec `a(x)` (croisements Linkwitz-Riley). L'amplitude rendue est
donc exactement :

    |y| / |a(x)| = 1 − (1 − g·trim) · |B(f)|

C'est la courbe que dessine l'interface ; le banc vérifie l'accord à 0,01 dB près pour les 3 formes, les 3 qualités et
les 4 facteurs de suréchantillonnage.

## 2. Les formes de bande (drapeaux de 50 Hz à 20 kHz)

| Forme   | `|B(f)|` (w = f / bas, v = f / haut, fréquences déformées par la bilinéaire) | Pente   |
|---------|-------------------------------------------------------------------------------|---------|
| SPLIT   | `w⁴ / (1 + w⁴)` (Linkwitz-Riley au drapeau bas)                              | 24 dB/oct |
| DEFINED | `w⁴ / (1 + w⁴) · 1 / (1 + v⁴)` (entre les deux drapeaux)                      | 24 dB/oct |
| FULL HF | `w² / (1 + w²)` (Linkwitz-Riley du 2e ordre)                                  | 12 dB/oct |

- En DEFINED, une demi-octave au moins sépare les drapeaux. Une position invalide est corrigée : le drapeau du haut
  est repoussé, et s'il ne peut plus monter, c'est celui du bas qui descend. L'interface repousse le partenaire
  pendant le glisser.
- Les fréquences « déformées » (`tan(π f / fs)`) donnent la réponse **numérique** exacte des filtres IIR. Le FIR à
  phase linéaire copie la même amplitude, si bien que les trois qualités ont la même courbe jusqu'à Nyquist.

### Mise en œuvre

**IIR (DYNAMIC, LIVE)**

- Des SVF TPT (Simper) aux coefficients lissés : les drapeaux glissent sans clic.
- SPLIT = croisement LR4 : la bande est le haut, `a` est la somme bas + haut (un passe-tout).
- DEFINED = le haut recoupé au drapeau haut. Le bas passe dans le passe-tout du drapeau haut pour rester aligné.
- FULL HF = LR2 : `a = bas − haut` (passe-tout du 1er ordre) et la bande vaut `−haut`, en phase avec `a`.
- Les trois formes tournent en permanence. Changer de forme fond leurs sorties en 30 ms, sans clic.

**FIR (LINEAR)**

- Une réponse symétrique, obtenue par échantillonnage fréquentiel de `|B(f)|` et une fenêtre de Kaiser (β = 7).
- Sa **longueur est fixe** pour une fréquence d'échantillonnage (≈ 170 ms, de quoi suivre une pente à 24 dB/oct dès 50 Hz), si bien que la latence ne bouge pas quand
  on déplace les drapeaux.
- Elle est appliquée par une convolution FFT à partitions uniformes, écrite pour HALO et synchrone : c'est ce qui permet
  de mesurer sa latence.
- Une nouvelle réponse est calculée hors du thread audio (minuterie à 25 Hz), puis fondue sur 4 partitions.

## 3. Le limiteur d'accélération (MAIN et PARALIMIT)

Le gain est calculé **échantillon par échantillon**, au rythme suréchantillonné, dans le domaine des dB.

    L(n)   niveau détecté : maximum glissant de |clé| sur l'anticipation, et au moins une demi-période
           du drapeau bas (l'énergie de la bande commande, pas l'oscillation de chaque cycle)
    Gd(n)  = min(0, T − L(n))                     gain voulu (T : seuil, dBFS de crête de bande)
    Gd     ← Gd + link · (min(Gd_A, Gd_B) − Gd)   liaison stéréo continue (0 … 100 %)
    a      = −G(n)                                réduction déjà en cours
    τ      = max(τmin, τ0 / (1 + k · a))          τ0, k, τmin de l'ATTACK si Gd < G, de la RELEASE sinon
    G(n+1) = G(n) + (Gd − G(n)) · (1 − e^(−1/(τ·fs)))

**Pas de temps d'attaque ni de relâchement fixes.** La constante de temps se raccourcit à mesure que la réduction
grandit :

- une crête isolée et forte est saisie vite et rendue vite ;
- un passage dense et modéré est tenu plus calmement, sans pompage.

ATTACK et RELEASE ont chacun deux profils indépendants, SLOW et FAST, qui changent la loi (τ0, k, τmin) et non un temps
fixe. Les coefficients sont tabulés tous les 0,1 dB de réduction : aucun `exp()` par échantillon pour la constante de
temps. Les constantes de la loi sont internes (`AccelLimiter` dans `HaloDsp.h`) ; le temps effectif dépend du programme.

**Ce que le banc mesure (48 kHz, LIVE 1x, sinus à 9 kHz, seuil −30 dB) :**

| Mesure                                        | SLOW      | FAST     |
|-----------------------------------------------|-----------|----------|
| 90 % de la réduction finale (12 dB au-dessus) | 2,67 ms   | 0,67 ms  |
| ATTACK SLOW, dépassement de 4 dB / de 29 dB   | 6,67 ms / 2,00 ms | — |
| retour à 10 % de la réduction                 | 292 ms    | 66 ms    |

### Stéréo de MAIN

- **L/R** : chaque canal a sa détection.
- **M/S** : la détection et la bande passent en `M = (L + R) / 2`, `S = (L − R) / 2`, chacune avec son seuil, puis
  l'écart est redécodé `L = M + S`, `R = M − S`.
- Avec l'icône de liaison des seuils, le second suit le premier.

### Anticipation

En LINEAR et DYNAMIC, les bandes sont retardées de 1,5 ms (au rythme suréchantillonné) et la détection lit la clé non
retardée : le gain est déjà bas quand la crête arrive. LIVE n'a pas d'anticipation.

## 4. PARALIMIT

1. Il reçoit le signal d'origine et isole **sa propre** bande, avec ses drapeaux et sa forme.
2. Il limite cette bande **sur le vrai L/R**, avec son seuil, sa liaison, son ATTACK et sa RELEASE.
3. Un rattrapage ramène la bande dense au niveau moyen de la bande d'origine (rapport de puissances lent, 0 … +24 dB).
4. Il extrait ensuite **MID** (`(L + R) / 2` dans les deux canaux), **SIDE** (`±(L − R) / 2`) ou **LR**.
5. Il applique TRIM et AMOUNT (−∞ … +12 dB), puis ajoute le résultat au signal.

PARALIMIT ne double pas MAIN : sa bande vient de l'entrée, pas de la sortie de MAIN. MAIN DRY/WET ne le touche pas.

## 5. Suréchantillonnage et latence

Seules les bandes et les clés sont suréchantillonnées (1x, 2x, 4x, 8x), et le rendu hors ligne a son propre facteur.
Le reste du signal ne passe par aucun filtre de suréchantillonnage, ce qui permet le nul exact de LINEAR.

| Qualité | Séparation                     | Suréchantillonnage                                  | Anticipation |
|---------|--------------------------------|-----------------------------------------------------|--------------|
| LINEAR  | FIR à phase linéaire           | FIR demi-bande à phase linéaire                     | oui          |
| DYNAMIC | LR IIR (phase minimale)        | FIR demi-bande à phase linéaire                     | oui          |
| LIVE    | LR IIR                         | IIR polyphase (le plus court) ; `a(x)` y passe aussi | non          |

**La latence n'est pas annoncée : elle est mesurée** au `prepare`, sur les vrais étages.

- **FIR de séparation** : une impulsion passe dans le convolueur, et la latence est la position du maximum de la réponse.
- **Suréchantillonnage FIR** : on mesure d'abord le retard au pic de corrélation sur bruit blanc, puis le retard exact
  (souvent fractionnaire) par la phase d'un sinus à fs/32. La fraction est complétée jusqu'à l'échantillon entier
  suivant par un retard dans le domaine suréchantillonné, si bien que l'alignement est exact. *(Le banc a montré que la
  latence annoncée par JUCE en 4x/8x en mode « entier » cassait la phase linéaire : HALO ne s'y fie pas.)*
- **LIVE** : la chaîne réelle (passe-tout IIR puis suréchantillonnage IIR), avec le retard au pic de corrélation sur
  bruit blanc.

La latence déclarée au DAW est celle de la chaîne en service. Quand elle change (qualité, facteur), la minuterie du
processeur la redéclare. Le changement de chaîne passe par un fondu au silence, maintenu le temps que les nouvelles
lignes de retard se remplissent.

**Latences mesurées à 48 kHz (en échantillons) :**

| Qualité | 1x  | 2x  | 4x  | 8x  |
|---------|-----|-----|-----|-----|
| LINEAR  | 4280 | 4329 | 4340 | 4345 |
| DYNAMIC | 72  | 121 | 132 | 137 |
| LIVE    | 0   | 7   | 8   | 9   |

L'interface affiche la latence de la chaîne en service. Le menu ⚙ (Latences mesurées) donne le tableau à la fréquence
de la session.

## 6. Écoute, sorties, sécurité

- **DELTA** = `y − a(x)`, compté avec PARALIMIT et le DRY/WET : exactement ce que HALO enlève ou ajoute, dans la bonne
  polarité et le bon alignement. Au repos, il est silencieux dans les trois qualités (< −240 dBFS).
- **Solos de MAIN** :
  - la bande traitée ;
  - au-dessus du drapeau bas, ou au-dessus du drapeau haut : le vrai signal filtré, par les croisements LR4 aux
    drapeaux, au retard total ;
  - le reste (hors bande).
- **Solo de PARALIMIT** : seulement ce qu'il ajoute.
- **Bascules sans clic** : SOLO, DELTA et bypass sont fondus (10 à 12 ms).
- **MATCH** (compensation de niveau) : un rapport de puissances lent (1,5 s) entre `a(x)` et `y`, borné à ±12 dB.
- **EXT** : la sidechain externe (bus stéréo facultatif) passe par les drapeaux de chaque moteur et remplace sa clé.
  MAIN et PARALIMIT s'y branchent séparément. Une sidechain absente ou débranchée est ignorée.
- **Thread audio** :
  - tout est réservé dans `prepare`, et il n'y a ni allocation, ni verrou, ni fichier dans le thread audio ;
  - les NaN et inf de l'hôte sont remplacés par 0 ;
  - les dénormaux sont désactivés ;
  - un mono est traité comme un stéréo identique.

## 7. Le banc (`Tests/HaloBench.cpp`, lancé par la CI)

Le banc couvre :

- **Latences** : latence réelle (corrélation) égale à la latence déclarée, pour les 3 qualités et les 4 facteurs.
- **Repos** : nul exact de LINEAR, et amplitude plate de DYNAMIC et LIVE.
- **Limitation** :
  - la bande descend au seuil : −19,8 dB pour un seuil de −20 dB, dans les 9 combinaisons testées ;
  - le hors-bande reste intact.
- **Forme de bande** égale à la courbe affichée (3 formes × 3 qualités × 4 facteurs).
- **DELTA** : polarité et alignement ; silencieux au repos.
- **Temps** : ATTACK et RELEASE FAST/SLOW, attaque plus vive quand la réduction grandit.
- **Stéréo** : seuils M/S séparés, liaison à 100 %.
- **EXT** : actif et coupé.
- **PARALIMIT** : extraction MID / SIDE / LR, vrai L/R, rattrapage, AMOUNT au minimum.
- **Sans clic** : drapeaux balayés (IIR et FIR), formes, bypass, SOLO / DELTA, changement de qualité.
- **Robustesse** : automation aléatoire extrême, mono.
- **CPU** pour chaque chaîne.

## 8. Limites connues

- **LINEAR en rendu hors ligne** : quand les drapeaux sont automatisés vite, les réponses FIR suivent au rythme de la
  minuterie (25 Hz) avec un fondu. Les autres qualités suivent à l'échantillon près.
- **Changer de qualité ou de facteur** change la latence. Le DAW recale après le fondu ; ce n'est pas un réglage à
  automatiser.
- **Retard de LIVE** : le suréchantillonnage IIR de LIVE n'est pas à phase linéaire. Le retard déclaré est celui du
  programme (corrélation large bande) ; `a(x)` passe par le même suréchantillonnage, si bien que la bande reste
  alignée sur le reste.
