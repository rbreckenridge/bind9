/*
 * Copyright (C) 1999, 2000  Internet Software Consortium.
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

/* $Id: afsdb_18.c,v 1.24 2000/05/04 22:19:06 gson Exp $ */

/* Reviewed: Wed Mar 15 14:59:00 PST 2000 by explorer */

/* RFC 1183 */

#ifndef RDATA_GENERIC_AFSDB_18_C
#define RDATA_GENERIC_AFSDB_18_C

#define RRTYPE_AFSDB_ATTRIBUTES (0)

static inline isc_result_t
fromtext_afsdb(dns_rdataclass_t rdclass, dns_rdatatype_t type,
	   isc_lex_t *lexer, dns_name_t *origin,
	   isc_boolean_t downcase, isc_buffer_t *target) 
{
	isc_token_t token;
	isc_buffer_t buffer;
	dns_name_t name;

	UNUSED(rdclass);

	REQUIRE(type == 18);

	/* subtype */
	RETERR(gettoken(lexer, &token, isc_tokentype_number, ISC_FALSE));
	RETERR(uint16_tobuffer(token.value.as_ulong, target));
	
	/* hostname */
	RETERR(gettoken(lexer, &token, isc_tokentype_string, ISC_FALSE));
	dns_name_init(&name, NULL);
	buffer_fromregion(&buffer, &token.value.as_region);
	origin = (origin != NULL) ? origin : dns_rootname;
	return (dns_name_fromtext(&name, &buffer, origin, downcase, target));
}

static inline isc_result_t
totext_afsdb(dns_rdata_t *rdata, dns_rdata_textctx_t *tctx, 
	     isc_buffer_t *target) 
{
	dns_name_t name;
	dns_name_t prefix;
	isc_region_t region;
	char buf[sizeof "64000 "];
	isc_boolean_t sub;
	unsigned int num;

	REQUIRE(rdata->type == 18);

	dns_name_init(&name, NULL);
	dns_name_init(&prefix, NULL);

	dns_rdata_toregion(rdata, &region);
	num = uint16_fromregion(&region);
	isc_region_consume(&region, 2);
	sprintf(buf, "%u ", num);
	RETERR(str_totext(buf, target));
	dns_name_fromregion(&name, &region);
	sub = name_prefix(&name, tctx->origin, &prefix);
	return (dns_name_totext(&prefix, sub, target));
}

static inline isc_result_t
fromwire_afsdb(dns_rdataclass_t rdclass, dns_rdatatype_t type,
	       isc_buffer_t *source, dns_decompress_t *dctx,
	       isc_boolean_t downcase, isc_buffer_t *target)
{
	dns_name_t name;
	isc_region_t sr;
	isc_region_t tr;

	UNUSED(rdclass);

	REQUIRE(type == 18);
	
	dns_decompress_setmethods(dctx, DNS_COMPRESS_NONE);

	dns_name_init(&name, NULL);

	isc_buffer_activeregion(source, &sr);
	isc_buffer_availableregion(target, &tr);
	if (tr.length < 2)
		return (ISC_R_NOSPACE);
	if (sr.length < 2)
		return (ISC_R_UNEXPECTEDEND);
	memcpy(tr.base, sr.base, 2);
	isc_buffer_forward(source, 2);
	isc_buffer_add(target, 2);
	return (dns_name_fromwire(&name, source, dctx, downcase, target));
}

static inline isc_result_t
towire_afsdb(dns_rdata_t *rdata, dns_compress_t *cctx, isc_buffer_t *target)
{
	isc_region_t tr;
	isc_region_t sr;
	dns_name_t name;

	REQUIRE(rdata->type == 18);

	dns_compress_setmethods(cctx, DNS_COMPRESS_NONE);
	isc_buffer_availableregion(target, &tr);
	dns_rdata_toregion(rdata, &sr);
	if (tr.length < 2)
		return (ISC_R_NOSPACE);
	memcpy(tr.base, sr.base, 2);
	isc_region_consume(&sr, 2);
	isc_buffer_add(target, 2);

	dns_name_init(&name, NULL);
	dns_name_fromregion(&name, &sr);

	return (dns_name_towire(&name, cctx, target));
}

static inline int
compare_afsdb(dns_rdata_t *rdata1, dns_rdata_t *rdata2)
{
	int result;
	dns_name_t name1;
	dns_name_t name2;
	isc_region_t region1;
	isc_region_t region2;

	REQUIRE(rdata1->type == rdata2->type);
	REQUIRE(rdata1->rdclass == rdata2->rdclass);
	REQUIRE(rdata1->type == 18);

	result = memcmp(rdata1->data, rdata2->data, 2);
	if (result != 0)
		return (result < 0 ? -1 : 1);

	dns_name_init(&name1, NULL);
	dns_name_init(&name2, NULL);

	dns_rdata_toregion(rdata1, &region1);
	dns_rdata_toregion(rdata2, &region2);

	isc_region_consume(&region1, 2);
	isc_region_consume(&region2, 2);

	dns_name_fromregion(&name1, &region1);
	dns_name_fromregion(&name2, &region2);

	return (dns_name_rdatacompare(&name1, &name2));
}

static inline isc_result_t
fromstruct_afsdb(dns_rdataclass_t rdclass, dns_rdatatype_t type, void *source,
		 isc_buffer_t *target)
{
	UNUSED(rdclass);
	UNUSED(source);
	UNUSED(target);

	REQUIRE(type == 18);
	
	return (ISC_R_NOTIMPLEMENTED);
}

static inline isc_result_t
tostruct_afsdb(dns_rdata_t *rdata, void *target, isc_mem_t *mctx)
{
	REQUIRE(rdata->type == 18);
	REQUIRE(target != NULL);

	UNUSED(rdata);
	UNUSED(target);
	UNUSED(mctx);

	return (ISC_R_NOTIMPLEMENTED);
}

static inline void
freestruct_afsdb(void *source)
{
	dns_rdata_afsdb_t *afsdb = source;

	REQUIRE(source != NULL);
	REQUIRE(afsdb->common.rdtype == 18);
	REQUIRE(ISC_FALSE);

	UNUSED(source);
	UNUSED(afsdb);
}

static inline isc_result_t
additionaldata_afsdb(dns_rdata_t *rdata, dns_additionaldatafunc_t add,
		     void *arg)
{
	dns_name_t name;
	isc_region_t region;

	REQUIRE(rdata->type == 18);

	dns_name_init(&name, NULL);
	dns_rdata_toregion(rdata, &region);
	isc_region_consume(&region, 2);
	dns_name_fromregion(&name, &region);

	return ((add)(arg, &name, dns_rdatatype_a));
}

static inline isc_result_t
digest_afsdb(dns_rdata_t *rdata, dns_digestfunc_t digest, void *arg)
{
	isc_region_t r1, r2;
	dns_name_t name;

	REQUIRE(rdata->type == 18);

	dns_rdata_toregion(rdata, &r1);
	r2 = r1;
	isc_region_consume(&r2, 2);
	r1.length = 2;
	RETERR((digest)(arg, &r1));
	dns_name_init(&name, NULL);
	dns_name_fromregion(&name, &r2);

	return (dns_name_digest(&name, digest, arg));
}

#endif	/* RDATA_GENERIC_AFSDB_18_C */
