/*
 * Copyright (C) 1998, 1999 Internet Software Consortium.
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

 /* $Id: rdata.c,v 1.39 1999/05/03 03:07:15 marka Exp $ */

#include <config.h>

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <isc/buffer.h>
#include <isc/lex.h>
#include <isc/assertions.h>
#include <isc/error.h>
#include <isc/region.h>

#include <dns/types.h>
#include <dns/result.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdatatype.h>
#include <dns/rcode.h>
#include <dns/cert.h>
#include <dns/secalg.h>
#include <dns/fixedname.h>
#include <dns/rdatastruct.h>

#define RETERR(x) do { \
	dns_result_t __r = (x); \
	if (__r != DNS_R_SUCCESS) \
		return (__r); \
	} while (0)

static dns_result_t	txt_totext(isc_region_t *source, isc_buffer_t *target);
static dns_result_t	txt_fromtext(isc_textregion_t *source,
				     isc_buffer_t *target);
static dns_result_t	txt_fromwire(isc_buffer_t *source,
				    isc_buffer_t *target);
static isc_boolean_t	name_prefix(dns_name_t *name, dns_name_t *origin,
				    dns_name_t *target);
static unsigned int 	name_length(dns_name_t *name);
static dns_result_t	str_totext(char *source, isc_buffer_t *target);
static isc_boolean_t	buffer_empty(isc_buffer_t *source);
static void		buffer_fromregion(isc_buffer_t *buffer,
					  isc_region_t *region,
					  unsigned int type);
static dns_result_t	uint32_tobuffer(isc_uint32_t,
					isc_buffer_t *target);
static dns_result_t	uint16_tobuffer(isc_uint32_t,
					isc_buffer_t *target);
static isc_uint32_t	uint32_fromregion(isc_region_t *region);
static isc_uint16_t	uint16_fromregion(isc_region_t *region);
static dns_result_t	gettoken(isc_lex_t *lexer, isc_token_t *token,
				 isc_tokentype_t expect, isc_boolean_t eol);
static dns_result_t	mem_tobuffer(isc_buffer_t *target, void *base,
				     unsigned int length);
static int		compare_region(isc_region_t *r1, isc_region_t *r2);
static int		hexvalue(char value);
static int		decvalue(char value);
static dns_result_t	base64_totext(isc_region_t *source,
				      isc_buffer_t *target);
static dns_result_t	base64_tobuffer(isc_lex_t *lexer,
					isc_buffer_t *target,
					int length);
static dns_result_t	time_totext(unsigned long value,
				    isc_buffer_t *target);
static dns_result_t	time_tobuffer(char *source, isc_buffer_t *target);
static dns_result_t	btoa_totext(unsigned char *inbuf, int inbuflen,
				    isc_buffer_t *target);
static dns_result_t	atob_tobuffer(isc_lex_t *lexer, isc_buffer_t *target);
static void		default_fromtext_callback(
					    dns_rdatacallbacks_t *callbacks,
						  char *, ...);

static void		fromtext_error(void (*callback)(dns_rdatacallbacks_t *,
							char *, ...),
				       dns_rdatacallbacks_t *callbacks,
				       char *name, int line,
				       isc_token_t *token,
				       dns_result_t result);

static const char hexdigits[] = "0123456789abcdef";
static const char decdigits[] = "0123456789";
static const char octdigits[] = "01234567";

#include "code.h"

#define META 0x0001
#define RESERVED 0x0002

#define METATYPES \
	{ 0, "NONE", META }, \
	{ 31, "EID", RESERVED }, \
	{ 32, "NIMLOC", RESERVED }, \
	{ 34, "ATMA", RESERVED }, \
	{ 100, "UINFO", RESERVED }, \
	{ 101, "UID", RESERVED }, \
	{ 102, "GID", RESERVED }, \
	{ 249, "TKEY", META }, \
	{ 250, "TSIG", META }, \
	{ 251, "IXFR", META }, \
	{ 252, "AXFR", META }, \
	{ 253, "MAILB", META }, \
	{ 254, "MAILA", META }, \
	{ 255, "ANY", META },

#define METACLASSES \
	{ 0, "NONE", META }, \
	{ 255, "ANY", META },

#define RCODENAMES \
	/* standard rcodes */ \
	{ dns_rcode_noerror, "NOERROR", 0}, \
	{ dns_rcode_formerr, "FORMERR", 0}, \
	{ dns_rcode_servfail, "SERVFAIL", 0}, \
	{ dns_rcode_nxdomain, "NXDOMAIN", 0}, \
	{ dns_rcode_notimp, "NOTIMP", 0}, \
	{ dns_rcode_refused, "REFUSED", 0}, \
	{ dns_rcode_yxdomain, "YXDOMAIN", 0}, \
	{ dns_rcode_yxrrset, "YXRRSET", 0}, \
	{ dns_rcode_nxrrset, "NXRRSET", 0}, \
	{ dns_rcode_notauth, "NOTAUTH", 0}, \
	{ dns_rcode_notzone, "NOTZONE", 0}, \
	/* extended rcodes */ \
	{ dns_rcode_badsig, "BADSIG", 0}, \
	{ dns_rcode_badkey, "BADKEY", 0}, \
	{ dns_rcode_badtime, "BADTIME", 0}, \
	{ dns_rcode_badmode, "BADMODE", 0}, \
	{ 0, NULL, 0 }

#define CERTNAMES \
	{ 1, "SKIX", 0}, \
	{ 2, "SPKI", 0}, \
	{ 3, "PGP", 0}, \
	{ 253, "URI", 0}, \
	{ 254, "OID", 0}, \
	{ 0, NULL, 0}

/* draft-ietf-dnssec-secext2-07.txt section 7 */

#define SECALGNAMES \
	{ 1, "RSAMD5", 0 }, \
	{ 2, "DH", 0 }, \
	{ 3, "DSA", 0 }, \
	{ 4, "ECC", 0 }, \
	{ 252, "INDIRECT", 0 }, \
	{ 253, "PRIVATEDNS", 0 }, \
	{ 254, "PRIVATEOID", 0 }, \
	{ 0, NULL, 0}


static struct tbl {
	unsigned int	value;
	char	*name;
	int	flags;
} types[] = { TYPENAMES METATYPES {0, NULL, 0} },
classes[] = { CLASSNAMES METACLASSES { 0, NULL, 0} },
rcodes[] = { RCODENAMES },
certs[] = { CERTNAMES },
secalgs[] = { SECALGNAMES };

/***
 *** Initialization
 ***/

void
dns_rdata_init(dns_rdata_t *rdata) {

	REQUIRE(rdata != NULL);

	rdata->data = NULL;
	rdata->length = 0;
	rdata->class = 0;
	rdata->type = 0;
	ISC_LINK_INIT(rdata, link);
	/* ISC_LIST_INIT(rdata->list); */
}

/***
 *** Comparisons
 ***/

int
dns_rdata_compare(dns_rdata_t *rdata1, dns_rdata_t *rdata2) {
	int result = 0;
	isc_boolean_t use_default = ISC_FALSE;

	REQUIRE(rdata1 != NULL);
	REQUIRE(rdata2 != NULL);
	REQUIRE(rdata1->data != NULL);
	REQUIRE(rdata2->data != NULL);

	if (rdata1->class != rdata2->class)
		return (rdata1->class < rdata2->class ? -1 : 1);

	if (rdata1->type != rdata2->type)
		return (rdata1->type < rdata2->type ? -1 : 1);

	COMPARESWITCH

	if (use_default) {
		isc_region_t r1;
		isc_region_t r2;

		dns_rdata_toregion(rdata1, &r1);
		dns_rdata_toregion(rdata2, &r2);
		result = compare_region(&r1, &r2);
	}
	return (result);
}

/***
 *** Conversions
 ***/

void
dns_rdata_fromregion(dns_rdata_t *rdata, dns_rdataclass_t class,
		     dns_rdatatype_t type, isc_region_t *r)
{
			  
	REQUIRE(rdata != NULL);
	REQUIRE(r != NULL);

	rdata->data = r->base;
	rdata->length = r->length;
	rdata->class = class;
	rdata->type = type;
}

void
dns_rdata_toregion(dns_rdata_t *rdata, isc_region_t *r) {

	REQUIRE(rdata != NULL);
	REQUIRE(r != NULL);

	r->base = rdata->data;
	r->length = rdata->length;
}

dns_result_t
dns_rdata_fromwire(dns_rdata_t *rdata, dns_rdataclass_t class,
		   dns_rdatatype_t type, isc_buffer_t *source,
		   dns_decompress_t *dctx, isc_boolean_t downcase,
		   isc_buffer_t *target)
{
	dns_result_t result = DNS_R_NOTIMPLEMENTED;
	isc_region_t region;
	isc_buffer_t ss;
	isc_buffer_t st;
	isc_boolean_t use_default = ISC_FALSE;

	REQUIRE(isc_buffer_type(source) == ISC_BUFFERTYPE_BINARY);
	REQUIRE(isc_buffer_type(target) == ISC_BUFFERTYPE_BINARY);
	REQUIRE(dctx != NULL);

	ss = *source;
	st = *target;
	/* XXX */
	region.base = (unsigned char *)(target->base) + target->used;

	FROMWIRESWITCH

	if (use_default)
		(void)NULL;

	/* We should have consumed all out buffer */
	if (result == DNS_R_SUCCESS && !buffer_empty(source))
		result = DNS_R_EXTRADATA;

	if (rdata && result == DNS_R_SUCCESS) {
		region.length = target->used - st.used;
		dns_rdata_fromregion(rdata, class, type, &region);
	}

	if (result != DNS_R_SUCCESS) {
		*source = ss;
		*target = st;
	}
	return (result);
}

dns_result_t
dns_rdata_towire(dns_rdata_t *rdata, dns_compress_t *cctx,
	         isc_buffer_t *target)
{
	dns_result_t result = DNS_R_NOTIMPLEMENTED;
	isc_boolean_t use_default = ISC_FALSE;
	isc_region_t tr;
	isc_buffer_t st;

	REQUIRE(rdata != NULL);
	REQUIRE(isc_buffer_type(target) == ISC_BUFFERTYPE_BINARY);
	st = *target;

	TOWIRESWITCH
	
	if (use_default) {
		isc_buffer_available(target, &tr);
		if (tr.length < rdata->length) 
			return (DNS_R_NOSPACE);
		memcpy(tr.base, rdata->data, rdata->length);
		isc_buffer_add(target, rdata->length);
		return (DNS_R_SUCCESS);
	}
	if (result != DNS_R_SUCCESS) {
		*target = st;
		dns_compress_rollback(cctx, target->used);
	}
	return (result);
}

dns_result_t
dns_rdata_fromtext(dns_rdata_t *rdata, dns_rdataclass_t class,
		   dns_rdatatype_t type, isc_lex_t *lexer,
		   dns_name_t *origin, isc_boolean_t downcase,
		   isc_buffer_t *target, dns_rdatacallbacks_t *callbacks)
{
	dns_result_t result = DNS_R_NOTIMPLEMENTED;
	isc_region_t region;
	isc_buffer_t st;
	isc_boolean_t use_default = ISC_FALSE;
	isc_token_t token;
	unsigned int options = ISC_LEXOPT_EOL | ISC_LEXOPT_EOF |
			       ISC_LEXOPT_DNSMULTILINE;
	char *name;
	int line;
	void (*callback)(dns_rdatacallbacks_t *, char *, ...);
	isc_result_t iresult;

	if (origin != NULL)
		REQUIRE(dns_name_isabsolute(origin) == ISC_TRUE);
	REQUIRE(isc_buffer_type(target) == ISC_BUFFERTYPE_BINARY);

	st = *target;
	region.base = (unsigned char *)(target->base) + target->used;

	FROMTEXTSWITCH

	if (use_default)
		(void)NULL;

	if (callbacks == NULL)
		callback = NULL;
	else
		callback = callbacks->error;

	if (callback == NULL)
		callback = default_fromtext_callback;
	/*
	 * Consume to end of line / file.
	 * If not at end of line initially set error code.
	 * Call callback via fromtext_error once if there was an error.
	 */
	do {
		name = isc_lex_getsourcename(lexer);
		line = isc_lex_getsourceline(lexer);
		iresult = isc_lex_gettoken(lexer, options, &token);
		if (iresult != ISC_R_SUCCESS) {
			if (result == DNS_R_SUCCESS) {
				switch (iresult) {
				case ISC_R_NOMEMORY:
					result = DNS_R_NOMEMORY;
					break;
				case ISC_R_NOSPACE:
					result = DNS_R_NOSPACE;
					break;
				default:
					UNEXPECTED_ERROR(__FILE__, __LINE__,
					    "isc_lex_gettoken() failed: %s\n",
					    isc_result_totext(result));
					result = DNS_R_UNEXPECTED;
					break;
				}
			}
			if (callback != NULL)
				fromtext_error(callback, callbacks, name,
					       line, NULL, result);
			break;
		} else if (token.type != isc_tokentype_eol &&
			   token.type != isc_tokentype_eof) {
			if (result == DNS_R_SUCCESS)
				result = DNS_R_EXTRATOKEN;
			if (callback != NULL) {
				fromtext_error(callback, callbacks, name,
					       line, &token, result);
				callback = NULL;
			}
		} else if (result != DNS_R_SUCCESS && callback != NULL) {
			fromtext_error(callback, callbacks, name, line,
				       &token, result);
			break;
		} else
			break;
	} while (1);

	if (rdata != NULL && result == DNS_R_SUCCESS) {
		region.length = target->used - st.used;
		dns_rdata_fromregion(rdata, class, type, &region);
	}
	if (result != DNS_R_SUCCESS) {
		*target = st;
	}
	return (result);
}

dns_result_t
dns_rdata_totext(dns_rdata_t *rdata, dns_name_t *origin,
		 isc_buffer_t *target)
{
	dns_result_t result = DNS_R_NOTIMPLEMENTED;
	isc_boolean_t use_default = ISC_FALSE;
	
	REQUIRE(rdata != NULL);
	REQUIRE(isc_buffer_type(target) == ISC_BUFFERTYPE_TEXT);
	if (origin != NULL)
		REQUIRE(dns_name_isabsolute(origin) == ISC_TRUE);

	TOTEXTSWITCH

	if (use_default)
		(void)NULL;

	return (result);
}

dns_result_t
dns_rdata_fromstruct(dns_rdata_t *rdata, dns_rdataclass_t class,
		     dns_rdatatype_t type, void *source,
		     isc_buffer_t *target)
{
	dns_result_t result = DNS_R_NOTIMPLEMENTED;
	isc_buffer_t st;
	isc_region_t region;
	isc_boolean_t use_default = ISC_FALSE;

	REQUIRE(source != NULL);
	REQUIRE(isc_buffer_type(target) == ISC_BUFFERTYPE_BINARY);

	region.base = (unsigned char *)(target->base) + target->used;
	st = *target;

	FROMSTRUCTSWITCH

	if (use_default)
		(void)NULL;

	if (rdata != NULL && result == DNS_R_SUCCESS) {
		region.length = target->used - st.used;
		dns_rdata_fromregion(rdata, class, type, &region);
	}
	if (result != DNS_R_SUCCESS)
		*target = st;
	return (result);
}

dns_result_t
dns_rdata_tostruct(dns_rdata_t *rdata, void *target) {
	dns_result_t result = DNS_R_NOTIMPLEMENTED;
	isc_boolean_t use_default = ISC_FALSE;

	REQUIRE(rdata != NULL);

	TOSTRUCTSWITCH

	if (use_default)
		(void)NULL;

	return (result);
}

dns_result_t
dns_rdataclass_fromtext(dns_rdataclass_t *classp, isc_textregion_t *source) {
	int i = 0;
	unsigned int n;

	while (classes[i].name != NULL) {
		n = strlen(classes[i].name);
		if (n == source->length &&
		    strncasecmp(source->base, classes[i].name, n) == 0) {
			*classp = classes[i].value;
			if ((classes[i].flags & RESERVED) != 0)
				return (DNS_R_NOTIMPLEMENTED);
			return (DNS_R_SUCCESS);
		}
		i++;
	}
	return (DNS_R_UNKNOWN);
}

dns_result_t
dns_rdataclass_totext(dns_rdataclass_t class, isc_buffer_t *target) {
	int i = 0;
	char buf[sizeof "65000"];

	while (classes[i].name != NULL) {
		if (classes[i].value == class) {
			return (str_totext(classes[i].name, target));
		}
		i++;
	}
	sprintf(buf, "%u", class);
	return (str_totext(buf, target));
}

dns_result_t
dns_rdatatype_fromtext(dns_rdatatype_t *typep, isc_textregion_t *source) {
	int i = 0;
	unsigned int n;

	while (types[i].name != NULL) {
		n = strlen(types[i].name);
		if (n == source->length &&
		    strncasecmp(source->base, types[i].name, n) == 0) {
			*typep = types[i].value;
			if ((types[i].flags & RESERVED) != 0)
				return (DNS_R_NOTIMPLEMENTED);
			return (DNS_R_SUCCESS);
		}
		i++;
	}
	return (DNS_R_UNKNOWN);
}

dns_result_t
dns_rdatatype_totext(dns_rdatatype_t type, isc_buffer_t *target) {
	int i = 0;
	char buf[sizeof "65000"];

	while (types[i].name != NULL) {
		if (types[i].value == type) {
			return (str_totext(types[i].name, target));
		}
		i++;
	}
	sprintf(buf, "%u", type);
	return (str_totext(buf, target));
}

dns_result_t
dns_rcode_fromtext(dns_rcode_t *rcodep, isc_textregion_t *source) {
	int i = 0;
	unsigned int n;

	while (rcodes[i].name != NULL) {
		n = strlen(rcodes[i].name);
		if (n == source->length &&
		    strncasecmp(source->base, rcodes[i].name, n) == 0) {
			*rcodep = rcodes[i].value;
			return (DNS_R_SUCCESS);
		}
		i++;
	}
	return (DNS_R_UNKNOWN);
}

dns_result_t
dns_rcode_totext(dns_rcode_t rcode, isc_buffer_t *target) {
	int i = 0;
	char buf[sizeof "65000"];

	while (rcodes[i].name != NULL) {
		if (rcodes[i].value == rcode) {
			return (str_totext(rcodes[i].name, target));
		}
		i++;
	}
	sprintf(buf, "%u", rcode);
	return (str_totext(buf, target));
}

dns_result_t
dns_cert_fromtext(dns_cert_t *certp, isc_textregion_t *source) {
	int i = 0;
	unsigned int n;

	while (certs[i].name != NULL) {
		n = strlen(certs[i].name);
		if (n == source->length &&
		    strncasecmp(source->base, certs[i].name, n) == 0) {
			*certp = certs[i].value;
			return (DNS_R_SUCCESS);
		}
		i++;
	}
	return (DNS_R_UNKNOWN);
}

dns_result_t
dns_cert_totext(dns_cert_t cert, isc_buffer_t *target) {
	int i = 0;
	char buf[sizeof "65000"];

	while (certs[i].name != NULL) {
		if (certs[i].value == cert) {
			return (str_totext(certs[i].name, target));
		}
		i++;
	}
	sprintf(buf, "%u", cert);
	return (str_totext(buf, target));
}

dns_result_t
dns_secalg_fromtext(dns_secalg_t *secalgp, isc_textregion_t *source) {
	int i = 0;
	unsigned int n;

	while (secalgs[i].name != NULL) {
		n = strlen(secalgs[i].name);
		if (n == source->length &&
		    strncasecmp(source->base, secalgs[i].name, n) == 0) {
			*secalgp = secalgs[i].value;
			return (DNS_R_SUCCESS);
		}
		i++;
	}
	return (DNS_R_UNKNOWN);
}

dns_result_t
dns_secalg_totext(dns_secalg_t secalg, isc_buffer_t *target) {
	int i = 0;
	char buf[sizeof "65000"];

	while (secalgs[i].name != NULL) {
		if (secalgs[i].value == secalg) {
			return (str_totext(secalgs[i].name, target));
		}
		i++;
	}
	sprintf(buf, "%u", secalg);
	return (str_totext(buf, target));
}

 /* Private function */

static unsigned int
name_length(dns_name_t *name) {
	return (name->length);
}

static dns_result_t
txt_totext(isc_region_t *source, isc_buffer_t *target) {
	unsigned int tl;
	unsigned int n;
	unsigned char *sp;
	char *tp;
	isc_region_t region;

	isc_buffer_available(target, &region);
	sp = source->base;
	tp = (char *)region.base;
	tl = region.length;

	n = *sp++;

	REQUIRE(n + 1 <= source->length);

	if (tl < 1)
		return (DNS_R_NOSPACE);
	*tp++ = '"';
	tl--;
	while (n--) {
		if (*sp < 0x20 || *sp > 0x7f) {
			if (tl < 4)
				return (DNS_R_NOSPACE);
			sprintf(tp, "\\%03u", *sp++);
			tp += 4;
			tl -= 4;
			continue;
		}
		if (*sp == 0x22 || *sp == 0x3b || *sp == 0x5c) {
			if (tl < 2)
				return (DNS_R_NOSPACE);
			*tp++ = '\\';
			tl--;
		}
		if (tl < 1)
			return (DNS_R_NOSPACE);
		*tp++ = *sp++;
		tl--;
	}
	if (tl < 1)
		return (DNS_R_NOSPACE);
	*tp++ = '"';
	tl--;
	isc_buffer_add(target, tp - (char *)region.base);
	isc_region_consume(source, *source->base + 1);
	return (DNS_R_SUCCESS);
}

static dns_result_t
txt_fromtext(isc_textregion_t *source, isc_buffer_t *target) {
	isc_region_t tregion;

	isc_buffer_available(target, &tregion);
	if (tregion.length < source->length + 1)
		return (DNS_R_NOSPACE);
	if (source->length > 255)
		return (DNS_R_TEXTTOLONG);
	*tregion.base = source->length;
	memcpy(tregion.base + 1, source->base, source->length);
	isc_buffer_add(target, source->length + 1);
	return (DNS_R_SUCCESS);
}

static dns_result_t
txt_fromwire(isc_buffer_t *source, isc_buffer_t *target) {
	unsigned int n;
	isc_region_t sregion;
	isc_region_t tregion;

	isc_buffer_active(source, &sregion);
	if (sregion.length == 0)
		return(DNS_R_UNEXPECTEDEND);
	n = *sregion.base + 1;
	if (n > sregion.length)
		return (DNS_R_UNEXPECTEDEND);
	
	isc_buffer_available(target, &tregion);
	if (n > tregion.length)
		return (DNS_R_NOSPACE);

	memcpy(tregion.base, sregion.base, n);
	isc_buffer_forward(source, n);
	isc_buffer_add(target, n);
	return (DNS_R_SUCCESS);
}

static isc_boolean_t
name_prefix(dns_name_t *name, dns_name_t *origin, dns_name_t *target) {
	int l1, l2;

	if (origin == NULL)
		goto return_false;

	if (dns_name_compare(origin, dns_rootname) == 0)
		goto return_false;

	if (!dns_name_issubdomain(name, origin))
		goto return_false;

	l1 = dns_name_countlabels(name);
	l2 = dns_name_countlabels(origin);
	
	if (l1 == l2)
		goto return_false;

	dns_name_getlabelsequence(name, 0, l1 - l2, target);
	return (ISC_TRUE);

return_false:
	*target = *name;
	return (ISC_FALSE);
}

static dns_result_t
str_totext(char *source, isc_buffer_t *target) {
	unsigned int l;
	isc_region_t region;

	isc_buffer_available(target, &region);
	l = strlen(source);

	if (l > region.length)
		return (DNS_R_NOSPACE);

	memcpy(region.base, source, l);
	isc_buffer_add(target, l);
	return (DNS_R_SUCCESS);
}

static isc_boolean_t
buffer_empty(isc_buffer_t *source) {
	return((source->current == source->active) ? ISC_TRUE : ISC_FALSE);
}

static void
buffer_fromregion(isc_buffer_t *buffer, isc_region_t *region,
		  unsigned int type) {

	isc_buffer_init(buffer, region->base, region->length, type);
	isc_buffer_add(buffer, region->length);
	isc_buffer_setactive(buffer, region->length);
}

static dns_result_t
uint32_tobuffer(isc_uint32_t value, isc_buffer_t *target) {
	isc_region_t region;

	isc_buffer_available(target, &region);
	if (region.length < 4)
		return (DNS_R_NOSPACE);
	isc_buffer_putuint32(target, value);
	return (DNS_R_SUCCESS);
}

static dns_result_t
uint16_tobuffer(isc_uint32_t value, isc_buffer_t *target) {
	isc_region_t region;

	if (value > 0xffff)
		return (DNS_R_RANGE);
	isc_buffer_available(target, &region);
	if (region.length < 2)
		return (DNS_R_NOSPACE);
	isc_buffer_putuint16(target, value);
	return (DNS_R_SUCCESS);
}

static isc_uint32_t
uint32_fromregion(isc_region_t *region) {
	unsigned long value;
	
	REQUIRE(region->length >= 4);
	value = region->base[0] << 24;
	value |= region->base[1] << 16;
	value |= region->base[2] << 8;
	value |= region->base[3];
	return(value);
}

static isc_uint16_t
uint16_fromregion(isc_region_t *region) {
	
	REQUIRE(region->length >= 2);

	return ((region->base[0] << 8) | region->base[1]);
}

static dns_result_t
gettoken(isc_lex_t *lexer, isc_token_t *token, isc_tokentype_t expect,
	 isc_boolean_t eol)
{
	unsigned int options = ISC_LEXOPT_EOL | ISC_LEXOPT_EOF |
			       ISC_LEXOPT_DNSMULTILINE;
	isc_result_t result;
	
	if (expect == isc_tokentype_qstring)
		options |= ISC_LEXOPT_QSTRING;
	else if (expect == isc_tokentype_number)
		options |= ISC_LEXOPT_NUMBER;
	result = isc_lex_gettoken(lexer, options, token);
	switch (result) {
	case ISC_R_SUCCESS:
		break;
	case ISC_R_NOMEMORY:
		return (DNS_R_NOMEMORY);
	case ISC_R_NOSPACE:
		return (DNS_R_NOSPACE);
	default:
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_lex_gettoken() failed: %s\n",
				 isc_result_totext(result));
                return (DNS_R_UNEXPECTED);
	}
	if (eol && ((token->type == isc_tokentype_eol) || 
		    (token->type == isc_tokentype_eof)))
		return (DNS_R_SUCCESS);
	if (token->type == isc_tokentype_string &&
	    expect == isc_tokentype_qstring)
		return (DNS_R_SUCCESS);
        if (token->type != expect) {
                isc_lex_ungettoken(lexer, token);
                if (token->type == isc_tokentype_eol ||
                    token->type == isc_tokentype_eof)
                        return (DNS_R_UNEXPECTEDEND);
                return (DNS_R_UNEXPECTEDTOKEN);
        }
	return (DNS_R_SUCCESS);
}

static dns_result_t
mem_tobuffer(isc_buffer_t *target, void *base, unsigned int length) {
	isc_region_t tr;

	isc_buffer_available(target, &tr);
        if (length > tr.length)
		return (DNS_R_NOSPACE);
	memcpy(tr.base, base, length);
	isc_buffer_add(target, length);
	return (DNS_R_SUCCESS);
}

static int
compare_region(isc_region_t *r1, isc_region_t *r2) {
	unsigned int l;
	int result;

	l = (r1->length < r2->length) ? r1->length : r2->length;

	if ((result = memcmp(r1->base, r2->base, l)) != 0)
		return ((result < 0) ? -1 : 1);
	else
		return ((r1->length == r2->length) ? 0 :
			(r1->length < r2->length) ? -1 : 1);
}

static int
hexvalue(char value) {
	char *s;
	if (!isascii(value&0xff))
		return (-1);
	if (isupper(value))
		value = tolower(value);
	if ((s = strchr(hexdigits, value)) == NULL)
		return (-1);
	return (s - hexdigits);
}

static int
decvalue(char value) {
	char *s;
	if (!isascii(value&0xff))
		return (-1);
	if ((s = strchr(decdigits, value)) == NULL)
		return (-1);
	return (s - decdigits);
}

static const char base64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";

static dns_result_t
base64_totext(isc_region_t *source, isc_buffer_t *target) {
	char buf[5];
	int loops = 0;

	memset(buf, 0, sizeof buf);
	RETERR(str_totext("( " /*)*/, target));
	while (source->length > 2) {
		buf[0] = base64[(source->base[0]>>2)&0x3f];
		buf[1] = base64[((source->base[0]<<4)&0x30)|
				((source->base[1]>>4)&0x0f)];
		buf[2] = base64[((source->base[1]<<2)&0x3c)|
				((source->base[2]>>6)&0x03)];
		buf[3] = base64[source->base[2]&0x3f];
		RETERR(str_totext(buf, target));
		isc_region_consume(source, 3);
		if (source->length != 0 && ++loops == 15) {
			loops = 0;
			RETERR(str_totext(" ", target));
		}
	}
	if (source->length == 2) {
		buf[0] = base64[(source->base[0]>>2)&0x3f];
		buf[1] = base64[((source->base[0]<<4)&0x30)|
				((source->base[1]>>4)&0x0f)];
		buf[2] = base64[((source->base[1]<<4)&0x3c)];
		buf[3] = '=';
		RETERR(str_totext(buf, target));
	} else if (source->length == 1) {
		buf[0] = base64[(source->base[0]>>2)&0x3f];
		buf[1] = base64[((source->base[0]<<4)&0x30)];
		buf[2] = buf[3] = '=';
		RETERR(str_totext(buf, target));
	}
	RETERR(str_totext(" )", target));
	return (DNS_R_SUCCESS);
}

static dns_result_t
base64_tobuffer(isc_lex_t *lexer, isc_buffer_t *target, int length) {
	int digits = 0;
	isc_textregion_t *tr;
	int val[4];
	unsigned char buf[3];
	int seen_end = 0;
	unsigned int i;
	isc_token_t token;
	char *s;
	int n;

	
	while (!seen_end && (length != 0)) {
		if (length > 0)
			RETERR(gettoken(lexer, &token, isc_tokentype_string,
					ISC_FALSE));
		else
			RETERR(gettoken(lexer, &token, isc_tokentype_string,
					ISC_TRUE));
		if (token.type != isc_tokentype_string)
			break;
		tr = &token.value.as_textregion;
		for (i = 0 ;i < tr->length; i++) {
			if (seen_end)
				return (DNS_R_BADBASE64);
			if ((s = strchr(base64, tr->base[i])) == NULL)
				return (DNS_R_BADBASE64);
			val[digits++] = s - base64;
			if (digits == 4) {
				if (val[0] == 64 || val[1] == 64)
					return (DNS_R_BADBASE64);
				if (val[2] == 64 && val[3] != 64)
					return (DNS_R_BADBASE64);
				n = (val[2] == 64) ? 1 :
				    (val[3] == 64) ? 2 : 3;
				if (n != 3) {
					seen_end = 1;
					if (val[2] == 64)
						val[2] = 0;
					if (val[3] == 64)
						val[3] = 0;
				}
				buf[0] = (val[0]<<2)|(val[1]>>4);
				buf[1] = (val[1]<<4)|(val[2]>>2);
				buf[2] = (val[2]<<6)|(val[3]);
				RETERR(mem_tobuffer(target, buf, n));
				if (length >= 0) {
					if (n > length)
						return (DNS_R_BADBASE64);
					else
						length -= n;
				}
				digits = 0;
			}
		}
	}
	if (length < 0 && !seen_end)
		isc_lex_ungettoken(lexer, &token);
	if (length > 0)
		return (DNS_R_UNEXPECTEDEND);
	if (digits != 0)
		return (DNS_R_BADBASE64);
	return (DNS_R_SUCCESS);
}

static int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static dns_result_t
time_totext(unsigned long value, isc_buffer_t *target) {
	isc_int64_t start;
	isc_int64_t base;
	isc_int64_t t;
	struct tm tm;
	char buf[sizeof "YYYYMMDDHHMMSS"];
	int secs;

	/* find the right epoch */
	start = time(NULL);
	start -= 0x7fffffff;
	base = 0;
	while ((t = (base + value)) < start) {
		base += 0x80000000;
		base += 0x80000000;
	}

#define is_leap(y) ((((y) % 4) == 0 && ((y) % 100) != 0) || ((y) % 400) == 0)
#define year_secs(y) ((is_leap(y) ? 366 : 365 ) * 86400)
#define month_secs(m, y) ((days[m] + ((m == 1 && is_leap(y)) ? 1 : 0 )) * 86400)


	tm.tm_year = 70;
	while ((secs = year_secs(tm.tm_year + 1900 + 1)) <= t) {
		t -= secs;
		tm.tm_year++;
	}
	tm.tm_mon = 0;
	while ((secs = month_secs(tm.tm_mon, tm.tm_year + 1900)) <= t) {
		t -= secs;
		tm.tm_mon++;
	}
	tm.tm_mday = 1;
	while (86400 <= t) {
		t -= 86400;
		tm.tm_mday++;
	}
	tm.tm_hour = 0;
	while (3600 <= t) {
		t -= 3600;
		tm.tm_hour++;
	}
	tm.tm_min = 0;
	while (60 <= t) {
		t -= 60;
		tm.tm_min++;
	}
	tm.tm_sec = t;
		    /* yy  mm  dd  HH  MM  SS */
	sprintf(buf, "%04d%02d%02d%02d%02d%02d",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
	return (str_totext(buf, target));
}

static dns_result_t
time_tobuffer(char *source, isc_buffer_t *target) {
	int year, month, day, hour, minute, second;
	unsigned long value;
	int secs;
	int i;

#define RANGE(min, max, value) \
	do { \
		if (value < (min) || value > (max)) \
			return (DNS_R_RANGE); \
	} while (0)

	if (strlen(source) != 14)
		return (DNS_R_SYNTAX);
	if (sscanf(source, "%4d%2d%2d%2d%2d%2d",
		   &year, &month, &day, &hour, &minute, &second) != 6)
		return (DNS_R_SYNTAX);

	RANGE(1970, 9999, year);
	RANGE(1, 12, month);
	RANGE(1, days[month - 1] +
		 ((month == 2 && is_leap(year)) ? 1 : 0), day);
	RANGE(0, 23, hour);
	RANGE(0, 59, minute);
	RANGE(0, 60, second);	/* leap second */

	/* calulate seconds since epoch */
	value = second + (60 * minute) + (3600 * hour) + ((day - 1) * 86400);
	for (i = 0; i < (month - 1) ; i++)
		value += days[i] * 86400;
	if (is_leap(year) && month > 2)
		value += 86400;
	for (i = 1970; i < year; i++) {
		secs = (is_leap(i) ? 366 : 365) * 86400;
		value += secs;
	}
	
	return (uint32_tobuffer(value, target));
}

static const char atob_digits[86] =
"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstu";
/*
 * Subroutines to convert between 8 bit binary bytes and printable ASCII.
 * Computes the number of bytes, and three kinds of simple checksums.
 * Incoming bytes are collected into 32-bit words, then printed in base 85:
 *	exp(85,5) > exp(2,32)
 * The ASCII characters used are between '!' and 'u';
 * 'z' encodes 32-bit zero; 'x' is used to mark the end of encoded data.
 *
 * Originally by Paul Rutter (philabs!per) and Joe Orost (petsd!joe) for
 * the atob/btoa programs, released with the compress program, in mod.sources.
 * Modified by Mike Schwartz 8/19/86 for use in BIND.
 * Modified to be re-entrant 3/2/99.
 */


struct state {
	isc_int32_t Ceor;
	isc_int32_t Csum;
	isc_int32_t Crot;
	isc_int32_t word;
	isc_int32_t bcount;
};

#define Ceor state->Ceor
#define Csum state->Csum
#define Crot state->Crot
#define word state->word
#define bcount state->bcount

#define times85(x)	((((((x<<2)+x)<<2)+x)<<2)+x)

static dns_result_t	byte_atob(int c, isc_buffer_t *target,
				  struct state *state);
static dns_result_t	putbyte(int c, isc_buffer_t *, struct state *state);
static dns_result_t	byte_btoa(int c, isc_buffer_t *, struct state *state);

/* Decode ASCII-encoded byte c into binary representation and 
 * place into *bufp, advancing bufp 
 */
static dns_result_t
byte_atob(int c, isc_buffer_t *target, struct state *state) {
	char *s;
	if (c == 'z') {
		if (bcount != 0)
			return(DNS_R_SYNTAX);
		else {
			RETERR(putbyte(0, target, state));
			RETERR(putbyte(0, target, state));
			RETERR(putbyte(0, target, state));
			RETERR(putbyte(0, target, state));
		}
	} else if ((s = strchr(atob_digits, c)) != NULL) {
		if (bcount == 0) {
			word = s - atob_digits;
			++bcount;
		} else if (bcount < 4) {
			word = times85(word);
			word += s - atob_digits;
			++bcount;
		} else {
			word = times85(word);
			word += s - atob_digits;
			RETERR(putbyte((word >> 24) & 0xff, target, state));
			RETERR(putbyte((word >> 16) & 0xff, target, state));
			RETERR(putbyte((word >> 8) & 0xff, target, state));
			RETERR(putbyte(word & 0xff, target, state));
			word = 0;
			bcount = 0;
		}
	} else
		return(DNS_R_SYNTAX);
	return(DNS_R_SUCCESS);
}

/* Compute checksum info and place c into target */
static dns_result_t
putbyte(int c, isc_buffer_t *target, struct state *state) {
	isc_region_t tr;

	Ceor ^= c;
	Csum += c;
	Csum += 1;
	if ((Crot & 0x80000000)) {
		Crot <<= 1;
		Crot += 1;
	} else {
		Crot <<= 1;
	}
	Crot += c;
	isc_buffer_available(target, &tr);
	if (tr.length < 1)
		return (DNS_R_NOSPACE);
	tr.base[0] = c;
	isc_buffer_add(target, 1);
	return (DNS_R_SUCCESS);
}

/* Read the ASCII-encoded data from inbuf, of length inbuflen, and convert
   it into T_UNSPEC (binary data) in outbuf, not to exceed outbuflen bytes;
   outbuflen must be divisible by 4.  (Note: this is because outbuf is filled
   in 4 bytes at a time.  If the actual data doesn't end on an even 4-byte
   boundary, there will be no problem...it will be padded with 0 bytes, and
   numbytes will indicate the correct number of bytes.  The main point is
   that since the buffer is filled in 4 bytes at a time, even if there is
   not a full 4 bytes of data at the end, there has to be room to 0-pad the
   data, so the buffer must be of size divisible by 4).  Place the number of
   output bytes in numbytes, and return a failure/success status  */

static dns_result_t
atob_tobuffer(isc_lex_t *lexer, isc_buffer_t *target) {
	isc_int32_t oeor, osum, orot;
	struct state statebuf, *state= &statebuf;
	isc_token_t token;
	char c;
	char *e;

	Ceor = Csum = Crot = word = bcount = 0;

	RETERR(gettoken(lexer, &token, isc_tokentype_string, ISC_FALSE));
	while (token.value.as_textregion.length != 0) {
		if ((c = token.value.as_textregion.base[0]) == 'x') {
			break;
		} else
			RETERR(byte_atob(c, target, state));
		isc_textregion_consume(&token.value.as_textregion, 1);
	}

	/* number of bytes */
	RETERR(gettoken(lexer, &token, isc_tokentype_number, ISC_FALSE));
	if ((token.value.as_ulong % 4) != 0)
		isc_buffer_subtract(target,  4 - (token.value.as_ulong % 4));

	/* checksum */
	RETERR(gettoken(lexer, &token, isc_tokentype_string, ISC_FALSE));
	oeor = strtoul(token.value.as_pointer, &e, 16);
	if (*e != 0)
		return (DNS_R_SYNTAX);

	/* checksum */
	RETERR(gettoken(lexer, &token, isc_tokentype_string, ISC_FALSE));
	osum = strtoul(token.value.as_pointer, &e, 16);
	if (*e != 0)
		return (DNS_R_SYNTAX);

	/* checksum */
	RETERR(gettoken(lexer, &token, isc_tokentype_string, ISC_FALSE));
	orot = strtoul(token.value.as_pointer, &e, 16);
	if (*e != 0)
		return (DNS_R_SYNTAX);

	if ((oeor != Ceor) || (osum != Csum) || (orot != Crot))
		return(DNS_R_BADCKSUM);
	return(DNS_R_SUCCESS);
}

/* Encode binary byte c into ASCII representation and place into *bufp,
   advancing bufp */
static dns_result_t
byte_btoa(int c, isc_buffer_t *target, struct state *state) {
	isc_region_t tr;

	isc_buffer_available(target, &tr);
	Ceor ^= c;
	Csum += c;
	Csum += 1;
	if ((Crot & 0x80000000)) {
		Crot <<= 1;
		Crot += 1;
	} else {
		Crot <<= 1;
	}
	Crot += c;

	word <<= 8;
	word |= c;
	if (bcount == 3) {
		if (word == 0) {
			if (tr.length < 1)
				return (DNS_R_NOSPACE);
			tr.base[0] = 'z';
			isc_buffer_add(target, 1);
		} else {
		    register int tmp = 0;
		    register isc_int32_t tmpword = word;
			
		    if (tmpword < 0) {	
			   /* Because some don't support u_long */
		    	tmp = 32;
		    	tmpword -= (isc_int32_t)(85 * 85 * 85 * 85 * 32);
		    }
		    if (tmpword < 0) {
		    	tmp = 64;
		    	tmpword -= (isc_int32_t)(85 * 85 * 85 * 85 * 32);
		    }
			if (tr.length < 5)
				return (DNS_R_NOSPACE);
		    	tr.base[0] = atob_digits[(tmpword /
					      (isc_int32_t)(85 * 85 * 85 * 85))
						+ tmp];
			tmpword %= (isc_int32_t)(85 * 85 * 85 * 85);
			tr.base[1] = atob_digits[tmpword / (85 * 85 * 85)];
			tmpword %= (85 * 85 * 85);
			tr.base[2] = atob_digits[tmpword / (85 * 85)];
			tmpword %= (85 * 85);
			tr.base[3] = atob_digits[tmpword / 85];
			tmpword %= 85;
			tr.base[4] = atob_digits[tmpword];
			isc_buffer_add(target, 5);
		}
		bcount = 0;
	} else {
		bcount += 1;
	}
	return (DNS_R_SUCCESS);
}


/*
 * Encode the binary data from inbuf, of length inbuflen, into a
 * target.  Return success/failure status
 */
static dns_result_t
btoa_totext(unsigned char *inbuf, int inbuflen, isc_buffer_t *target) {
	int inc;
	struct state statebuf, *state = &statebuf;
	char buf[sizeof "x 2000000000 ffffffff ffffffff ffffffff"];

	Ceor = Csum = Crot = word = bcount = 0;
	for (inc = 0; inc < inbuflen; inbuf++, inc++)
		RETERR(byte_btoa(*inbuf, target, state));
	
	while (bcount != 0)
		RETERR(byte_btoa(0, target, state));
	
	/*
	 * Put byte count and checksum information at end of buffer,
	 * delimited by 'x'
	 */
	sprintf(buf, "x %d %x %x %x", inbuflen, Ceor, Csum, Crot);
	return (str_totext(buf, target));
}


static void
default_fromtext_callback(dns_rdatacallbacks_t *callbacks, char *fmt, ...) {
	va_list ap;

	callbacks = callbacks; /*unused*/

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void
fromtext_error(void (*callback)(dns_rdatacallbacks_t *, char *, ...),
	       dns_rdatacallbacks_t *callbacks, char *name, int line,
	       isc_token_t *token, dns_result_t result)
{
	if (name == NULL)
		name = "UNKNOWN";

	if (token != NULL) {
		switch (token->type) {
		case isc_tokentype_eol:
			(*callback)(callbacks, "%s: %s:%d: near eol: %s\n",
				    "dns_rdata_fromtext", name, line,
				    dns_result_totext(result));
			break;
		case isc_tokentype_eof:
			(*callback)(callbacks, "%s: %s:%d: near eof: %s\n",
				    "dns_rdata_fromtext", name, line,
				    dns_result_totext(result));
			break;
		case isc_tokentype_number:
			(*callback)(callbacks, "%s: %s:%d: near %lu: %s\n",
				    "dns_rdata_fromtext", name, line,
				    token->value.as_ulong,
				    dns_result_totext(result));
			break;
		case isc_tokentype_string:
		case isc_tokentype_qstring:
			(*callback)(callbacks, "%s: %s:%d: near \"%s\": %s\n",
				    "dns_rdata_fromtext", name, line,
				    (char *)token->value.as_pointer,
				    dns_result_totext(result));
			break;
		default:
			(*callback)(callbacks, "%s: %s:%d: %s\n",
				    "dns_rdata_fromtext", name, line,
				    dns_result_totext(result));
			break;
		}
	} else {
		(*callback)(callbacks, "dns_rdata_fromtext: %s:%d: %s\n",
			    name, line, dns_result_totext(result));
	}
}
