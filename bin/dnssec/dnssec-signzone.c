/*
 * Portions Copyright (C) 1999, 2000  Internet Software Consortium.
 * Portions Copyright (C) 1995-2000 by Network Associates, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM AND
 * NETWORK ASSOCIATES DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE CONSORTIUM OR NETWORK
 * ASSOCIATES BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: dnssec-signzone.c,v 1.105 2000/10/27 20:27:51 bwelling Exp $ */

#include <config.h>

#include <stdlib.h>

#include <isc/commandline.h>
#include <isc/entropy.h>
#include <isc/mem.h>
#include <isc/stdio.h>
#include <isc/string.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/dnssec.h>
#include <dns/fixedname.h>
#include <dns/journal.h>
#include <dns/keyvalues.h>
#include <dns/log.h>
#include <dns/master.h>
#include <dns/masterdump.h>
#include <dns/nxt.h>
#include <dns/rdata.h>
#include <dns/rdataset.h>
#include <dns/rdataclass.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatastruct.h>
#include <dns/rdatatype.h>
#include <dns/result.h>
#include <dns/secalg.h>
#include <dns/time.h>

#include <dst/dst.h>
#include <dst/result.h>

#include "dnssectool.h"

const char *program = "dnssec-signzone";
int verbose;

#define BUFSIZE 2048

typedef struct signer_key_struct signer_key_t;

struct signer_key_struct {
	dst_key_t *key;
	isc_boolean_t isdefault;
	unsigned int position;
	ISC_LINK(signer_key_t) link;
};

static ISC_LIST(signer_key_t) keylist;
static unsigned int keycount = 0;
static isc_stdtime_t starttime = 0, endtime = 0, now;
static int cycle = -1;
static isc_boolean_t tryverify = ISC_FALSE;
static isc_mem_t *mctx = NULL;
static isc_entropy_t *ectx = NULL;
static dns_ttl_t zonettl;
static FILE *fp;
static const dns_master_style_t *masterstyle = &dns_master_style_explicitttl;
static unsigned int nsigned = 0, nretained = 0, ndropped = 0;
static unsigned int nverified = 0, nverifyfailed = 0;

static inline void
set_bit(unsigned char *array, unsigned int index, unsigned int bit) {
	unsigned int shift, mask;

	shift = 7 - (index % 8);
	mask = 1 << shift;

	if (bit != 0)
		array[index / 8] |= mask;
	else
		array[index / 8] &= (~mask & 0xFF);
}

static signer_key_t *
newkeystruct(dst_key_t *dstkey, isc_boolean_t isdefault) {
	signer_key_t *key;

	key = isc_mem_get(mctx, sizeof(signer_key_t));
	if (key == NULL)
		fatal("out of memory");
	key->key = dstkey;
	key->isdefault = isdefault;
	key->position = keycount++;
	ISC_LINK_INIT(key, link);
	return (key);
}

static void
signwithkey(dns_name_t *name, dns_rdataset_t *rdataset, dns_rdata_t *rdata,
	    dst_key_t *key, isc_buffer_t *b)
{
	isc_result_t result;

	result = dns_dnssec_sign(name, rdataset, key, &starttime, &endtime,
				 mctx, b, rdata);
	isc_entropy_stopcallbacksources(ectx);
	if (result != ISC_R_SUCCESS) {
		char keystr[KEY_FORMATSIZE];
		key_format(key, keystr, sizeof keystr);
		fatal("key '%s' failed to sign data: %s",
		      keystr, isc_result_totext(result));
	}
	nsigned++;

	if (tryverify) {
		result = dns_dnssec_verify(name, rdataset, key,
					   ISC_TRUE, mctx, rdata);
		if (result == ISC_R_SUCCESS) {
			vbprintf(3, "\tsignature verified\n");
			nverified++;
		} else {
			vbprintf(3, "\tsignature failed to verify\n");
			nverifyfailed++;
		}
	}
}

static inline isc_boolean_t
issigningkey(signer_key_t *key) {
	return (key->isdefault);
}

static inline isc_boolean_t
iszonekey(signer_key_t *key, dns_db_t *db) {
	return (ISC_TF(dns_name_equal(dst_key_name(key->key),
				      dns_db_origin(db)) &&
		       dst_key_iszonekey(key->key)));
}

/*
 * Finds the key that generated a SIG, if possible.  First look at the keys
 * that we've loaded already, and then see if there's a key on disk.
 */
static signer_key_t *
keythatsigned(dns_rdata_sig_t *sig) {
	isc_result_t result;
	dst_key_t *pubkey = NULL, *privkey = NULL;
	signer_key_t *key;

	key = ISC_LIST_HEAD(keylist);
	while (key != NULL) {
		if (sig->keyid == dst_key_id(key->key) &&
		    sig->algorithm == dst_key_alg(key->key) &&
		    dns_name_equal(&sig->signer, dst_key_name(key->key)))
			return key;
		key = ISC_LIST_NEXT(key, link);
	}

	result = dst_key_fromfile(&sig->signer, sig->keyid, sig->algorithm,
				  DST_TYPE_PUBLIC, NULL, mctx, &pubkey);
	if (result != ISC_R_SUCCESS)
		return (NULL);

	key = isc_mem_get(mctx, sizeof(signer_key_t));
	if (key == NULL)
		fatal("out of memory");

	result = dst_key_fromfile(&sig->signer, sig->keyid, sig->algorithm,
				  DST_TYPE_PRIVATE, NULL, mctx, &privkey);
	if (result == ISC_R_SUCCESS) {
		dst_key_free(&pubkey);
		key = newkeystruct(privkey, ISC_FALSE);
	} else
		key = newkeystruct(pubkey, ISC_FALSE);
	ISC_LIST_APPEND(keylist, key, link);
	return (key);
}

/*
 * Check to see if we expect to find a key at this name.  If we see a SIG
 * and can't find the signing key that we expect to find, we drop the sig.
 * I'm not sure if this is completely correct, but it seems to work.
 */
static isc_boolean_t
expecttofindkey(dns_name_t *name, dns_db_t *db, dns_dbversion_t *version) {
	unsigned int options = DNS_DBFIND_NOWILD;
	dns_fixedname_t fname;
	isc_result_t result;
	char namestr[DNS_NAME_FORMATSIZE];

	dns_fixedname_init(&fname);
	result = dns_db_find(db, name, version, dns_rdatatype_key, options,
			     0, NULL, dns_fixedname_name(&fname), NULL, NULL);
	switch (result) {
	case ISC_R_SUCCESS:
	case DNS_R_NXDOMAIN:
	case DNS_R_NXRRSET:
		return (ISC_TRUE);
	case DNS_R_DELEGATION:
	case DNS_R_CNAME:
	case DNS_R_DNAME:
		return (ISC_FALSE);
	}
	dns_name_format(name, namestr, sizeof namestr);
	fatal("failure looking for '%s KEY' in database: %s",
	      namestr, isc_result_totext(result));
	return (ISC_FALSE); /* removes a warning */
}

static inline isc_boolean_t
setverifies(dns_name_t *name, dns_rdataset_t *set, signer_key_t *key,
	    dns_rdata_t *sig)
{
	isc_result_t result;
	result = dns_dnssec_verify(name, set, key->key, ISC_FALSE, mctx, sig);
	if (result == ISC_R_SUCCESS) {
		nverified++;
		return (ISC_TRUE);
	} else {
		nverifyfailed++;
		return (ISC_FALSE);
	}
}

/*
 * Signs a set.  Goes through contortions to decide if each SIG should
 * be dropped or retained, and then determines if any new SIGs need to
 * be generated.
 */
static void
signset(dns_db_t *db, dns_dbversion_t *version, dns_diff_t *diff,
	dns_dbnode_t *node, dns_name_t *name, dns_rdataset_t *set)
{
	dns_rdataset_t sigset;
	dns_rdata_t sigrdata = DNS_RDATA_INIT;
	dns_rdata_sig_t sig;
	signer_key_t *key;
	isc_result_t result;
	isc_boolean_t nosigs = ISC_FALSE;
	isc_boolean_t *wassignedby, *nowsignedby;
	int arraysize;
	dns_difftuple_t *tuple;
	dns_ttl_t ttl;
	int i;
	char namestr[DNS_NAME_FORMATSIZE];
	char typestr[TYPE_FORMATSIZE];
	char sigstr[SIG_FORMATSIZE];

	dns_name_format(name, namestr, sizeof namestr);
	type_format(set->type, typestr, sizeof typestr);

	ttl = ISC_MIN(set->ttl, endtime - starttime);

	dns_rdataset_init(&sigset);
	result = dns_db_findrdataset(db, node, version, dns_rdatatype_sig,
				     set->type, 0, &sigset, NULL);
	if (result == ISC_R_NOTFOUND) {
		result = ISC_R_SUCCESS;
		nosigs = ISC_TRUE;
	}
	if (result != ISC_R_SUCCESS)
		fatal("failed while looking for '%s SIG %s': %s",
		      namestr, typestr, isc_result_totext(result));

	vbprintf(1, "%s/%s:\n", namestr, typestr);

	arraysize = keycount;
	if (!nosigs)
		arraysize += dns_rdataset_count(&sigset);
	wassignedby = isc_mem_get(mctx,
				  arraysize * sizeof(isc_boolean_t));
	nowsignedby = isc_mem_get(mctx,
				  arraysize * sizeof(isc_boolean_t));
	if (wassignedby == NULL || nowsignedby == NULL)
		fatal("out of memory");

	for (i = 0; i < arraysize; i++)
		wassignedby[i] = nowsignedby[i] = ISC_FALSE;

	if (nosigs)
		result = ISC_R_NOMORE;
	else
		result = dns_rdataset_first(&sigset);

	while (result == ISC_R_SUCCESS) {
		isc_boolean_t expired, future;
		isc_boolean_t keep = ISC_FALSE, resign = ISC_FALSE;

		dns_rdataset_current(&sigset, &sigrdata);

		result = dns_rdata_tostruct(&sigrdata, &sig, NULL);
		check_result(result, "dns_rdata_tostruct");

		expired = ISC_TF(now + cycle > sig.timeexpire);
		future = ISC_TF(now < sig.timesigned);

		key = keythatsigned(&sig);
		sig_format(&sig, sigstr, sizeof sigstr);

		if (sig.timesigned > sig.timeexpire) {
			/* sig is dropped and not replaced */
			vbprintf(2, "\tsig by %s dropped - "
				 "invalid validity period\n",
				 sigstr);
		} else if (key == NULL && !future &&
			 expecttofindkey(&sig.signer, db, version))
		{
			/* sig is dropped and not replaced */
			vbprintf(2, "\tsig by %s dropped - "
				 "private key not found\n",
				 sigstr);
		} else if (key == NULL || future) {
			vbprintf(2, "\tsig by %s %s - key not found\n",
				 expired ? "retained" : "dropped", sigstr);
			if (!expired)
				keep = ISC_TRUE;
		} else if (issigningkey(key)) {
			if (!expired && setverifies(name, set, key, &sigrdata))
			{
				vbprintf(2, "\tsig by %s retained\n", sigstr);
				keep = ISC_TRUE;
				wassignedby[key->position] = ISC_TRUE;
				nowsignedby[key->position] = ISC_TRUE;
			} else {
				vbprintf(2, "\tsig by %s dropped - %s\n",
					 sigstr,
					 expired ? "expired" :
						   "failed to verify");
				wassignedby[key->position] = ISC_TRUE;
				resign = ISC_TRUE;
			}
		} else if (iszonekey(key, db)) {
			if (!expired && setverifies(name, set, key, &sigrdata))
			{
				vbprintf(2, "\tsig by %s retained\n", sigstr);
				keep = ISC_TRUE;
				wassignedby[key->position] = ISC_TRUE;
				nowsignedby[key->position] = ISC_TRUE;
			} else {
				vbprintf(2, "\tsig by %s dropped - %s\n",
					 sigstr,
					 expired ? "expired" :
						   "failed to verify");
				wassignedby[key->position] = ISC_TRUE;
			}
		} else if (!expired) {
			vbprintf(2, "\tsig by %s retained\n", sigstr);
			keep = ISC_TRUE;
		} else {
			vbprintf(2, "\tsig by %s expired\n", sigstr);
		}

		if (keep) {
			nowsignedby[key->position] = ISC_TRUE;
			nretained++;
		} else {
			tuple = NULL;
			result = dns_difftuple_create(mctx, DNS_DIFFOP_DEL,
						      name, 0, &sigrdata,
						      &tuple);
			check_result(result, "dns_difftuple_create");
			dns_diff_append(diff, &tuple);
			ndropped++;
		}

		if (resign) {
			isc_buffer_t b;
			dns_rdata_t trdata = DNS_RDATA_INIT;
			unsigned char array[BUFSIZE];
			char keystr[KEY_FORMATSIZE];

			key_format(key->key, keystr, sizeof keystr);
			vbprintf(1, "\tresigning with key %s\n", keystr);
			isc_buffer_init(&b, array, sizeof(array));
			signwithkey(name, set, &trdata, key->key, &b);
			nowsignedby[key->position] = ISC_TRUE;
			tuple = NULL;
			result = dns_difftuple_create(mctx, DNS_DIFFOP_ADD,
						      name, ttl, &trdata,
						      &tuple);
			check_result(result, "dns_difftuple_create");
			dns_diff_append(diff, &tuple);
		}

		dns_rdata_invalidate(&sigrdata);
		dns_rdata_freestruct(&sig);
		result = dns_rdataset_next(&sigset);
	}
	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;

	check_result(result, "dns_rdataset_first/next");
	if (dns_rdataset_isassociated(&sigset))
		dns_rdataset_disassociate(&sigset);

	key = ISC_LIST_HEAD(keylist);
	while (key != NULL) {
		if (key->isdefault && !nowsignedby[key->position]) {
			isc_buffer_t b;
			dns_rdata_t trdata = DNS_RDATA_INIT;
			unsigned char array[BUFSIZE];
			char keystr[KEY_FORMATSIZE];

			key_format(key->key, keystr, sizeof keystr);
			vbprintf(1, "\tsigning with key %s\n", keystr);
			isc_buffer_init(&b, array, sizeof(array));
			signwithkey(name, set, &trdata, key->key, &b);
			tuple = NULL;
			result = dns_difftuple_create(mctx, DNS_DIFFOP_ADD,
						      name, ttl, &trdata,
						      &tuple);
			check_result(result, "dns_difftuple_create");
			dns_diff_append(diff, &tuple);
		}
		key = ISC_LIST_NEXT(key, link);
	}

	isc_mem_put(mctx, wassignedby, arraysize * sizeof(isc_boolean_t));
	isc_mem_put(mctx, nowsignedby, arraysize * sizeof(isc_boolean_t));
}

/* Determine if a KEY set contains a null key */
static isc_boolean_t
hasnullkey(dns_rdataset_t *rdataset) {
	isc_result_t result;
	dns_rdata_t rdata;
	isc_boolean_t found = ISC_FALSE;

	result = dns_rdataset_first(rdataset);
	while (result == ISC_R_SUCCESS) {
		dst_key_t *key = NULL;

		dns_rdata_init(&rdata);
		dns_rdataset_current(rdataset, &rdata);
		result = dns_dnssec_keyfromrdata(dns_rootname,
						 &rdata, mctx, &key);
		if (result != ISC_R_SUCCESS)
			fatal("could not convert KEY into internal format");
		if (dst_key_isnullkey(key))
			found = ISC_TRUE;
                dst_key_free(&key);
		if (found == ISC_TRUE)
			return (ISC_TRUE);
                result = dns_rdataset_next(rdataset);
        }
        if (result != ISC_R_NOMORE)
                fatal("failure looking for null keys");
        return (ISC_FALSE);
}

static void
opendb(const char *prefix, dns_name_t *name, dns_rdataclass_t rdclass,
       dns_db_t **db)
{
	char filename[256];
	isc_buffer_t b;
	isc_result_t result;

	isc_buffer_init(&b, filename, sizeof(filename));
	isc_buffer_putstr(&b, prefix);
	result = dns_name_totext(name, ISC_FALSE, &b);
	check_result(result, "dns_name_totext()");
	if (isc_buffer_availablelength(&b) == 0) {
		char namestr[DNS_NAME_FORMATSIZE];
		dns_name_format(name, namestr, sizeof namestr);
		fatal("name '%s' is too long", namestr);
	}
	isc_buffer_putuint8(&b, 0);

	result = dns_db_create(mctx, "rbt", name, dns_dbtype_zone,
			       rdclass, 0, NULL, db);
	check_result(result, "dns_db_create()");

	result = dns_db_load(*db, filename);
	if (result != ISC_R_SUCCESS && result != DNS_R_SEENINCLUDE)
		dns_db_detach(db);
}

/*
 * Looks for signatures of the zone keys by the parent, and imports them
 * if found.
 */
static void
importparentsig(dns_db_t *db, dns_diff_t *diff, dns_name_t *name,
		dns_rdataset_t *set)
{
	dns_db_t *newdb = NULL;
	dns_dbnode_t *newnode = NULL;
	dns_rdataset_t newset, sigset;
	dns_rdata_t rdata, newrdata;
	isc_result_t result;

	dns_rdataset_init(&newset);
	dns_rdataset_init(&sigset);

	opendb("signedkey-", name, dns_db_class(db), &newdb);
	if (newdb == NULL)
		return;

	result = dns_db_findnode(newdb, name, ISC_FALSE, &newnode);
	if (result != ISC_R_SUCCESS)
		goto failure;
	result = dns_db_findrdataset(newdb, newnode, NULL, dns_rdatatype_key,
				     0, 0, &newset, &sigset);
	if (result != ISC_R_SUCCESS)
		goto failure;

	if (!dns_rdataset_isassociated(&newset) ||
	    !dns_rdataset_isassociated(&sigset))
		goto failure;

	if (dns_rdataset_count(set) != dns_rdataset_count(&newset)) {
		result = DNS_R_BADDB;
		goto failure;
	}

	dns_rdata_init(&rdata);
	dns_rdata_init(&newrdata);

	result = dns_rdataset_first(set);
	check_result(result, "dns_rdataset_first()");
	for (; result == ISC_R_SUCCESS; result = dns_rdataset_next(set)) {
		dns_rdataset_current(set, &rdata);
		result = dns_rdataset_first(&newset);
		check_result(result, "dns_rdataset_first()");
		for (;
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(&newset))
		{
			dns_rdataset_current(&newset, &newrdata);
			if (dns_rdata_compare(&rdata, &newrdata) == 0)
				break;
		}
		if (result != ISC_R_SUCCESS)
			break;
	}
	if (result != ISC_R_NOMORE)
		goto failure;

	vbprintf(2, "found the parent's signature of our zone key\n");

	result = dns_rdataset_first(&sigset);
	while (result == ISC_R_SUCCESS) {
		dns_difftuple_t *tuple = NULL;

		dns_rdataset_current(&sigset, &rdata);
		result = dns_difftuple_create(mctx, DNS_DIFFOP_ADD, name, 
					      sigset.ttl, &rdata, &tuple);
		check_result(result, "dns_difftuple_create");
		dns_diff_append(diff, &tuple);
		result = dns_rdataset_next(&sigset);
	}
	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;

 failure:
	if (dns_rdataset_isassociated(&newset))
		dns_rdataset_disassociate(&newset);
	if (dns_rdataset_isassociated(&sigset))
		dns_rdataset_disassociate(&sigset);
	if (newnode != NULL)
		dns_db_detachnode(newdb, &newnode);
	if (newdb != NULL)
		dns_db_detach(&newdb);
	if (result != ISC_R_SUCCESS)
		fatal("zone signedkey file is invalid or does not match zone");
}

/*
 * Looks for our signatures of child keys.  If present, inform the caller.
 */
static isc_boolean_t
haschildkey(dns_db_t *db, dns_name_t *name) {
	dns_db_t *newdb = NULL;
	dns_dbnode_t *newnode = NULL;
	dns_rdataset_t set, sigset;
	dns_rdata_t sigrdata = DNS_RDATA_INIT;
	isc_result_t result;
	isc_boolean_t found = ISC_FALSE;
	dns_rdata_sig_t sig;
	signer_key_t *key;

	dns_rdataset_init(&set);
	dns_rdataset_init(&sigset);

	opendb("signedkey-", name, dns_db_class(db), &newdb);
	if (newdb == NULL)
		return (ISC_FALSE);

	result = dns_db_findnode(newdb, name, ISC_FALSE, &newnode);
	if (result != ISC_R_SUCCESS)
		goto failure;
	result = dns_db_findrdataset(newdb, newnode, NULL, dns_rdatatype_key,
				     0, 0, &set, &sigset);
	if (result != ISC_R_SUCCESS)
		goto failure;

	if (!dns_rdataset_isassociated(&set) ||
	    !dns_rdataset_isassociated(&sigset))
		goto failure;

	result = dns_rdataset_first(&sigset);
	check_result(result, "dns_rdataset_first()");
	dns_rdata_init(&sigrdata);
	for (; result == ISC_R_SUCCESS; result = dns_rdataset_next(&sigset)) {
		dns_rdataset_current(&sigset, &sigrdata);
		result = dns_rdata_tostruct(&sigrdata, &sig, NULL);
		if (result != ISC_R_SUCCESS)
			goto failure;
		key = keythatsigned(&sig);
		dns_rdata_freestruct(&sig);
		if (key == NULL)
			goto failure;
		result = dns_dnssec_verify(name, &set, key->key,
					   ISC_FALSE, mctx, &sigrdata);
		if (result == ISC_R_SUCCESS) {
			found = ISC_TRUE;
			break;
		}
		dns_rdata_invalidate(&sigrdata);
	}

 failure:
	if (dns_rdataset_isassociated(&set))
		dns_rdataset_disassociate(&set);
	if (dns_rdataset_isassociated(&sigset))
		dns_rdataset_disassociate(&sigset);
	if (newnode != NULL)
		dns_db_detachnode(newdb, &newnode);
	if (newdb != NULL)
		dns_db_detach(&newdb);

	return (found);
}

/*
 * There probably should be a dns_nxt_setbit, but it can get complicated if
 * the length of the bit set needs to be increased.  In this case, since the
 * NXT bit is set and both SIG and KEY are less than NXT, the easy way works.
 */
static void
nxt_setbit(dns_rdataset_t *rdataset, dns_rdatatype_t type) {
	unsigned char *nxt_bits;
	dns_name_t nxtname;
	isc_region_t r, r2;
	isc_result_t result;
	dns_rdata_t rdata = DNS_RDATA_INIT;

	result = dns_rdataset_first(rdataset);
	check_result(result, "dns_rdataset_first()");
	dns_rdataset_current(rdataset, &rdata);
	dns_rdata_toregion(&rdata, &r);
	dns_name_init(&nxtname, NULL);
	dns_name_fromregion(&nxtname, &r);
	dns_name_toregion(&nxtname, &r2);
	nxt_bits = r.base + r2.length;
	set_bit(nxt_bits, type, 1);
}

static void
createnullkey(dns_db_t *db, dns_dbversion_t *version, dns_name_t *name) {
	unsigned char keydata[4];
	dns_rdata_t keyrdata = DNS_RDATA_INIT;
	dns_rdata_key_t key;
	dns_diff_t diff;
	dns_difftuple_t *tuple = NULL;
	isc_buffer_t b;
	isc_result_t result;
	char namestr[DNS_NAME_FORMATSIZE];

	dns_name_format(name, namestr, sizeof namestr);
	vbprintf(2, "adding null key at %s\n", namestr);

	key.common.rdclass = dns_db_class(db);
	key.common.rdtype = dns_rdatatype_key;
	ISC_LINK_INIT(&key.common, link);
	key.mctx = NULL;
	key.flags = DNS_KEYTYPE_NOKEY | DNS_KEYOWNER_ZONE;
	key.protocol = DNS_KEYPROTO_DNSSEC;
	key.algorithm = DNS_KEYALG_DSA;
	key.datalen = 0;
	key.data = NULL;
	isc_buffer_init(&b, keydata, sizeof keydata);
	result = dns_rdata_fromstruct(&keyrdata, dns_db_class(db),
				      dns_rdatatype_key, &key, &b);
	if (result != ISC_R_SUCCESS)
		fatal("failed to build null key");

	dns_diff_init(mctx, &diff);

	result = dns_difftuple_create(mctx, DNS_DIFFOP_ADD, name, zonettl,
				      &keyrdata, &tuple);
	check_result(result, "dns_difftuple_create");

	dns_diff_append(&diff, &tuple);

	result = dns_diff_apply(&diff, db, version);
	check_result(result, "dns_diff_apply");

	dns_diff_clear(&diff);
}

/*
 * Signs all records at a name.  This mostly just signs each set individually,
 * but also adds the SIG bit to any NXTs generated earlier, deals with
 * parent/child KEY signatures, and handles other exceptional cases.
 */
static void
signname(dns_db_t *db, dns_dbversion_t *version, dns_dbnode_t *node,
	 dns_name_t *name)
{
	isc_result_t result;
	dns_rdataset_t rdataset;
	dns_rdatasetiter_t *rdsiter;
	isc_boolean_t isdelegation = ISC_FALSE;
	isc_boolean_t childkey = ISC_FALSE;
	static int warnwild = 0;
	isc_boolean_t atorigin;
	isc_boolean_t neednullkey = ISC_FALSE;
	dns_diff_t diff;

	if (dns_name_iswildcard(name)) {
		char namestr[DNS_NAME_FORMATSIZE];
		dns_name_format(name, namestr, sizeof namestr);
		if (warnwild++ == 0) {
			fprintf(stderr, "%s: warning: BIND 9 doesn't properly "
				"handle wildcards in secure zones:\n",
				program);
			fprintf(stderr, "\t- wildcard nonexistence proof is "
				"not generated by the server\n");
			fprintf(stderr, "\t- wildcard nonexistence proof is "
				"not required by the resolver\n");
		}
		fprintf(stderr, "%s: warning: wildcard name seen: %s\n",
			program, namestr);
	}

	atorigin = dns_name_equal(name, dns_db_origin(db));

	/*
	 * If this is not the origin, determine if it's a delegation point.
	 */
	if (!atorigin) {
		dns_rdataset_t nsset;

		dns_rdataset_init(&nsset);
		result = dns_db_findrdataset(db, node, version,
					     dns_rdatatype_ns, 0, 0, &nsset,
					     NULL);
		/* Is this a delegation point? */
		if (result == ISC_R_SUCCESS) {
			isdelegation = ISC_TRUE;
			dns_rdataset_disassociate(&nsset);
		}
	}

	/*
	 * If this is a delegation point, determine if we need to generate
	 * a null key.
	 */
	if (isdelegation) {
		dns_rdataset_t keyset;

		childkey = haschildkey(db, name);
		neednullkey = ISC_TRUE;

		dns_rdataset_init(&keyset);
		result = dns_db_findrdataset(db, node, version,
					     dns_rdatatype_key, 0, 0, &keyset,
					     NULL);
		if (result == ISC_R_SUCCESS && childkey) {
			char namestr[DNS_NAME_FORMATSIZE];
			dns_name_format(name, namestr, sizeof namestr);
			fatal("%s has both a signedkey file and KEY "
			      "records in the zone.  Aborting.", namestr);
		}
		else if (result == ISC_R_SUCCESS) {
			if (hasnullkey(&keyset))
				neednullkey = ISC_FALSE;
			dns_rdataset_disassociate(&keyset);
		} else if (childkey) {
			char namestr[DNS_NAME_FORMATSIZE];
			dns_name_format(name, namestr, sizeof namestr);
			vbprintf(2, "child key for %s found\n", namestr);
			neednullkey = ISC_FALSE;
		}

		if (neednullkey)
			createnullkey(db, version, name);
	}

	/*
	 * Now iterate through the rdatasets.
	 */
	dns_diff_init(mctx, &diff);
	dns_rdataset_init(&rdataset);
	rdsiter = NULL;
	result = dns_db_allrdatasets(db, node, version, 0, &rdsiter);
	check_result(result, "dns_db_allrdatasets()");
	result = dns_rdatasetiter_first(rdsiter);
	while (result == ISC_R_SUCCESS) {
		dns_rdatasetiter_current(rdsiter, &rdataset);

		/* If this is a SIG set, skip it. */
		if (rdataset.type == dns_rdatatype_sig)
			goto skip;

		/*
		 * If this is a KEY set at the apex, look for a signedkey file.
		 */
		if (atorigin && rdataset.type == dns_rdatatype_key) {
			importparentsig(db, &diff, name, &rdataset);
			goto skip;
		}

		/*
		 * If this name is a delegation point, skip all records
		 * except an NXT set a KEY set containing a null key.
		 */
		if (isdelegation) {
			if (!(rdataset.type == dns_rdatatype_nxt ||
			      (rdataset.type == dns_rdatatype_key &&
			       hasnullkey(&rdataset))))
				goto skip;
		}

		if (rdataset.type == dns_rdatatype_nxt) {
			nxt_setbit(&rdataset, dns_rdatatype_sig);
			if (neednullkey)
				nxt_setbit(&rdataset, dns_rdatatype_key);
		}

		signset(db, version, &diff, node, name, &rdataset);

 skip:
		dns_rdataset_disassociate(&rdataset);
		result = dns_rdatasetiter_next(rdsiter);
	}
	if (result != ISC_R_NOMORE) {
		char namestr[DNS_NAME_FORMATSIZE];
		dns_name_format(name, namestr, sizeof namestr);
		fatal("rdataset iteration for name '%s' failed: %s",
		      namestr, isc_result_totext(result));
	}
	dns_rdatasetiter_destroy(&rdsiter);

	result = dns_diff_apply(&diff, db, version);
	if (result != ISC_R_SUCCESS) {
		char namestr[DNS_NAME_FORMATSIZE];
		dns_name_format(name, namestr, sizeof namestr);
		fatal("failed to add SIGs at node %s", namestr);
	}
	dns_diff_clear(&diff);
}

static inline isc_boolean_t
active_node(dns_db_t *db, dns_dbversion_t *version, dns_dbnode_t *node) {
	dns_rdatasetiter_t *rdsiter;
	isc_boolean_t active = ISC_FALSE;
	isc_result_t result;
	dns_rdataset_t rdataset;

	dns_rdataset_init(&rdataset);
	rdsiter = NULL;
	result = dns_db_allrdatasets(db, node, version, 0, &rdsiter);
	check_result(result, "dns_db_allrdatasets()");
	result = dns_rdatasetiter_first(rdsiter);
	while (result == ISC_R_SUCCESS) {
		dns_rdatasetiter_current(rdsiter, &rdataset);
		if (rdataset.type != dns_rdatatype_nxt)
			active = ISC_TRUE;
		dns_rdataset_disassociate(&rdataset);
		if (!active)
			result = dns_rdatasetiter_next(rdsiter);
		else
			result = ISC_R_NOMORE;
	}
	if (result != ISC_R_NOMORE)
		fatal("rdataset iteration failed: %s",
		      isc_result_totext(result));
	dns_rdatasetiter_destroy(&rdsiter);

	if (!active) {
		/*
		 * Make sure there is no NXT record for this node.
		 */
		result = dns_db_deleterdataset(db, node, version,
					       dns_rdatatype_nxt, 0);
		if (result == DNS_R_UNCHANGED)
			result = ISC_R_SUCCESS;
		check_result(result, "dns_db_deleterdataset");
	}

	return (active);
}

static inline isc_result_t
next_active(dns_db_t *db, dns_dbversion_t *version, dns_dbiterator_t *dbiter,
	    dns_name_t *name, dns_dbnode_t **nodep)
{
	isc_result_t result;
	isc_boolean_t active;

	do {
		active = ISC_FALSE;
		result = dns_dbiterator_current(dbiter, nodep, name);
		if (result == ISC_R_SUCCESS) {
			active = active_node(db, version, *nodep);
			if (!active) {
				dns_db_detachnode(db, nodep);
				result = dns_dbiterator_next(dbiter);
			}
		}
	} while (result == ISC_R_SUCCESS && !active);

	return (result);
}

static inline isc_result_t
next_nonglue(dns_db_t *db, dns_dbversion_t *version, dns_dbiterator_t *dbiter,
	    dns_name_t *name, dns_dbnode_t **nodep, dns_name_t *origin,
	    dns_name_t *lastcut)
{
	isc_result_t result, dresult;

	do {
		result = next_active(db, version, dbiter, name, nodep);
		if (result == ISC_R_SUCCESS) {
			if (dns_name_issubdomain(name, origin) &&
			    (lastcut == NULL ||
			     !dns_name_issubdomain(name, lastcut)))
				return (ISC_R_SUCCESS);
			dresult = dns_master_dumpnodetostream(mctx, db,
							      version,
							      *nodep, name,
							      masterstyle, fp);
			check_result(dresult, "dns_master_dumpnodetostream");
			dns_db_detachnode(db, nodep);
			result = dns_dbiterator_next(dbiter);
		}
	} while (result == ISC_R_SUCCESS);
	return (result);
}

/*
 * Extracts the zone minimum TTL from the SOA.
 */
static dns_ttl_t
minimumttl(dns_db_t *db, dns_dbversion_t *version) {
	dns_rdataset_t soaset;
	dns_name_t *origin;
	dns_fixedname_t fname;
	dns_name_t *name;
	dns_rdata_t soarr = DNS_RDATA_INIT;
	dns_rdata_soa_t soa;
	isc_result_t result;
	dns_ttl_t ttl;

	origin = dns_db_origin(db);

	dns_fixedname_init(&fname);
	name = dns_fixedname_name(&fname);
	dns_rdataset_init(&soaset);
	result = dns_db_find(db, origin, version, dns_rdatatype_soa,
			     0, 0, NULL, name, &soaset, NULL);
	if (result != ISC_R_SUCCESS) {
		char namestr[DNS_NAME_FORMATSIZE];
		dns_name_format(name, namestr, sizeof namestr);
		fatal("failed to find '%s SOA' in the zone: %s",
		      namestr, isc_result_totext(result));
	}
	result = dns_rdataset_first(&soaset);
	check_result(result, "dns_rdataset_first()");
	dns_rdataset_current(&soaset, &soarr);
	result = dns_rdata_tostruct(&soarr, &soa, NULL);
	check_result(result, "dns_rdataset_tostruct()");
	ttl = soa.minimum;
	dns_rdata_freestruct(&soa);
	dns_rdataset_disassociate(&soaset);

	return (ttl);
}

static void
cleannode(dns_db_t *db, dns_dbversion_t *version, dns_dbnode_t *node) {
	dns_rdatasetiter_t *rdsiter = NULL;
	dns_rdataset_t set;
	isc_result_t result, dresult;

	dns_rdataset_init(&set);
	result = dns_db_allrdatasets(db, node, version, 0, &rdsiter);
	check_result(result, "dns_db_allrdatasets");
	result = dns_rdatasetiter_first(rdsiter);
	while (result == ISC_R_SUCCESS) {
		isc_boolean_t destroy = ISC_FALSE;
		dns_rdatatype_t covers = 0;
		dns_rdatasetiter_current(rdsiter, &set);
		if (set.type == dns_rdatatype_sig) {
			covers = set.covers;
			destroy = ISC_TRUE;
		}
		dns_rdataset_disassociate(&set);
		result = dns_rdatasetiter_next(rdsiter);
		if (destroy) {
			dresult = dns_db_deleterdataset(db, node, version,
							dns_rdatatype_sig,
							covers);
			check_result(dresult, "dns_db_allrdatasets");
		}
	}
	if (result != ISC_R_NOMORE)
		fatal("rdataset iteration failed: %s",
		      isc_result_totext(result));
	dns_rdatasetiter_destroy(&rdsiter);
}

/*
 * Generates NXTs and SIGs for each non-glue name in the zone.
 */
static void
signzone(dns_db_t *db, dns_dbversion_t *version) {
	isc_result_t result, nxtresult, dresult;
	dns_dbnode_t *node, *nextnode;
	dns_fixedname_t fname, fnextname;
	dns_name_t *name, *nextname, *target, *lastcut;
	dns_dbiterator_t *dbiter;
	dns_name_t *origin;

	zonettl = minimumttl(db, version);

	dns_fixedname_init(&fname);
	name = dns_fixedname_name(&fname);
	dns_fixedname_init(&fnextname);
	nextname = dns_fixedname_name(&fnextname);

	origin = dns_db_origin(db);

	lastcut = NULL;
	dbiter = NULL;
	result = dns_db_createiterator(db, ISC_FALSE, &dbiter);
	check_result(result, "dns_db_createiterator()");
	result = dns_dbiterator_first(dbiter);
	node = NULL;
	dns_name_concatenate(origin, NULL, name, NULL);
	result = next_nonglue(db, version, dbiter, name, &node, origin,
			      lastcut);
	while (result == ISC_R_SUCCESS) {
		nextnode = NULL;
		if (!dns_name_equal(name, dns_db_origin(db))) {
			dns_rdatasetiter_t *rdsiter = NULL;
			dns_rdataset_t set;

			dns_rdataset_init(&set);
			result = dns_db_allrdatasets(db, node, version,
						     0, &rdsiter);
			check_result(result, "dns_db_allrdatasets");
			result = dns_rdatasetiter_first(rdsiter);
			while (result == ISC_R_SUCCESS) {
				dns_rdatasetiter_current(rdsiter, &set);
				if (set.type == dns_rdatatype_ns) {
					dns_rdataset_disassociate(&set);
					break;
				}
				dns_rdataset_disassociate(&set);
				result = dns_rdatasetiter_next(rdsiter);
			}
			if (result != ISC_R_SUCCESS && result != ISC_R_NOMORE)
				fatal("rdataset iteration failed: %s",
				      isc_result_totext(result));
			if (result == ISC_R_SUCCESS) {
				if (lastcut != NULL)
					dns_name_free(lastcut, mctx);
				else {
					lastcut = isc_mem_get(mctx,
							sizeof(dns_name_t));
					if (lastcut == NULL)
						fatal("out of memory");
				}
				dns_name_init(lastcut, NULL);
				result = dns_name_dup(name, mctx, lastcut);
				check_result(result, "dns_name_dup()");
			}
			dns_rdatasetiter_destroy(&rdsiter);
		}
		result = dns_dbiterator_next(dbiter);
		if (result == ISC_R_SUCCESS)
			result = next_nonglue(db, version, dbiter, nextname,
					      &nextnode, origin, lastcut);
		if (result == ISC_R_SUCCESS)
			target = nextname;
		else if (result == ISC_R_NOMORE)
			target = origin;
		else {
			target = NULL;	/* Make compiler happy. */
			fatal("iterating through the database failed: %s",
			      isc_result_totext(result));
		}
		nxtresult = dns_nxt_build(db, version, node, target, zonettl);
		check_result(nxtresult, "dns_nxt_build()");
		signname(db, version, node, name);
		dresult = dns_master_dumpnodetostream(mctx, db, version,
						      node, name,
						      masterstyle, fp);
		check_result(dresult, "dns_master_dumpnodetostream");
		cleannode(db, version, node);
		dns_db_detachnode(db, &node);
		node = nextnode;
		dns_name_concatenate(nextname, NULL, name, NULL);
	}
	if (result != ISC_R_NOMORE)
		fatal("iterating through the database failed: %s",
		      isc_result_totext(result));
	if (lastcut != NULL) {
		dns_name_free(lastcut, mctx);
		isc_mem_put(mctx, lastcut, sizeof(dns_name_t));
	}
	dns_dbiterator_destroy(&dbiter);
}

/*
 * Load the zone file from disk
 */
static void
loadzone(char *file, char *origin, dns_rdataclass_t rdclass, dns_db_t **db) {
	isc_buffer_t b, b2;
	unsigned char namedata[1024];
	int len;
	dns_name_t name;
	isc_result_t result;

	len = strlen(origin);
	isc_buffer_init(&b, origin, len);
	isc_buffer_add(&b, len);

	isc_buffer_init(&b2, namedata, sizeof(namedata));

	dns_name_init(&name, NULL);
	result = dns_name_fromtext(&name, &b, dns_rootname, ISC_FALSE, &b2);
	if (result != ISC_R_SUCCESS)
		fatal("failed converting name '%s' to dns format: %s",
		      origin, isc_result_totext(result));

	result = dns_db_create(mctx, "rbt", &name, dns_dbtype_zone,
			       rdclass, 0, NULL, db);
	check_result(result, "dns_db_create()");

	result = dns_db_load(*db, file);
	if (result != ISC_R_SUCCESS)
		fatal("failed loading zone from '%s': %s",
		      file, isc_result_totext(result));
}

/*
 * Finds all public zone keys in the zone, and attempts to load the
 * private keys from disk.
 */
static void
loadzonekeys(dns_db_t *db) {
	dns_name_t *origin;
	dns_dbnode_t *node;
	dns_dbversion_t *currentversion;
	isc_result_t result;
	dst_key_t *keys[20];
	unsigned int nkeys, i;

	origin = dns_db_origin(db);
	currentversion = NULL;
	dns_db_currentversion(db, &currentversion);

	node = NULL;
	result = dns_db_findnode(db, origin, ISC_FALSE, &node);
	if (result != ISC_R_SUCCESS)
		fatal("failed to find the zone's origin: %s",
		      isc_result_totext(result));

	result = dns_dnssec_findzonekeys(db, currentversion, node, origin, mctx,
					 20, keys, &nkeys);
	if (result == ISC_R_NOTFOUND)
		result = ISC_R_SUCCESS;
	if (result != ISC_R_SUCCESS)
		fatal("failed to find the zone keys: %s",
		      isc_result_totext(result));

	for (i = 0; i < nkeys; i++) {
		signer_key_t *key;

		key = newkeystruct(keys[i], ISC_FALSE);
		ISC_LIST_APPEND(keylist, key, link);
	}
	dns_db_detachnode(db, &node);
	dns_db_closeversion(db, &currentversion, ISC_FALSE);
}

/*
 * Finds all public zone keys in the zone.
 */
static void
loadzonepubkeys(dns_db_t *db) {
	dns_name_t *origin;
	dns_dbversion_t *currentversion = NULL;
	dns_dbnode_t *node = NULL;
	dns_rdataset_t rdataset;
	dns_rdata_t rdata;
	dst_key_t *pubkey;
	signer_key_t *key;
	isc_result_t result;

	origin = dns_db_origin(db);
	dns_db_currentversion(db, &currentversion);

	result = dns_db_findnode(db, origin, ISC_FALSE, &node);
	if (result != ISC_R_SUCCESS)
		fatal("failed to find the zone's origin: %s",
		      isc_result_totext(result));

	dns_rdataset_init(&rdataset);
	result = dns_db_findrdataset(db, node, currentversion,
				     dns_rdatatype_key, 0, 0, &rdataset, NULL);
	if (result != ISC_R_SUCCESS)
		fatal("failed to find keys at the zone apex: %s",
		      isc_result_totext(result));
	result = dns_rdataset_first(&rdataset);
	check_result(result, "dns_rdataset_first");
	while (result == ISC_R_SUCCESS) {
		pubkey = NULL;
		dns_rdata_init(&rdata);
		dns_rdataset_current(&rdataset, &rdata);
		result = dns_dnssec_keyfromrdata(origin, &rdata, mctx,
						 &pubkey);
		if (result != ISC_R_SUCCESS)
			goto next;
		if (!dst_key_iszonekey(pubkey)) {
			dst_key_free(&pubkey);
			goto next;
		}

		key = newkeystruct(pubkey, ISC_FALSE);
		ISC_LIST_APPEND(keylist, key, link);
 next:
		result = dns_rdataset_next(&rdataset);
	}
	dns_rdataset_disassociate(&rdataset);
	dns_db_detachnode(db, &node);
	dns_db_closeversion(db, &currentversion, ISC_FALSE);
}

static void
usage(void) {
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s [options] zonefile [keys]\n", program);

	fprintf(stderr, "\n");

	fprintf(stderr, "Options: (default value in parenthesis) \n");
	fprintf(stderr, "\t-c class (IN)\n");
	fprintf(stderr, "\t-s YYYYMMDDHHMMSS|+offset:\n");
	fprintf(stderr, "\t\tSIG start time - absolute|offset (now)\n");
	fprintf(stderr, "\t-e YYYYMMDDHHMMSS|+offset|\"now\"+offset]:\n");
	fprintf(stderr, "\t\tSIG end time  - absolute|from start|from now "
				"(now + 30 days)\n");
	fprintf(stderr, "\t-i interval:\n");
	fprintf(stderr, "\t\tcycle interval - resign "
				"if < interval from end ( (end-start)/4 )\n");
	fprintf(stderr, "\t-v debuglevel (0)\n");
	fprintf(stderr, "\t-o origin:\n");
	fprintf(stderr, "\t\tzone origin (name of zonefile)\n");
	fprintf(stderr, "\t-f outfile:\n");
	fprintf(stderr, "\t\tfile the signed zone is written in "
				"(zonefile + .signed)\n");
	fprintf(stderr, "\t-r randomdev:\n");
	fprintf(stderr,	"\t\ta file containing random data\n");
	fprintf(stderr, "\t-a:\t");
	fprintf(stderr, "verify generated signatures\n");
	fprintf(stderr, "\t-p:\t");
	fprintf(stderr, "use pseudorandom data (faster but less secure)\n");
	fprintf(stderr, "\t-t:\t");
	fprintf(stderr, "print statistics\n");

	fprintf(stderr, "\n");

	fprintf(stderr, "Signing Keys: ");
	fprintf(stderr, "(default: all zone keys that have private keys)\n");
	fprintf(stderr, "\tkeyfile (Kname+alg+tag)\n");
	exit(0);
}

int
main(int argc, char *argv[]) {
	int i, ch;
	char *startstr = NULL, *endstr = NULL, *classname = NULL;
	char *origin = NULL, *file = NULL, *output = NULL;
	char *randomfile = NULL;
	char *endp;
	dns_db_t *db;
	dns_dbversion_t *version;
	signer_key_t *key;
	isc_result_t result;
	isc_log_t *log = NULL;
	isc_boolean_t pseudorandom = ISC_FALSE;
	isc_boolean_t printstats = ISC_FALSE;
	unsigned int eflags;
	isc_boolean_t free_output = ISC_FALSE;
	dns_rdataclass_t rdclass;
	isc_textregion_t r;

	result = isc_mem_create(0, 0, &mctx);
	if (result != ISC_R_SUCCESS)
		fatal("out of memory");

	dns_result_register();

	while ((ch = isc_commandline_parse(argc, argv, "c:s:e:i:v:o:f:ahpr:t"))
	       != -1) {
		switch (ch) {
		case 'c':
			classname = isc_commandline_argument;
			break;

		case 's':
			startstr = isc_commandline_argument;
			break;

		case 'e':
			endstr = isc_commandline_argument;
			break;

		case 'i':
			endp = NULL;
			cycle = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0' || cycle < 0)
				fatal("cycle period must be numeric and "
				      "positive");
			break;

		case 'p':
			pseudorandom = ISC_TRUE;
			break;

		case 'r':
			randomfile = isc_commandline_argument;
			break;

		case 'v':
			endp = NULL;
			verbose = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0')
				fatal("verbose level must be numeric");
			break;

		case 'o':
			origin = isc_commandline_argument;
			break;

		case 'f':
			output = isc_commandline_argument;
			break;

		case 'a':
			tryverify = ISC_TRUE;
			break;

		case 't':
			printstats = ISC_TRUE;
			break;

		case 'h':
		default:
			usage();

		}
	}

	setup_entropy(mctx, randomfile, &ectx);
	eflags = ISC_ENTROPY_BLOCKING;
	if (!pseudorandom)
		eflags |= ISC_ENTROPY_GOODONLY;
	result = dst_lib_init(mctx, ectx, eflags);
	if (result != ISC_R_SUCCESS)
		fatal("could not initialize dst");

	isc_stdtime_get(&now);

	if (startstr != NULL)
		starttime = strtotime(startstr, now, now);
	else
		starttime = now;

	if (endstr != NULL)
		endtime = strtotime(endstr, now, starttime);
	else
		endtime = starttime + (30 * 24 * 60 * 60);

	if (cycle == -1)
		cycle = (endtime - starttime) / 4;


	if (classname != NULL) {
		r.base = classname;
		r.length = strlen(classname);
		result = dns_rdataclass_fromtext(&rdclass, &r);
		if (result != ISC_R_SUCCESS)
			fatal("unknown class %s",classname);
	} else
		rdclass = dns_rdataclass_in;

	setup_logging(verbose, mctx, &log);

	argc -= isc_commandline_index;
	argv += isc_commandline_index;

	if (argc < 1)
		usage();

	file = argv[0];

	argc -= 1;
	argv += 1;

	if (output == NULL) {
		free_output = ISC_TRUE;
		output = isc_mem_allocate(mctx,
					  strlen(file) + strlen(".signed") + 1);
		if (output == NULL)
			fatal("out of memory");
		sprintf(output, "%s.signed", file);
	}

	if (origin == NULL)
		origin = file;

	db = NULL;
	loadzone(file, origin, rdclass, &db);

	ISC_LIST_INIT(keylist);

	if (argc == 0) {
		signer_key_t *key;

		loadzonekeys(db);

		key = ISC_LIST_HEAD(keylist);
		while (key != NULL) {
			key->isdefault = ISC_TRUE;
			key = ISC_LIST_NEXT(key, link);
		}
	} else {
		for (i = 0; i < argc; i++) {
			dst_key_t *newkey = NULL;

			result = dst_key_fromnamedfile(argv[i],
						       DST_TYPE_PRIVATE,
						       mctx, &newkey);
			if (result != ISC_R_SUCCESS)
				usage();

			key = ISC_LIST_HEAD(keylist);
			while (key != NULL) {
				dst_key_t *dkey = key->key;
				if (dst_key_id(dkey) == dst_key_id(newkey) &&
				    dst_key_alg(dkey) == dst_key_alg(newkey) &&
				    dns_name_equal(dst_key_name(dkey),
					    	   dst_key_name(newkey)))
				{
					key->isdefault = ISC_TRUE;
					if (!dst_key_isprivate(dkey))
						fatal("cannot sign zone with "
						      "non-private key %s",
						      argv[i]);
					break;
				}
				key = ISC_LIST_NEXT(key, link);
			}
			if (key == NULL) {
				key = newkeystruct(newkey, ISC_TRUE);
				ISC_LIST_APPEND(keylist, key, link);
			} else
				dst_key_free(&newkey);
		}

		loadzonepubkeys(db);
	}

	if (ISC_LIST_EMPTY(keylist))
		fprintf(stderr, "%s: warning: No keys specified or found\n",
			program);

	version = NULL;
	result = dns_db_newversion(db, &version);
	check_result(result, "dns_db_newversion()");

	fp = NULL;
	result = isc_stdio_open(output, "w", &fp);
	if (result != ISC_R_SUCCESS)
		fatal("failed to open output file %s: %s", output,
		      isc_result_totext(result));

	signzone(db, version);

	result = isc_stdio_close(fp);
	check_result(result, "isc_stdio_close");

	dns_db_closeversion(db, &version, ISC_FALSE);

	dns_db_detach(&db);

	while (!ISC_LIST_EMPTY(keylist)) {
		key = ISC_LIST_HEAD(keylist);
		ISC_LIST_UNLINK(keylist, key, link);
		dst_key_free(&key->key);
		isc_mem_put(mctx, key, sizeof(signer_key_t));
	}

	if (free_output)
		isc_mem_free(mctx, output);

	cleanup_logging(&log);
	dst_lib_destroy();
	cleanup_entropy(&ectx);
	if (verbose > 10)
		isc_mem_stats(mctx, stdout);
	isc_mem_destroy(&mctx);

	if (printstats) {
		printf("Number of signatures generated:  %d\n", nsigned);
		printf("Number of signatures retained:   %d\n", nretained);
		printf("Number of signatures dropped:    %d\n", ndropped);
		printf("Number of signatures verified:   %d\n", nverified);
		printf("Number of signatures unverified: %d\n", nverifyfailed);
	}

	return (0);
}
