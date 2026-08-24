#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

// config mbedTLS minimale pour un client TLS 1.2 (utilisee par http_client.c en https)
// voir https://github.com/Mbed-TLS/mbedtls/blob/development/include/mbedtls/mbedtls_config.h

// contourne un souci de certains fichiers mbedtls qui utilisent INT_MAX sans inclure limits.h
#include <limits.h>

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#define MBEDTLS_SSL_OUT_CONTENT_LEN    2048

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT

#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_ECP_DP_SECP192R1_ENABLED
#define MBEDTLS_ECP_DP_SECP224R1_ENABLED
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_SECP192K1_ENABLED
#define MBEDTLS_ECP_DP_SECP224K1_ENABLED
#define MBEDTLS_ECP_DP_SECP256K1_ENABLED
#define MBEDTLS_ECP_DP_BP256R1_ENABLED
#define MBEDTLS_ECP_DP_BP384R1_ENABLED
#define MBEDTLS_ECP_DP_BP512R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_AES_FEWER_TABLES

// TLS 1.2 uniquement (suffisant pour un client qui va chercher des pages http/json)
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

// ECDHE-RSA, indispensable des qu'on parle a un serveur a certificat RSA.
//
// Sans cette ligne le client ne propose que du RSA statique (refuse par tout serveur
// moderne) et de l'ECDHE-ECDSA. Face a un certificat RSA il n'y a alors aucune suite en
// commun : le serveur ferme, et lwIP remonte -15 (ERR_CLSD) - un symptome qui ressemble a
// une panne reseau et n'a rien a voir.
//
// Mesure sur le broker EdelCheck le 23.08.2026 :
//   openssl s_client -cipher ECDHE-ECDSA-...  -> sslv3 alert handshake failure (40)
//   openssl s_client -cipher ECDHE-RSA-...    -> ECDHE-RSA-AES256-GCM-SHA384
//   Peer signature type                       -> RSA-PSS
//
// PKCS1_V21 va avec : RSA-PSS est une signature PSS, que V15 seul ne sait pas verifier.
// Ca concerne aussi le futur deploiement VPS - Let's Encrypt delivre du RSA par defaut.
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_GCM_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_WRITE_C

// necessaire pour parser un certificat
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

// accelere significativement mbedtls (optimisations NIST)
#define MBEDTLS_ECP_NIST_OPTIM

#endif
