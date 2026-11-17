/* tls13_verify.h — checks the server's certificate chain and CertificateVerify, then
 * releases application keys (tls13_client_authenticate). Call it once the client reaches
 * TLS_ST_WAIT_AUTH. Nothing else can make the client READY. */
#ifndef HELIX_TLS13_VERIFY_H
#define HELIX_TLS13_VERIFY_H

#include <stddef.h>

#include "tls13_client.h"
#include "x509_verify.h"

/* Returns 1 and moves the client to READY if the chain is trusted for pol->host and the
 * server signed the handshake transcript with the leaf key. Otherwise returns 0, leaves
 * the client in WAIT_AUTH, and writes the reason into err. */
int tls13_client_verify_server(tls13_client_t *c, const x509_policy_t *pol,
                               char *err, size_t err_cap);

#endif /* HELIX_TLS13_VERIFY_H */
