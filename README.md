# ONIRIQUE HALO

Limiteur d'accélération des aigus (VST3 Windows, VST3 + AU macOS universel), JUCE 8.

- `Source/dsp/` : le traitement (séparations, limiteur adaptatif, PARALIMIT, suréchantillonnage, latences mesurées).
- `Source/ui/` : l'interface, dans la DA ONIRIQUE de VOCALBOX.
- `Tests/HaloBench.cpp` : le banc de mesure, lancé par la CI. Son code de sortie est le nombre d'échecs.
- `Documentation/DSP.md` : le modèle mathématique, les latences, les tests et les limites.
- `Documentation/RECHERCHE.md` : ce qui vient de la documentation publique, et ce qui a été conçu pour HALO.

## Compiler

    cmake -B build
    cmake --build build --config Release
    build/HaloBench_artefacts/Release/HaloBench.exe

Options :

- `-DHALO_LICENCE=OFF` : un build d'essai sans licence, jamais publié.
- `-DONIRIQUE_UI_TEST=ON` : captures et démo.

À chaque envoi sur `main`, la CI :

1. compile Mac et Windows ;
2. passe le banc ;
3. publie dans ONIRIQUE CENTRAL, si le secret de publication est présent.
