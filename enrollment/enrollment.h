#ifndef EDEL_CHECK_PICO_ENROLLMENT_H
#define EDEL_CHECK_PICO_ENROLLMENT_H

#include <stdbool.h>

// Appairage d'un boitier, tel que decrit par le contrat MQTT §8.
//
//   1. POST /provisioning/claim   -> code affiche + ticket garde en RAM
//   2. l'operateur saisit le code dans son portail
//   3. POST /provisioning/poll    -> identite et secret, UNE SEULE FOIS
//
// Ce sont les seuls appels HTTPS du boitier de toute sa vie ; ensuite tout passe par MQTT.
//
// Le code d'appairage est visible de toute la salle, c'est assume : ce n'est pas lui qui
// autorise l'etape 3, c'est le claim_ticket, tire sur 32 octets et jamais affiche.

typedef enum {
    ENROLLMENT_DEJA_APPAIRE,  // une identite valide etait deja en flash, rien n'a ete fait
    ENROLLMENT_REUSSI,        // identite recue ET persistee ET relue
    ENROLLMENT_ECHEC          // definitif : code expire, secret deja delivre, ou flash HS
} enrollment_result_t;

// Appele des qu'un code d'appairage est obtenu, puis a chaque relance apres expiration.
// C'est a l'appelant de decider comment le montrer - console, ecran e-ink, ou les deux.
typedef void (*enrollment_code_cb)(const char *pairing_code, int poll_interval_s);

// Bloque jusqu'a ce que le boitier ait une identite, ou jusqu'a un echec definitif.
//
// Ne fait rien si le boitier est deja appaire : c'est le cas nominal a chaque demarrage.
enrollment_result_t enrollment_run(enrollment_code_cb on_code);

#endif //EDEL_CHECK_PICO_ENROLLMENT_H
