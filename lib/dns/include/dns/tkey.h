/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifndef DNS_TKEY_H
#define DNS_TKEY_H 1

#include <isc/mem.h>
#include <isc/lang.h>

#include <dns/types.h>
#include <dns/name.h>
#include <dns/confctx.h>

#include <dst/dst.h>

ISC_LANG_BEGINDECLS

/* Key agreement modes */
#define DNS_TKEYMODE_SERVERASSIGNED		1
#define DNS_TKEYMODE_DIFFIEHELLMAN		2
#define DNS_TKEYMODE_GSSAPI			3
#define DNS_TKEYMODE_RESOLVERASSIGNED		4
#define DNS_TKEYMODE_DELETE			5

isc_result_t
dns_tkey_init(isc_log_t *lctx, dns_c_ctx_t *cfg, isc_mem_t *mctx);
/*
 *	Obtains TKEY configuration information, including default DH key
 *	and default domain.
 *
 * 	Requires:
 *		'lctx' is not NULL
 *		'cfg' is not NULL
 *		'mctx' is not NULL
 *
 *	Returns
 *		ISC_R_SUCCESS
 *		ISC_R_NOMEMORY
 *		return codes from dns_name_fromtext()
 */

isc_result_t
dns_tkey_processquery(dns_message_t *msg);
/*
 *	Processes a query containing a TKEY record, adding or deleting TSIG
 *	keys if necessary, and modifies the message to contain the response.
 *
 *	Requires:
 *		'msg' is a valid message
 *
 *	Returns
 *		ISC_R_SUCCESS	msg was updated (the TKEY operation succeeded,
 *				or msg now includes a TKEY with an error set)
 *		DNS_R_FORMERR	the packet was malformed (missing a TKEY
 *				or KEY).
 *		other		An error occurred while processing the message
 */

isc_result_t
dns_tkey_builddhquery(dns_message_t *msg, dst_key_t *key, dns_name_t *name,
		      dns_name_t *algorithm, isc_buffer_t *nonce);
/*
 *	Builds a query containing a TKEY that will generate a shared
 *	secret using a Diffie-Hellman key exchange.  The shared key
 *	will be of the specified algorithm (only DNS_TSIG_HMACMD5_NAME
 *	is supported), and will be named either 'name',
 *	'name' + server chosen domain, or random data + server chosen domain
 *	if 'name' == dns_rootname.  If nonce is not NULL, it supplies
 *	random data used in the shared secret computation.
 *
 *	Requires:
 *		'msg' is a valid message
 *		'key' is a valid Diffie Hellman dst key
 *		'name' is a valid name
 *		'algorithm' is a valid name
 *
 *	Returns:
 *		ISC_R_SUCCESS	msg was successfully updated to include the
 *				query to be sent
 *		other		an error occurred while building the message
 */

isc_result_t
dns_tkey_builddeletequery(dns_message_t *msg, dns_tsigkey_t *key);
/*
 *	Builds a query containing a TKEY record that will delete the
 *	specified shared secret from the server.
 *
 *	Requires:
 *		'msg' is a valid message
 *		'key' is a valid TSIG key
 *
 *	Returns:
 *		ISC_R_SUCCESS	msg was successfully updated to include the
 *				query to be sent
 *		other		an error occurred while building the message
 */

isc_result_t
dns_tkey_processdhresponse(dns_message_t *qmsg, dns_message_t *rmsg,
                           dst_key_t *key, dns_tsigkey_t **outkey);
/*
 *	Processes a response to a query containing a TKEY that was
 *	designed to generate a shared secret using a Diffie-Hellman key
 *	exchange.  If the query was successful, a new shared key
 *	is created and added to the list of shared keys.
 *
 *	Requires:
 *		'qmsg' is a valid message (the query)
 *		'rmsg' is a valid message (the response)
 *		'key' is a valid Diffie Hellman dst key
 *		'outkey' is either NULL or a pointer to NULL
 *
 *	Returns:
 *		ISC_R_SUCCESS	the shared key was successfully added
 *		ISC_R_NOTFOUND	an error occurred while looking for a
 *				component of the query or response
 */

isc_result_t
dns_tkey_processdeleteresponse(dns_message_t *qmsg, dns_message_t *rmsg);
/*
 *	Processes a response to a query containing a TKEY that was
 *	designed to delete a shared secret.  If the query was successful,
 *	the shared key is deleted from the list of shared keys.
 *
 *	Requires:
 *		'qmsg' is a valid message (the query)
 *		'rmsg' is a valid message (the response)
 *
 *	Returns:
 *		ISC_R_SUCCESS	the shared key was successfully deleted
 *		ISC_R_NOTFOUND	an error occurred while looking for a
 *				component of the query or response
 */


ISC_LANG_ENDDECLS

#endif /* DNS_TKEY_H */
