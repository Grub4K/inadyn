/* GnuTLS interface for optional HTTPS functions
 *
 * Copyright (C) 2014-2016  Joachim Nilsson <troglobit@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, visit the Free Software Foundation
 * website at http://www.gnu.org/licenses/gpl-2.0.html or write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "debug.h"
#include "http.h"
#include <gnutls/x509.h>

#define CAFILE "/etc/ssl/certs/ca-certificates.crt"

static gnutls_certificate_credentials_t xcred;

/* This function will verify the peer's certificate, and check
 * if the hostname matches, as well as the activation, expiration dates.
 */
static int verify_certificate_callback(gnutls_session_t session)
{
	unsigned int status;
	const gnutls_datum_t *cert_list;
	unsigned int cert_list_size;
	int ret;
	gnutls_x509_crt_t cert;
	const char *hostname;

	/* read hostname */
	hostname = gnutls_session_get_ptr(session);

	/* This verification function uses the trusted CAs in the credentials
	 * structure. So you must have installed one or more CA certificates.
	 */
	ret = gnutls_certificate_verify_peers2(session, &status);
	if (ret < 0) {
		logit(LOG_ERR, "Failed verifying certificate peers.");
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
		logit(LOG_WARNING, "The certificate does not have a known issuer.");

	if (status & GNUTLS_CERT_REVOKED)
		logit(LOG_WARNING, "The certificate has been revoked.");

	if (status & GNUTLS_CERT_EXPIRED)
		logit(LOG_WARNING, "The certificate has expired.");

	if (status & GNUTLS_CERT_NOT_ACTIVATED)
		logit(LOG_WARNING, "The certificate is not yet activated.");

	if (status & GNUTLS_CERT_INVALID) {
		logit(LOG_ERR, "The certificate is not trusted.");
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	/* Up to here the process is the same for X.509 certificates and
	 * OpenPGP keys. From now on X.509 certificates are assumed. This can
	 * be easily extended to work with openpgp keys as well.
	 */
	if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
		logit(LOG_ERR, "Not a valid X.509 certificate");
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	if (gnutls_x509_crt_init(&cert) < 0) {
		logit(LOG_ERR, "Failed init of X.509 cert engine");
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
	if (cert_list == NULL) {
		logit(LOG_ERR, "No certificate was found!");
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	if (gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER) < 0) {
		logit(LOG_ERR, "Error while parsing certificate.");
		return GNUTLS_E_CERTIFICATE_ERROR;
	}


	if (!gnutls_x509_crt_check_hostname(cert, hostname)) {
		logit(LOG_ERR, "The certificate's owner does not match the hostname '%s'", hostname);
		return GNUTLS_E_CERTIFICATE_ERROR;
	}

	gnutls_x509_crt_deinit(cert);

	/* notify gnutls to continue handshake normally */
	return 0;
}

void ssl_init(void)
{
	if (!gnutls_check_version("3.1.4")) {
		logit(LOG_ERR, "%s requires GnuTLS 3.1.4 or later for SSL", __progname);
		exit(1);
	}

	/* for backwards compatibility with gnutls < 3.3.0 */
	gnutls_global_init();

	/* X509 stuff */
	gnutls_certificate_allocate_credentials(&xcred);

	/* sets the trusted cas file */
	gnutls_certificate_set_x509_trust_file(xcred, CAFILE, GNUTLS_X509_FMT_PEM);
	gnutls_certificate_set_verify_function(xcred, verify_certificate_callback);
}


void ssl_exit(void)
{
	gnutls_certificate_free_credentials(xcred);
	gnutls_global_deinit();
}

void ssl_get_info(http_t *client)
{
#ifndef gnutls_session_get_desc
	(void)client;
#else
	char *info;

	/* Available since 3.1.10  */
	info = gnutls_session_get_desc(client->ssl);
	logit(LOG_INFO, "SSL connection using: %s", info);
	gnutls_free(info);
#endif
}


int ssl_open(http_t *client, char *msg)
{
	int ret;
	char buf[256];
	size_t len;
	const char *sn, *err;
	const gnutls_datum_t *cert_list;
	unsigned int cert_list_size = 0;
	gnutls_x509_crt_t cert;

	if (!client->ssl_enabled)
		return tcp_init(&client->tcp, msg);

	/* Initialize TLS session */
	logit(LOG_INFO, "%s, initiating HTTPS ...", msg);
	gnutls_init(&client->ssl, GNUTLS_CLIENT);

	/* SSL SNI support: tell the servername we want to speak to */
	http_get_remote_name(client, &sn);
	gnutls_session_set_ptr(client->ssl, (void *)sn);
	if (gnutls_server_name_set(client->ssl, GNUTLS_NAME_DNS, sn, strlen(sn)))
		return RC_HTTPS_SNI_ERROR;

	/* Use default priorities */
	ret = gnutls_priority_set_direct(client->ssl, "NORMAL", &err);
	if (ret < 0) {
		if (ret == GNUTLS_E_INVALID_REQUEST)
			logit(LOG_ERR, "Syntax error at: %s", err);

		return RC_HTTPS_INVALID_REQUEST;
	}

	/* put the x509 credentials to the current session */
	gnutls_credentials_set(client->ssl, GNUTLS_CRD_CERTIFICATE, xcred);

	/* connect to the peer */
	tcp_set_port(&client->tcp, 443);
	DO(tcp_init(&client->tcp, msg));

	/* Forward TCP socket to GnuTLS, the set_int() API is perhaps too new still ... since 3.1.9 */
//	gnutls_transport_set_int(client->ssl, client->tcp.ip.socket);
	gnutls_transport_set_ptr(client->ssl, (gnutls_transport_ptr_t)(intptr_t)client->tcp.ip.socket);

	/* Perform the TLS handshake */
	do {
		ret = gnutls_handshake(client->ssl);
	}
	while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);
	/* Note that DTLS may also receive GNUTLS_E_LARGE_PACKET */

	if (ret < 0) {
		logit(LOG_ERR, "SSL handshake with %s failed: %s", sn, gnutls_strerror(ret));
		return RC_HTTPS_FAILED_CONNECT;
	}

	ssl_get_info(client);

	/* Get server's certificate (note: beware of dynamic allocation) - opt */
	cert_list = gnutls_certificate_get_peers(client->ssl, &cert_list_size);
	if (cert_list_size > 0) {
		if (gnutls_x509_crt_init(&cert))
			return RC_HTTPS_FAILED_GETTING_CERT;

		gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER);

		len = sizeof(buf);
		gnutls_x509_crt_get_dn(cert, buf, &len);
		logit(LOG_INFO, "SSL server cert subject: %s", buf);

		len = sizeof(buf);
		gnutls_x509_crt_get_issuer_dn(cert, buf, &len);
		logit(LOG_INFO, "SSL server cert issuer: %s", buf);

		/* We could do all sorts of certificate verification stuff here before
		   deallocating the certificate. */
		gnutls_x509_crt_deinit(cert);
	}

	return 0;
}

int ssl_close(http_t *client)
{
	if (client->ssl_enabled) {
		gnutls_bye(client->ssl, GNUTLS_SHUT_WR);
		gnutls_deinit(client->ssl);
	}

	return tcp_exit(&client->tcp);
}

int ssl_send(http_t *client, const char *buf, int len)
{
	int ret;

	if (!client->ssl_enabled)
		return tcp_send(&client->tcp, buf, len);

	do {
		ret = gnutls_record_send(client->ssl, buf, len);
	} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

	if (ret < 0)
		return RC_HTTPS_SEND_ERROR;

	logit(LOG_DEBUG, "Successfully sent DDNS update using HTTPS!");

	return 0;
}

int ssl_recv(http_t *client, char *buf, int buf_len, int *recv_len)
{
	int ret, len = buf_len;

	if (!client->ssl_enabled)
		return tcp_recv(&client->tcp, buf, buf_len, recv_len);

	/* Read HTTP header */
	do {
		ret = gnutls_record_recv(client->ssl, buf, len);
	} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

	if (ret < 0)
		return RC_HTTPS_RECV_ERROR;

	/* Read HTTP body */
	len = ret;
	buf_len -= ret;
	do {
		ret = gnutls_record_recv(client->ssl, &buf[len], buf_len);
		if (ret >= 0) {
			len += ret;
			buf_len -= ret;
		}
	} while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);

	if (ret < 0)
		return RC_HTTPS_RECV_ERROR;

	*recv_len = len;
	logit(LOG_DEBUG, "Successfully received DDNS update response (%d bytes) using HTTPS!", len);

	return 0;
}

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */