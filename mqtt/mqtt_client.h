#ifndef EDEL_CHECK_PICO_MQTT_CLIENT_H
#define EDEL_CHECK_PICO_MQTT_CLIENT_H

#include <stdbool.h>

// Connexion permanente du boitier au broker EdelCheck, selon le contrat MQTT §1 et §2.
//
// Le protocole lui-meme vient de lwIP (pico_lwip_mqtt) : il n'y a pas de client MQTT
// ecrit a la main ici, seulement la logique propre au contrat - identifiant de client
// impose, testament, abonnements topic par topic.
//
// Les identifiants et l'adresse du broker sont lus dans la flash : appairer suffit, il n'y
// a rien a recompiler quand l'infrastructure demenage.

// Ouvre la connexion. Retourne false si le boitier n'est pas appaire, ou si l'allocation
// echoue. Un echec reseau n'est PAS un echec ici : la reconnexion est prise en charge par
// edel_mqtt_poll().
bool edel_mqtt_start(void);

// A appeler regulierement depuis la boucle principale. Reconnecte si la connexion est
// tombee, en espacant les tentatives.
void edel_mqtt_poll(void);

bool edel_mqtt_is_connected(void);

// Publie sur dev/{id}/evt. Retourne false si la connexion n'est pas etablie.
bool edel_mqtt_publish_evt(const char *json);

// Rappel invoque quand un ecran d'erreur transitoire a fini son temps d'affichage, pour
// que l'interface reprenne la main.
//
// Sans lui, un abandon d'image laisse l'operateur devant un message qui dit " reessayer "
// sans dire comment : le menu des profils a ete efface par le message lui-meme. Le boitier
// a l'air plante alors qu'il attend simplement un appui.
void edel_mqtt_set_ui_restore_cb(void (*cb)(void));

#endif //EDEL_CHECK_PICO_MQTT_CLIENT_H
