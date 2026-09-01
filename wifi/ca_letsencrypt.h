#ifndef EDEL_CA_LETSENCRYPT_H
#define EDEL_CA_LETSENCRYPT_H

#include <stddef.h>

// Racine ISRG X1. La longueur INCLUT le zero final : mbedTLS l'exige pour une
// autorite au format PEM.
extern const char   EDEL_CA_LETSENCRYPT[];
extern const size_t EDEL_CA_LETSENCRYPT_LEN;

#endif
