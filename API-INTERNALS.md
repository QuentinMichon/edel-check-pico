# Fonctionnement interne de l'API Edel-ID

> Doc de référence technique (pour Claude / dev interne), à distinguer de [HOWTO.md](./HOWTO.md)
> qui est la doc "consommateur" (= ce qui est publié sur https://portal.edel-id.app/#docs).
> Ce fichier explique **comment ça marche derrière**, avec les fichiers sources exacts.
> État du code au 2026-08-23.

## Architecture en une phrase

```
Client  →  APISIX (auth JWT + rate-limit)  →  api-gateway (Spring Boot)  →  verifier backend (SWIYU ou EUDI)
                     ↑
                  Zitadel (OAuth2 / OIDC provider)
```

- **Zitadel** (`auth.edel-id.app`) : IdP OAuth2, délivre les tokens et publie le JWKS.
- **APISIX** (`api.edel-id.app`) : gateway. Vérifie le JWT (plugin `openid-connect`), applique le
  rate-limit, route vers le service `api-gateway`. Config : `apisix/init.sh`.
- **api-gateway** (Spring Boot, `api-gateway/src/main/java/ch/edelid/apigateway`) : traduit les
  appels publics `/api/verification/{ch,eu}` vers les verifier backends réels, normalise les
  réponses, génère le QR code, décode les credentials retournés par le wallet.

---

## 1. Obtenir un token (Client Credentials)

```bash
curl -X POST https://auth.edel-id.app/oauth/v2/token \
  -u "<CLIENT_ID>:<CLIENT_SECRET>" \
  -d "grant_type=client_credentials&scope=openid"
```

- C'est du OAuth2 standard, géré entièrement par **Zitadel** — l'`api-gateway` n'y participe pas.
- Le `CLIENT_ID`/`CLIENT_SECRET` sont ceux d'un client Zitadel créé pour l'organisation (onglet
  "API Keys" du portail). `-u` = HTTP Basic auth avec ces identifiants.
- Réponse : JSON avec `access_token` (un JWT), `expires_in`, etc.
- **Durée de vie courte** : configurée côté Zitadel (2 min par défaut dans le script d'init,
  ajustable dans la console admin Zitadel). → il faut re-fetch un token régulièrement, pas de
  refresh token dans ce flow (client credentials pur).

### Comment le token est validé côté gateway

`apisix/init.sh` définit un `plugin_config` partagé `auth`, appliqué à toutes les routes `/api/*` :

```jsonc
"openid-connect": {
  "client_id": "...", "client_secret": "...",
  "discovery": "https://auth.edel-id.app/.well-known/openid-configuration",
  "bearer_only": true,     // pas de redirection login, juste vérif du Bearer
  "use_jwks": true         // vérif de signature via le JWKS publié par Zitadel
}
```

APISIX ne fait **aucun appel** à `api-gateway` pour valider le token : il vérifie la signature
JWT localement contre le JWKS de Zitadel (cache le JWKS). Si le token est absent/expiré/invalide
→ `401` renvoyé directement par APISIX, la requête n'atteint jamais `api-gateway`.

Un `serverless-pre-function` (même fichier) décode en plus le payload du JWT pour en extraire
soit l'org (`client_id` au format `ak-<orgId>-...`), soit `sub`, et l'utilise comme clé de
rate-limit (`consumer_name`) : **120 req/min par organisation** sur les routes API classiques
(route `api`), route `api-stream` séparée (sans rate-limit, buffering nginx désactivé via
`X-Accel-Buffering: no` — indispensable pour que le SSE ne soit pas bufferisé).

---

## 2. Démarrer une vérification

Deux flows distincts, deux verifier backends différents, deux formats de claims différents.

| | Swiss (eID / SWIYU) | European (eIDAS / EUDI) |
|---|---|---|
| Endpoint | `POST /api/verification/ch` | `POST /api/verification/eu` |
| Verifier backend | `verifier.edel-id.app` (SWIYU) | `verifier-backend.eudiw.dev` (EUDI) |
| Format des claims | JSONPath : `"$.given_name"` | nom simple : `"given_name"`, nested en dot notation : `"place_of_birth.locality"` |
| Format credential | SD-JWT VC | SD-JWT VC **ou** mdoc/CBOR |
| Controller | `SwiyuVerificationController.java` | `EudiVerificationController.java` |
| Input model | `SwiyuVerificationInput.java` | `EudiVerificationInput.java` |

Les deux endpoints valident `verificationClaims` contre une **allowlist figée côté backend**
(`VALID_CLAIMS` dans chaque `*VerificationInput.java`) → `400 Bad Request` si vide ou si une
claim inconnue est demandée (`Unknown claims: [...]`).

### Ce qui se passe côté `api-gateway`

**CH (Swiyu)** — `SwiyuVerificationController.createVerification` :
1. Construit un `SwiyuPresentationRequest` à partir des claims JSONPath.
2. `POST {verifier.edel-id.app}/management/api/verifications`.
3. Le verifier répond `{id, verification_url, verification_deeplink}`.
4. Mappé vers `VerificationResponse(id, verificationUrl, verificationUri=verification_deeplink)`.

**EU (EUDI)** — `EudiVerificationController.transfererRequete` :
1. Résout un `intended_use_id` (requis par le verifier EUDI pour son certificat
   d'enregistrement de relying party — sinon erreur `MissingRegistrationCertificate`). Soit
   fixé par config (`eudi.verifier.intended-use-id`), soit auto-résolu (1er élément de
   `GET /ui/intended-uses`) et mis en cache en mémoire (`cachedIntendedUseId`).
2. Construit une **DCQL query** (`EudiPresentationRequest`) : credential au format `SD_JWT`,
   `vct_values=["urn:eudi:pid:1"]`, et une liste de `claims` où chaque claim est un chemin
   (`["family_name"]`, `["place_of_birth","locality"]`...). Cas particulier : `nationalities`
   est une claim tableau → chemin `["nationalities", null]`, le `null` final demandant au wallet
   de divulguer *tous* les éléments du tableau (spec DCQL §7.1.1).
3. `POST {verifier-backend.eudiw.dev}/ui/presentations`.
4. Le verifier répond `{transaction_id, request_uri, client_id, request_uri_method}`.
5. `EudiResponse.generateQrCodeUri()` **construit lui-même** l'URI de deep link (le verifier EUDI
   ne la fournit pas telle quelle) :
   ```
   haip-vp://?client_id=<urlencoded>&request_uri=<urlencoded>&request_uri_method=get
   ```
   (`openid4vp://` si le profil est `OPENID4VP` au lieu de `HAIP`, mais `HAIP` est le seul
   profil utilisé actuellement — cf. `toPresentationRequest`).
6. Mappé vers `VerificationResponse(transactionId, requestUri, qrCodeUri)`.

### Réponse commune (`VerificationResponse.java`)

```json
{
  "id": "...",                 // id (CH) ou transactionId (EU)
  "verificationUrl": "...",    // URL brute de la request object hébergée par le verifier
  "verificationUri": "...",    // deep link à scanner (openid4vp:// ou haip-vp://)
  "qrCodeBitMap": { ... }      // voir section 3
}
```

`qrCodeBitMap` est généré **automatiquement dans le constructeur** de `VerificationResponse`
(`this.qrCodeBitMap = QrCodeUtils.encode(verificationUri)`) — c'est l'ajout maison par rapport à
l'API EUDI/SWIYU brute, pour éviter au client d'avoir à embarquer une lib QR.

---

## 3. Format du QR code / bitmap (`qrCodeBitMap`)

Généré par `QrCodeUtils.encode()` avec la lib **ZXing** (`com.google.zxing`), à partir du
contenu exact de `verificationUri` (pas `verificationUrl`).

Paramètres fixes (pas configurables actuellement) :
- **Error correction level : `M`** (~15% de tolérance aux dégâts/occlusion).
- **Quiet zone : 4 modules** de marge claire de chaque côté (recommandation standard du spec QR
  pour une lecture fiable).

```json
{
  "size": 65,              // largeur/hauteur totale en modules, quiet zone incluse
  "quietZone": 4,          // épaisseur de la marge claire, en modules
  "errorCorrection": "M",
  "rows": [                // "size" lignes, haut → bas
    "0000000000000000000000000000000000000000000000000000000000000",
    "0000...",
    ...
  ]
}
```

- Chaque `row` est une string de `size` caractères, chacun `'1'` (module sombre = pixel à
  peindre) ou `'0'` (module clair = fond).
- `size = moduleCount + 2 * quietZone`, où `moduleCount` est calculé par ZXing selon la
  longueur du contenu et le niveau `M` (pas fixe : plus l'URI est longue, plus `moduleCount`,
  donc `size`, grandit).
- Rendu côté client : boucler sur `rows`, pour chaque caractère dessiner un carré (canvas,
  `<rect>` SVG, ou même une grille CSS) — taille de module en px au choix du client selon la
  résolution voulue. Aucune lib QR requise côté client puisque le bitmap est déjà décodé.
- Alternative : ignorer `qrCodeBitMap` et passer `verificationUri` brut à n'importe quelle lib QR
  standard (`qrcode.js`, `zxing-js`, etc.) — c'est strictement équivalent, `qrCodeBitMap` est un
  raccourci fourni en plus, pas une donnée obligatoire.

Fichiers : `QrCode.java` (modèle) + `QrCodeUtils.java` (encodage).

---

## 4. Récupérer le résultat (stream SSE ou blocking)

Deux options exposées, mêmes données sous-jacentes, l'`api-gateway` **poll le verifier backend
toutes les 2 secondes** (`POLL_INTERVAL_MS = 2000`) dans les deux cas — la différence est juste
la manière dont la réponse est livrée au client :

### Option SSE (`/stream`)

```bash
curl -N https://api.edel-id.app/api/verification/ch/<ID>/stream \
  -H "Authorization: Bearer <ACCESS_TOKEN>"
# ou /api/verification/eu/<TRANSACTION_ID>/stream
```

- Le serveur ouvre un `SseEmitter` (timeout 5 min), poll en arrière-plan (thread pool
  `executor`), et émet **un seul événement** nommé `verification-complete` une fois le wallet
  répondu, puis ferme la connexion (`emitter.complete()`).
- Attention : `EventSource` natif du navigateur ne permet pas de fixer un header
  `Authorization` → utiliser `fetch` en stream ou une lib type `@microsoft/fetch-event-source`
  côté client (exemples dans `HOWTO.md`).

### Option blocking (`/blocking`)

```
GET /api/verification/{ch,eu}/<ID>/blocking
```
Bloque la requête HTTP jusqu'à complétion (même boucle de poll interne) et renvoie directement
le JSON — plus simple si le client ne veut pas gérer du SSE. Timeout ~10 min
(`spring.mvc.async.request-timeout=10m`).

### Payload final (`VerificationClaimsOutput.java`)

```json
{
  "state": "SUCCESS",            // ou "PENDING" tant que le wallet n'a pas répondu
  "verifiedClaims": [
    { "given_name": "Alice" },
    { "family_name": "Smith" }
  ]
}
```

### Ce qui se passe pendant le poll (décodage des credentials)

**CH (Swiyu)** :
`GET {verifier}/management/api/verifications/{id}` → champ `state` (`PENDING`/`SUCCESS`). Une
fois `SUCCESS`, les claims sont déjà en clair dans `wallet_response.credential_subject_data` —
on filtre juste les champs de métadonnées (`vct`, `iss`, `cnf`, `iat`, `status`, et tout ce qui
commence par `vct_`).

**EU (EUDI)** — plus complexe car le verifier EUDI renvoie le **VP token brut** :
`GET {verifier}/ui/presentations/{transactionId}` → tant que le wallet n'a pas répondu, le
verifier renvoie `400 Bad Request` (c'est le signal "pas encore prêt", on continue de poller).
Une fois disponible, `vp_token.<requestId>[0]` contient soit :
- un **SD-JWT** (contient `~`) → décodage manuel : payload JWT en base64url, puis chaque
  disclosure est indexée par son digest SHA-256, et les claims `_sd` sont résolues
  récursivement (y compris dans les tableaux, ex. `nationalities`, où un wallet peut ne
  divulguer qu'une partie des éléments) ;
- un **mdoc/CBOR** (`device_response`) → envoyé au verifier lui-même
  (`POST /utilities/validations/msoMdoc/deviceResponse`) qui le décode en JSON.

Puis filtrage des métadonnées JWT (`_sd`, `_sd_alg`, `iss`, `iat`, `exp`, `nbf`, `vct`, `cnf`,
`sub`, `status`). Toute cette logique est dans `EudiVerificationController` (`decodeSdJwtClaims`,
`decodeCborClaims`, `resolveObject`/`resolveArray`).

---

## Résumé rapide des différences public docs ↔ implémentation

- La doc publique (`portal.edel-id.app/#docs`) décrit les 3 étapes token → create → stream.
- `qrCodeBitMap` dans la réponse de create est **un ajout maison**, absent des specs SWIYU/EUDI
  brutes : généré côté `api-gateway` à la volée à partir de `verificationUri`.
- Il existe aussi un endpoint `/blocking` (poll synchrone) non mentionné dans le message collé
  par l'utilisateur mais bien exposé — alternative au `/stream` SSE.
- Il existe aussi un 3ᵉ flow, **Custom VC** (`/api/verification/custom`), pour des credentials
  émis par l'infra Edel-ID elle-même (même verifier backend que CH, mais `vctType` +
  JSONPath libres sans allowlist) — voir `HOWTO.md` § "Custom VC".

## Sources

- `api-gateway/src/main/java/ch/edelid/apigateway/verification/**`
- `apisix/init.sh`, `apisix/config.yaml`, `apisix/bruno/**` (exemples de requêtes)
- `HOWTO.md` (doc consommateur, à jour avec ce qui est publié sur le portail)
