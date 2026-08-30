/*
 * product.h — Product identity for the memory-for-ai distribution.
 *
 * Keep externally visible names and operating-system rendezvous identifiers
 * here so this distribution cannot accidentally share state with the upstream
 * the upstream installation.
 */
#ifndef CBM_PRODUCT_H
#define CBM_PRODUCT_H

#define CBM_PRODUCT_NAME "memory-for-ai"
#define CBM_PRODUCT_BINARY_NAME "memory-for-ai"
#define CBM_PRODUCT_BINARY_NAME_WINDOWS "memory-for-ai.exe"
#define CBM_PRODUCT_REPOSITORY "LonelyTraderBay/memory-for-ai"
#define CBM_PRODUCT_REPOSITORY_URL "https://github.com/LonelyTraderBay/memory-for-ai"

#define CBM_PRODUCT_CACHE_ENV "MFA_CACHE_DIR"
#define CBM_PRODUCT_RUNTIME_ENV "MFA_RUNTIME_DIR"
#define CBM_PRODUCT_INDEX_LOG_ENV "MFA_INDEX_LOG"
#define CBM_PRODUCT_CACHE_DIR_NAME "memory-for-ai"
#define CBM_PRODUCT_RUNTIME_PREFIX "memory-for-ai-daemon-"
#define CBM_PRODUCT_IPC_PREFIX "mfa-"
#define CBM_PRODUCT_DAEMON_DOMAIN "memory-for-ai:coordination-daemon"

#endif /* CBM_PRODUCT_H */
