# edel-check-pico

Firmware du boîtier **EdelCheck** - le terminal de comptoir qui vérifie une identité
numérique (e-ID suisse, EUDI Wallet, permis de conduire mobile) sans révéler plus que
nécessaire.

Ce dépôt contient uniquement l'embarqué. Le portail, la base, le broker MQTT et le
simulateur vivent dans le dépôt voisin [`edelcheck`](https://gitlab.com/hliosone/edelcheck).

| | |
|---|---|
| Cible | Raspberry Pi Pico 2 W (RP2350, Wi-Fi CYW43439) |
| Écran | e-ink 4,2" 400x300, contrôleur SSD1683, breakout Adafruit E-Ink Friend |
| Langage | C11, SDK Raspberry Pi Pico |
| Projet | PDG, HEIG-VD, semestre d'été 2026 - soutenance le 4 septembre 2026 |

---

## Ce que fait le firmware aujourd'hui

Le boîtier implémente le contrat MQTT d'EdelCheck de bout en bout, validé sur le matériel
le 24 août 2026.

**Au premier démarrage**, il n'a aucune identité. Il demande alors un code d'appairage au
cloud, l'affiche en grand sur la dalle, et interroge le serveur toutes les trois secondes
jusqu'à ce qu'un opérateur ait saisi ce code dans son portail. Il reçoit alors son
identifiant et son secret, les écrit en flash, les relit pour vérifier, et ne redemandera
plus jamais rien.

**Aux démarrages suivants**, il lit son identité, se connecte au broker en MQTT sur TLS,
déclare son testament, publie sa présence et s'abonne à ses trois topics. Le cloud lui
envoie la liste des profils de vérification que l'opérateur lui a assignés.

**Pendant le service**, une touche ouvre une session : le boîtier publie une demande, le
cloud contacte le service de vérification, rend un QR code en 400x300, et le pousse en huit
fragments. Le boîtier les seuille à la volée vers son framebuffer et rafraîchit l'écran en
638 ms. Le citoyen scanne, le verdict redescend de la même façon.

La navigation se fait **aux quatre boutons poussoirs**, ou au clavier par le port série USB
(touches `1`-`4`, `x`) tant qu'un câble est branché. Les deux sources produisent le meme
caractere. Les broches sont lues dans la boucle principale, pas par interruption : une
interruption sur ces broches empeche l'ecran de repondre.

**Le reseau Wi-Fi se saisit sur le boitier.** Apres deux echecs de connexion, il ouvre son
propre point d'acces et sert un formulaire, ce qui evite de reflasher l'appareil pour
changer de reseau.

Le boîtier ne détient aucun secret d'organisation et ne voit jamais un attribut personnel :
il reçoit une décision, un libellé et des pixels.

Ce qui **n'existe pas encore** : NFC, mise a jour du firmware a distance, rotation de
secret. Le niveau de batterie, lui, est mesure sur VSYS par le convertisseur
analogique-numerique et remonte au portail.

## Structure

```
main.c            séquence de démarrage et boucle de polling
screen/           driver SSD1683, framebuffer 1 bit/pixel, primitives de dessin
assets/           images en dur (écrans 400x300, police 16x15, icône batterie)
gpio/             initialisation SPI0 et broches de l'écran
boutons/          les quatre boutons, echantillonnes dans la boucle principale
power/            niveau de batterie, lu sur VSYS par l'ADC
wifi/             pilote cyw43 + client HTTP/HTTPS écrit sur altcp/mbedTLS
json/             frozen (tiers) + utilitaires
storage/          persistance en flash, deux secteurs alternés avec CRC
enrollment/       appairage : claim, affichage du code, poll, persistance
mqtt/             connexion au broker, abonnements, réception
profiles/         les profils de vérification reçus du cloud
session/          ouverture d'une session de vérification
image/            réassemblage des images, conversion 2 bpp vers 1 bpp
navigation/       machine à états du menu
config/           lwipopts.h, mbedtls_config.h
docs/             documentation d'architecture
tests/unit/       tests unitaires, exécutés sur PC (make)
tests/*/          bancs de test matériel (projets CMake séparés)
```

## Tests

```bash
cd tests/unit && make
```

Cinquante vérifications sur la conversion d'images, l'analyse de la configuration et le
rendu de texte. Aucune dépendance : ni SDK Pico, ni chaîne arm-none-eabi, ni matériel - le
pilote d'écran est remplacé par un bouchon.

Ce que ces tests ne couvrent pas, et qui reste du ressort des bancs matériels : la pile
TLS, le client MQTT, la flash et le panneau.

## Brochage

| Signal | GPIO | |
|---|---|---|
| SCK | 18 | SPI0, 2 MHz |
| MOSI | 19 | SPI0 |
| MISO | 16 | SPI0, réservé mais inutilisé |
| ECS | 17 | GPIO manuel (pas le CSn matériel) |
| DC | 21 | 0 = commande, 1 = donnée |
| RST | 22 | actif bas |
| BUSY | 20 | entrée, haut = occupé |

Les quatre boutons, en entrée avec tirage au plus. L'ordre suit les touches sur la face
avant, pas la numérotation des GPIO.

| Touche | GPIO | Rôle dans le menu |
|---|---|---|
| B1 | 9 | monter |
| B2 | 11 | descendre |
| B3 | 10 | valider |
| B4 | 8 | retour |

## Compiler et téléverser

Prérequis : `arm-none-eabi-gcc` (avec le support C++, le projet déclare `C CXX ASM`),
CMake, Ninja, `picotool`, et le [pico-sdk](https://github.com/raspberrypi/pico-sdk).

Le SDK doit être assez récent pour connaître la carte. Le test :

```bash
ls $PICO_SDK_PATH/src/boards/include/boards/pico2_w.h
```

S'il manque, mettre le SDK à jour.

```bash
export PICO_SDK_PATH=~/pico-sdk

cmake -B build -G Ninja \
      -DWIFI_SSID="mon_reseau" \
      -DWIFI_PASSWORD="mon_mot_de_passe"
cmake --build build
```

Les identifiants Wi-Fi sont obligatoires : sans eux la compilation s'arrête sur un
`#error`. Ils ne servent qu'au **premier** démarrage d'une carte vierge. Ensuite la
configuration vient de la flash, et un changement de réseau se fait par le portail captif
du boîtier, sans recompiler.

Téléversement, carte en mode BOOTSEL :

```bash
picotool load build/edel-check-pico.uf2 -f && picotool reboot
```

Puis, **immédiatement** :

```bash
picocom -b 115200 /dev/ttyACM0        # quitter : Ctrl-A Ctrl-X
```

> Le firmware est compilé avec `PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=-1` : il **attend
> indéfiniment** qu'un terminal série s'attache avant d'exécuter la moindre instruction.
> Une carte fraîchement flashée qui semble morte attend simplement `picocom`.

Si `/dev/ttyACM0` refuse l'ouverture : `sudo usermod -aG dialout $USER`, puis se
déconnecter et se reconnecter.

## Banc de test écran

Un projet CMake séparé permet de qualifier le panneau e-ink sans rien démarrer d'autre -
ni Wi-Fi, ni stockage, ni réseau :

```bash
./tests/screen_test/flash.sh          # compile puis téléverse (carte en BOOTSEL)
```

Il enchaîne en boucle : blanc, noir plein, mire géométrique, texte, image plein écran,
rafraîchissements partiels, balayage, menu. Chaque rafraîchissement est chronométré et
l'état réel de la broche `BUSY` est relevé après chaque opération - parce que
`epd_wait_busy()` retourne `true` même sur un panneau mort.

Contrairement au firmware principal, il démarre **même sans terminal série attaché** et
n'écrit rien dans le stockage persistant de la carte.

## Documentation

| Document | Contenu |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | **Commencer ici.** Fonctionnement détaillé, format des images e-ink, pièges connus, dette technique, divergence avec le contrat cloud |
| [`tests/unit/`](tests/unit/) | Tests unitaires, exécutés sur PC |
| [`tests/mqtt_test/`](tests/mqtt_test/) | Banc de test MQTT sur TLS |
| [`tests/screen_test/`](tests/screen_test/) | Banc de test du panneau e-ink |
| [`docs/mqtt-contract.md` du cloud](https://gitlab.com/hliosone/edelcheck/-/blob/main/docs/mqtt-contract.md) | Contrat MQTT v1.0 **figé** - ce que le cloud publie et attend |
| [`docs/ARCHITECTURE.md`](https://gitlab.com/hliosone/edelcheck/-/blob/main/docs/ARCHITECTURE.md) | Architecture d'ensemble du produit |

## Limitations connues

Le détail et les références de ligne sont dans `docs/ARCHITECTURE.md` §10.

* Le **certificat serveur TLS n'est pas vérifié**. La CA reçue à l'appairage est bien
  stockée en flash, mais l'épinglage attend que le certificat du broker couvre l'adresse
  réelle - son `subjectAltName` est écrit en dur dans `edelcheck/scripts/dev-up.sh`.
* `PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=-1` fait attendre un terminal série au
  démarrage : un boîtier branché sur une prise, sans ordinateur, ne démarre jamais.
* `epd_wait_busy()` retourne `true` même quand le panneau ne répond pas : un écran mort
  produit un message de succès.
* L'outil `epd_paint.html` qui a généré les assets **est absent du dépôt**. Le texte ne
  s'en sert plus (cf. `screen/epd_text.c`), les images de fond si.
* Les libellés de profils sont repliés sur `A-Z 0-9` : « Majorité » s'affiche « MAJORITE ».
* Aucun `.gitignore` à la racine.
* Un secret OAuth2 a été committé en clair dans `wifi/http_client.h` avant le 24.08.2026.
  Il a été retiré du code, mais **reste dans l'historique git : à révoquer et régénérer**.

## Équipe

Dylan Fehlmann · Quentin Michon (firmware) · Stan Stelcher - encadrant : Fouad Hanna.
