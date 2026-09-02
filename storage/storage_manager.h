//
// Created by Quentin Michon on 14.08.2026.
//

#ifndef EDEL_CHECK_PICO_STORAGE_MANAGER_H
#define EDEL_CHECK_PICO_STORAGE_MANAGER_H

#include <pico/stdlib.h>

#define MEM_BEARER_TOKEN_SIZE 800
#define MEM_SSID_SIZE 32
#define MEM_PASSWORD_SIZE 64

// --- appairage (contrat MQTT §8) ---
#define MEM_DEVICE_ID_SIZE 40      // UUID + NUL
#define MEM_SECRET_SIZE 72         // 64 caracteres hexa + NUL, avec marge
#define MEM_BROKER_HOST_SIZE 64
#define MEM_CA_CERT_SIZE 1600      // mesure : la CA de developpement fait 1151 octets

// Change a chaque modification de la disposition de persistent_storage_t.
//
// ⚠ INDISPENSABLE. load_local_storage() ne valide que la magie : agrandir la structure
// sans changer cette valeur ferait relire, sur une carte deja flashee, l'ancienne
// structure suivie de flash effacee (0xFF) - avec une magie toujours valide. Le firmware
// croirait alors detenir un device_id et un secret qui ne sont que du 0xFF.
//
//   0xCAFE7478  disposition d'origine (Quentin, 14.08.2026)
//   0xCAFE7479  ajout de l'appairage, du CRC et de l'alternance A/B
#define CONFIG_MAGIC 0xCAFE7479

typedef struct {
    uint32_t magic;
    uint32_t seq;               // compteur de generation : le plus grand des deux gagne
    uint32_t crc32;             // calcule sur tous les octets qui SUIVENT ce champ

    uint8_t flags;              // flags[0] ~ manufacturer state (1 true / 0 false)

    // Vestige de l epoque ou le boitier appelait l API d identite directement, avec un
    // jeton OAuth2. Il passe desormais par la passerelle et ce champ n est plus ni ecrit
    // ni lu. Il RESTE parce que le retirer changerait la disposition de la structure :
    // il faudrait alors changer CONFIG_MAGIC, donc invalider la flash de tout boitier
    // deja appaire et le forcer a se reappairer. 800 octets sur un secteur de 4096.
    char bearer_token[MEM_BEARER_TOKEN_SIZE];

    char wifi_1_ssid[MEM_SSID_SIZE];
    char wifi_1_password[MEM_PASSWORD_SIZE];
    char wifi_2_ssid[MEM_SSID_SIZE];
    char wifi_2_password[MEM_PASSWORD_SIZE];
    char wifi_3_ssid[MEM_SSID_SIZE];
    char wifi_3_password[MEM_PASSWORD_SIZE];

    // --- identite du boitier, recue une seule fois a l'appairage ---
    char     device_id[MEM_DEVICE_ID_SIZE];
    char     device_secret[MEM_SECRET_SIZE];
    char     broker_host[MEM_BROKER_HOST_SIZE];
    uint16_t broker_port;
    char     ca_cert_pem[MEM_CA_CERT_SIZE];

} persistent_storage_t;

bool init_local_storage(void);
persistent_storage_t *get_local_storage();

// --- appairage ---------------------------------------------------------------

// vrai si un device_id ET un secret sont presents : le boitier a une identite et doit
// aller directement au broker, sans repasser par l'appairage.
bool storage_is_enrolled(void);

// Enregistre l'identite recue de /provisioning/poll, puis RELIT la flash et verifie le
// CRC. Retourne false si la relecture ne correspond pas - l'appelant ne doit alors PAS
// considerer l'appairage comme acquis.
//
// ⚠ L'ordre importe. Le serveur ne sert le secret qu'une fois, et un second poll reussi
// est traite comme une preuve de duplication (le boitier passe en SUSPECT, avec alerte
// critique). Une coupure de courant entre la reponse 200 et une ecriture terminee ferait
// donc s'auto-denoncer le boitier au redemarrage suivant. D'ou les deux secteurs alternes :
// l'ancien contenu reste intact tant que le nouveau n'est pas integralement ecrit et relu.
bool storage_save_enrollment(const char *device_id, const char *device_secret,
                             const char *broker_host, uint16_t broker_port,
                             const char *ca_cert_pem);

// Efface l'identite (commande `revoked` du contrat MQTT §3). Les identifiants Wi-Fi et le
// reste de la configuration survivent : le boitier reste utilisable, il est seulement
// redevenu anonyme.
bool storage_clear_enrollment(void);

#endif //EDEL_CHECK_PICO_STORAGE_MANAGER_H
