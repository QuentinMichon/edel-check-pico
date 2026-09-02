#ifndef EDEL_CHECK_PICO_SESSION_H
#define EDEL_CHECK_PICO_SESSION_H

#include <stdbool.h>

// Ouverture d'une session de verification, cote boitier.
//
// Le boitier ne fait qu'UNE chose : dire "le profil N a ete demande". Il ne construit
// aucune requete de presentation, n'appelle aucun service de verification et ne connait
// aucun attribut. Tout cela se passe dans device-service, qui repond ensuite par des
// images deja tramees.
//
//   boitier ──evt──▶ session_open {req_id, profile_id}
//   boitier ◀──cmd── session_ready {req_id, img_id, chunks}
//   boitier ◀──img── 8 fragments                      → afficher le QR
//   boitier ◀──cmd── result {verdict, label, img_id}
//   boitier ◀──img── 8 fragments                      → afficher le verdict

// Ouvre une session pour le profil d'indice donne dans la liste recue en cfg.
//
// Retourne false si l'indice est hors bornes, ou si la connexion au broker est tombee.
// Le `req_id` est tire ici et repris tel quel par le serveur dans session_ready : MQTT
// 3.1.1 n'a pas les donnees de correlation de MQTT 5, la correlation passe donc par la
// charge utile.
bool session_open(int profile_index);

// L'operateur a quitte la page avant le verdict.
//
// Le contrat (v1.0, fige) n'a que trois messages du boitier vers le cloud : session_open,
// img_abort et rotate_ack. Il n'existe aucun moyen de dire au serveur qu'on abandonne, et
// celui-ci publiera son verdict jusqu'a 120 s plus tard. L'abandon est donc purement local :
// le boitier cesse d'afficher ce qui concerne cette session.
//
// Cote serveur rien ne fuit : la session passe en `timeout` toute seule et le permis de
// concurrence, tenu au plus 120 s, est rendu dans un finally.
void session_abandon(void);

bool session_est_abandonnee(void);

// Le req_id de la derniere session ouverte, chaine vide si aucune.
const char *session_last_req_id(void);

#endif //EDEL_CHECK_PICO_SESSION_H
