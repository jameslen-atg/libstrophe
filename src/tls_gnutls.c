/* tls.c
** strophe XMPP client library -- TLS abstraction header
**
** Copyright (C) 2005-2009 Collecta, Inc.
**
**  This software is provided AS-IS with no warranty, either express
**  or implied.
**
**  This program is dual licensed under the MIT and GPLv3 licenses.
*/

/** @file
 *  TLS implementation with GNUTLS
 */

#include <string.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gnutls/x509-ext.h>

#include "common.h"
#include "tls.h"
#include "sock.h"

struct _tls {
    xmpp_ctx_t *ctx; /* do we need this? */
    xmpp_conn_t *conn;
    gnutls_session_t session;
    gnutls_certificate_credentials_t cred;
    gnutls_x509_crt_t client_cert;
    int lasterror;
};

void tls_initialize(void)
{
    /* initialize the GNU TLS global state */
    gnutls_global_init();

    /* TODO: wire in xmpp_ctx_t allocator somehow?
       unfortunately in gnutls it's global, so we can
       only do so much. */
}

void tls_shutdown(void)
{
    /* tear down the GNU TLS global state */
    gnutls_global_deinit();
}

static gnutls_x509_crt_t _tls_load_cert(xmpp_conn_t *conn)
{
    if (conn->tls && conn->tls->client_cert)
        return conn->tls->client_cert;
    gnutls_x509_crt_t cert;
    gnutls_datum_t data;
    int res;
    if (gnutls_x509_crt_init(&cert) < 0)
        return NULL;
    if (gnutls_load_file(conn->tls_client_cert, &data) < 0)
        goto LBL_ERR;
    res = gnutls_x509_crt_import(cert, &data, GNUTLS_X509_FMT_PEM);
    gnutls_free(data.data);
    if (res < 0)
        goto LBL_ERR;
    return cert;
LBL_ERR:
    gnutls_x509_crt_deinit(cert);
    return NULL;
}

static void _tls_free_cert(xmpp_conn_t *conn, gnutls_x509_crt_t cert)
{
    if (conn->tls && conn->tls->client_cert == cert)
        return;
    gnutls_x509_crt_deinit(cert);
}

static int _tls_get_id_on_xmppaddr(xmpp_conn_t *conn,
                                   gnutls_x509_crt_t cert,
                                   unsigned int n,
                                   char **ret)
{
    gnutls_datum_t san;
    size_t name_len, oid_len;
    char oid[128], name[128];
    name_len = oid_len = 128;
    int res =
        gnutls_x509_crt_get_subject_alt_name(cert, n, name, &name_len, NULL);
    if (res == GNUTLS_SAN_OTHERNAME_XMPP) {
        /* This is the happy flow path with fixed GnuTLS.
         * While implementing this I stumbled over an issue in GnuTLS
         * which lead to
         * https://gitlab.com/gnutls/gnutls/-/merge_requests/1397
         */
        if (ret) {
            *ret = xmpp_strdup(conn->ctx, name);
        }
        return GNUTLS_SAN_OTHERNAME_XMPP;
    }
    if (res == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)
        return GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE;
    if (res != GNUTLS_SAN_OTHERNAME)
        return GNUTLS_E_X509_UNKNOWN_SAN;
    res = gnutls_x509_crt_get_subject_alt_othername_oid(cert, n, oid, &oid_len);
    if (res == GNUTLS_SAN_OTHERNAME_XMPP) {
        gnutls_datum_t xmpp_addr;
        san.data = (unsigned char *)name;
        san.size = name_len;
        res = gnutls_x509_othername_to_virtual(oid, &san, NULL, &xmpp_addr);
        if (res < 0) {
            gnutls_free(xmpp_addr.data);
            return GNUTLS_E_MEMORY_ERROR;
        }
        if (ret) {
            *ret = xmpp_strdup(conn->ctx, (char *)xmpp_addr.data);
        }
        gnutls_free(xmpp_addr.data);
        return GNUTLS_SAN_OTHERNAME_XMPP;
    }
    return GNUTLS_E_X509_UNKNOWN_SAN;
}

static int _tls_id_on_xmppaddr(xmpp_conn_t *conn,
                               gnutls_x509_crt_t cert,
                               unsigned int n,
                               char **ret)
{
    int res = GNUTLS_E_SUCCESS;
    unsigned int i, j;
    for (i = j = 0; res != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE; ++i) {
        res = _tls_get_id_on_xmppaddr(conn, cert, i, NULL);
        if (res == GNUTLS_SAN_OTHERNAME_XMPP) {
            if (j == n) {
                res = _tls_get_id_on_xmppaddr(conn, cert, i, ret);
                break;
            }
            j++;
        }
    }
    return res;
}

/** Search through the SubjectAlternativeNames and return the next
 *  id-on-xmppAddr element starting from `n`.
 */
char *tls_id_on_xmppaddr(xmpp_conn_t *conn, unsigned int n)
{
    char *ret = NULL;
    gnutls_x509_crt_t cert = _tls_load_cert(conn);
    if (cert == NULL)
        return NULL;
    _tls_id_on_xmppaddr(conn, cert, n, &ret);
    _tls_free_cert(conn, cert);
    return ret;
}

unsigned int tls_id_on_xmppaddr_num(xmpp_conn_t *conn)
{
    unsigned int ret = 0, n;
    int res = GNUTLS_E_SUCCESS;
    gnutls_x509_crt_t cert = _tls_load_cert(conn);
    if (cert == NULL)
        return 0;
    for (n = 0; res != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE; ++n) {
        res = _tls_id_on_xmppaddr(conn, cert, n, NULL);
        if (res == GNUTLS_SAN_OTHERNAME_XMPP)
            ret++;
    }
    _tls_free_cert(conn, cert);
    return ret;
}

static xmpp_tlscert_t *_x509_to_tlscert(xmpp_ctx_t *ctx, gnutls_x509_crt_t cert)
{
    int res;
    char buf[512], smallbuf[64];
    size_t size, m;
    unsigned int algo, n;
    gnutls_datum_t data;
    time_t time_val;
    xmpp_tlscert_t *tlscert = tlscert_new(ctx);

    gnutls_x509_crt_export2(cert, GNUTLS_X509_FMT_PEM, &data);
    tlscert->pem = xmpp_alloc(ctx, data.size + 1);
    memcpy(tlscert->pem, data.data, data.size);
    tlscert->pem[data.size] = '\0';
    gnutls_free(data.data);

    size = sizeof(buf);
    gnutls_x509_crt_get_dn(cert, buf, &size);
    tlscert->elements[XMPP_CERT_SUBJECT] = xmpp_strdup(ctx, buf);
    size = sizeof(buf);
    gnutls_x509_crt_get_issuer_dn(cert, buf, &size);
    tlscert->elements[XMPP_CERT_ISSUER] = xmpp_strdup(ctx, buf);

    time_val = gnutls_x509_crt_get_activation_time(cert);
    tlscert->elements[XMPP_CERT_NOTBEFORE] = xmpp_strdup(ctx, ctime(&time_val));
    tlscert->elements[XMPP_CERT_NOTBEFORE]
                     [strlen(tlscert->elements[XMPP_CERT_NOTBEFORE]) - 1] =
        '\0';
    time_val = gnutls_x509_crt_get_expiration_time(cert);
    tlscert->elements[XMPP_CERT_NOTAFTER] = xmpp_strdup(ctx, ctime(&time_val));
    tlscert->elements[XMPP_CERT_NOTAFTER]
                     [strlen(tlscert->elements[XMPP_CERT_NOTAFTER]) - 1] = '\0';

    size = sizeof(smallbuf);
    gnutls_x509_crt_get_fingerprint(cert, GNUTLS_DIG_SHA1, smallbuf, &size);
    hex_encode(buf, smallbuf, size);
    tlscert->elements[XMPP_CERT_FINGERPRINT_SHA1] = xmpp_strdup(ctx, buf);
    size = sizeof(smallbuf);
    gnutls_x509_crt_get_fingerprint(cert, GNUTLS_DIG_SHA256, smallbuf, &size);
    hex_encode(buf, smallbuf, size);
    tlscert->elements[XMPP_CERT_FINGERPRINT_SHA256] = xmpp_strdup(ctx, buf);

    xmpp_snprintf(buf, sizeof(buf), "%d", gnutls_x509_crt_get_version(cert));
    tlscert->elements[XMPP_CERT_VERSION] = xmpp_strdup(ctx, buf);

    algo = gnutls_x509_crt_get_pk_algorithm(cert, NULL);
    tlscert->elements[XMPP_CERT_KEYALG] =
        xmpp_strdup(ctx, gnutls_pk_algorithm_get_name(algo));
    algo = gnutls_x509_crt_get_signature_algorithm(cert);
    tlscert->elements[XMPP_CERT_SIGALG] =
        xmpp_strdup(ctx, gnutls_sign_get_name(algo));

    size = sizeof(smallbuf);
    gnutls_x509_crt_get_serial(cert, smallbuf, &size);
    hex_encode(buf, smallbuf, size);
    tlscert->elements[XMPP_CERT_SERIALNUMBER] = xmpp_strdup(ctx, buf);

    for (n = 0, m = 0, res = 0; res != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE;
         ++n) {
        size = sizeof(buf);
        res = gnutls_x509_crt_get_subject_alt_name(cert, n, buf, &size, NULL);
        if (res == GNUTLS_SAN_DNSNAME) {
            if (tlscert_add_dnsname(tlscert, buf))
                xmpp_debug(ctx, "tls", "Can't store dnsName(%zu): %s", m, buf);
            m++;
        }
    }

    return tlscert;
}

static int _tls_verify(gnutls_session_t session)
{
    tls_t *tls = gnutls_session_get_ptr(session);
    const gnutls_datum_t *cert_list;
    gnutls_certificate_type_t type;
    gnutls_datum_t out;
    unsigned int cert_list_size = 0, status;
    gnutls_x509_crt_t cert;

    if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509)
        return -1;

    if (gnutls_certificate_verify_peers2(session, &status) < 0) {
        xmpp_error(tls->ctx, "tls", "Verify peers failed");
        return -1;
    }
    type = gnutls_certificate_type_get(session);
    if (gnutls_certificate_verification_status_print(status, type, &out, 0) <
        0) {
        xmpp_error(tls->ctx, "tls", "Status print failed");
        return -1;
    }

    /* Return early if the Certificate is trusted
     * OR if we trust all Certificates */
    if (status == 0 || tls->conn->tls_trust)
        return 0;

    if (!tls->conn->certfail_handler) {
        xmpp_error(tls->ctx, "tls",
                   "No certfail handler set, canceling connection attempt");
        return -1;
    }

    cert_list = gnutls_certificate_get_peers(session, &cert_list_size);

    /* OpenSSL displays the certificate chain in reverse order than GnuTLS.
     * To show consistent behavior to the user, traverse the list from the
     * end.
     */
    while (cert_list_size--) {
        gnutls_x509_crt_init(&cert);

        gnutls_x509_crt_import(cert, &cert_list[cert_list_size],
                               GNUTLS_X509_FMT_DER);

        xmpp_tlscert_t *tlscert = _x509_to_tlscert(tls->ctx, cert);

        if (!tlscert) {
            gnutls_x509_crt_deinit(cert);
            gnutls_free(out.data);
            return -1;
        }

        if (tls->conn->certfail_handler(tlscert, (char *)out.data) == 0) {
            xmpp_tlscert_free(tlscert);
            gnutls_x509_crt_deinit(cert);
            gnutls_free(out.data);
            return -1;
        }
        xmpp_tlscert_free(tlscert);
        gnutls_x509_crt_deinit(cert);
    }
    gnutls_free(out.data);
    return 0;
}

tls_t *tls_new(xmpp_conn_t *conn)
{
    tls_t *tls = xmpp_alloc(conn->ctx, sizeof(tls_t));

    if (tls) {
        memset(tls, 0, sizeof(*tls));
        tls->ctx = conn->ctx;
        tls->conn = conn;
        gnutls_init(&tls->session, GNUTLS_CLIENT);

        gnutls_certificate_allocate_credentials(&tls->cred);
        tls_set_credentials(tls, NULL);

        if (conn->tls_client_cert && conn->tls_client_key) {
            tls->client_cert = _tls_load_cert(conn);
            if (!tls->client_cert) {
                xmpp_error(tls->ctx, "tls",
                           "could not read client certificate");
                gnutls_certificate_free_credentials(tls->cred);
                gnutls_deinit(tls->session);
                xmpp_free(tls->ctx, tls);
                return NULL;
            }
            gnutls_certificate_set_x509_key_file(
                tls->cred, conn->tls_client_cert, conn->tls_client_key,
                GNUTLS_X509_FMT_PEM);
        }

        gnutls_certificate_set_verify_function(tls->cred, _tls_verify);

        gnutls_set_default_priority(tls->session);
        gnutls_session_set_ptr(tls->session, tls);

        /* fixme: this may require setting a callback on win32? */
        gnutls_transport_set_int(tls->session, conn->sock);
    }

    return tls;
}

void tls_free(tls_t *tls)
{
    if (tls->client_cert)
        gnutls_x509_crt_deinit(tls->client_cert);
    gnutls_deinit(tls->session);
    gnutls_certificate_free_credentials(tls->cred);
    xmpp_free(tls->ctx, tls);
}

xmpp_tlscert_t *tls_peer_cert(xmpp_conn_t *conn)
{
    xmpp_tlscert_t *tlscert = NULL;
    if (conn && conn->tls && conn->tls->session) {
        unsigned int list_size = 0;
        const gnutls_datum_t *der_cert =
            gnutls_certificate_get_peers(conn->tls->session, &list_size);
        if (der_cert && list_size) {
            gnutls_x509_crt_t cert;
            if (gnutls_x509_crt_init(&cert) < 0)
                return NULL;
            if (gnutls_x509_crt_import(cert, der_cert, GNUTLS_X509_FMT_DER) ==
                0)
                tlscert = _x509_to_tlscert(conn->ctx, cert);
            gnutls_x509_crt_deinit(cert);
        }
    }
    return tlscert;
}

int tls_set_credentials(tls_t *tls, const char *cafilename)
{
    UNUSED(cafilename);

    /* set trusted credentials -- takes a .pem filename */
    int err = gnutls_certificate_set_x509_system_trust(tls->cred);
    if (err >= 0 && tls->conn->tls_cafile)
        err = gnutls_certificate_set_x509_trust_file(
            tls->cred, tls->conn->tls_cafile, GNUTLS_X509_FMT_PEM);
    if (err >= 0 && tls->conn->tls_capath)
        err = gnutls_certificate_set_x509_trust_dir(
            tls->cred, tls->conn->tls_capath, GNUTLS_X509_FMT_PEM);
    if (err >= 0) {
        err = gnutls_credentials_set(tls->session, GNUTLS_CRD_CERTIFICATE,
                                     tls->cred);
    }
    tls->lasterror = err;

    return err == GNUTLS_E_SUCCESS;
}

int tls_start(tls_t *tls)
{
    sock_set_blocking(tls->conn->sock);
    tls->lasterror = gnutls_handshake(tls->session);
    sock_set_nonblocking(tls->conn->sock);

    return tls->lasterror == GNUTLS_E_SUCCESS;
}

int tls_stop(tls_t *tls)
{
    tls->lasterror = gnutls_bye(tls->session, GNUTLS_SHUT_RDWR);
    return tls->lasterror == GNUTLS_E_SUCCESS;
}

int tls_error(tls_t *tls)
{
    return tls->lasterror;
}

int tls_is_recoverable(int error)
{
    return !gnutls_error_is_fatal(error);
}

int tls_pending(tls_t *tls)
{
    return gnutls_record_check_pending(tls->session);
}

int tls_read(tls_t *tls, void *buff, size_t len)
{
    int ret;

    ret = gnutls_record_recv(tls->session, buff, len);
    tls->lasterror = ret < 0 ? ret : 0;

    return ret;
}

int tls_write(tls_t *tls, const void *buff, size_t len)
{
    int ret;

    ret = gnutls_record_send(tls->session, buff, len);
    tls->lasterror = ret < 0 ? ret : 0;

    return ret;
}

int tls_clear_pending_write(tls_t *tls)
{
    UNUSED(tls);
    return 0;
}
