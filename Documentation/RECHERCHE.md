# Ce qui vient de la documentation publique, et ce que HALO a choisi

HALO s'inspire des concepts **publiquement documentés** d'un limiteur d'accélération des aigus du marché (documentation
utilisateur en ligne de son éditeur). Il ne reprend aucun code, aucune ressource graphique, aucun nom de marque et aucune
donnée de calibration de cet éditeur. Aucun binaire commercial n'a été désassemblé ou analysé.

## Repris de la documentation publique (fonctionnalités)

**Le traitement**

- Traitement échantillon par échantillon.
- Une attaque et un relâchement qui accélèrent quand la réduction grandit, sans réglage de temps.
- Deux sélecteurs SLOW / FAST.

**Les drapeaux de fréquence**

- De 1 à 20 kHz, sur une échelle logarithmique.
- Une demi-octave minimum entre les deux.
- Trois configurations de bande.
- Un solo par drapeau, et un solo momentané au clic droit.

**La stéréo**

- Les modes L/R et M/S, avec des seuils séparés.
- Une liaison stéréo continue.
- Un TRIM de ±6 dB indépendant du seuil.

**PARALIMIT**

- Une bande parallèle à seuil propre, avec un rattrapage automatique.
- Une extraction M, LR ou S, limitée sur le vrai L/R avant l'extraction.
- Ses propres drapeaux et un solo.
- La bande d'origine coupée dans la voie parallèle.

**Sidechain, analyse et réglages**

- Une sidechain EXT vers la détection.
- Un affichage de balance tonale avec référence.
- Un suréchantillonnage, et un mode à phase linéaire.

## Choisi pour HALO (non documenté chez l'éditeur, conçu indépendamment)

**Le traitement**

- La structure complémentaire `y = a(x) + (g·trim − 1)·b` et ses trois mises en œuvre (FIR, LR4/LR2 IIR).
- Le nul exact de LINEAR.
- La forme FULL HF à 12 dB/oct.
- La loi de temps `τ = τ0 / (1 + k·réduction)`, ses constantes et leurs tables.
- La détection par maximum glissant (anticipation, demi-période du drapeau bas).

**PARALIMIT**

- Le rattrapage par rapport de puissances lent, borné à +24 dB.

**Latence, qualités et écoute**

- La mesure de latence au prepare : impulsion, corrélation, phase, et complément fractionnaire en domaine
  suréchantillonné.
- Les qualités LINEAR / DYNAMIC / LIVE comme trois chaînes différentes.
- DELTA = `y − a(x)`.
- MATCH par rapport de puissances.

**L'interface et l'analyse**

- Les courbes de référence tonale (Neutral = bruit rose, Modern Mix, Vocal, Custom capturée) : des courbes originales
  ONIRIQUE.
- Le lissage gaussien de l'analyseur (celui de VOCALBOX).
- L'interface : la DA ONIRIQUE de VOCALBOX (verre, braise, potards, polices). La disposition suit l'ergonomie
  fonctionnelle d'un limiteur d'aigus de studio : fader de seuils, sélecteurs, liaison, LED, TRIM, puis PARALIMIT.
