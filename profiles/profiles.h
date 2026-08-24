#ifndef EDEL_CHECK_PICO_PROFILES_H
#define EDEL_CHECK_PICO_PROFILES_H

#include <stdbool.h>
#include <stddef.h>

// Les profils de verification assignes a ce boitier, tels que le cloud les envoie sur
// dev/{id}/cfg.
//
// Un profil, c'est ce qu'un bouton declenche : " Controle 18 ans ", " Identite complete ".
// Le boitier n'apprend PAS ce qui est verifie - ni quels attributs sont demandes, ni a
// quel emetteur on fait confiance. Seulement un identifiant a renvoyer et un libelle a
// afficher. La requete de presentation est construite cote serveur, et c'est ce qui permet
// d'ajouter le support d'un nouveau format de credential sans toucher au parc.
//
// Le message cfg est RETENU par le broker : un boitier qui redemarre, ou qui revient apres
// des semaines dans un carton, retrouve sa configuration a l'abonnement, sans aucun
// aller-retour.

#define PROFILES_MAX 8
#define PROFILE_ID_SIZE 40      // UUID + NUL
#define PROFILE_LABEL_SIZE 64

typedef struct {
    char id[PROFILE_ID_SIZE];
    char label[PROFILE_LABEL_SIZE];
    int  order;                 // ordre de navigation, a partir de 0, contigu
    bool holder_binding;        // le portrait est demande : l'affichage sera plus long
} device_profile_t;

// Traite une charge utile cfg complete. Le JSON doit etre entier - la charge arrive en
// plusieurs tranches sur MQTT, c'est a l'appelant de les avoir rassemblees.
//
// Retourne true si la configuration a ete adoptee, false si elle a ete ignoree (version
// deja connue, ou JSON inexploitable).
bool profiles_handle_cfg(const char *json, size_t len);

int profiles_count(void);

// NULL si l'index est hors bornes. Les profils sont ranges par `order` croissant.
const device_profile_t *profiles_get(int index);

// Version de la configuration courante, -1 si aucune n'a encore ete recue.
int profiles_version(void);

// Oublie tout. Appele a la revocation, et quand le message retenu est efface.
void profiles_clear(void);

#endif //EDEL_CHECK_PICO_PROFILES_H
