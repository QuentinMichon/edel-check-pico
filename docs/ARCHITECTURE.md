# EdelCheck — Firmware Pico 2 W : architecture et reprise

**Document de reprise.** Écrit pour quelqu'un qui arrive sans contexte — un coéquipier,
un correcteur, ou une session d'assistant repartie de zéro. Il dit *ce qui existe
réellement dans ce dépôt*, *pourquoi c'est comme ça*, et *ce qui ne correspond pas encore
au contrat figé côté cloud*.

| | |
|---|---|
| Dernière mise à jour | 22 août 2026 |
| Périmètre | firmware embarqué uniquement (dépôt `edel-check-pico`) |
| Auteur du code | Quentin Michon |
| Hors périmètre | portail, base, broker (dépôt `edelcheck`), service de vérification (Dylan) |
| Dépôt amont | `git@github.com:QuentinMichon/edel-check-pico.git`, branche `main` |
| Dernier commit documenté | `eb54350` |

Documents liés, dans le dépôt voisin `../edelcheck` :
[`docs/ARCHITECTURE.md`](../../edelcheck/docs/ARCHITECTURE.md) ·
[`docs/mqtt-contract.md`](../../edelcheck/docs/mqtt-contract.md) (**le contrat qui engage ce
firmware**) · [`contracts/verification-service.md`](../../edelcheck/contracts/verification-service.md)

---

## 0. À lire avant de toucher au matériel

Cinq comportements qui font perdre une heure chacun si on ne les connaît pas. Ils ne sont
écrits nulle part ailleurs.

### 0.1 Après le flash, la carte a l'air morte. C'est normal.

`CMakeLists.txt` définit :

```cmake
PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=-1
```

`-1` = **attente infinie**. `stdio_init_all()` dans `main.c:37` bloque tant qu'aucun hôte
série n'a ouvert `/dev/ttyACM0`. Rien ne s'affiche, l'écran reste vide, la LED ne bouge
pas. Le firmware **n'a pas planté** : il attend un terminal.

> Toujours ouvrir le terminal série *avant* de conclure quoi que ce soit sur un flash.

### 0.2 Recompiler avec un autre `-DWIFI_SSID=` ne change rien sur une carte déjà démarrée

`WIFI_SSID` / `WIFI_PASSWORD` ne sont **pas** utilisés à la connexion. Ils ne servent qu'à
*semer* le stockage flash au tout premier démarrage (`storage/storage_manager.c:47-57`),
et uniquement si le mot magique `CONFIG_MAGIC = 0xCAFE7478` est absent du dernier secteur.

Ensuite, `main.c:77` se connecte avec `local_storage->wifi_1_ssid`, lu depuis la flash.
Une carte qui a déjà démarré une fois garde son ancien SSID pour toujours, quel que soit
le `cmake`. Voir §9.4 pour forcer la remise à zéro.

### 0.3 Un écran mort ressemble à un rafraîchissement réussi

`epd_wait_busy()` (`screen/epd_driver.c:66`) calcule `seen_high` — « la broche BUSY
est-elle seulement montée ? » — puis **jette la valeur** : elle n'apparaît que dans un
`printf` de debug, et la fonction retourne `true` dans tous les cas sauf timeout.

Un panneau débranché, mal alimenté ou en reset permanent produit donc :

```
[screen] epd_display_update_full: termine.
```

…alors que rien ne s'est affiché. **Ne jamais prendre ce message pour une preuve.** La
seule preuve est visuelle.

### 0.4 Tous les diagnostics de l'écran sont compilés hors du binaire

`epd_driver.c` est truffé de `#ifdef DEBUG_PRINT` — durée du BUSY, séquence d'init,
timeouts. Or `DEBUG_PRINT` n'est défini **nulle part** : ni dans `CMakeLists.txt`, ni dans
un header. Tous ces `printf` sont absents du binaire.

Avant toute séance de debug écran, ajouter `DEBUG_PRINT` à
`target_compile_definitions` dans `CMakeLists.txt`, ou configurer avec
`-DCMAKE_C_FLAGS=-DDEBUG_PRINT`.

### 0.5 Le port série n'est pas accessible à l'utilisateur courant

Sur cette machine (22.08.2026) :

```
crw-rw---- 1 root dialout 166, 0  /dev/ttyACM0
$ id -nG
hliosone adm cdrom sudo dip plugdev lpadmin docker sambashare kvm     ← pas de "dialout"
```

Il faut soit `sudo usermod -aG dialout $USER` **puis se déconnecter/reconnecter**, soit
préfixer chaque appel par `sudo`. Sans ça, `picocom` échoue en « permission denied » et on
croit à un problème de carte.

---

## 1. Ce que fait le firmware aujourd'hui

Séquence complète de `main.c`, dans l'ordre :

```
stdio_init_all()             ← BLOQUE jusqu'à ouverture du port série (§0.1)
sleep_ms(1000)
bannière "EDEL CHECK v1.0.6"
init_local_storage()         ← lit/sème le dernier secteur de flash
  └─ affiche ssid, mot de passe et bearer token EN CLAIR sur la console
init_gpio()                  ← SPI0 + broches de l'écran (rien d'autre)
epd_init()                   ← reset + séquence SSD1683
epd_fb_clear(true) + full    ← écran blanc, ~2 s
memcpy(fullscreen_chargement) + partial   ← écran de chargement
wifi_init()                  ← cyw43, mode station
wifi_connect(ssid, pwd, 30s) ← échec = return -1, la carte s'arrête là
display_menu(true, 4, "check","settings","post token","mcquenty")
add_repeating_timer_ms(-5000, periodic_check_callback)   ← lève un flag jamais lu
while (running) { poll_usb_nav_key(); sleep_ms(10); }
epd_fb_clear(true) + full    ← sortie sur 'x'
```

Deux remarques :

* **Sans Wi-Fi, le firmware ne démarre pas.** `wifi_connect()` qui échoue fait
  `return -1` depuis `main()`, l'écran reste sur « chargement ». Pas de mode dégradé.
* Le menu affiché à l'écran au démarrage (4 entrées) et celui affiché par `go_to_menu()`
  en cours de navigation (2 entrées) sont **différents**. Voir §10.

---

## 2. Carte du dépôt

```
main.c                     séquence de démarrage + boucle de polling
CMakeLists.txt             une seule cible, pas de sous-projets
gpio/                      init SPI0 + broches écran
screen/                    driver SSD1683 + framebuffer + primitives de dessin
assets/                    images 1 bit/pixel en dur, générées par epd_paint.html (ABSENT, §4.5)
  ├─ full_screen/          3 écrans 400x300 + 1 PNG source survivant
  ├─ typo/                 26 lettres + apostrophe, 16x15 px
  └─ battery/              icône batterie 46x22
wifi/                      cyw43 (wifi_setup) + client HTTP/HTTPS écrit à la main (http_client)
json/                      frozen (bibliothèque tierce) + parsing token & QR code
storage/                   persistance dans le dernier secteur de flash
navigation/                machine à états du menu, pilotée au clavier via USB
```

Pas de `.gitignore` — un `build/` créé à la racine serait committé. Voir §10.

---

## 3. Matériel et brochage

| Élément | Référence |
|---|---|
| MCU | Raspberry Pi Pico 2 W (RP2350, Wi-Fi CYW43439) |
| Écran | e-ink 4,2" 400x300, contrôleur **SSD1683** (panneau type GDEY042T81) |
| Interface | Adafruit E-Ink Breakout Friend (level shifter 74LCX245 sur le chemin des signaux) |

Brochage, défini dans `gpio/gpio_driver.h` :

| Signal | GPIO | Fonction | Note |
|---|---|---|---|
| SCK | **18** | `GPIO_FUNC_SPI` (SPI0 SCK) | |
| MOSI | **19** | `GPIO_FUNC_SPI` (SPI0 TX) | |
| MISO | **16** | `GPIO_FUNC_SPI` (SPI0 RX) | réservé mais **jamais lu** — le panneau est écrit seul |
| ECS (CS) | **17** | **GPIO manuel**, pas `SPI0 CSn` | basculé à la main autour de chaque octet |
| DC | **21** | GPIO out | 0 = commande, 1 = donnée |
| RST | **22** | GPIO out | actif bas, ≥10 ms (§4.2) |
| BUSY | **20** | GPIO in | haut = occupé |

Horloge SPI : **2 MHz** (`EPD_SPI_BAUDRATE`).

Deux pièges pour qui ajouterait un périphérique :

* **GP17 est le CSn matériel de SPI0**, mais le driver le pilote en GPIO nu. Brancher un
  second esclave sur SPI0 sans comprendre ça donne des collisions de sélection.
* **GP16 est immobilisé** pour un MISO inutilisé. Il est récupérable.

Matériel prévu par le projet mais **absent du code** : les 4 boutons poussoirs (aucun GPIO
d'entrée en dehors de BUSY), le contrôleur NFC PN7160, la jauge de batterie BQ27441 —
l'icône batterie affichée est une image fixe « 80 % », pas une mesure.

---

## 4. L'écran e-ink — le cœur de ce qu'on veut tester

### 4.1 Le modèle mental : un framebuffer 1 bit/pixel en RAM

```c
uint8_t epd_framebuffer[15000];   // 50 octets/ligne x 300 lignes
```

| | |
|---|---|
| Convention | **bit 1 = blanc, bit 0 = noir** (inverse de l'intuition) |
| Ordre des bits | **MSB = pixel le plus à gauche** du groupe de 8 |
| Adresse d'un pixel | `octet = y * 50 + x/8`, `masque = 1 << (7 - x%8)` |

Toutes les fonctions `epd_fb_*` ne touchent **que** cette RAM. Rien n'apparaît à l'écran
tant qu'on n'a pas appelé un `epd_display_update_*`. C'est la distinction à garder en
tête pendant les tests d'affichage.

### 4.2 Initialisation du panneau

`epd_init()` suit le flux SSD1683 (datasheet §9.1) :

| Commande | Rôle | Valeur ici |
|---|---|---|
| reset matériel | RST 1→0→1, 10/10/20 ms | timings Adafruit (le 2 ms initial était trop court pour le level shifter) |
| `0x12` | SW reset | |
| `0x01` | Driver Output Control | 299 lignes, 3 octets |
| `0x11` | Data Entry Mode | `0x03` — X puis Y croissants |
| `0x44` | RAM X start/end | 0 → 49 (granularité **8 pixels**) |
| `0x45` | RAM Y start/end | 0 → 299 |
| `0x3C` | Border Waveform | `0x01` (valeur GxEPD2 pour ce panneau) |
| `0x18` | Temperature Sensor | `0x80` = capteur interne |

Pas de chargement de LUT explicite : la séquence `0x22` au moment du refresh charge la
waveform depuis l'OTP.

### 4.3 Les deux plans RAM, et pourquoi il y a deux refresh

Le SSD1683 tient **deux images** en RAM :

| Plan | Commande | Sens |
|---|---|---|
| « nouvelle image » | `0x24` | ce qui va être affiché |
| « ancienne image » | `0x26` | référence du diff en partiel |

```
epd_display_update_full()      ~2,3 s   flashs noir/blanc, efface le ghosting
  écrit 0x24 ET 0x26 → 0x21=0x40 (bypass du plan 0x26) → 0x22=0xF7 → 0x20

epd_display_update_partial()   ~0,3-0,7 s   pas de flash, laisse un léger ghosting
  écrit 0x24 → 0x21=0x00 (plan 0x26 actif) → 0x22=0xFC → 0x20
  puis RE-écrit 0x26 ET 0x24 pour resynchroniser la référence du prochain diff
```

Règle d'usage : **partiel pour la navigation, full périodiquement** (et à l'entrée dans
un écran important) pour purger le ghosting.

Avant chaque envoi de plan, `epd_write_plane()` repositionne les compteurs d'adresse
`0x4E`/`0x4F`. C'est obligatoire : le compteur reste là où la dernière écriture s'est
arrêtée — l'analogie POSIX est un `write()` sans `lseek()` préalable.

### 4.4 Les primitives de dessin

| Fonction | Ce qu'elle fait |
|---|---|
| `epd_fb_clear(bool white)` | remplit tout le framebuffer |
| `epd_fb_set_pixel(x, y, white)` | un pixel, **clipping inclus** (hors écran = ignoré) |
| `epd_fb_fill_rect(x, y, w, h, white)` | rectangle, via `set_pixel` |
| `epd_fb_draw_image(x, y, img, w, h)` | blit 1bpp ; recalcule le stride source `(w+7)/8` — c'est ce désalignement avec les 50 octets/ligne du framebuffer qui interdit un `memcpy` direct |
| `epd_fb_write_typo(x, y, text)` | texte, via un `switch` sur 27 glyphes |
| `display_menu(full, n, ...)` | fond `fullscreen_base` + batterie + « edel id » + n lignes (max 4) espacées de 52 px à partir de y=46 |

Limites de `epd_fb_write_typo` à connaître avant d'écrire du texte :

* **A-Z et `'` seulement.** Minuscules acceptées (`toupper`), mais **aucun chiffre**,
  aucune ponctuation, aucun accent. Tout autre caractère imprime `char not supported` sur
  la console et avance de 8 px.
* Glyphes **16x15 px**, largeur fixe ; l'espace vaut 8 px.
* Aucun retour à la ligne, aucun clipping horizontal au niveau du texte : au-delà de
  ~24 caractères ça sort de l'écran (les pixels sont jetés par `set_pixel`, sans erreur).

Un plein écran se pose par `memcpy` direct, puisqu'il fait exactement 15000 octets :

```c
memcpy(epd_framebuffer, fullscreen_chargement, EPD_BUFFER_SIZE);
epd_display_update_partial();
```

### 4.5 Format des assets — et l'outil manquant

Chaque header d'asset porte le même en-tête généré :

```
// Genere par epd_paint.html — image 400x300, 1 bit/pixel
// bit 1 = blanc, bit 0 = noir, MSB = pixel de gauche, 50 octet(s)/ligne
```

Contrat, identique pour tous :

| | |
|---|---|
| Encodage | 1 bit/pixel, **bit 1 = blanc** |
| Ordre | MSB = pixel de gauche, balayage ligne par ligne |
| Stride | `(largeur + 7) / 8` octets par ligne |
| Symboles | `static const uint8_t <nom>[N]` + `<NOM>_W`, `<NOM>_H`, `<NOM>_W_BYTES` |
| Contrainte panneau | la fenêtre X du SSD1683 a une granularité de 8 px — l'asset batterie note un bourrage blanc de 46 → 48 px |

> **`epd_paint.html` n'existe dans aucun des deux dépôts.** Recherché sur tout
> `~/Documents/HEIG-VD/BA6/PDG` : absent. C'est le **blocage n°1** pour produire de
> nouvelles images.
>
> Deux issues : demander le fichier à Quentin, ou réécrire un convertisseur PNG → header
> (le format ci-dessus est entièrement spécifié, c'est une vingtaine de lignes en Python
> avec Pillow). `assets/full_screen/check_fullscreen.png` (400x300 RGBA) survit comme
> échantillon source pour valider un convertisseur : le convertir doit redonner le
> contenu de `check_fullscreen.h`.

Assets présents : `fullscreen_base`, `fullscreen_chargement`, `check_fullscreen`,
`epd_scan` (**jamais utilisé**), `epd_typo_5_[a-z]` + `_apos`, `epd_image_bat_80`.

### 4.6 Coût réel d'un rafraîchissement — ce n'est pas un bug

`epd_send_data()` envoie **un octet par appel**, avec une bascule de CS avant et après.
Un plan = 15 000 appels. Donc :

| Opération | Plans écrits | Bascules de CS |
|---|---|---|
| `update_full` | 2 (0x24, 0x26) | ~30 000 |
| `update_partial` | 3 (0x24, puis 0x26+0x24 en resync) | ~45 000 |

**Mesuré sur matériel le 22.08.2026** (banc `tests/api_screen`) : un rafraîchissement
partiel prend **638 ms**, de façon très stable d'un cycle à l'autre. C'est une mesure, pas
une reprise des commentaires du driver.

À 2 MHz, un rafraîchissement partiel passe donc plus de temps à taper sur des GPIO qu'à
transférer. **C'est lent, et c'est attendu** — ne pas partir en chasse au bug de timing.
L'optimisation évidente (un seul `spi_write_blocking` de 15 000 octets, CS maintenu bas)
est notée en §10, pas appliquée.

---

## 5. Réseau et authentification

### 5.1 Wi-Fi

`wifi/wifi_setup.c` : `cyw43_arch_init()`, mode station, `WPA2 AES PSK` **en dur**. Un
réseau ouvert ou WPA3 échouera. Pas de reconnexion automatique.

### 5.2 Le client HTTP

`wifi/http_client.c` (612 lignes) est écrit à la main sur `altcp` de lwIP — la couche
qui rend TCP et TCP+TLS interchangeables. Ce choix donne le TLS « gratuitement » tout en
laissant le contrôle sur les en-têtes, ce que `httpc_get_file_dns` ne permet pas.

Il gère : DNS, TLS via mbedTLS, `GET`/`POST`, capture du corps seul (sans en-têtes) dans
un buffer appelant, **et le décodage `Transfer-Encoding: chunked`** (automate
`http_chunk_state_t`) — le commentaire de `http_client.h` qui dit le contraire est
périmé.

> ⚠️ **Le certificat serveur n'est pas vérifié.** `http_client_alloc()` appelle
> `altcp_tls_create_config_client(NULL, 0)` : aucune CA. Le SNI est bien envoyé, la
> connexion est chiffrée, mais **n'importe quel certificat est accepté** — un MITM sur le
> réseau local lit et modifie le flux OAuth et les réponses de vérification. Acceptable
> pour un banc de test, pas pour une soutenance ni pour de vrais jetons.

### 5.3 Le flux d'authentification

```
post_token()                                       nav.c:81
  Basic base64(client_id:client_secret)
  POST https://auth.edel-id.app/oauth/v2/token
       grant_type=client_credentials&scope=openid
  → handle_token() : json_scanf, vérifie token_type == "Bearer"
  → put_bearer_token() : écrit en flash

verify_ch()                                        nav.c:120
  si le token stocké est vide → post_token()
  POST https://api.edel-id.app/api/verification/ch
       Authorization: Bearer <token>
       {"verificationClaims": ["$.given_name","$.family_name","$.birth_date"]}
  → print_qr_code(body, 78, 0, 3) : lit .qrCodeBitMap.rows (tableau de chaînes '0'/'1'),
    chaque module devient un carré scale x scale
  → fond check_fullscreen + "back to profiles" + refresh full
```

Buffers dimensionnés `static` volontairement : **la pile du core0 fait 4 Ko** sur ce chip
(SCRATCH_Y). Un `char body[10240]` local la ferait déborder. Ne pas « nettoyer » ces
`static` en locales.

Tailles : réponse de vérification ~7-8 Ko (le bitmap QR en JSON), d'où `body[10240]` ;
token JWT jusqu'à 800 octets (`MEM_BEARER_TOKEN_SIZE`).

> ⚠️ **`OAUTH_CLIENT_ID` et `OAUTH_CLIENT_SECRET` sont en clair dans
> `wifi/http_client.h:30-31`, committés, sur un dépôt avec remote GitHub** — alors que le
> commentaire juste au-dessus dit « ne pas commiter de vrais secrets ». Le secret est à
> considérer comme compromis : il faut le **révoquer côté Zitadel** puis passer par le
> stockage flash ou une injection au build. Les valeurs ne sont volontairement pas
> recopiées ici.

### 5.4 ⛔ Le firmware ne parle qu'aux serveurs à certificat ECDSA

**Constaté sur matériel le 22 août 2026, après 81 échecs consécutifs.** C'est le défaut le
plus sérieux trouvé dans ce dépôt, et il est invisible tant qu'on ne teste qu'un seul
serveur.

`config/mbedtls_config.h` n'active que **deux** échanges de clés :

```c
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED          // RSA statique — refusé par tout serveur moderne
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED  // ECDHE, mais seulement avec un certificat ECDSA
```

Il manque **`MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED`**. Conséquence : face à un serveur
qui présente un **certificat RSA** — le cas le plus courant — le client n'a aucune suite
cryptographique en commun, le serveur ferme la connexion, et lwIP remonte `-15`
(`ERR_CLSD`). Le message affiché est `[http] erreur de connexion (-15)`, qui ne dit rien
de la cause.

Mesuré des deux côtés :

| Hôte | Certificat | Suite négociée | Le firmware sait faire ? |
|---|---|---|---|
| `auth.edel-id.app` | ECDSA (`id-ecPublicKey`) | `ECDHE-ECDSA-AES256-GCM-SHA384` | ✅ |
| `*.vercel.app` | **RSA** (`rsaEncryption`) | `ECDHE-RSA-AES128-GCM-SHA256` | ❌ |

**Le firmware ne marche avec Edel-ID que par chance** : leur certificat est ECDSA.

> **Ce que ça implique pour le produit.** Le `device-service` d'EdelCheck sera exposé
> derrière nginx avec un certificat Let's Encrypt. **Let's Encrypt délivre du RSA par
> défaut** — il faut le demander explicitement en ECDSA. Autrement dit : le jour du
> déploiement sur le VPS, le parc entier échouera à joindre l'API, avec pour seul indice
> un `-15`. Deux issues, à choisir consciemment :
>
> 1. ajouter `MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED` et `MBEDTLS_PKCS1_V21` à
>    `config/mbedtls_config.h` — deux lignes, la bonne solution ;
> 2. imposer un certificat ECDSA côté serveur, et le documenter comme une contrainte de
>    déploiement — fragile, parce que ça se perd au premier renouvellement.

`MBEDTLS_PKCS1_V21` va avec : les serveurs récents signent en **RSA-PSS**, pas en
PKCS#1 v1.5.

Rien n'a été modifié dans `config/mbedtls_config.h` : le contournement vit dans
`tests/api_screen/CMakeLists.txt`, en `-D`.

### 5.5 Configuration lwIP / mbedTLS

`config/lwipopts.h` : `TCP_WND = 32768`, choisi **strictement supérieur** à la taille max
d'un record TLS en clair (16384) — sans ça, un record complet ne tient pas dans la fenêtre
et la connexion se bloque. `LWIP_DEBUG` est désactivé volontairement (bug lwIP connu quand
`LWIP_ALTCP` et `LWIP_DEBUG` sont actifs ensemble).

`config/mbedtls_config.h` : TLS 1.2 client, `MBEDTLS_SSL_OUT_CONTENT_LEN = 2048` (on
n'émet jamais de gros corps), entropie matérielle RP2350.

---

## 6. Stockage persistant

`storage/storage_manager.c` — **le dernier secteur de flash** :

```c
FLASH_TARGET_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE   // 4 Mo - 4 Ko = 0x3FF000
// adresse lisible en XIP : 0x10000000 + 0x3FF000 = 0x103FF000
```

Contenu (`persistent_storage_t`) : `magic` (`0xCAFE7478`), `flags`, `bearer_token[800]`,
et **trois** couples SSID/mot de passe (seul le premier est utilisé aujourd'hui).

Mécanique : `load` = `memcpy` depuis XIP + test du magic. `save` = `save_and_disable_interrupts()`
→ `flash_range_erase` → `flash_range_program` → `restore_interrupts`. Un
`_Static_assert` garantit que la structure tient dans un secteur.

Points à connaître :

* Le commentaire « 2 secteurs = 8192 octets réservés » est **faux** : `FLASH_STORAGE_SIZE`
  vaut `FLASH_SECTOR_SIZE`, soit un seul secteur de 4096 octets.
* `get_local_storage()` **relit la flash à chaque appel** et n'a pas de mutex (`TODO` dans
  le code). Sans second cœur ni DMA aujourd'hui, ça tient.
* `main.c:55-57` **imprime le SSID, le mot de passe Wi-Fi et le bearer token en clair**
  sur la console à chaque démarrage.
* Toute écriture flash pendant que le second cœur exécute du XIP planterait ; hors sujet
  tant qu'on est mono-cœur, à revoir si un jour on ajoute `multicore_launch_core1`.

---

## 7. Navigation

`navigation/nav.c` — machine à états à 4 pages :

```
NAV_PAGE_MENU ──1──▶ NAV_PAGE_PROFILE ──1──▶ post_token()
      │                     │          ──2──▶ verify_ch() ──▶ NAV_PAGE_CHECK ──1──▶ PROFILE
      └──2──▶ NAV_PAGE_SETTINGS ──4──▶ MENU
      └──x──▶ running = false
```

**L'entrée se fait exclusivement au clavier, via le port série USB** :
`getchar_timeout_us(0)` en polling toutes les 10 ms dans `main.c`. Les touches `1`-`4` et
`x`. Les 4 boutons poussoirs du matériel n'ont **aucun code** — `init_gpio()` ne configure
que les broches de l'écran.

Conséquence pratique : **toute la navigation se pilote depuis `picocom`**, en tapant des
chiffres.

Pages `SETTINGS` (wifi, pairing) : squelettes vides, `printf("NA")`.

---

## 8. La divergence avec le contrat MQTT — à lire avant de planifier quoi que ce soit

`../edelcheck/docs/mqtt-contract.md` est en **v1.0, figé**, et modifiable seulement par
accord explicite des deux côtés. Ce firmware ne l'implémente pas. Ce n'est pas un retard
de détail : les deux architectures sont incompatibles telles quelles.

| Sujet | Contrat MQTT v1.0 | Ce firmware |
|---|---|---|
| Transport | MQTT 3.1.1 / TLS **8883**, `client_id = dev-{device_id}`, `clean_session=true`, LWT retenu | **aucun code MQTT** |
| Identité | enrôlement `POST /provisioning/claim` + `/poll`, code d'appairage affiché, secret servi **une seule fois** | `client_id`/`client_secret` OAuth2 en dur dans un header |
| Wi-Fi | configuré à l'appairage | en dur au build, semé en flash |
| Origine de l'image | **le cloud pousse des pixels déjà tramés** sur `dev/{id}/img` | le boîtier appelle l'API et **dessine le QR lui-même** |
| Format image | **2 bits/pixel, 4 niveaux de gris**, 400x300 = 30 000 o, 8 fragments de 4088 o, `img_id` u32 + `seq`/`total` u16 big-endian | **1 bit/pixel**, 15 000 o, tout en RAM |
| Cycle | `session_open` → `session_ready` → fragments → `result` → fragments | un seul POST synchrone bloquant |
| Rotation de secret | `rotate` / `rotate_ack`, persistance **avant** l'accusé | absent |
| OTA | manifeste signé, signature vérifiée contre une clé embarquée | absent |
| `hw_id` | `pico_get_unique_board_id()` | jamais appelé |

**Le point le plus coûteux : les 4 niveaux de gris.** Ce n'est pas un paramètre à changer.
Le pipeline actuel est mono-plan (`epd_write_plane(0x24)`, un bit par pixel, le plan `0x26`
servant de référence de diff). Le niveau de gris sur SSD1683 se fait en pilotant **deux
plans comme deux plans de bits** avec une LUT dédiée. Le framebuffer double de taille
(30 000 o), la convention de bits change, `epd_fb_set_pixel` et tous les assets changent
de format, et `epd_display_update_partial()` perd son sens actuel. C'est une réécriture du
driver, pas un ajustement.

**Question ouverte à trancher en équipe, pas seul :** est-ce le firmware qui rejoint le
contrat, ou le contrat qui s'aligne sur ce que le firmware sait faire (1bpp, rendu local) ?
Le contrat dit qu'il faut un accord explicite des deux côtés. Le simulateur navigateur du
dépôt `edelcheck` implémente déjà, lui, le côté contrat — donc la démonstration cloud
fonctionne sans ce firmware.

---

## 9. Compiler et téléverser

> ✅ **Section vérifiée sur cette machine le 22 août 2026** : chaîne installée, banc de
> test `tests/screen_test` configuré, compilé et téléversé avec succès sur le matériel.
> Les commandes ci-dessous sont celles qui ont réellement fonctionné.

### 9.1 Prérequis

Ce qui est installé et validé (Ubuntu 24.04) :

| Outil | Version qui marche |
|---|---|
| `cmake` | 3.30.5 |
| `ninja` | 1.11.1 |
| `arm-none-eabi-gcc` / `g++` | **13.2.1** (paquet Ubuntu `15:13.2.rel1-2`) |
| pico-sdk | **2.3.0**, dans `~/pico-sdk`, submodules inclus |
| `picocom` | 3.1 |
| `picotool` | **inutile d'en installer un** — le SDK le télécharge et le compile seul au premier `cmake` |

```bash
sudo apt install -y gcc-arm-none-eabi libnewlib-arm-none-eabi \
                    libstdc++-arm-none-eabi-newlib picocom
sudo usermod -aG dialout $USER        # pour /dev/ttyACM0, cf. §0.5

git clone --depth 1 --recurse-submodules --shallow-submodules \
          https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
export PICO_SDK_PATH=~/pico-sdk       # à mettre dans ~/.bashrc
```

`libstdc++-arm-none-eabi-newlib` est nécessaire parce que `CMakeLists.txt` déclare
`project(... C CXX ASM)` : le SDK réclame le C++ même si aucun `.cpp` n'existe (l'édition
de liens finale passe d'ailleurs par `g++`).

`--depth 1 --shallow-submodules` ramène **396 Mo** au lieu de plusieurs gigaoctets, et
suffit entièrement. **`usermod` ne prend effet qu'après déconnexion/reconnexion de la
session** — en attendant, préfixer par `sudo`.

Pour que `picotool` puisse parler à la carte en USB (reset à distance, `picotool info`),
il faut en plus `libusb-1.0-0-dev` **avant** le premier `cmake` : sans lui le SDK compile
un picotool sans support USB, qui ne sait que manipuler des fichiers.

**Le seul test fiable de la version du SDK** est l'existence du fichier de carte :

```bash
ls ~/pico-sdk/src/boards/include/boards/pico2_w.h
```

S'il manque, le SDK est trop ancien — mettre à jour plutôt que de chercher un numéro de
version de mémoire.

### 9.2 Configurer et compiler

```bash
cmake -B build -G Ninja \
      -DWIFI_SSID="mon_reseau" \
      -DWIFI_PASSWORD="mon_mot_de_passe"
cmake --build build
```

Sorties dans `build/` : `edel-check-pico.uf2` (glisser-déposer) et `.elf` (débogueur).
Sans `-DWIFI_SSID`, `cmake` émet un warning et la compilation échoue sur le `#error` de
`main.c:17`.

Pour activer les traces écran (§0.4) : ajouter `DEBUG_PRINT` dans
`target_compile_definitions` de `CMakeLists.txt`.

### 9.3 Téléverser

Mode BOOTSEL : maintenir le bouton BOOTSEL, brancher l'USB, relâcher. La carte apparaît en
`2e8a:000f` (RP2350) et monte un volume `RP2350`.

```bash
cp build/edel-check-pico.uf2 /media/$USER/RP2350/     # ou
picotool load build/edel-check-pico.uf2 -f && picotool reboot
```

**La copie de fichier est le chemin le plus simple** : ni `sudo`, ni `picotool`
fonctionnel en USB. C'est ce que fait `tests/screen_test/flash.sh` (§9.6). Corollaire
gênant : **sans picotool USB, on ne peut pas redémarrer la carte depuis le PC** — il faut
débrancher/rebrancher physiquement. D'où l'intérêt d'installer `libusb-1.0-0-dev` (§9.1),
ou d'écrire les bancs de test en boucle infinie plutôt qu'en passe unique.

Puis **ouvrir immédiatement la console**, sinon la carte reste bloquée (§0.1) :

```bash
picocom -b 115200 /dev/ttyACM0        # sudo si pas dans dialout, §0.5
# quitter : Ctrl-A Ctrl-X
```

Le débit est ignoré sur USB CDC ; `115200` est conventionnel.

### 9.4 Repartir d'un stockage vierge

Nécessaire dès qu'on change de réseau Wi-Fi (§0.2). Trois voies, par ordre de préférence :

1. `picotool erase -r 0x103FF000 0x10400000` — n'efface que le secteur de config
   (syntaxe exacte à vérifier selon la version de picotool).
2. `flash_nuke.uf2` (pico-examples) — efface **toute** la flash, firmware compris.
3. Changer temporairement `CONFIG_MAGIC` dans `storage_manager.h`, flasher, remettre.
   Sale mais toujours disponible.

### 9.5 État actuel du matériel branché

Au 22.08.2026, la carte est **connectée et démarrée** :

```
$ lsusb
Bus 003 Device 003: ID 2e8a:0009 Raspberry Pi Pico
$ ls /dev/ttyACM*
/dev/ttyACM0
$ lsblk        →  aucun volume RP2350 monté
```

Ce qui est **certain** : un port CDC est exposé et aucun volume de stockage de masse n'est
monté — donc un firmware tourne, la carte n'est **pas** en BOOTSEL, et d'après §0.1 elle
est très probablement parquée dans l'attente bloquante de `stdio_init_all()`.

**Correspondance des identifiants USB — observée dans les deux états le 22.08.2026** :

| État de la carte | Identifiant USB | Ce qui apparaît côté hôte |
|---|---|---|
| Firmware en cours d'exécution | `2e8a:0009` | `/dev/ttyACM0`, aucun volume |
| BOOTSEL | `2e8a:000f` | volume `RP2350` monté, aucun `/dev/ttyACM*` |

`lsusb | grep 2e8a` est donc le test le plus rapide pour savoir dans quel état est la
carte.

### 9.6 Le banc de test écran (`tests/screen_test/`)

Projet CMake **séparé** : il ne modifie pas le `CMakeLists.txt` de la racine et ne compile
que `screen_test.c` + `screen/epd_driver.c` + `gpio/gpio_driver.c`.

```bash
./tests/screen_test/flash.sh            # compile puis téléverse (carte en BOOTSEL)
./tests/screen_test/flash.sh --build    # compile seulement
```

Quatre écarts volontaires avec le firmware principal, qui font tout l'intérêt du banc :

| Écart | Pourquoi |
|---|---|
| Aucune dépendance Wi-Fi / lwIP / mbedTLS | pas de `-DWIFI_SSID` à fournir, et pas de `return -1` si le réseau ne répond pas (§1) |
| Aucune dépendance au stockage flash | la configuration persistante de la carte reste intacte (§0.2) |
| `PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000` au lieu de `-1` | **la carte exécute le test même sans terminal série attaché** (§0.1) |
| `DEBUG_PRINT` activé | les diagnostics de `epd_driver.c` sont enfin compilés (§0.4) |

La démo tourne **en boucle infinie** : elle rejoue sans qu'on ait à rebrancher la carte —
ce qui compense l'absence de reset à distance (§9.3).

Huit étapes : blanc, noir plein, mire géométrique, texte, image plein écran, quatre
rafraîchissements partiels, balayage d'une barre en huit partiels enchaînés, menu. Chaque
rafraîchissement est chronométré, et le banc **lit `gpio_get(PIN_BUSY)` directement** après
chaque opération — parce que `epd_wait_busy()` ne dit pas la vérité (§0.3). Ce sont ces
valeurs de BUSY, et non les messages « termine. », qui disent si le panneau répond.

La mire de l'étape 3 est faite pour lever les ambiguïtés : un cadre complet vérifie les
bornes de la fenêtre RAM, un « L » épais en haut à gauche détecte un miroir ou une
rotation, un petit carré en bas à droite marque le pixel (399, 299). Le balayage de
l'étape 7 enchaîne huit partiels sans full intercalé : c'est là que le ghosting devient
visible, et c'est ce qui dit à quelle fréquence il faut intercaler un full.

Empreinte : 92 Ko de code, 17,6 Ko de RAM (dont les 15 Ko du framebuffer).

---

## 10. Dette technique repérée — constatée, non corrigée

Aucun de ces points n'a été modifié : le dépôt est resté intact à la demande.

| Où | Constat |
|---|---|
| `wifi/http_client.h:30-31` | **secret OAuth2 committé** — à révoquer et sortir du dépôt (§5.3) |
| `http_client.c:386` | **TLS sans vérification de certificat** (§5.2) |
| racine | **pas de `.gitignore`** — `build/`, `.idea/`, `.DS_Store` sont ou seront committés |
| `screen/epd_driver.c:66` | `epd_wait_busy` retourne `true` même si BUSY n'est jamais monté (§0.3) |
| `CMakeLists.txt` | `DEBUG_PRINT` jamais défini → tous les diagnostics écran compilés hors binaire (§0.4) |
| `navigation/nav.c` | `case 'x':` **tombe dans `default:`** (pas de `break`) → « EXIT » puis « NOT SUPPORTED » |
| `main.c` vs `nav.c` | `display_menu(true, 4, "check","settings","post token","mcquenty")` au boot, `display_menu(false, 2, ...)` ensuite — deux menus différents, dont un de test |
| `main.c:24,92` | `flags_irq` et le timer 5 s : le flag est levé, **jamais lu** |
| `storage_manager.c:16` | commentaire « 2 secteurs = 8192 octets » faux — un seul secteur |
| `storage_manager.c` | `get_local_storage()` sans mutex (`TODO` explicite) |
| `main.c:55-57` | mot de passe Wi-Fi et bearer token imprimés en clair au démarrage |
| `http_client.h` | commentaire « pas de support de `Transfer-Encoding: chunked` » périmé — c'est implémenté |
| `assets/full_screen/epd_image_scanner.h` | asset `epd_scan` jamais utilisé |
| `screen/epd_driver.c` | envoi octet par octet avec bascule de CS (§4.6) — un `spi_write_blocking` global diviserait le temps de refresh |
| assets | `static const` dans des headers : chaque `.c` qui inclut `epd_driver.h` embarque sa propre copie des images qu'il utilise |
| `wifi_setup.c` | `CYW43_AUTH_WPA2_AES_PSK` en dur, pas de reconnexion |
| `nav.c:122` | `TODO` : pas de renouvellement du token quand il a expiré |

---

## 11. Prochaines étapes proposées

Par ordre de dépendance, du plus bloquant au plus confortable :

1. **Installer la chaîne de compilation** et valider §9 par un build réel. Rien n'est
   possible avant.
2. **Retrouver ou réécrire `epd_paint.html`** (§4.5). Sans lui, aucune nouvelle image.
   `check_fullscreen.png` sert de test de non-régression du convertisseur.
3. **Révoquer le secret OAuth2** et le sortir du dépôt.
4. **Un binaire de test écran minimal** : ni Wi-Fi ni stockage, juste `init_gpio` +
   `epd_init` + des motifs (damier, rectangles, texte, chronométrage full vs partiel).
   C'est le moyen le plus rapide de qualifier le panneau, et ça contourne §0.2 et le
   `return -1` sur échec Wi-Fi.
5. **Rendre `epd_wait_busy` honnête** (retourner `false` si BUSY n'est jamais monté),
   sinon tous les tests d'écran mentent.
6. **Câbler les 4 boutons** — la navigation série est un échafaudage de développement.
7. **Trancher la question MQTT** (§8) en équipe, avant d'écrire la moindre ligne dans
   cette direction.

---

## Journal des versions

| Version | Date | Changement |
|---|---|---|
| 1.0 | 22 août 2026 | Rédaction initiale, à partir du dépôt au commit `eb54350` |
