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

/*
 * Implementation notes
 * --------------------
 *
 * In finds, if task == NULL, no events will be generated, and no events
 * have been sent.  If task != NULL but taskaction == NULL, an event has been
 * posted but not yet freed.  If neither are NULL, no event was posted.
 *
 */

/*
 * After we have cleaned all buckets, dump the database contents.
 */
#define DUMP_ADB_AFTER_CLEANING

#include <config.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <isc/assertions.h>
#include <isc/condition.h>
#include <isc/event.h>
#include <isc/log.h>
#include <isc/magic.h>
#include <isc/mutex.h>
#include <isc/mutexblock.h>
#include <isc/random.h>
#include <isc/timer.h>
#include <isc/util.h>

#include <dns/a6.h>
#include <dns/adb.h>
#include <dns/db.h>
#include <dns/events.h>
#include <dns/fixedname.h>
#include <dns/log.h>
#include <dns/name.h>
#include <dns/rdata.h>
#include <dns/rdataset.h>
#include <dns/resolver.h>
#include <dns/types.h>
#include <dns/view.h>

#define DNS_ADB_MAGIC		  0x44616462	/* Dadb. */
#define DNS_ADB_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADB_MAGIC)
#define DNS_ADBNAME_MAGIC	  0x6164624e	/* adbN. */
#define DNS_ADBNAME_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADBNAME_MAGIC)
#define DNS_ADBNAMEHOOK_MAGIC	  0x61644e48	/* adNH. */
#define DNS_ADBNAMEHOOK_VALID(x)  ISC_MAGIC_VALID(x, DNS_ADBNAMEHOOK_MAGIC)
#define DNS_ADBZONEINFO_MAGIC	  0x6164625a	/* adbZ. */
#define DNS_ADBZONEINFO_VALID(x)  ISC_MAGIC_VALID(x, DNS_ADBZONEINFO_MAGIC)
#define DNS_ADBENTRY_MAGIC	  0x61646245	/* adbE. */
#define DNS_ADBENTRY_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADBENTRY_MAGIC)
#define DNS_ADBFETCH_MAGIC	  0x61644634	/* adF4. */
#define DNS_ADBFETCH_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADBFETCH_MAGIC)
#define DNS_ADBFETCH6_MAGIC	  0x61644636	/* adF6. */
#define DNS_ADBFETCH6_VALID(x)	  ISC_MAGIC_VALID(x, DNS_ADBFETCH6_MAGIC)

/*
 * The number of buckets needs to be a prime (for good hashing).
 *
 * XXXRTH  How many buckets do we need?
 *
 * This value must be coordinated with CLEAN_SECONDS (below).
 */
#define NBUCKETS	       1009	/* how many buckets for names/addrs */

/*
 * For type 3 negative cache entries, we will remember that the address is
 * broken for this long.  XXXMLG This is also used for actual addresses, too.
 * The intent is to keep us from constantly asking about A/A6/AAAA records
 * if the zone has extremely low TTLs.
 */
#define ADB_CACHE_MINIMUM	10	/* seconds */

/*
 * Clean one bucket every CLEAN_SECONDS.
 */
#define CLEAN_SECONDS		(300 / NBUCKETS)
#if CLEAN_SECONDS < 1
#undef CLEAN_SECONDS
#define CLEAN_SECONDS		1
#endif

#define FREE_ITEMS		16	/* free count for memory pools */
#define FILL_COUNT		 8	/* fill count for memory pools */

#define DNS_ADB_INVALIDBUCKET (-1)	/* invalid bucket address */

typedef ISC_LIST(dns_adbname_t) dns_adbnamelist_t;
typedef struct dns_adbnamehook dns_adbnamehook_t;
typedef ISC_LIST(dns_adbnamehook_t) dns_adbnamehooklist_t;
typedef struct dns_adbzoneinfo dns_adbzoneinfo_t;
typedef ISC_LIST(dns_adbentry_t) dns_adbentrylist_t;
typedef struct dns_adbfetch dns_adbfetch_t;
typedef struct dns_adbfetch6 dns_adbfetch6_t;

struct dns_adb {
	unsigned int			magic;

	isc_mutex_t			lock;
	isc_mutex_t			ilock;
	isc_mem_t		       *mctx;
	dns_view_t		       *view;
	isc_timermgr_t		       *timermgr;
	isc_timer_t		       *timer;
	isc_taskmgr_t		       *taskmgr;
	isc_task_t		       *task;

	isc_interval_t			tick_interval;
	int				next_cleanbucket;

	unsigned int			irefcnt;
	unsigned int			erefcnt;

	isc_mutex_t			mplock;
	isc_mempool_t		       *nmp;	/* dns_adbname_t */
	isc_mempool_t		       *nhmp;	/* dns_adbnamehook_t */
	isc_mempool_t		       *zimp;	/* dns_adbzoneinfo_t */
	isc_mempool_t		       *emp;	/* dns_adbentry_t */
	isc_mempool_t		       *ahmp;	/* dns_adbfind_t */
	isc_mempool_t		       *aimp;	/* dns_adbaddrinfo_t */
	isc_mempool_t		       *afmp;	/* dns_adbfetch_t */
	isc_mempool_t		       *af6mp;	/* dns_adbfetch6_t */

	isc_random_t			rand;

	/*
	 * Bucketized locks and lists for names.
	 *
	 * XXXRTH  Have a per-bucket structure that contains all of these?
	 */
	dns_adbnamelist_t		names[NBUCKETS];
	isc_mutex_t			namelocks[NBUCKETS];
	isc_boolean_t			name_sd[NBUCKETS];
	unsigned int			name_refcnt[NBUCKETS];

	/*
	 * Bucketized locks for entries.
	 *
	 * XXXRTH  Have a per-bucket structure that contains all of these?
	 */
	dns_adbentrylist_t		entries[NBUCKETS];
	isc_mutex_t			entrylocks[NBUCKETS];
	isc_boolean_t			entry_sd[NBUCKETS];
	unsigned int			entry_refcnt[NBUCKETS];

	isc_event_t			cevent;
	isc_boolean_t			cevent_sent;
	isc_boolean_t			shutting_down;
	isc_eventlist_t			whenshutdown;
};

/*
 * XXXMLG  Document these structures.
 */

struct dns_adbname {
	unsigned int			magic;
	dns_name_t			name;
	dns_adb_t		       *adb;
	unsigned int			partial_result;
	unsigned int			flags;
	int				lock_bucket;
	dns_name_t			target;
	isc_stdtime_t			expire_target;
	isc_stdtime_t			expire_v4;
	isc_stdtime_t			expire_v6;
	unsigned int			chains;
	dns_adbnamehooklist_t		v4;
	dns_adbnamehooklist_t		v6;
	dns_adbfetch_t		       *fetch_a;
	dns_adbfetch_t		       *fetch_aaaa;
	ISC_LIST(dns_adbfetch6_t)	fetches_a6;
	dns_adbfindlist_t		finds;
	ISC_LINK(dns_adbname_t)		plink;
};

struct dns_adbfetch {
	unsigned int			magic;
	dns_adbnamehook_t	       *namehook;
	dns_adbentry_t		       *entry;
	dns_fetch_t		       *fetch;
	dns_rdataset_t			rdataset;
};

struct dns_adbfetch6 {
	unsigned int			magic;
	unsigned int			flags;
	dns_adbnamehook_t	       *namehook;
	dns_adbentry_t		       *entry;
	dns_fetch_t		       *fetch;
	dns_rdataset_t			rdataset;
	dns_a6context_t			a6ctx;
	ISC_LINK(dns_adbfetch6_t)	plink;
};

/*
 * dns_adbnamehook_t
 *
 * This is a small widget that dangles off a dns_adbname_t.  It contains a
 * pointer to the address information about this host, and a link to the next
 * namehook that will contain the next address this host has.
 */
struct dns_adbnamehook {
	unsigned int			magic;
	dns_adbentry_t		       *entry;
	ISC_LINK(dns_adbnamehook_t)	plink;
};

/*
 * dns_adbzoneinfo_t
 *
 * This is a small widget that holds zone-specific information about an
 * address.  Currently limited to lameness, but could just as easily be
 * extended to other types of information about zones.
 */
struct dns_adbzoneinfo {
	unsigned int			magic;

	dns_name_t			zone;
	isc_stdtime_t			lame_timer;

	ISC_LINK(dns_adbzoneinfo_t)	plink;
};

/*
 * An address entry.  It holds quite a bit of information about addresses,
 * including edns state, rtt, and of course the address of the host.
 */
struct dns_adbentry {
	unsigned int			magic;

	int				lock_bucket;
	unsigned int			refcnt;

	unsigned int			flags;
	int				edns_level;	/* must be int! */
	int				goodness;	/* bad < 0 <= good */
	unsigned int			srtt;
	isc_sockaddr_t			sockaddr;
	isc_stdtime_t			expires;

	ISC_LIST(dns_adbzoneinfo_t)	zoneinfo;
	ISC_LINK(dns_adbentry_t)	plink;
};

/*
 * Internal functions (and prototypes).
 */
static inline dns_adbname_t *new_adbname(dns_adb_t *, dns_name_t *);
static inline void free_adbname(dns_adb_t *, dns_adbname_t **);
static inline dns_adbnamehook_t *new_adbnamehook(dns_adb_t *,
						 dns_adbentry_t *);
static inline void free_adbnamehook(dns_adb_t *, dns_adbnamehook_t **);
static inline dns_adbzoneinfo_t *new_adbzoneinfo(dns_adb_t *, dns_name_t *);
static inline void free_adbzoneinfo(dns_adb_t *, dns_adbzoneinfo_t **);
static inline dns_adbentry_t *new_adbentry(dns_adb_t *);
static inline void free_adbentry(dns_adb_t *, dns_adbentry_t **);
static inline dns_adbfind_t *new_adbfind(dns_adb_t *);
static inline void free_adbfind(dns_adb_t *, dns_adbfind_t **);
static inline dns_adbaddrinfo_t *new_adbaddrinfo(dns_adb_t *,
						 dns_adbentry_t *);
static inline dns_adbfetch_t *new_adbfetch(dns_adb_t *);
static inline void free_adbfetch(dns_adb_t *, dns_adbfetch_t **);
static inline dns_adbfetch6_t *new_adbfetch6(dns_adb_t *, dns_adbname_t *,
					     dns_a6context_t *);
static inline void free_adbfetch6(dns_adb_t *, dns_adbfetch6_t **);
static inline dns_adbname_t *find_name_and_lock(dns_adb_t *, dns_name_t *,
						int *);
static inline dns_adbentry_t *find_entry_and_lock(dns_adb_t *,
						  isc_sockaddr_t *, int *);
static void dump_adb(dns_adb_t *, FILE *);
static void print_dns_name(FILE *, dns_name_t *);
static void print_namehook_list(FILE *, dns_adbname_t *);
static void print_find_list(FILE *, dns_adbname_t *);
static void print_fetch_list(FILE *, dns_adbname_t *);
static inline void dec_adb_irefcnt(dns_adb_t *);
static inline void inc_adb_erefcnt(dns_adb_t *, isc_boolean_t);
static inline void dec_adb_erefcnt(dns_adb_t *, isc_boolean_t);
static inline void inc_entry_refcnt(dns_adb_t *, dns_adbentry_t *,
				    isc_boolean_t);
static inline void dec_entry_refcnt(dns_adb_t *, dns_adbentry_t *,
				    isc_boolean_t);
static inline void violate_locking_hierarchy(isc_mutex_t *, isc_mutex_t *);
static void clean_namehooks(dns_adb_t *, dns_adbnamehooklist_t *);
static void clean_target(dns_adb_t *, dns_name_t *);
static void clean_finds_at_name(dns_adbname_t *, isc_eventtype_t,
				unsigned int);
static void check_expire_namehooks(dns_adbname_t *, isc_stdtime_t);
static void cancel_fetches_at_name(dns_adbname_t *);
static isc_result_t dbfind_name(dns_adbname_t *, isc_stdtime_t,
				isc_boolean_t, dns_rdatatype_t);
static isc_result_t fetch_name_v4(dns_adbname_t *, isc_boolean_t);
static isc_result_t fetch_name_aaaa(dns_adbname_t *);
static isc_result_t fetch_name_a6(dns_adbname_t *, isc_boolean_t,
				  isc_boolean_t);
static inline void check_exit(dns_adb_t *);
static void timer_cleanup(isc_task_t *, isc_event_t *);
static void destroy(dns_adb_t *);
static void shutdown_names(dns_adb_t *);
static void shutdown_entries(dns_adb_t *);
static inline void link_name(dns_adb_t *, int, dns_adbname_t *);
static inline void unlink_name(dns_adb_t *, dns_adbname_t *);
static inline void link_entry(dns_adb_t *, int, dns_adbentry_t *);
static inline void unlink_entry(dns_adb_t *, dns_adbentry_t *);
static void kill_name(dns_adbname_t **, isc_eventtype_t);
static void fetch_callback_a6(isc_task_t *, isc_event_t *);
static isc_result_t dbfind_a6(dns_adbname_t *, isc_stdtime_t, isc_boolean_t);

/*
 * MUST NOT overlap DNS_ADBFIND_* flags!
 */
#define FIND_EVENT_SENT		0x40000000
#define FIND_EVENT_FREED	0x80000000
#define FIND_EVENTSENT(h)	(((h)->flags & FIND_EVENT_SENT) != 0)
#define FIND_EVENTFREED(h)	(((h)->flags & FIND_EVENT_FREED) != 0)

#define NAME_NEEDS_POKE		0x80000000
#define NAME_IS_DEAD		0x40000000
#define NAME_DEAD(n)		(((n)->flags & NAME_IS_DEAD) != 0)
#define NAME_NEEDSPOKE(n)	(((n)->flags & NAME_NEEDS_POKE) != 0)

/*
 * To the name, address classes are all that really exist.  If it has a
 * V6 address it doesn't care if it came from an A6 chain or an AAAA query.
 */
#define NAME_HAS_V4(n)		(!ISC_LIST_EMPTY((n)->v4))
#define NAME_HAS_V6(n)		(!ISC_LIST_EMPTY((n)->v6))
#define NAME_HAS_ADDRS(n)	(NAME_HAS_V4(n) || NAME_HAS_V6(n))

/*
 * Fetches are broken out into A, AAAA, and A6 types.  In some cases,
 * however, it makes more sense to test for a particular class of fetches,
 * like V4 or V6 above.
 */
#define NAME_FETCH_A(n)		((n)->fetch_a != NULL)
#define NAME_FETCH_AAAA(n)	((n)->fetch_aaaa != NULL)
#define NAME_FETCH_A6(n)	(!ISC_LIST_EMPTY((n)->fetches_a6))
#define NAME_FETCH_V4(n)	(NAME_FETCH_A(n))
#define NAME_FETCH_V6(n)	(NAME_FETCH_AAAA(n) || NAME_FETCH_A6(n))
#define NAME_FETCH(n)		(NAME_FETCH_V4(n) || NAME_FETCH_V6(n))

/*
 * Was this fetch started using the hints database?
 * Was this the initial fetch for the A6 record?  If so, we might want to
 * start AAAA queries if it fails.
 */
#define FETCH_USE_HINTS		0x80000000
#define FETCH_FIRST_A6		0x40000000
#define FETCH_USEHINTS(f)	(((f)->flags & FETCH_USE_HINTS) != 0)
#define FETCH_FIRSTA6(f)	(((f)->flags & FETCH_FIRST_A6) != 0)

/*
 * Find options and tests to see if there are addresses on the list.
 */
#define FIND_WANTEVENT(fn)	(((fn)->options & DNS_ADBFIND_WANTEVENT) != 0)
#define FIND_WANTEMPTYEVENT(fn)	(((fn)->options & DNS_ADBFIND_EMPTYEVENT) != 0)
#define FIND_AVOIDFETCHES(fn)	(((fn)->options & DNS_ADBFIND_AVOIDFETCHES) \
				 != 0)
#define FIND_STARTATROOT(fn)	(((fn)->options & DNS_ADBFIND_STARTATROOT) \
				 != 0)
#define FIND_HAS_ADDRS(fn)	(!ISC_LIST_EMPTY((fn)->list))


/*
 * These are currently used on simple unsigned ints, so they are
 * not really associated with any particular type.
 */
#define WANT_INET(x)		(((x) & DNS_ADBFIND_INET) != 0)
#define WANT_INET6(x)		(((x) & DNS_ADBFIND_INET6) != 0)

#define EXPIRE_OK(exp, now)	((exp == INT_MAX) || (exp < now))

#define ENTER_LEVEL		50
#define EXIT_LEVEL		ENTER_LEVEL
#define CLEAN_LEVEL		100
#define DEF_LEVEL		5
#define NCACHE_LEVEL		20

#define NCACHE_RESULT(r)	((r) == DNS_R_NCACHENXDOMAIN || \
				 (r) == DNS_R_NCACHENXRRSET)
#define AUTH_NX(r)		((r) == DNS_R_NXDOMAIN || \
				 (r) == DNS_R_NXRRSET)


static void
DP(int level, char *format, ...)
{
	va_list args;

	va_start(args, format);
	isc_log_vwrite(dns_lctx,
		       DNS_LOGCATEGORY_DATABASE, DNS_LOGMODULE_ADB,
		       ISC_LOG_DEBUG(level), format, args);
	va_end(args);
}

/*
 * Requires the adbname bucket be locked and that no entry buckets be locked.
 *
 * This code handles A and AAAA rdatasets only.
 */
static isc_result_t
import_rdataset(dns_adbname_t *adbname, dns_rdataset_t *rdataset,
		isc_stdtime_t now)
{
	isc_result_t result;
	dns_adb_t *adb;
	dns_adbnamehook_t *nh;
	dns_rdata_t rdata;
	struct in_addr ina;
	struct in6_addr in6a;
	isc_sockaddr_t sockaddr;
	dns_adbentry_t *foundentry;  /* NO CLEAN UP! */
	int addr_bucket;
	isc_boolean_t new_addresses_added;
	dns_rdatatype_t rdtype;
	unsigned int findoptions;

	INSIST(DNS_ADBNAME_VALID(adbname));
	adb = adbname->adb;
	INSIST(DNS_ADB_VALID(adb));

	rdtype = rdataset->type;
	INSIST((rdtype == dns_rdatatype_a) || (rdtype == dns_rdatatype_aaaa));
	if (rdtype == dns_rdatatype_a)
		findoptions = DNS_ADBFIND_INET;
	else
		findoptions = DNS_ADBFIND_INET6;

	addr_bucket = DNS_ADB_INVALIDBUCKET;
	new_addresses_added = ISC_FALSE;

	nh = NULL;
	result = dns_rdataset_first(rdataset);
	while (result == ISC_R_SUCCESS) {
		dns_rdataset_current(rdataset, &rdata);
		if (rdtype == dns_rdatatype_a) {
			INSIST(rdata.length == 4);
			memcpy(&ina.s_addr, rdata.data, 4);
			isc_sockaddr_fromin(&sockaddr, &ina, 53);
		} else {
			INSIST(rdata.length == 16);
			memcpy(in6a.s6_addr, rdata.data, 16);
			isc_sockaddr_fromin6(&sockaddr, &in6a, 53);
		}

		if (IN6_IS_ADDR_V4MAPPED(&sockaddr.type.sin6.sin6_addr)
		    || IN6_IS_ADDR_V4COMPAT(&sockaddr.type.sin6.sin6_addr)) {
			DP(1, "Ignoring IPv6 mapped IPv4 address");
			goto next;
		}

		INSIST(nh == NULL);
		nh = new_adbnamehook(adb, NULL);
		if (nh == NULL) {
			adbname->partial_result |= findoptions;
			result = ISC_R_NOMEMORY;
			goto fail;
		}

		foundentry = find_entry_and_lock(adb, &sockaddr, &addr_bucket);
		if (foundentry == NULL) {
			dns_adbentry_t *entry;

			entry = new_adbentry(adb);
			if (entry == NULL) {
				adbname->partial_result |= findoptions;
				result = ISC_R_NOMEMORY;
				goto fail;
			}

			entry->sockaddr = sockaddr;
			entry->refcnt = 1;

			nh->entry = entry;

			link_entry(adb, addr_bucket, entry);
		} else {
			foundentry->refcnt++;
			nh->entry = foundentry;
		}

		new_addresses_added = ISC_TRUE;
		if (rdtype == dns_rdatatype_a)
			ISC_LIST_APPEND(adbname->v4, nh, plink);
		else
			ISC_LIST_APPEND(adbname->v6, nh, plink);
		nh = NULL;

	next:

		result = dns_rdataset_next(rdataset);
	}

 fail:
	if (nh != NULL)
		free_adbnamehook(adb, &nh);

	if (addr_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->entrylocks[addr_bucket]);

	rdataset->ttl = ISC_MAX(rdataset->ttl, ADB_CACHE_MINIMUM);

	if (rdtype == dns_rdatatype_a) {
		DP(NCACHE_LEVEL, "expire_v4 set to MIN(%u,%u) import_rdataset",
		   adbname->expire_v4, now + rdataset->ttl);
		adbname->expire_v4 = ISC_MIN(adbname->expire_v4,
					     now + rdataset->ttl);
	} else {
		DP(NCACHE_LEVEL, "expire_v6 set to MIN(%u,%u) import_rdataset",
		   adbname->expire_v6, now + rdataset->ttl);
		adbname->expire_v6 = ISC_MIN(adbname->expire_v6,
					     now + rdataset->ttl);
	}

	if (new_addresses_added) {
		/*
		 * Lie a little here.  This is more or less so code that cares
		 * can find out if any new information was added or not.
		 */
		return (ISC_R_SUCCESS);
	}

	return (result);
}

static void
import_a6(dns_a6context_t *a6ctx)
{
	dns_adbname_t *name;
	dns_adb_t *adb;
	dns_adbnamehook_t *nh;
	dns_adbentry_t *foundentry;  /* NO CLEAN UP! */
	int addr_bucket;
	isc_sockaddr_t sockaddr;

	name = a6ctx->arg;
	INSIST(DNS_ADBNAME_VALID(name));
	adb = name->adb;
	INSIST(DNS_ADB_VALID(adb));

	addr_bucket = DNS_ADB_INVALIDBUCKET;

	DP(ENTER_LEVEL, "ENTER: import_a6() name %p", name);
	
	nh = new_adbnamehook(adb, NULL);
	if (nh == NULL) {
		name->partial_result |= DNS_ADBFIND_INET6; /* clear for AAAA */
		goto fail;
	}

	isc_sockaddr_fromin6(&sockaddr, &a6ctx->in6addr, 53);

	if (IN6_IS_ADDR_V4MAPPED(&sockaddr.type.sin6.sin6_addr)
	    || IN6_IS_ADDR_V4COMPAT(&sockaddr.type.sin6.sin6_addr)) {
		DP(1, "Ignoring IPv6 mapped IPv4 address");
		goto fail;
	}

	foundentry = find_entry_and_lock(adb, &sockaddr, &addr_bucket);
	if (foundentry == NULL) {
		dns_adbentry_t *entry;
		entry = new_adbentry(adb);
		if (entry == NULL) {
			name->partial_result |= DNS_ADBFIND_INET6;
			goto fail;
		}

		entry->sockaddr = sockaddr;
		entry->refcnt = 1;
		nh->entry = entry;
		link_entry(adb, addr_bucket, entry);
	} else {
		foundentry->refcnt++;
		nh->entry = foundentry;
	}

	ISC_LIST_APPEND(name->v6, nh, plink);
	nh = NULL;

 fail:
	DP(NCACHE_LEVEL, "expire_v6 set to MIN(%u,%u) in import_v6",
	   name->expire_v6, a6ctx->expiration);
	name->expire_v6 = ISC_MIN(name->expire_v6, a6ctx->expiration);

	name->flags |= NAME_NEEDS_POKE;

	if (nh != NULL)
		free_adbnamehook(adb, &nh);

	if (addr_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->entrylocks[addr_bucket]);
}

/*
 * Requires the name's bucket be locked.
 */
static void
kill_name(dns_adbname_t **n, isc_eventtype_t ev)
{
	dns_adbname_t *name;
	dns_adb_t *adb;

	INSIST(n != NULL);
	name = *n;
	*n = NULL;
	INSIST(DNS_ADBNAME_VALID(name));
	adb = name->adb;
	INSIST(DNS_ADB_VALID(adb));

	DP(DEF_LEVEL, "killing name %p", name);

	/*
	 * If we're dead already, just check to see if we should go
	 * away now or not.
	 */
	if (NAME_DEAD(name) && !NAME_FETCH(name)) {
		unlink_name(adb, name);
		free_adbname(adb, &name);
		return;
	}

	/*
	 * Clean up the name's various lists.  These two are destructive
	 * in that they will always empty the list.
	 */
	clean_finds_at_name(name, ev, DNS_ADBFIND_ADDRESSMASK);
	clean_namehooks(adb, &name->v4);
	clean_namehooks(adb, &name->v6);
	clean_target(adb, &name->target);

	/*
	 * If fetches are running, cancel them.  If none are running, we can
	 * just kill the name here.
	 */
	if (!NAME_FETCH(name)) {
		unlink_name(adb, name);
		free_adbname(adb, &name);
	} else {
		name->flags |= NAME_IS_DEAD;
		cancel_fetches_at_name(name);
	}
}

/*
 * Requires the name's bucket be locked and no entry buckets be locked.
 */
static void
check_expire_namehooks(dns_adbname_t *name, isc_stdtime_t now)
{
	dns_adb_t *adb;

	INSIST(DNS_ADBNAME_VALID(name));
	adb = name->adb;
	INSIST(DNS_ADB_VALID(adb));

	/*
	 * Check to see if we need to remove the v4 addresses
	 */
	if (!NAME_FETCH_V4(name) && EXPIRE_OK(name->expire_v4, now)) {
		if (NAME_HAS_V4(name)) {
			DP(DEF_LEVEL, "expiring v4 for name %p", name);
			clean_namehooks(adb, &name->v4);
			name->partial_result &= ~DNS_ADBFIND_INET;
		}
		name->expire_v4 = INT_MAX;
	}

	/*
	 * Check to see if we need to remove the v6 addresses
	 */
	if (!NAME_FETCH_V6(name) && EXPIRE_OK(name->expire_v6, now)) {
		if (NAME_HAS_V6(name)) {
			DP(DEF_LEVEL, "expiring v6 for name %p", name);
			clean_namehooks(adb, &name->v6);
			name->partial_result &= ~DNS_ADBFIND_INET6;
		}
		name->expire_v6 = INT_MAX;
	}

	/*
	 * Check to see if we need to remove the alias target.
	 */
	if (EXPIRE_OK(name->expire_target, now)) {
		clean_target(adb, &name->target);
		name->expire_target = INT_MAX;
	}
}

/*
 * Requires the name's bucket be locked.
 */
static inline void
link_name(dns_adb_t *adb, int bucket, dns_adbname_t *name)
{
	INSIST(name->lock_bucket == DNS_ADB_INVALIDBUCKET);

	ISC_LIST_PREPEND(adb->names[bucket], name, plink);
	name->lock_bucket = bucket;
	adb->name_refcnt[bucket]++;
}

/*
 * Requires the name's bucket be locked.
 */
static inline void
unlink_name(dns_adb_t *adb, dns_adbname_t *name)
{
	int bucket;

	bucket = name->lock_bucket;
	INSIST(bucket != DNS_ADB_INVALIDBUCKET);

	ISC_LIST_UNLINK(adb->names[bucket], name, plink);
	name->lock_bucket = DNS_ADB_INVALIDBUCKET;
	INSIST(adb->name_refcnt[bucket] > 0);
	adb->name_refcnt[bucket]--;
	if (adb->name_sd[bucket] && adb->name_refcnt[bucket] == 0)
		dec_adb_irefcnt(adb);
}

/*
 * Requires the entry's bucket be locked.
 */
static inline void
link_entry(dns_adb_t *adb, int bucket, dns_adbentry_t *entry)
{
	ISC_LIST_PREPEND(adb->entries[bucket], entry, plink);
	entry->lock_bucket = bucket;
	adb->entry_refcnt[bucket]++;
}

/*
 * Requires the entry's bucket be locked.
 */
static inline void
unlink_entry(dns_adb_t *adb, dns_adbentry_t *entry)
{
	int bucket;

	bucket = entry->lock_bucket;
	INSIST(bucket != DNS_ADB_INVALIDBUCKET);

	ISC_LIST_UNLINK(adb->entries[bucket], entry, plink);
	entry->lock_bucket = DNS_ADB_INVALIDBUCKET;
	INSIST(adb->entry_refcnt[bucket] > 0);
	adb->entry_refcnt[bucket]--;
	if (adb->entry_sd[bucket] && adb->entry_refcnt[bucket] == 0)
		dec_adb_irefcnt(adb);
}

static inline void
violate_locking_hierarchy(isc_mutex_t *have, isc_mutex_t *want)
{
	if (isc_mutex_trylock(want) != ISC_R_SUCCESS) {
		UNLOCK(have);
		LOCK(want);
		LOCK(have);
	}
}

/*
 * The ADB _MUST_ be locked before calling.  Also, exit conditions must be
 * checked after calling this function.
 */
static void
shutdown_names(dns_adb_t *adb)
{
	int bucket;
	dns_adbname_t *name;
	dns_adbname_t *next_name;

	for (bucket = 0 ; bucket < NBUCKETS ; bucket++) {
		LOCK(&adb->namelocks[bucket]);
		adb->name_sd[bucket] = ISC_TRUE;

		name = ISC_LIST_HEAD(adb->names[bucket]);
		if (name == NULL) {
			/*
			 * This bucket has no names.  We must decrement the
			 * irefcnt ourselves, since it will not be
			 * automatically triggered by a name being unlinked.
			 */
			dec_adb_irefcnt(adb);
		} else {
			/*
			 * Run through the list.  For each name, clean up finds
			 * found there, and cancel any fetches running.  When
			 * all the fetches are canceled, the name will destroy
			 * itself.
			 */
			while (name != NULL) {
				next_name = ISC_LIST_NEXT(name, plink);
				kill_name(&name, DNS_EVENT_ADBSHUTDOWN);
				name = next_name;
			}
		}

		UNLOCK(&adb->namelocks[bucket]);
	}
}

/*
 * The ADB _MUST_ be locked before calling.  Also, exit conditions must be
 * checked after calling this function.
 */
static void
shutdown_entries(dns_adb_t *adb)
{
	int bucket;
	dns_adbentry_t *entry;
	dns_adbentry_t *next_entry;

	for (bucket = 0 ; bucket < NBUCKETS ; bucket++) {
		LOCK(&adb->entrylocks[bucket]);
		adb->entry_sd[bucket] = ISC_TRUE;

		entry = ISC_LIST_HEAD(adb->entries[bucket]);
		if (entry == NULL) {
			/*
			 * This bucket has no entries.  We must decrement the
			 * irefcnt ourselves, since it will not be
			 * automatically triggered by an entry being unlinked.
			 */
			dec_adb_irefcnt(adb);
		} else {
			/*
			 * Run through the list.  Cleanup any entries not
			 * associated with names, and which are not in use.
			 */
			while (entry != NULL) {
				next_entry = ISC_LIST_NEXT(entry, plink);
				if (entry->refcnt == 0 &&
				    entry->expires != 0) {
					unlink_entry(adb, entry);
					free_adbentry(adb, &entry);
				}
				entry = next_entry;
			}
		}

		UNLOCK(&adb->entrylocks[bucket]);
	}
}

/*
 * Name bucket must be locked
 */
static void
cancel_fetches_at_name(dns_adbname_t *name)
{
	dns_adbfetch6_t *fetch6;

	if (NAME_FETCH_A(name))
	    dns_resolver_cancelfetch(name->fetch_a->fetch);
				     

	if (NAME_FETCH_AAAA(name))
	    dns_resolver_cancelfetch(name->fetch_aaaa->fetch);
				     

	fetch6 = ISC_LIST_HEAD(name->fetches_a6);
	while (fetch6 != NULL) {
		dns_resolver_cancelfetch(fetch6->fetch);
		fetch6 = ISC_LIST_NEXT(fetch6, plink);
	}
}

/*
 * Assumes the name bucket is locked.
 */
static void
clean_namehooks(dns_adb_t *adb, dns_adbnamehooklist_t *namehooks)
{
	dns_adbentry_t *entry;
	dns_adbnamehook_t *namehook;
	int addr_bucket;

	addr_bucket = DNS_ADB_INVALIDBUCKET;
	namehook = ISC_LIST_HEAD(*namehooks);
	while (namehook != NULL) {
		INSIST(DNS_ADBNAMEHOOK_VALID(namehook));

		/*
		 * Clean up the entry if needed.
		 */
		entry = namehook->entry;
		if (entry != NULL) {
			INSIST(DNS_ADBENTRY_VALID(entry));

			if (addr_bucket != entry->lock_bucket) {
				if (addr_bucket != DNS_ADB_INVALIDBUCKET)
					UNLOCK(&adb->entrylocks[addr_bucket]);
				addr_bucket = entry->lock_bucket;
				LOCK(&adb->entrylocks[addr_bucket]);
			}

			dec_entry_refcnt(adb, entry, ISC_FALSE);
		}

		/*
		 * Free the namehook
		 */
		namehook->entry = NULL;
		ISC_LIST_UNLINK(*namehooks, namehook, plink);
		free_adbnamehook(adb, &namehook);

		namehook = ISC_LIST_HEAD(*namehooks);
	}

	if (addr_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->entrylocks[addr_bucket]);
}

static void
clean_target(dns_adb_t *adb, dns_name_t *target) {
	if (dns_name_countlabels(target) > 0) {
		dns_name_free(target, adb->mctx);
		dns_name_init(target, NULL);
	}
}

static isc_result_t
set_target(dns_adb_t *adb, dns_name_t *name, dns_name_t *fname,
	   dns_rdataset_t *rdataset, dns_name_t *target)
{
	isc_result_t result;
	dns_namereln_t namereln;
	unsigned int nlabels, nbits;
	int order;
	dns_rdata_t rdata;
	isc_region_t r;
	dns_name_t tname;
	dns_fixedname_t fixed1, fixed2;
	dns_name_t *prefix, *new_target;

	REQUIRE(dns_name_countlabels(target) == 0);

	if (rdataset->type == dns_rdatatype_cname) {
		/*
		 * Copy the CNAME's target into the target name.
		 */
		result = dns_rdataset_first(rdataset);
		if (result != ISC_R_SUCCESS)
			return (result);
		dns_rdataset_current(rdataset, &rdata);
		r.base = rdata.data;
		r.length = rdata.length;
		dns_name_init(&tname, NULL);
		dns_name_fromregion(&tname, &r);
		result = dns_name_dup(&tname, adb->mctx, target);
		if (result != ISC_R_SUCCESS)
			return (result);
	} else {
		INSIST(rdataset->type == dns_rdatatype_dname);
		namereln = dns_name_fullcompare(name, fname, &order,
						&nlabels, &nbits);
		INSIST(namereln == dns_namereln_subdomain);
		/*
		 * Get the target name of the DNAME.
		 */
		result = dns_rdataset_first(rdataset);
		if (result != ISC_R_SUCCESS)
			return (result);
		dns_rdataset_current(rdataset, &rdata);
		r.base = rdata.data;
		r.length = rdata.length;
		dns_name_init(&tname, NULL);
		dns_name_fromregion(&tname, &r);
		/*
		 * Construct the new target name.
		 */
		dns_fixedname_init(&fixed1);
		prefix = dns_fixedname_name(&fixed1);
		dns_fixedname_init(&fixed2);
		new_target = dns_fixedname_name(&fixed2);
		result = dns_name_split(name, nlabels, nbits, prefix, NULL);
		if (result != ISC_R_SUCCESS)
			return (result);
		result = dns_name_concatenate(prefix, &tname, new_target,
					      NULL);
		if (result != ISC_R_SUCCESS)
			return (result);
		result = dns_name_dup(new_target, adb->mctx, target);
		if (result != ISC_R_SUCCESS)
			return (result);
	}

	return (ISC_R_SUCCESS);
}

/*
 * Assumes nothing is locked, since this is called by the client.
 */
static void
event_free(isc_event_t *event)
{
	dns_adbfind_t *find;

	INSIST(event != NULL);
	find = event->destroy_arg;
	INSIST(DNS_ADBFIND_VALID(find));

	LOCK(&find->lock);
	find->flags |= FIND_EVENT_FREED;
	event->destroy_arg = NULL;
	UNLOCK(&find->lock);
}

/*
 * Assumes the name bucket is locked.
 */
static void
clean_finds_at_name(dns_adbname_t *name, isc_eventtype_t evtype,
		    unsigned int addrs)
{
	isc_event_t *ev;
	isc_task_t *task;
	dns_adbfind_t *find;
	dns_adbfind_t *next_find;
	isc_boolean_t process;
	unsigned int wanted;

	DP(ENTER_LEVEL,
	   "ENTER clean_finds_at_name, name %p, evtype %08x, addrs %08x",
	   name, evtype, addrs);

	find = ISC_LIST_HEAD(name->finds);
	while (find != NULL) {
		LOCK(&find->lock);
		next_find = ISC_LIST_NEXT(find, plink);

		process = ISC_FALSE;
		wanted = find->flags & DNS_ADBFIND_ADDRESSMASK;

		switch (evtype) {
		case DNS_EVENT_ADBMOREADDRESSES:
			DP(3, "DNS_EVENT_ADBMOREADDRESSES");
			if ((wanted & addrs) != 0) {
				DP(3, "processing");
				find->flags &= ~addrs;
				process = ISC_TRUE;
			}
			break;
		case DNS_EVENT_ADBNOMOREADDRESSES:
			DP(3, "DNS_EVENT_ADBNOMOREADDRESSES");
			find->flags &= ~addrs;
			wanted = find->flags & DNS_ADBFIND_ADDRESSMASK;
			if (wanted == 0) {
				process = ISC_TRUE;
				DP(3, "processing");
			}
			break;
		default:
			find->flags &= ~addrs;
			process = ISC_TRUE;
		}

		if (process) {
			DP(DEF_LEVEL, "cfan: processing find %p", find);
			/*
			 * Unlink the find from the name, letting the caller
			 * call dns_adb_destroyfind() on it to clean it up
			 * later.
			 */
			ISC_LIST_UNLINK(name->finds, find, plink);
			find->adbname = NULL;
			find->name_bucket = DNS_ADB_INVALIDBUCKET;

			INSIST(!FIND_EVENTSENT(find));

			ev = &find->event;
			task = ev->sender;
			ev->sender = find;
			ev->type = evtype;
			ev->destroy = event_free;
			ev->destroy_arg = find;

			DP(DEF_LEVEL,
			   "Sending event %p to task %p for find %p",
			   ev, task, find);

			isc_task_sendanddetach(&task, &ev);
		} else {
			DP(DEF_LEVEL, "cfan: skipping find %p", find);
		}

		UNLOCK(&find->lock);
		find = next_find;
	}

	DP(ENTER_LEVEL, "EXIT clean_finds_at_name, name %p", name);
}

static inline void
check_exit(dns_adb_t *adb)
{
	isc_event_t *event, *next_event;
	isc_task_t *etask;
	isc_boolean_t zeroirefcnt;

	/*
	 * The caller must be holding the adb lock.
	 */

	LOCK(&adb->ilock);
	if (adb->irefcnt == 0)
		zeroirefcnt = ISC_TRUE;
	else
		zeroirefcnt = ISC_FALSE;
	UNLOCK(&adb->ilock);

	if (adb->shutting_down && zeroirefcnt &&
	    isc_mempool_getallocated(adb->ahmp) == 0) {
		/*
		 * We're now shutdown.  Send any whenshutdown events.
		 */
		for (event = ISC_LIST_HEAD(adb->whenshutdown);
		     event != NULL;
		     event = next_event) {
			next_event = ISC_LIST_NEXT(event, link);
			ISC_LIST_UNLINK(adb->whenshutdown, event, link);
			etask = event->sender;
			event->sender = adb;
			isc_task_sendanddetach(&etask, &event);
		}
		/*
		 * If there aren't any external references either, we're
		 * done.  Send the control event to initiate shutdown.
		 */
		if (adb->erefcnt == 0) {
			INSIST(!adb->cevent_sent);	/* Sanity check. */
			event = &adb->cevent;
			isc_task_send(adb->task, &event);
			adb->cevent_sent = ISC_TRUE;
		}
	}
}

static inline void
dec_adb_irefcnt(dns_adb_t *adb)
{
	LOCK(&adb->ilock);

	INSIST(adb->irefcnt > 0);
	adb->irefcnt--;

	UNLOCK(&adb->ilock);
}

static inline void
inc_adb_erefcnt(dns_adb_t *adb, isc_boolean_t lock)
{
	if (lock)
		LOCK(&adb->lock);

	adb->erefcnt++;

	if (lock)
		UNLOCK(&adb->lock);
}

static inline void
dec_adb_erefcnt(dns_adb_t *adb, isc_boolean_t lock)
{
	if (lock)
		LOCK(&adb->lock);

	INSIST(adb->erefcnt > 0);
	adb->erefcnt--;

	if (adb->erefcnt == 0)
		check_exit(adb);

	if (lock)
		UNLOCK(&adb->lock);
}

static inline void
inc_entry_refcnt(dns_adb_t *adb, dns_adbentry_t *entry, isc_boolean_t lock)
{
	int bucket;

	bucket = entry->lock_bucket;

	if (lock)
		LOCK(&adb->entrylocks[bucket]);

	entry->refcnt++;

	if (lock)
		UNLOCK(&adb->entrylocks[bucket]);
}

static inline void
dec_entry_refcnt(dns_adb_t *adb, dns_adbentry_t *entry, isc_boolean_t lock)
{
	int bucket;
	isc_boolean_t destroy_entry;

	bucket = entry->lock_bucket;

	if (lock)
		LOCK(&adb->entrylocks[bucket]);

	INSIST(entry->refcnt > 0);
	entry->refcnt--;

	destroy_entry = ISC_FALSE;
	if (entry->refcnt == 0 &&
	    (adb->entry_sd[bucket] || entry->expires == 0)) {
		destroy_entry = ISC_TRUE;
		unlink_entry(adb, entry);
	}

	if (lock)
		UNLOCK(&adb->entrylocks[bucket]);

	if (!destroy_entry)
		return;

	entry->lock_bucket = DNS_ADB_INVALIDBUCKET;

	free_adbentry(adb, &entry);
}

static inline dns_adbname_t *
new_adbname(dns_adb_t *adb, dns_name_t *dnsname)
{
	dns_adbname_t *name;

	name = isc_mempool_get(adb->nmp);
	if (name == NULL)
		return (NULL);

	dns_name_init(&name->name, NULL);
	if (dns_name_dup(dnsname, adb->mctx, &name->name) != ISC_R_SUCCESS) {
		isc_mempool_put(adb->nmp, name);
		return (NULL);
	}
	dns_name_init(&name->target, NULL);
	name->magic = DNS_ADBNAME_MAGIC;
	name->adb = adb;
	name->partial_result = 0;
	name->flags = 0;
	name->expire_v4 = INT_MAX;
	name->expire_v6 = INT_MAX;
	name->expire_target = INT_MAX;
	name->chains = 0;
	name->lock_bucket = DNS_ADB_INVALIDBUCKET;
	ISC_LIST_INIT(name->v4);
	ISC_LIST_INIT(name->v6);
	name->fetch_a = NULL;
	name->fetch_aaaa = NULL;
	ISC_LIST_INIT(name->fetches_a6);
	ISC_LIST_INIT(name->finds);
	ISC_LINK_INIT(name, plink);

	return (name);
}

static inline void
free_adbname(dns_adb_t *adb, dns_adbname_t **name)
{
	dns_adbname_t *n;

	INSIST(name != NULL && DNS_ADBNAME_VALID(*name));
	n = *name;
	*name = NULL;

	INSIST(!NAME_HAS_V4(n));
	INSIST(!NAME_HAS_V6(n));
	INSIST(!NAME_FETCH(n));
	INSIST(ISC_LIST_EMPTY(n->finds));
	INSIST(!ISC_LINK_LINKED(n, plink));
	INSIST(n->lock_bucket == DNS_ADB_INVALIDBUCKET);
	INSIST(n->adb == adb);

	n->magic = 0;
	dns_name_free(&n->name, adb->mctx);

	isc_mempool_put(adb->nmp, n);
}

static inline dns_adbnamehook_t *
new_adbnamehook(dns_adb_t *adb, dns_adbentry_t *entry)
{
	dns_adbnamehook_t *nh;

	nh = isc_mempool_get(adb->nhmp);
	if (nh == NULL)
		return (NULL);

	nh->magic = DNS_ADBNAMEHOOK_MAGIC;
	nh->entry = entry;
	ISC_LINK_INIT(nh, plink);

	return (nh);
}

static inline void
free_adbnamehook(dns_adb_t *adb, dns_adbnamehook_t **namehook)
{
	dns_adbnamehook_t *nh;

	INSIST(namehook != NULL && DNS_ADBNAMEHOOK_VALID(*namehook));
	nh = *namehook;
	*namehook = NULL;

	INSIST(nh->entry == NULL);
	INSIST(!ISC_LINK_LINKED(nh, plink));

	nh->magic = 0;
	isc_mempool_put(adb->nhmp, nh);
}

static inline dns_adbzoneinfo_t *
new_adbzoneinfo(dns_adb_t *adb, dns_name_t *zone)
{
	dns_adbzoneinfo_t *zi;

	zi = isc_mempool_get(adb->zimp);
	if (zi == NULL)
		return (NULL);

	dns_name_init(&zi->zone, NULL);
	if (dns_name_dup(zone, adb->mctx, &zi->zone) != ISC_R_SUCCESS) {
		isc_mempool_put(adb->zimp, zi);
		return (NULL);
	}

	zi->magic = DNS_ADBZONEINFO_MAGIC;
	zi->lame_timer = 0;
	ISC_LINK_INIT(zi, plink);

	return (zi);
}

static inline void
free_adbzoneinfo(dns_adb_t *adb, dns_adbzoneinfo_t **zoneinfo)
{
	dns_adbzoneinfo_t *zi;

	INSIST(zoneinfo != NULL && DNS_ADBZONEINFO_VALID(*zoneinfo));
	zi = *zoneinfo;
	*zoneinfo = NULL;

	INSIST(!ISC_LINK_LINKED(zi, plink));

	dns_name_free(&zi->zone, adb->mctx);

	zi->magic = 0;

	isc_mempool_put(adb->zimp, zi);
}

static inline dns_adbentry_t *
new_adbentry(dns_adb_t *adb)
{
	dns_adbentry_t *e;
	isc_uint32_t r;

	e = isc_mempool_get(adb->emp);
	if (e == NULL)
		return (NULL);

	e->magic = DNS_ADBENTRY_MAGIC;
	e->lock_bucket = DNS_ADB_INVALIDBUCKET;
	e->refcnt = 0;
	e->flags = 0;
	e->edns_level = -1;
	e->goodness = 0;
	isc_random_get(&adb->rand, &r);
	e->srtt = (r & 0x1f) + 1;
	e->expires = 0;
	ISC_LIST_INIT(e->zoneinfo);
	ISC_LINK_INIT(e, plink);

	return (e);
}

static inline void
free_adbentry(dns_adb_t *adb, dns_adbentry_t **entry)
{
	dns_adbentry_t *e;
	dns_adbzoneinfo_t *zi;

	INSIST(entry != NULL && DNS_ADBENTRY_VALID(*entry));
	e = *entry;
	*entry = NULL;

	INSIST(e->lock_bucket == DNS_ADB_INVALIDBUCKET);
	INSIST(e->refcnt == 0);
	INSIST(!ISC_LINK_LINKED(e, plink));

	e->magic = 0;

	zi = ISC_LIST_HEAD(e->zoneinfo);
	while (zi != NULL) {
		ISC_LIST_UNLINK(e->zoneinfo, zi, plink);
		free_adbzoneinfo(adb, &zi);
		zi = ISC_LIST_HEAD(e->zoneinfo);
	}

	isc_mempool_put(adb->emp, e);
}

static inline dns_adbfind_t *
new_adbfind(dns_adb_t *adb)
{
	dns_adbfind_t *h;
	isc_result_t result;

	h = isc_mempool_get(adb->ahmp);
	if (h == NULL)
		return (NULL);

	/*
	 * public members
	 */
	h->magic = 0;
	h->adb = adb;
	h->partial_result = 0;
	h->options = 0;
	h->flags = 0;
	ISC_LINK_INIT(h, publink);
	ISC_LINK_INIT(h, plink);
	ISC_LIST_INIT(h->list);
	h->adbname = NULL;
	h->name_bucket = DNS_ADB_INVALIDBUCKET;

	/*
	 * private members
	 */
	result = isc_mutex_init(&h->lock);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init failed in new_adbfind()");
		isc_mempool_put(adb->ahmp, h);
		return (NULL);
	}

	ISC_EVENT_INIT(&h->event, sizeof (isc_event_t), 0, 0, 0, NULL, NULL,
		       NULL, NULL, h);

	h->magic = DNS_ADBFIND_MAGIC;
	return (h);
}

static inline dns_adbfetch_t *
new_adbfetch(dns_adb_t *adb)
{
	dns_adbfetch_t *f;

	f = isc_mempool_get(adb->afmp);
	if (f == NULL)
		return (NULL);

	f->magic = 0;
	f->namehook = NULL;
	f->entry = NULL;
	f->fetch = NULL;

	f->namehook = new_adbnamehook(adb, NULL);
	if (f->namehook == NULL)
		goto err;

	f->entry = new_adbentry(adb);
	if (f->entry == NULL)
		goto err;

	dns_rdataset_init(&f->rdataset);

	f->magic = DNS_ADBFETCH_MAGIC;

	return (f);

 err:
	if (f->namehook != NULL)
		free_adbnamehook(adb, &f->namehook);
	if (f->entry != NULL)
		free_adbentry(adb, &f->entry);
	isc_mempool_put(adb->afmp, f);
	return (NULL);
}

static inline void
free_adbfetch(dns_adb_t *adb, dns_adbfetch_t **fetch)
{
	dns_adbfetch_t *f;

	INSIST(fetch != NULL && DNS_ADBFETCH_VALID(*fetch));
	f = *fetch;
	*fetch = NULL;

	f->magic = 0;

	if (f->namehook != NULL)
		free_adbnamehook(adb, &f->namehook);
	if (f->entry != NULL)
		free_adbentry(adb, &f->entry);

	if (dns_rdataset_isassociated(&f->rdataset))
		dns_rdataset_disassociate(&f->rdataset);

	isc_mempool_put(adb->afmp, f);
}

/*
 * Caller must be holding the name lock.
 */
static isc_result_t
a6find(void *arg, dns_name_t *a6name, dns_rdatatype_t type, isc_stdtime_t now,
       dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	dns_adbname_t *name;
	dns_adb_t *adb;

	name = arg;
	INSIST(DNS_ADBNAME_VALID(name));
	adb = name->adb;
	INSIST(DNS_ADB_VALID(adb));
	
	return (dns_view_simplefind(adb->view, a6name, type, now,
				    DNS_DBFIND_GLUEOK, ISC_FALSE,
				    rdataset, sigrdataset));
}

/*
 * Caller must be holding the name lock.
 */
static void
a6missing(dns_a6context_t *a6ctx, dns_name_t *a6name) {
	dns_adbname_t *name;
	dns_adb_t *adb;
	dns_adbfetch6_t *fetch;
	isc_result_t result;
	
	name = a6ctx->arg;
	INSIST(DNS_ADBNAME_VALID(name));
	adb = name->adb;
	INSIST(DNS_ADB_VALID(adb));

	fetch = new_adbfetch6(adb, name, a6ctx);
	if (fetch == NULL) {
		name->partial_result |= DNS_ADBFIND_INET6;
		return;
	}

	result = dns_resolver_createfetch(adb->view->resolver, a6name,
					  dns_rdatatype_a6,
					  NULL, NULL, NULL, 0,
					  adb->task, fetch_callback_a6,
					  name, &fetch->rdataset, NULL,
					  &fetch->fetch);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	name->chains = a6ctx->chains;
	ISC_LIST_APPEND(name->fetches_a6, fetch, plink);

 cleanup:
	if (result != ISC_R_SUCCESS) {
		free_adbfetch6(adb, &fetch);
		name->partial_result |= DNS_ADBFIND_INET6;
	}
}

static inline dns_adbfetch6_t *
new_adbfetch6(dns_adb_t *adb, dns_adbname_t *name, dns_a6context_t *a6ctx)
{
	dns_adbfetch6_t *f;

	f = isc_mempool_get(adb->af6mp);
	if (f == NULL)
		return (NULL);

	f->magic = 0;
	f->namehook = NULL;
	f->entry = NULL;
	f->fetch = NULL;
	f->flags = 0;

	f->namehook = new_adbnamehook(adb, NULL);
	if (f->namehook == NULL)
		goto err;

	f->entry = new_adbentry(adb);
	if (f->entry == NULL)
		goto err;

	dns_rdataset_init(&f->rdataset);

	dns_a6_init(&f->a6ctx, a6find, NULL, import_a6,
		    a6missing, name);
	if (a6ctx != NULL)
		dns_a6_copy(a6ctx, &f->a6ctx);

	f->magic = DNS_ADBFETCH6_MAGIC;

	return (f);

 err:
	if (f->namehook != NULL)
		free_adbnamehook(adb, &f->namehook);
	if (f->entry != NULL)
		free_adbentry(adb, &f->entry);
	isc_mempool_put(adb->af6mp, f);
	return (NULL);
}

static inline void
free_adbfetch6(dns_adb_t *adb, dns_adbfetch6_t **fetch)
{
	dns_adbfetch6_t *f;

	INSIST(fetch != NULL && DNS_ADBFETCH6_VALID(*fetch));
	f = *fetch;
	*fetch = NULL;

	f->magic = 0;

	if (f->namehook != NULL)
		free_adbnamehook(adb, &f->namehook);
	if (f->entry != NULL)
		free_adbentry(adb, &f->entry);

	if (dns_rdataset_isassociated(&f->rdataset))
		dns_rdataset_disassociate(&f->rdataset);

	isc_mempool_put(adb->af6mp, f);
}

static inline void
free_adbfind(dns_adb_t *adb, dns_adbfind_t **findp)
{
	dns_adbfind_t *find;

	INSIST(findp != NULL && DNS_ADBFIND_VALID(*findp));
	find = *findp;
	*findp = NULL;

	INSIST(!FIND_HAS_ADDRS(find));
	INSIST(!ISC_LINK_LINKED(find, publink));
	INSIST(!ISC_LINK_LINKED(find, plink));
	INSIST(find->name_bucket == DNS_ADB_INVALIDBUCKET);
	INSIST(find->adbname == NULL);

	find->magic = 0;

	isc_mutex_destroy(&find->lock);
	isc_mempool_put(adb->ahmp, find);
}

/*
 * Copy bits from the entry into the newly allocated addrinfo.  The entry
 * must be locked, and the reference count must be bumped up by one
 * if this function returns a valid pointer.
 */
static inline dns_adbaddrinfo_t *
new_adbaddrinfo(dns_adb_t *adb, dns_adbentry_t *entry)
{
	dns_adbaddrinfo_t *ai;

	ai = isc_mempool_get(adb->aimp);
	if (ai == NULL)
		return (NULL);

	ai->magic = DNS_ADBADDRINFO_MAGIC;
	ai->sockaddr = &entry->sockaddr;
	ai->goodness = entry->goodness;
	ai->srtt = entry->srtt;
	ai->flags = entry->flags;
	ai->entry = entry;
	ISC_LINK_INIT(ai, publink);

	return (ai);
}

static inline void
free_adbaddrinfo(dns_adb_t *adb, dns_adbaddrinfo_t **ainfo)
{
	dns_adbaddrinfo_t *ai;

	INSIST(ainfo != NULL && DNS_ADBADDRINFO_VALID(*ainfo));
	ai = *ainfo;
	*ainfo = NULL;

	INSIST(ai->sockaddr == NULL);
	INSIST(ai->entry == NULL);
	INSIST(!ISC_LINK_LINKED(ai, publink));

	ai->magic = 0;

	isc_mempool_put(adb->aimp, ai);
}

/*
 * Search for the name.  NOTE:  The bucket is kept locked on both
 * success and failure, so it must always be unlocked by the caller!
 *
 * On the first call to this function, *bucketp must be set to
 * DNS_ADB_INVALIDBUCKET.
 */
static inline dns_adbname_t *
find_name_and_lock(dns_adb_t *adb, dns_name_t *name, int *bucketp)
{
	dns_adbname_t *adbname;
	int bucket;

	bucket = dns_name_hash(name, ISC_FALSE) % NBUCKETS;

	if (*bucketp == DNS_ADB_INVALIDBUCKET) {
		LOCK(&adb->namelocks[bucket]);
		*bucketp = bucket;
	} else if (*bucketp != bucket) {
		UNLOCK(&adb->namelocks[*bucketp]);
		LOCK(&adb->namelocks[bucket]);
		*bucketp = bucket;
	}

	adbname = ISC_LIST_HEAD(adb->names[bucket]);
	while (adbname != NULL) {
		if (!NAME_DEAD(adbname)) {
			if (dns_name_equal(name, &adbname->name))
				return (adbname);
		}
		adbname = ISC_LIST_NEXT(adbname, plink);
	}

	return (NULL);
}

/*
 * Search for the address.  NOTE:  The bucket is kept locked on both
 * success and failure, so it must always be unlocked by the caller.
 *
 * On the first call to this function, *bucketp must be set to
 * DNS_ADB_INVALIDBUCKET.  This will cause a lock to occur.  On
 * later calls (within the same "lock path") it can be left alone, so
 * if this function is called multiple times locking is only done if
 * the bucket changes.
 */
static inline dns_adbentry_t *
find_entry_and_lock(dns_adb_t *adb, isc_sockaddr_t *addr, int *bucketp)
{
	dns_adbentry_t *entry;
	int bucket;

	bucket = isc_sockaddr_hash(addr, ISC_TRUE) % NBUCKETS;

	if (*bucketp == DNS_ADB_INVALIDBUCKET) {
		LOCK(&adb->entrylocks[bucket]);
		*bucketp = bucket;
	} else if (*bucketp != bucket) {
		UNLOCK(&adb->entrylocks[*bucketp]);
		LOCK(&adb->entrylocks[bucket]);
		*bucketp = bucket;
	}

	entry = ISC_LIST_HEAD(adb->entries[bucket]);
	while (entry != NULL) {
		if (isc_sockaddr_equal(addr, &entry->sockaddr))
			return (entry);
		entry = ISC_LIST_NEXT(entry, plink);
	}

	return (NULL);
}

/*
 * Entry bucket MUST be locked!
 */
static isc_boolean_t
entry_is_bad_for_zone(dns_adb_t *adb, dns_adbentry_t *entry, dns_name_t *zone,
		      isc_stdtime_t now)
{
	dns_adbzoneinfo_t *zi, *next_zi;
	isc_boolean_t is_bad;

	is_bad = ISC_FALSE;

	zi = ISC_LIST_HEAD(entry->zoneinfo);
	if (zi == NULL)
		return (ISC_FALSE);
	while (zi != NULL) {
		next_zi = ISC_LIST_NEXT(zi, plink);

		/*
		 * Has the entry expired?
		 */
		if (zi->lame_timer < now) {
			ISC_LIST_UNLINK(entry->zoneinfo, zi, plink);
			free_adbzoneinfo(adb, &zi);
		}

		/*
		 * Order tests from least to most expensive.
		 */
		if (zi != NULL && !is_bad) {
			if (dns_name_equal(zone, &zi->zone))
				is_bad = ISC_TRUE;
		}

		zi = next_zi;
	}
	
	return (is_bad);
}

static void
copy_namehook_lists(dns_adb_t *adb, dns_adbfind_t *find, dns_name_t *zone,
		    dns_adbname_t *name, isc_stdtime_t now)
{
	dns_adbnamehook_t *namehook;
	dns_adbaddrinfo_t *addrinfo;
	int bucket;

	bucket = DNS_ADB_INVALIDBUCKET;

	if (find->options & DNS_ADBFIND_INET) {
		namehook = ISC_LIST_HEAD(name->v4);
		while (namehook != NULL) {
			bucket = namehook->entry->lock_bucket;
			LOCK(&adb->entrylocks[bucket]);
			if (entry_is_bad_for_zone(adb, namehook->entry,
						  zone, now))
				goto nextv4;
			addrinfo = new_adbaddrinfo(adb, namehook->entry);
			if (addrinfo == NULL) {
				find->partial_result |= DNS_ADBFIND_INET;
				goto out;
			}
			/*
			 * Found a valid entry.  Add it to the find's list.
			 */
			inc_entry_refcnt(adb, namehook->entry, ISC_FALSE);
			ISC_LIST_APPEND(find->list, addrinfo, publink);
			addrinfo = NULL;
		nextv4:
			UNLOCK(&adb->entrylocks[bucket]);
			bucket = DNS_ADB_INVALIDBUCKET;
			namehook = ISC_LIST_NEXT(namehook, plink);
		}
	}

	if (find->options & DNS_ADBFIND_INET6) {
		namehook = ISC_LIST_HEAD(name->v6);
		while (namehook != NULL) {
			bucket = namehook->entry->lock_bucket;
			LOCK(&adb->entrylocks[bucket]);
			if (entry_is_bad_for_zone(adb, namehook->entry,
						  zone, now))
				goto nextv6;
			addrinfo = new_adbaddrinfo(adb, namehook->entry);
			if (addrinfo == NULL) {
				find->partial_result |= DNS_ADBFIND_INET6;
				goto out;
			}
			/*
			 * Found a valid entry.  Add it to the find's list.
			 */
			inc_entry_refcnt(adb, namehook->entry, ISC_FALSE);
			ISC_LIST_APPEND(find->list, addrinfo, publink);
			addrinfo = NULL;
		nextv6:
			UNLOCK(&adb->entrylocks[bucket]);
			bucket = DNS_ADB_INVALIDBUCKET;
			namehook = ISC_LIST_NEXT(namehook, plink);
		}
	}

 out:
	if (bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->entrylocks[bucket]);
}

static void
shutdown_task(isc_task_t *task, isc_event_t *ev)
{
	dns_adb_t *adb;

	(void)task;  /* not used */

	adb = ev->arg;
	INSIST(DNS_ADB_VALID(adb));

	/*
	 * Kill the timer, and then the ADB itself.  Note that this implies
	 * that this task was the one scheduled to get timer events.  If
	 * this is not true (and it is unfortunate there is no way to INSIST()
	 * this) badness will occur.
	 */
	LOCK(&adb->lock);
	isc_timer_detach(&adb->timer);
	UNLOCK(&adb->lock);
	isc_event_free(&ev);
	destroy(adb);
}

/*
 * name bucket must be locked; adb may be locked; no other locks held.
 */
static void
check_expire_name(dns_adbname_t **namep, isc_stdtime_t now)
{
	dns_adbname_t *name;

	INSIST(namep != NULL && DNS_ADBNAME_VALID(*namep));
	name = *namep;
	*namep = NULL;

	if (NAME_HAS_V4(name) || NAME_HAS_V6(name))
		return;
	if (NAME_FETCH(name))
		return;
	if (!EXPIRE_OK(name->expire_v4, now))
		return;
	if (!EXPIRE_OK(name->expire_v6, now))
		return;
	if (!EXPIRE_OK(name->expire_target, now))
		return;

	/*
	 * The name is empty.  Delete it.
	 */
	kill_name(&name, DNS_EVENT_ADBEXPIRED);

	/*
	 * Our caller, or one of its callers, will be calling check_exit() at
	 * some point, so we don't need to do it here.
	 */
}

/*
 * entry bucket must be locked; adb may be locked; no other locks held.
 */
static void
check_expire_entry(dns_adb_t *adb, dns_adbentry_t **entryp, isc_stdtime_t now)
{
	dns_adbentry_t *entry;

	INSIST(entryp != NULL && DNS_ADBENTRY_VALID(*entryp));
	entry = *entryp;
	*entryp = NULL;

	if (entry->refcnt != 0)
		return;
	if (entry->expires == 0 || entry->expires > now)
		return;
	
	/*
	 * The entry is not in use.  Delete it.
	 */
	DP(DEF_LEVEL, "killing entry %p", entry);
	INSIST(ISC_LINK_LINKED(entry, plink));
	unlink_entry(adb, entry);
	free_adbentry(adb, &entry);
}

/*
 * ADB must be locked, and no other locks held.
 */
static void
cleanup_names(dns_adb_t *adb, int bucket, isc_stdtime_t now)
{
	dns_adbname_t *name;
	dns_adbname_t *next_name;

	DP(CLEAN_LEVEL, "cleaning name bucket %d", bucket);

	LOCK(&adb->namelocks[bucket]);
	if (adb->name_sd[bucket]) {
		UNLOCK(&adb->namelocks[bucket]);
		return;
	}

	name = ISC_LIST_HEAD(adb->names[bucket]);
	while (name != NULL) {
		next_name = ISC_LIST_NEXT(name, plink);
		check_expire_namehooks(name, now);
		check_expire_name(&name, now);
		name = next_name;
	}
	UNLOCK(&adb->namelocks[bucket]);
}

/*
 * ADB must be locked, and no other locks held.
 */
static void
cleanup_entries(dns_adb_t *adb, int bucket, isc_stdtime_t now)
{
	dns_adbentry_t *entry, *next_entry;

	DP(CLEAN_LEVEL, "cleaning entry bucket %d", bucket);

	LOCK(&adb->entrylocks[bucket]);
	entry = ISC_LIST_HEAD(adb->entries[bucket]);
	while (entry != NULL) {
		next_entry = ISC_LIST_NEXT(entry, plink);
		check_expire_entry(adb, &entry, now);
		entry = next_entry;
	}
	UNLOCK(&adb->entrylocks[bucket]);
}

static void
timer_cleanup(isc_task_t *task, isc_event_t *ev)
{
	dns_adb_t *adb;
	isc_result_t result;
	isc_stdtime_t now;

	(void)task;  /* not used */

	adb = ev->arg;
	INSIST(DNS_ADB_VALID(adb));

	LOCK(&adb->lock);

	isc_stdtime_get(&now);

	/*
	 * Call our cleanup routines.
	 */
	cleanup_names(adb, adb->next_cleanbucket, now);
	cleanup_entries(adb, adb->next_cleanbucket, now);

	/*
	 * Set the next bucket to be cleaned.
	 */
	adb->next_cleanbucket++;
	if (adb->next_cleanbucket >= NBUCKETS) {
		adb->next_cleanbucket = 0;
#ifdef DUMP_ADB_AFTER_CLEANING
		dump_adb(adb, stdout);
#endif
	}

	/*
	 * Reset the timer.
	 */
	result = isc_timer_reset(adb->timer, isc_timertype_once, NULL,
				 &adb->tick_interval, ISC_FALSE);

	UNLOCK(&adb->lock);

	isc_event_free(&ev);
}

static void
destroy(dns_adb_t *adb)
{
	adb->magic = 0;

	/*
	 * The timer is already dead, from the task's shutdown callback.
	 */
	isc_task_detach(&adb->task);

	isc_mempool_destroy(&adb->nmp);
	isc_mempool_destroy(&adb->nhmp);
	isc_mempool_destroy(&adb->zimp);
	isc_mempool_destroy(&adb->emp);
	isc_mempool_destroy(&adb->ahmp);
	isc_mempool_destroy(&adb->aimp);
	isc_mempool_destroy(&adb->afmp);
	isc_mempool_destroy(&adb->af6mp);

	isc_mutexblock_destroy(adb->entrylocks, NBUCKETS);
	isc_mutexblock_destroy(adb->namelocks, NBUCKETS);

	isc_mutex_destroy(&adb->ilock);
	isc_mutex_destroy(&adb->lock);
	isc_mutex_destroy(&adb->mplock);

	isc_random_invalidate(&adb->rand);

	isc_mem_put(adb->mctx, adb, sizeof (dns_adb_t));
}


/*
 * Public functions.
 */

isc_result_t
dns_adb_create(isc_mem_t *mem, dns_view_t *view, isc_timermgr_t *timermgr,
	       isc_taskmgr_t *taskmgr, dns_adb_t **newadb)
{
	dns_adb_t *adb;
	isc_result_t result;
	int i;

	REQUIRE(mem != NULL);
	REQUIRE(view != NULL);
	REQUIRE(timermgr != NULL);
	REQUIRE(taskmgr != NULL);
	REQUIRE(newadb != NULL && *newadb == NULL);

	adb = isc_mem_get(mem, sizeof (dns_adb_t));
	if (adb == NULL)
		return (ISC_R_NOMEMORY);

	/*
	 * Initialize things here that cannot fail, and especially things
	 * that must be NULL for the error return to work properly.
	 */
	adb->magic = 0;
	adb->erefcnt = 1;
	adb->irefcnt = 0;
	adb->nmp = NULL;
	adb->nhmp = NULL;
	adb->zimp = NULL;
	adb->emp = NULL;
	adb->ahmp = NULL;
	adb->aimp = NULL;
	adb->afmp = NULL;
	adb->af6mp = NULL;
	adb->task = NULL;
	adb->timer = NULL;
	adb->mctx = mem;
	adb->view = view;
	adb->timermgr = timermgr;
	adb->taskmgr = taskmgr;
	adb->next_cleanbucket = 0;
	ISC_EVENT_INIT(&adb->cevent, sizeof adb->cevent, 0, NULL,
		       DNS_EVENT_ADBCONTROL, shutdown_task, adb,
		       adb, NULL, NULL);
	adb->cevent_sent = ISC_FALSE;
	adb->shutting_down = ISC_FALSE;
	ISC_LIST_INIT(adb->whenshutdown);

	result = isc_random_init(&adb->rand);
	if (result != ISC_R_SUCCESS)
		goto fail0a;

	result = isc_mutex_init(&adb->lock);
	if (result != ISC_R_SUCCESS)
		goto fail0b;

	result = isc_mutex_init(&adb->mplock);
	if (result != ISC_R_SUCCESS)
		goto fail0c;

	result = isc_mutex_init(&adb->ilock);
	if (result != ISC_R_SUCCESS)
		goto fail0d;

	/*
	 * Initialize the bucket locks for names and elements.
	 * May as well initialize the list heads, too.
	 */
	result = isc_mutexblock_init(adb->namelocks, NBUCKETS);
	if (result != ISC_R_SUCCESS)
		goto fail1;
	for (i = 0 ; i < NBUCKETS ; i++) {
		ISC_LIST_INIT(adb->names[i]);
		adb->name_sd[i] = ISC_FALSE;
		adb->name_refcnt[i] = 0;
		adb->irefcnt++;
	}
	for (i = 0 ; i < NBUCKETS ; i++) {
		ISC_LIST_INIT(adb->entries[i]);
		adb->entry_sd[i] = ISC_FALSE;
		adb->entry_refcnt[i] = 0;
		adb->irefcnt++;
	}
	result = isc_mutexblock_init(adb->entrylocks, NBUCKETS);
	if (result != ISC_R_SUCCESS)
		goto fail2;

	/*
	 * Memory pools
	 */
#define MPINIT(t, p, l, n) do { \
	result = isc_mempool_create(mem, sizeof (t), &(p)); \
	if (result != ISC_R_SUCCESS) \
		goto fail3; \
	isc_mempool_setfreemax((p), FREE_ITEMS); \
	isc_mempool_setfillcount((p), FILL_COUNT); \
	isc_mempool_setname((p), n); \
	if (l) \
		isc_mempool_associatelock((p), &adb->mplock); \
} while (0)

	MPINIT(dns_adbname_t, adb->nmp, ISC_TRUE, "adbname");
	MPINIT(dns_adbnamehook_t, adb->nhmp, ISC_TRUE, "adbnamehook");
	MPINIT(dns_adbzoneinfo_t, adb->zimp, ISC_TRUE, "adbzoneinfo");
	MPINIT(dns_adbentry_t, adb->emp, ISC_TRUE, "adbentry");
	MPINIT(dns_adbfind_t, adb->ahmp, ISC_TRUE, "adbfind");
	MPINIT(dns_adbaddrinfo_t, adb->aimp, ISC_TRUE, "adbaddrinfo");
	MPINIT(dns_adbfetch_t, adb->afmp, ISC_TRUE, "adbfetch");
	MPINIT(dns_adbfetch6_t, adb->af6mp, ISC_TRUE, "adbfetch6");

#undef MPINIT

	/*
	 * Allocate a timer and a task for our periodic cleanup.
	 */
	result = isc_task_create(adb->taskmgr, adb->mctx, 0, &adb->task);
	if (result != ISC_R_SUCCESS)
		goto fail3;
	isc_task_setname(adb->task, "ADB", adb);
	/*
	 * XXXMLG When this is changed to be a config file option,
	 */ 
	isc_interval_set(&adb->tick_interval, CLEAN_SECONDS, 0);
	result = isc_timer_create(adb->timermgr, isc_timertype_once,
				  NULL, &adb->tick_interval, adb->task,
				  timer_cleanup, adb, &adb->timer);
	if (result != ISC_R_SUCCESS)
		goto fail3;

	/*
	 * Normal return.
	 */
	adb->magic = DNS_ADB_MAGIC;
	*newadb = adb;
	return (ISC_R_SUCCESS);

 fail3:
	if (adb->task != NULL)
		isc_task_detach(&adb->task);
	if (adb->timer != NULL)
		isc_timer_detach(&adb->timer);

	/* clean up entrylocks */
	isc_mutexblock_destroy(adb->entrylocks, NBUCKETS);

 fail2: /* clean up namelocks */
	isc_mutexblock_destroy(adb->namelocks, NBUCKETS);

 fail1: /* clean up only allocated memory */
	if (adb->nmp != NULL)
		isc_mempool_destroy(&adb->nmp);
	if (adb->nhmp != NULL)
		isc_mempool_destroy(&adb->nhmp);
	if (adb->zimp != NULL)
		isc_mempool_destroy(&adb->zimp);
	if (adb->emp != NULL)
		isc_mempool_destroy(&adb->emp);
	if (adb->ahmp != NULL)
		isc_mempool_destroy(&adb->ahmp);
	if (adb->aimp != NULL)
		isc_mempool_destroy(&adb->aimp);
	if (adb->afmp != NULL)
		isc_mempool_destroy(&adb->afmp);
	if (adb->af6mp != NULL)
		isc_mempool_destroy(&adb->af6mp);

	isc_mutex_destroy(&adb->ilock);
 fail0d:
	isc_mutex_destroy(&adb->mplock);
 fail0c:
	isc_mutex_destroy(&adb->lock);
 fail0b:
	isc_random_invalidate(&adb->rand);
 fail0a:
	isc_mem_put(mem, adb, sizeof (dns_adb_t));

	return (result);
}

void
dns_adb_detach(dns_adb_t **adbx)
{
	dns_adb_t *adb;

	REQUIRE(adbx != NULL && DNS_ADB_VALID(*adbx));

	adb = *adbx;
	*adbx = NULL;

	LOCK(&adb->lock);
	dec_adb_erefcnt(adb, ISC_FALSE);
	if (adb->erefcnt == 0)
		INSIST(adb->shutting_down);
	UNLOCK(&adb->lock);
}

void
dns_adb_whenshutdown(dns_adb_t *adb, isc_task_t *task, isc_event_t **eventp)
{
	isc_task_t *clone;
	isc_event_t *event;
	isc_boolean_t zeroirefcnt = ISC_FALSE;

	/*
	 * Send '*eventp' to 'task' when 'adb' has shutdown.
	 */

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(eventp != NULL);

	event = *eventp;
	*eventp = NULL;

	LOCK(&adb->lock);

	LOCK(&adb->ilock);
	if (adb->irefcnt == 0)
		zeroirefcnt = ISC_TRUE;
	else
		zeroirefcnt = ISC_FALSE;
	UNLOCK(&adb->ilock);
	
	if (adb->shutting_down && zeroirefcnt &&
	    isc_mempool_getallocated(adb->ahmp) == 0) {
		/*
		 * We're already shutdown.  Send the event.
		 */
		event->sender = adb;
		isc_task_send(task, &event);
	} else {
		clone = NULL;
		isc_task_attach(task, &clone);
		event->sender = clone;
		ISC_LIST_APPEND(adb->whenshutdown, event, link);
	}
	
	UNLOCK(&adb->lock);
}

void
dns_adb_shutdown(dns_adb_t *adb) {

	/*
	 * Shutdown 'adb'.
	 */

	LOCK(&adb->lock);

	if (!adb->shutting_down) {
		adb->shutting_down = ISC_TRUE;
		shutdown_names(adb);
		shutdown_entries(adb);
		check_exit(adb);
	}

	UNLOCK(&adb->lock);
}

isc_result_t
dns_adb_createfind(dns_adb_t *adb, isc_task_t *task, isc_taskaction_t action,
		   void *arg, dns_name_t *name, dns_name_t *zone,
		   unsigned int options, isc_stdtime_t now, dns_name_t *target,
		   dns_adbfind_t **findp)
{
	dns_adbfind_t *find;
	dns_adbname_t *adbname;
	int bucket;
	isc_boolean_t use_hints, want_event, start_at_root, alias;
	isc_result_t result;
	unsigned int wanted_addresses;
	unsigned int wanted_fetches;
	unsigned int query_pending;

	REQUIRE(DNS_ADB_VALID(adb));
	if (task != NULL) {
		REQUIRE(action != NULL);
	}
	REQUIRE(name != NULL);
	REQUIRE(zone != NULL);
	REQUIRE(findp != NULL && *findp == NULL);
	REQUIRE(target == NULL || dns_name_hasbuffer(target));

	REQUIRE((options & DNS_ADBFIND_ADDRESSMASK) != 0);

	result = ISC_R_UNEXPECTED;
	wanted_addresses = (options & DNS_ADBFIND_ADDRESSMASK);
	wanted_fetches = 0;
	query_pending = 0;
	want_event = ISC_FALSE;
	start_at_root = ISC_FALSE;
	alias = ISC_FALSE;

	if (now == 0)
		isc_stdtime_get(&now);

	/*
	 * XXXMLG  Move this comment somewhere else!
	 *
	 * Look up the name in our internal database.
	 *
	 * Possibilities:  Note that these are not always exclusive.
	 *
	 *	No name found.  In this case, allocate a new name header and
	 *	an initial namehook or two.  If any of these allocations
	 *	fail, clean up and return ISC_R_NOMEMORY.
	 *
	 *	Name found, valid addresses present.  Allocate one addrinfo
	 *	structure for each found and append it to the linked list
	 *	of addresses for this header.
	 *
	 *	Name found, queries pending.  In this case, if a task was
	 *	passed in, allocate a job id, attach it to the name's job
	 *	list and remember to tell the caller that there will be
	 *	more info coming later.
	 */

	find = new_adbfind(adb);
	if (find == NULL)
		return (ISC_R_NOMEMORY);

	/*
	 * Remember what types of addresses we are interested in.
	 */
	find->options = options;
	find->flags |= wanted_addresses;
	if (FIND_WANTEVENT(find)) {
		REQUIRE(task != NULL);
	}

	/*
	 * Try to see if we know anything about this name at all.
	 */
	bucket = DNS_ADB_INVALIDBUCKET;
	adbname = find_name_and_lock(adb, name, &bucket);
	if (adb->name_sd[bucket]) {
		DP(DEF_LEVEL,
		   "dns_adb_createfind: returning ISC_R_SHUTTINGDOWN");
		free_adbfind(adb, &find);
		result = ISC_R_SHUTTINGDOWN;
		goto out;
	}

	/*
	 * Nothing found.  Allocate a new adbname structure for this name.
	 */
	if (adbname == NULL) {
		adbname = new_adbname(adb, name);
		if (adbname == NULL) {
			free_adbfind(adb, &find);
			result = ISC_R_NOMEMORY;
			goto out;
		}
		link_name(adb, bucket, adbname);
	}

	/*
	 * Expire old entries, etc.
	 */
	check_expire_namehooks(adbname, now);

	use_hints = dns_name_equal(zone, dns_rootname);

	/*
	 * Do we know that the name is an alias?
	 */
	if (!EXPIRE_OK(adbname->expire_target, now)) {
		/*
		 * Yes, it is.
		 */
		DP(DEF_LEVEL,
		   "dns_adb_createfind: name %p is an alias (cached)",
		   adbname);
		alias = ISC_TRUE;
		goto post_copy;
	}

	/*
	 * Try to populate the name from the database and/or
	 * start fetches.
	 */
	if (!NAME_HAS_V4(adbname) && !NAME_FETCH_V4(adbname)
	    && EXPIRE_OK(adbname->expire_v4, now)
	    && WANT_INET(wanted_addresses)) {
		result = dbfind_name(adbname, now, use_hints, dns_rdatatype_a);
		if (result == ISC_R_SUCCESS) {
			DP(DEF_LEVEL,
			   "dns_adb_createfind: Found A for name %p in db",
			   adbname);
			goto v6;
		}

		/*
		 * Did we get a CNAME or DNAME?
		 */
		if (result == DNS_R_CNAME || result == DNS_R_DNAME) {
			DP(DEF_LEVEL,
			   "dns_adb_createfind: name %p is an alias",
			   adbname);
			alias = ISC_TRUE;
			goto post_copy;
		}

		/*
		 * Listen to negative cache hints, and don't start
		 * another query.
		 */
		if (NCACHE_RESULT(result) || AUTH_NX(result))
			goto v6;

		wanted_fetches |= DNS_ADBFIND_INET;
	}

 v6:
	if (!NAME_HAS_V6(adbname) && !NAME_FETCH_V6(adbname)
	    && EXPIRE_OK(adbname->expire_v6, now)
	    && WANT_INET6(wanted_addresses)) {
		result = dbfind_a6(adbname, now, use_hints);
		if (result == ISC_R_SUCCESS) {
			DP(DEF_LEVEL,
			   "dns_adb_createfind: Found A6 for name %p",
			   adbname);
			goto fetch;
		}

		/*
		 * Did we get a CNAME or DNAME?
		 */
		if (result == DNS_R_CNAME || result == DNS_R_DNAME) {
			DP(DEF_LEVEL,
			   "dns_adb_createfind: name %p is an alias",
			   adbname);
			alias = ISC_TRUE;
			goto post_copy;
		}

		/*
		 * Listen to negative cache hints, and don't start
		 * another query.
		 */
		if (NCACHE_RESULT(result) || AUTH_NX(result))
			goto fetch;

		wanted_fetches |= DNS_ADBFIND_INET6;
	}

 fetch:
	if (wanted_fetches != 0 &&
	    (!FIND_AVOIDFETCHES(find) || wanted_addresses == wanted_fetches)) {
		/*
		 * We're missing at least one address family.  Either the
		 * caller hasn't instructed us to avoid fetches, or we don't
		 * know anything about any of the address families that would
		 * be acceptable so we have to launch fetches.
		 */

		if (FIND_STARTATROOT(find))
			start_at_root = ISC_TRUE;

		/*
		 * Start V4.
		 */
		if (WANT_INET(wanted_fetches) &&
		    fetch_name_v4(adbname, start_at_root) ==
		    ISC_R_SUCCESS) {
			DP(DEF_LEVEL,
			   "dns_adb_createfind: Started A fetch for name %p",
			   adbname);
		}

		/*
		 * Start V6.
		 */
		if (WANT_INET6(wanted_fetches) &&
		    fetch_name_a6(adbname, use_hints, start_at_root) ==
		    ISC_R_SUCCESS) {
			DP(DEF_LEVEL,
			   "dns_adb_createfind: Started A6 fetch for name %p",
			   adbname);
		}
	}

	/*
	 * Run through the name and copy out the bits we are
	 * interested in.
	 */
	copy_namehook_lists(adb, find, zone, adbname, now);

	if (NAME_FETCH_V4(adbname))
		query_pending |= DNS_ADBFIND_INET;
	if (NAME_FETCH_V6(adbname))
		query_pending |= DNS_ADBFIND_INET6;

 post_copy:
	/*
	 * Attach to the name's query list if there are queries
	 * already running, and we have been asked to.
	 */
	want_event = ISC_TRUE;
	if (!FIND_WANTEVENT(find))
		want_event = ISC_FALSE;
	if (FIND_WANTEMPTYEVENT(find) && FIND_HAS_ADDRS(find))
		want_event = ISC_FALSE;
	if ((wanted_addresses & query_pending) == 0)
		want_event = ISC_FALSE;
	if (alias)
		want_event = ISC_FALSE;
	if (want_event) {
		find->adbname = adbname;
		find->name_bucket = bucket;
		ISC_LIST_APPEND(adbname->finds, find, plink);
		find->query_pending = (query_pending & wanted_addresses);
		find->flags &= ~DNS_ADBFIND_ADDRESSMASK;
		find->flags |= (find->query_pending & DNS_ADBFIND_ADDRESSMASK);
		DP(DEF_LEVEL, "createfind: attaching find %p to adbname %p",
		   find, adbname);
	} else {
		/*
		 * Remove the flag so the caller knows there will never
		 * be an event, and set internal flags to fake that
		 * the event was sent and freed, so dns_adb_destroyfind() will
		 * do the right thing.
		 */
		find->options &= ~DNS_ADBFIND_WANTEVENT;
		find->flags |= (FIND_EVENT_SENT | FIND_EVENT_FREED);
		find->flags &= ~DNS_ADBFIND_ADDRESSMASK;
	}

	find->partial_result |= (adbname->partial_result & wanted_addresses);
	if (alias) {
		if (target != NULL) {
			result = dns_name_concatenate(&adbname->target, NULL,
						      target, NULL);
			if (result != ISC_R_SUCCESS)
				goto out;
		}
		result = DNS_R_ALIAS;
	} else
		result = ISC_R_SUCCESS;

 out:
	if (find != NULL) {
		*findp = find;

		if (want_event) {
			isc_task_t *taskp;

			INSIST((find->flags & DNS_ADBFIND_ADDRESSMASK) != 0);
			taskp = NULL;
			isc_task_attach(task, &taskp);
			find->event.sender = taskp;
			find->event.action = action;
			find->event.arg = arg;
		}
	}

	/* dns_adb_dumpfind(find, stderr); */

	if (bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->namelocks[bucket]);

	return (result);
}

isc_result_t
dns_adb_deletename(dns_adb_t *adb, dns_name_t *host)
{
	int name_bucket;
	dns_adbname_t *name;
	isc_boolean_t want_check_exit = ISC_FALSE;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(host != NULL);

	name = NULL;

	/*
	 * Find the name.
	 */
	name_bucket = DNS_ADB_INVALIDBUCKET;
	name = find_name_and_lock(adb, host, &name_bucket);
	if (name == NULL) {
		UNLOCK(&adb->namelocks[name_bucket]);
		return (ISC_R_NOTFOUND);
	}

	kill_name(&name, DNS_EVENT_ADBNAMEDELETED);

	/*
	 * If we're shutting down and this bucket is empty, we need to call
	 * check_exit() to see if we're done.
	 */
	if (adb->name_sd[name_bucket] && adb->name_refcnt[name_bucket] == 0)
		want_check_exit = ISC_TRUE;


	if (name_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->namelocks[name_bucket]);

	if (want_check_exit) {
		LOCK(&adb->lock);
		check_exit(adb);
		UNLOCK(&adb->lock);
	}

	return (DNS_R_SUCCESS);
}

/* XXXMLG needs v6 support */
isc_result_t
_dns_adb_insert(dns_adb_t *adb, dns_name_t *host, isc_sockaddr_t *addr,
		dns_ttl_t ttl, isc_stdtime_t now)
{
	dns_adbname_t *name;
	isc_boolean_t free_name;
	dns_adbentry_t *entry;
	isc_boolean_t free_entry;
	dns_adbnamehook_t *namehook;
	isc_boolean_t free_namehook;
	int name_bucket, addr_bucket; /* unlock if != DNS_ADB_INVALIDBUCKET */
	isc_result_t result;
	isc_stdtime_t expire_time;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(host != NULL);
	REQUIRE(addr != NULL);

	if (now == 0)
		isc_stdtime_get(&now);

	expire_time = now + ttl;

	name = NULL;
	free_name = ISC_FALSE;
	entry = NULL;
	free_entry = ISC_FALSE;
	namehook = NULL;
	free_namehook = ISC_FALSE;
	result = ISC_R_UNEXPECTED;

	/*
	 * First, see if the host is already in the database.  If it is,
	 * don't make a new host entry.  If not, copy the name and name's
	 * contents into our structure and allocate what we'll need
	 * to attach things together.
	 */
	name_bucket = DNS_ADB_INVALIDBUCKET;
	name = find_name_and_lock(adb, host, &name_bucket);
	if (name == NULL) {
		name = new_adbname(adb, host);
		if (name == NULL) {
			result = ISC_R_NOMEMORY;
			goto out;
		}
		free_name = ISC_TRUE;
		name->expire_v4 = ISC_MIN(expire_time, name->expire_v4);
	}

	/*
	 * Now, while keeping the name locked, search for the address.
	 * Three possibilities:  One, the address doesn't exist.
	 * Two, the address exists, but we aren't linked to it.
	 * Three, the address exists and we are linked to it.
	 * (1) causes a new entry and namehook to be created.
	 * (2) causes only a new namehook.
	 * (3) is an error.
	 */
	addr_bucket = DNS_ADB_INVALIDBUCKET;
	entry = find_entry_and_lock(adb, addr, &addr_bucket);
	/*
	 * Case (1):  new entry and namehook.
	 */
	if (entry == NULL) {
		entry = new_adbentry(adb);
		if (entry == NULL) {
			result = ISC_R_NOMEMORY;
			goto out;
		}
		free_entry = ISC_TRUE;
	}

	/*
	 * Case (3):  entry exists, we're linked.
	 */
	namehook = ISC_LIST_HEAD(name->v4);
	while (namehook != NULL) {
		if (namehook->entry == entry) {
			result = ISC_R_EXISTS;
			goto out;
		}
		namehook = ISC_LIST_NEXT(namehook, plink);
	}

	/*
	 * Case (2):  New namehook, link to entry from above.
	 */
	namehook = new_adbnamehook(adb, entry);
	if (namehook == NULL) {
		result = ISC_R_NOMEMORY;
		goto out;
	}
	free_namehook = ISC_TRUE;
	ISC_LIST_APPEND(name->v4, namehook, plink);

	entry->lock_bucket = addr_bucket;
	inc_entry_refcnt(adb, entry, ISC_FALSE);
	entry->sockaddr = *addr;

	/*
	 * If needed, string up the name and entry.
	 */
	if (!ISC_LINK_LINKED(name, plink))
		link_name(adb, name_bucket, name);
	if (!ISC_LINK_LINKED(entry, plink))
		link_entry(adb, addr_bucket, entry);

	name->expire_v4 = ISC_MIN(name->expire_v4, expire_time);

	UNLOCK(&adb->namelocks[name_bucket]);
	name_bucket = DNS_ADB_INVALIDBUCKET;
	UNLOCK(&adb->entrylocks[addr_bucket]);
	addr_bucket = DNS_ADB_INVALIDBUCKET;

	return (ISC_R_SUCCESS);

 out:
	if (free_name)
		free_adbname(adb, &name);
	if (free_entry)
		isc_mempool_put(adb->emp, entry);
	if (free_namehook)
		isc_mempool_put(adb->nhmp, namehook);
	if (name_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->namelocks[name_bucket]);
	if (addr_bucket != DNS_ADB_INVALIDBUCKET)
		UNLOCK(&adb->entrylocks[addr_bucket]);

	return (result);
}

void
dns_adb_destroyfind(dns_adbfind_t **findp)
{
	dns_adbfind_t *find;
	dns_adbentry_t *entry;
	dns_adbaddrinfo_t *ai;
	int bucket;
	dns_adb_t *adb;

	REQUIRE(findp != NULL && DNS_ADBFIND_VALID(*findp));
	find = *findp;
	*findp = NULL;

	LOCK(&find->lock);

	DP(DEF_LEVEL, "dns_adb_destroyfind on find %p", find);

	adb = find->adb;
	REQUIRE(DNS_ADB_VALID(adb));

	REQUIRE(FIND_EVENTFREED(find));

	bucket = find->name_bucket;
	INSIST(bucket == DNS_ADB_INVALIDBUCKET);

	UNLOCK(&find->lock);

	/*
	 * The find doesn't exist on any list, and nothing is locked.
	 * Return the find to the memory pool, and decrement the adb's
	 * reference count.
	 */
	ai = ISC_LIST_HEAD(find->list);
	while (ai != NULL) {
		ISC_LIST_UNLINK(find->list, ai, publink);
		entry = ai->entry;
		ai->entry = NULL;
		ai->sockaddr = NULL;
		INSIST(DNS_ADBENTRY_VALID(entry));
		dec_entry_refcnt(adb, entry, ISC_TRUE);
		free_adbaddrinfo(adb, &ai);
		ai = ISC_LIST_HEAD(find->list);
	}

	/*
	 * WARNING:  The find is freed with the adb locked.  This is done
	 * to avoid a race condition where we free the find, some other
	 * thread tests to see if it should be destroyed, detects it should
	 * be, destroys it, and then we try to lock it for our check, but the
	 * lock is destroyed.
	 */
	LOCK(&adb->lock);
	free_adbfind(adb, &find);
	check_exit(adb);
	UNLOCK(&adb->lock);
}

void
dns_adb_cancelfind(dns_adbfind_t *find)
{
	isc_event_t *ev;
	isc_task_t *task;
	dns_adb_t *adb;
	int bucket;
	int unlock_bucket;

	LOCK(&find->lock);

	DP(DEF_LEVEL, "dns_adb_cancelfind on find %p", find);

	adb = find->adb;
	REQUIRE(DNS_ADB_VALID(adb));

	REQUIRE(!FIND_EVENTFREED(find));
	REQUIRE(FIND_WANTEVENT(find));

	bucket = find->name_bucket;
	if (bucket == DNS_ADB_INVALIDBUCKET)
		goto cleanup;

	/*
	 * We need to get the adbname's lock to unlink the find.
	 */
	unlock_bucket = bucket;
	violate_locking_hierarchy(&find->lock, &adb->namelocks[unlock_bucket]);
	bucket = find->name_bucket;
	if (bucket != DNS_ADB_INVALIDBUCKET) {
		ISC_LIST_UNLINK(find->adbname->finds, find, plink);
		find->adbname = NULL;
		find->name_bucket = DNS_ADB_INVALIDBUCKET;
	}
	UNLOCK(&adb->namelocks[unlock_bucket]);
	bucket = DNS_ADB_INVALIDBUCKET;

 cleanup:

	if (!FIND_EVENTSENT(find)) {
		ev = &find->event;
		task = ev->sender;
		ev->sender = find;
		ev->type = DNS_EVENT_ADBCANCELED;
		ev->destroy = event_free;
		ev->destroy_arg = find;

		DP(DEF_LEVEL, "Sending event %p to task %p for find %p",
		   ev, task, find);

		isc_task_sendanddetach(&task, &ev);
	}

	UNLOCK(&find->lock);
}

void
dns_adb_dump(dns_adb_t *adb, FILE *f)
{
	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(f != NULL);

	/*
	 * Lock the adb itself, lock all the name buckets, then lock all
	 * the entry buckets.  This should put the adb into a state where
	 * nothing can change, so we can iterate through everything and
	 * print at our leasure.
	 */

	LOCK(&adb->lock);
	dump_adb(adb, f);
	UNLOCK(&adb->lock);
}

static void
dump_adb(dns_adb_t *adb, FILE *f)
{
	int i;
	isc_sockaddr_t *sa;
	dns_adbname_t *name;
	dns_adbentry_t *entry;
	char tmp[512];
	const char *tmpp;
	isc_stdtime_t now;

	isc_stdtime_get(&now);

	fprintf(f, "ADB %p DUMP:\n", adb);
	fprintf(f, "erefcnt %u, irefcnt %u, finds out %u\n",
		adb->erefcnt, adb->irefcnt,
		isc_mempool_getallocated(adb->nhmp));

	for (i = 0 ; i < NBUCKETS ; i++)
		LOCK(&adb->namelocks[i]);
	for (i = 0 ; i < NBUCKETS ; i++)
		LOCK(&adb->entrylocks[i]);

	/*
	 * Dump the names
	 */
	fprintf(f, "Names:\n");
	for (i = 0 ; i < NBUCKETS ; i++) {
		name = ISC_LIST_HEAD(adb->names[i]);
		if (name == NULL)
			continue;
		fprintf(f, "Name bucket %d:\n", i);
		while (name != NULL) {
			fprintf(f, "name %p\n", name);
			if (!DNS_ADBNAME_VALID(name))
				fprintf(f, "\tMAGIC %08x\n", name->magic);
			fprintf(f, "\texpiry [");
			if (name->expire_v4 == INT_MAX)
				fprintf(f, "inf ");
			else
				fprintf(f, "%d ", name->expire_v4 - now);
			if (name->expire_v6 == INT_MAX)
				fprintf(f, "inf ");
			else
				fprintf(f, "%d ", name->expire_v6 - now);
			if (name->expire_target == INT_MAX)
				fprintf(f, "inf] ");
			else
				fprintf(f, "%d] ", name->expire_target - now);
			print_dns_name(f, &name->name);
			if (dns_name_countlabels(&name->target) > 0) {
				fprintf(f, "\t\t alias for ");
				print_dns_name(f, &name->target);
			}
			fprintf(f, "\n");
			print_namehook_list(f, name);
			print_fetch_list(f, name);
			print_find_list(f, name);
			fprintf(f, "\n");

			name = ISC_LIST_NEXT(name, plink);
		}
	}

	/*
	 * Dump the entries
	 */
	fprintf(f, "Entries:\n");
	for (i = 0 ; i < NBUCKETS ; i++) {
		entry = ISC_LIST_HEAD(adb->entries[i]);
		if (entry == NULL)
			continue;
		fprintf(f, "Entry bucket %d:\n", i);
		while (entry != NULL) {
			if (!DNS_ADBENTRY_VALID(entry))
				fprintf(f, "\tMAGIC %08x\n", entry->magic);
			if (entry->lock_bucket != i)
				fprintf(f, "\tWRONG BUCKET!  lock_bucket %d\n",
					entry->lock_bucket);

			sa = &entry->sockaddr;
			switch (sa->type.sa.sa_family) {
			case AF_INET:
				tmpp = inet_ntop(AF_INET,
						 &sa->type.sin.sin_addr,
						 tmp, sizeof tmp);
				break;
			case AF_INET6:
				tmpp = inet_ntop(AF_INET6,
						 &sa->type.sin6.sin6_addr,
						 tmp, sizeof tmp);
				break;
			default:
				tmpp = "UnkFamily";
			}

			if (tmpp == NULL)
				tmpp = "CANNOT TRANSLATE ADDRESS!";

			fprintf(f, "\t%p: refcnt %u flags %08x goodness %d"
				" srtt %u addr %s\n",
				entry, entry->refcnt, entry->flags,
				entry->goodness, entry->srtt, tmpp);

			entry = ISC_LIST_NEXT(entry, plink);
		}
	}

	/*
	 * Unlock everything
	 */
	for (i = 0 ; i < NBUCKETS ; i++)
		UNLOCK(&adb->entrylocks[i]);
	for (i = 0 ; i < NBUCKETS ; i++)
		UNLOCK(&adb->namelocks[i]);
}

void
dns_adb_dumpfind(dns_adbfind_t *find, FILE *f)
{
	char tmp[512];
	const char *tmpp;
	dns_adbaddrinfo_t *ai;
	isc_sockaddr_t *sa;

	/*
	 * Not used currently, in the API Just In Case we
	 * want to dump out the name and/or entries too.
	 */

	LOCK(&find->lock);

	fprintf(f, "Find %p\n", find);
	fprintf(f, "\tqpending %08x partial %08x options %08x flags %08x\n",
		find->query_pending, find->partial_result,
		find->options, find->flags);
	fprintf(f, "\tname_bucket %d, name %p, event sender %p\n",
		find->name_bucket, find->adbname, find->event.sender);

	ai = ISC_LIST_HEAD(find->list);
	if (ai != NULL)
		fprintf(f, "\tAddresses:\n");
	while (ai != NULL) {
		sa = ai->sockaddr;
		switch (sa->type.sa.sa_family) {
		case AF_INET:
			tmpp = inet_ntop(AF_INET, &sa->type.sin.sin_addr,
					 tmp, sizeof tmp);
			break;
		case AF_INET6:
			tmpp = inet_ntop(AF_INET6, &sa->type.sin6.sin6_addr,
					 tmp, sizeof tmp);
			break;
		default:
			tmpp = "UnkFamily";
		}

		if (tmpp == NULL)
			tmpp = "CANNOT TRANSLATE ADDRESS!";

		fprintf(f, "\t\tentry %p, flags %08x goodness %d"
			" srtt %u addr %s\n",
			ai->entry, ai->flags, ai->goodness, ai->srtt, tmpp);

		ai = ISC_LIST_NEXT(ai, publink);
	}

	UNLOCK(&find->lock);
}

static void
print_dns_name(FILE *f, dns_name_t *name)
{
	char buf[1024];
	isc_buffer_t b;
	isc_region_t r;

	INSIST(f != NULL);

	isc_buffer_init(&b, buf, sizeof buf, ISC_BUFFERTYPE_TEXT);

	if (dns_name_totext(name, ISC_FALSE, &b) == ISC_R_SUCCESS) {
		isc_buffer_used(&b, &r);
		fprintf(f, "%.*s", (int)r.length, r.base);
	}
}

static void
print_namehook_list(FILE *f, dns_adbname_t *n)
{
	dns_adbnamehook_t *nh;

	nh = ISC_LIST_HEAD(n->v4);
	while (nh != NULL) {
		fprintf(f, "\t\tHook(V4) %p -> entry %p\n", nh, nh->entry);
		nh = ISC_LIST_NEXT(nh, plink);
	}
	nh = ISC_LIST_HEAD(n->v6);
	while (nh != NULL) {
		fprintf(f, "\t\tHook(V6) %p -> entry %p\n", nh, nh->entry);
		nh = ISC_LIST_NEXT(nh, plink);
	}
}

static inline void
print_fetch(FILE *f, dns_adbfetch_t *ft, char *type)
{
	fprintf(f, "\t\tFetch(%s): %p -> { nh %p, entry %p, fetch %p }\n",
		type, ft, ft->namehook, ft->entry, ft->fetch);
}

static inline void
print_fetch6(FILE *f, dns_adbfetch6_t *ft)
{
	fprintf(f, "\t\tFetch(A6): %p -> { nh %p, entry %p, fetch %p }\n",
		ft, ft->namehook, ft->entry, ft->fetch);
}

static void
print_fetch_list(FILE *f, dns_adbname_t *n)
{
	dns_adbfetch6_t *fetch6;

	if (NAME_FETCH_A(n))
		print_fetch(f, n->fetch_a, "A");
	if (NAME_FETCH_AAAA(n))
		print_fetch(f, n->fetch_aaaa, "AAAA");

	fetch6 = ISC_LIST_HEAD(n->fetches_a6);
	while (fetch6 != NULL) {
		print_fetch6(f, fetch6);
		fetch6 = ISC_LIST_NEXT(fetch6, plink);
	}
}

static void
print_find_list(FILE *f, dns_adbname_t *name)
{
	dns_adbfind_t *find;

	find = ISC_LIST_HEAD(name->finds);
	while (find != NULL) {
		dns_adb_dumpfind(find, f);
		find = ISC_LIST_NEXT(find, plink);
	}
}

static isc_result_t
dbfind_name(dns_adbname_t *adbname, isc_stdtime_t now,
	    isc_boolean_t use_hints, dns_rdatatype_t rdtype)
{
	isc_result_t result;
	dns_rdataset_t rdataset;
	dns_adb_t *adb;
	dns_fixedname_t foundname;
	dns_name_t *fname;

	INSIST(DNS_ADBNAME_VALID(adbname));
	adb = adbname->adb;
	INSIST(DNS_ADB_VALID(adb));
	INSIST(rdtype == dns_rdatatype_a || rdtype == dns_rdatatype_aaaa);

	dns_fixedname_init(&foundname);
	fname =	dns_fixedname_name(&foundname);
	dns_rdataset_init(&rdataset);

	result = dns_view_find(adb->view, &adbname->name, rdtype, now,
			       DNS_DBFIND_GLUEOK, use_hints, fname,
			       &rdataset, NULL);
	switch (result) {
	case DNS_R_GLUE:
	case DNS_R_HINT:
	case DNS_R_SUCCESS:
		/*
		 * Found in the database.  Even if we can't copy out
		 * any information, return success, or else a fetch
		 * will be made, which will only make things worse.
		 */
		result = import_rdataset(adbname, &rdataset, now);
		break;
	case DNS_R_NXDOMAIN:
	case DNS_R_NXRRSET:
		/*
		 * We're authoritative and the data doesn't exist.
		 * Make up a negative cache entry so we don't ask again
		 * for a while.
		 *
		 * XXXRTH  What time should we use?  I'm putting in 30 seconds
		 * for now.
		 */
		if (rdtype == dns_rdatatype_a) {
			adbname->expire_v4 = now + 30;
			DP(NCACHE_LEVEL,
			   "adb name %p: Caching auth negative entry for A",
			   adbname);
		} else {
			DP(NCACHE_LEVEL,
			   "adb name %p: Caching auth negative entry for AAAA",
			   adbname);
			adbname->expire_v6 = now + 30;
		}
		break;
	case DNS_R_NCACHENXDOMAIN:
	case DNS_R_NCACHENXRRSET:
		/*
		 * We found a negative cache entry.  Pull the TTL from it
		 * so we won't ask again for a while.
		 */
		rdataset.ttl = ISC_MAX(rdataset.ttl, ADB_CACHE_MINIMUM);
		if (rdtype == dns_rdatatype_a) {
			adbname->expire_v4 = rdataset.ttl + now;
			DP(NCACHE_LEVEL,
			  "adb name %p: Caching negative entry for A (ttl %u)",
			   adbname, rdataset.ttl);
		} else {
			DP(NCACHE_LEVEL,
		       "adb name %p: Caching negative entry for AAAA (ttl %u)",
			   adbname, rdataset.ttl);
			adbname->expire_v6 = rdataset.ttl + now;
		}
		break;
	case DNS_R_CNAME:
	case DNS_R_DNAME:
		rdataset.ttl = ISC_MAX(rdataset.ttl, ADB_CACHE_MINIMUM);
		clean_target(adb, &adbname->target);
		adbname->expire_target = INT_MAX;
		result = set_target(adb, &adbname->name, fname, &rdataset,
				    &adbname->target);
		if (result == ISC_R_SUCCESS) {
			DP(NCACHE_LEVEL,
			   "adb name %p: caching alias target",
			   adbname);
			adbname->expire_target = rdataset.ttl + now;
		}
		break;
	}

	if (dns_rdataset_isassociated(&rdataset))
		dns_rdataset_disassociate(&rdataset);

	return (result);
}

static isc_result_t
dbfind_a6(dns_adbname_t *adbname, isc_stdtime_t now, isc_boolean_t use_hints)
{
	isc_result_t result;
	dns_rdataset_t rdataset;
	dns_adb_t *adb;
	dns_a6context_t a6ctx;
	dns_fixedname_t foundname;
	dns_name_t *fname;

	INSIST(DNS_ADBNAME_VALID(adbname));
	adb = adbname->adb;
	INSIST(DNS_ADB_VALID(adb));
	INSIST(!NAME_FETCH_V6(adbname));

	result = ISC_R_UNEXPECTED;

	dns_fixedname_init(&foundname);
	fname =	dns_fixedname_name(&foundname);
	dns_rdataset_init(&rdataset);

	result = dns_view_find(adb->view, &adbname->name, dns_rdatatype_a6,
			       now, DNS_DBFIND_GLUEOK, use_hints, fname,
			       &rdataset, NULL);
	switch (result) {
	case DNS_R_GLUE:
	case DNS_R_HINT:
	case DNS_R_SUCCESS:
		/*
		 * Start a6 chain follower.  There is no need to poke people
		 * who might be waiting, since this is call requires there
		 * are none.
		 */
		dns_a6_init(&a6ctx, a6find, NULL, import_a6,
			    a6missing, adbname);
		(void)dns_a6_foreach(&a6ctx, &rdataset, now);
		result = ISC_R_SUCCESS;
		break;
	case DNS_R_NXDOMAIN:
	case DNS_R_NXRRSET:
		/*
		 * We're authoritative and the data doesn't exist.
		 * Make up a negative cache entry so we don't ask again
		 * for a while.
		 *
		 * XXXRTH  What time should we use?  I'm putting in 30 seconds
		 * for now.
		 */
		DP(NCACHE_LEVEL,
		   "adb name %p: Caching auth negative entry for AAAA",
		   adbname);
		adbname->expire_v6 = now + 30;
		break;
	case DNS_R_NCACHENXDOMAIN:
	case DNS_R_NCACHENXRRSET:
		/*
		 * We found a negative cache entry.  Pull the TTL from it
		 * so we won't ask again for a while.
		 */
		DP(NCACHE_LEVEL,
		   "adb name %p: Caching negative entry for A6 (ttl %u)",
		   adbname, rdataset.ttl);
		adbname->expire_v6 = ISC_MIN(rdataset.ttl + now,
					     adbname->expire_v6);
		break;
	case DNS_R_CNAME:
	case DNS_R_DNAME:
		rdataset.ttl = ISC_MAX(rdataset.ttl, ADB_CACHE_MINIMUM);
		clean_target(adb, &adbname->target);
		adbname->expire_target = INT_MAX;
		result = set_target(adb, &adbname->name, fname, &rdataset,
				    &adbname->target);
		if (result == ISC_R_SUCCESS) {
			DP(NCACHE_LEVEL,
			   "adb name %p: caching alias target",
			   adbname);
			adbname->expire_target = rdataset.ttl + now;
		}
		break;
	}

	if (dns_rdataset_isassociated(&rdataset))
		dns_rdataset_disassociate(&rdataset);

	return (result);
}

static void
fetch_callback(isc_task_t *task, isc_event_t *ev)
{
	dns_fetchevent_t *dev;
	dns_adbname_t *name;
	dns_adb_t *adb;
	dns_adbfetch_t *fetch;
	int bucket;
	isc_eventtype_t ev_status;
	isc_stdtime_t now;
	isc_result_t result;
	unsigned int address_type;
	isc_boolean_t want_check_exit = ISC_FALSE;

	(void)task;

	INSIST(ev->type == DNS_EVENT_FETCHDONE);
	dev = (dns_fetchevent_t *)ev;
	name = ev->arg;
	INSIST(DNS_ADBNAME_VALID(name));
	adb = name->adb;
	INSIST(DNS_ADB_VALID(adb));

	bucket = name->lock_bucket;
	LOCK(&adb->namelocks[bucket]);

	INSIST(NAME_FETCH_A(name) || NAME_FETCH_AAAA(name));
	address_type = 0;
	if (NAME_FETCH_A(name) && (name->fetch_a->fetch == dev->fetch)) {
		address_type = DNS_ADBFIND_INET;
		fetch = name->fetch_a;
		name->fetch_a = NULL;
	} else if (NAME_FETCH_AAAA(name)
		   && (name->fetch_aaaa->fetch == dev->fetch)) {
		address_type = DNS_ADBFIND_INET6;
		fetch = name->fetch_aaaa;
		name->fetch_aaaa = NULL;
	}
	INSIST(address_type != 0);

	dns_resolver_destroyfetch(&fetch->fetch);
	dev->fetch = NULL;

	ev_status = DNS_EVENT_ADBNOMOREADDRESSES;

	/*
	 * Cleanup things we don't care about.
	 */
	if (dev->node != NULL)
		dns_db_detachnode(dev->db, &dev->node);
	if (dev->db != NULL)
		dns_db_detach(&dev->db);

	/*
	 * If this name is marked as dead, clean up, throwing away
	 * potentially good data.
	 */
	if (NAME_DEAD(name)) {
		free_adbfetch(adb, &fetch);
		isc_event_free(&ev);

		kill_name(&name, DNS_EVENT_ADBCANCELED);

		/*
		 * If we're shutting down and this bucket is empty, we need
		 * to call check_exit() to see if we're done.
		 */
		if (adb->name_sd[bucket] && adb->name_refcnt[bucket] == 0)
			want_check_exit = ISC_TRUE;

		UNLOCK(&adb->namelocks[bucket]);

		if (want_check_exit) {
			LOCK(&adb->lock);
			check_exit(adb);
			UNLOCK(&adb->lock);
		}

		return;
	}

	isc_stdtime_get(&now);

	/*
	 * If we got a negative cache response, remember it.
	 */
	if (NCACHE_RESULT(dev->result)) {
		dev->rdataset->ttl = ISC_MAX(dev->rdataset->ttl,
					     ADB_CACHE_MINIMUM);
		if (address_type == DNS_ADBFIND_INET) {
			DP(NCACHE_LEVEL, "adb fetch name %p: "
			   "Caching negative entry for A (ttl %u)",
			   name, dev->rdataset->ttl);
			name->expire_v4 = ISC_MIN(name->expire_v4,
						  dev->rdataset->ttl + now);
		} else {
			DP(NCACHE_LEVEL, "adb fetch name %p: "
			   "Caching negative entry for AAAA (ttl %u)",
			   name, dev->rdataset->ttl);
			name->expire_v6 = ISC_MIN(name->expire_v6,
						  dev->rdataset->ttl + now);
		}
		goto out;
	}

	/*
	 * Handle CNAME/DNAME.
	 */
	if (dev->result == DNS_R_CNAME || dev->result == DNS_R_DNAME) {
		dev->rdataset->ttl = ISC_MAX(dev->rdataset->ttl,
					     ADB_CACHE_MINIMUM);
		clean_target(adb, &name->target);
		name->expire_target = INT_MAX;
		result = set_target(adb, &name->name,
				    dns_fixedname_name(&dev->foundname),
				    dev->rdataset,
				    &name->target);
		if (result == ISC_R_SUCCESS) {
			DP(NCACHE_LEVEL,
			   "adb fetch name %p: caching alias target",
			   name);
			name->expire_target = dev->rdataset->ttl + now;
		}
		goto check_result;
	}

	/*
	 * Did we get back junk?  If so, and there are no more fetches
	 * sitting out there, tell all the finds about it.
	 */
	if (dev->result != ISC_R_SUCCESS) {
		/* XXXMLG Don't pound on bad servers. */
		if (address_type == DNS_ADBFIND_INET)
			name->expire_v4 = ISC_MIN(name->expire_v4, now + 300);
		else
			name->expire_v6 = ISC_MIN(name->expire_v6, now + 300);
		DP(1, "got junk in fetch for name %p (%s)",
		   name, isc_result_totext(dev->result));

		goto out;
	}

	/*
	 * We got something potentially useful.
	 */
	result = import_rdataset(name, &fetch->rdataset, now);

 check_result:
	if (result == ISC_R_SUCCESS)
		ev_status = DNS_EVENT_ADBMOREADDRESSES;

 out:
	free_adbfetch(adb, &fetch);
	isc_event_free(&ev);

	clean_finds_at_name(name, ev_status, address_type);

	UNLOCK(&adb->namelocks[bucket]);
}

static void
fetch_callback_a6(isc_task_t *task, isc_event_t *ev)
{
	dns_fetchevent_t *dev;
	dns_adbname_t *name;
	dns_adb_t *adb;
	dns_adbfetch6_t *fetch;
	int bucket;
	isc_stdtime_t now;
	isc_result_t result;
	isc_boolean_t want_check_exit = ISC_FALSE;

	(void)task;

	INSIST(ev->type == DNS_EVENT_FETCHDONE);
	dev = (dns_fetchevent_t *)ev;
	name = ev->arg;
	INSIST(DNS_ADBNAME_VALID(name));
	adb = name->adb;
	INSIST(DNS_ADB_VALID(adb));

	bucket = name->lock_bucket;
	LOCK(&adb->namelocks[bucket]);

	INSIST(!NAME_NEEDSPOKE(name));

	for (fetch = ISC_LIST_HEAD(name->fetches_a6);
	     fetch != NULL;
	     fetch = ISC_LIST_NEXT(fetch, plink))
		if (fetch->fetch == dev->fetch)
			break;
	INSIST(fetch != NULL);
	ISC_LIST_UNLINK(name->fetches_a6, fetch, plink);

	DP(ENTER_LEVEL, "ENTER: fetch_callback_a6() name %p", name);
	
	dns_resolver_destroyfetch(&fetch->fetch);
	dev->fetch = NULL;

	/*
	 * Cleanup things we don't care about.
	 */
	if (dev->node != NULL)
		dns_db_detachnode(dev->db, &dev->node);
	if (dev->db != NULL)
		dns_db_detach(&dev->db);

	/*
	 * If this name is marked as dead, clean up, throwing away
	 * potentially good data.
	 */
	if (NAME_DEAD(name)) {
		free_adbfetch6(adb, &fetch);
		isc_event_free(&ev);

		kill_name(&name, DNS_EVENT_ADBCANCELED);

		/*
		 * If we're shutting down and this bucket is empty, we need
		 * to call check_exit() to see if we're done.
		 */
		if (adb->name_sd[bucket] && adb->name_refcnt[bucket] == 0)
			want_check_exit = ISC_TRUE;

		UNLOCK(&adb->namelocks[bucket]);

		if (want_check_exit) {
			LOCK(&adb->lock);
			check_exit(adb);
			UNLOCK(&adb->lock);
		}

		return;
	}

	isc_stdtime_get(&now);

	/*
	 * If the A6 query didn't succeed, and this is the first query
	 * in the A6 chain, try AAAA records instead.  For later failures,
	 * don't do this.
	 */
	if (dev->result != ISC_R_SUCCESS) {
		isc_boolean_t use_hints;

		DP(DEF_LEVEL, "name %p: A6 failed, result %u",
		   name, dev->result);

		/*
		 * If we got a negative cache response, remember it.
		 */
		if (NCACHE_RESULT(dev->result)) {
			dev->rdataset->ttl = ISC_MAX(dev->rdataset->ttl,
						     ADB_CACHE_MINIMUM);
			DP(NCACHE_LEVEL, "adb fetch name %p: "
			   "Caching negative entry for A6 (ttl %u)",
			   name, dev->rdataset->ttl);
			name->expire_v6 = ISC_MIN(name->expire_v6,
						  dev->rdataset->ttl + now);
		}

		/*
		 * Handle CNAME/DNAME.
		 */
		if (dev->result == DNS_R_CNAME || dev->result == DNS_R_DNAME) {
			dev->rdataset->ttl = ISC_MAX(dev->rdataset->ttl,
						     ADB_CACHE_MINIMUM);
			clean_target(adb, &name->target);
			name->expire_target = INT_MAX;
			result = set_target(adb, &name->name,
					  dns_fixedname_name(&dev->foundname),
					    dev->rdataset,
					    &name->target);
			if (result == ISC_R_SUCCESS) {
				DP(NCACHE_LEVEL,
				  "adb A6 fetch name %p: caching alias target",
				   name);
				name->expire_target = dev->rdataset->ttl + now;
				if (FETCH_FIRSTA6(fetch)) {
					/*
					 * Make this name 'pokeable', since
					 * we've learned that this name is an
					 * alias.
					 */
					name->flags |= NAME_NEEDS_POKE;
				}
			}
			goto out;
		}

		if (FETCH_USEHINTS(fetch))
			use_hints = ISC_TRUE;
		else
			use_hints = ISC_FALSE;


		if (FETCH_FIRSTA6(fetch) && !NAME_HAS_V6(name)) {
			DP(DEF_LEVEL,
			   "name %p: A6 query failed, starting AAAA", name);

			/*
			 * Since this is the very first fetch, and it
			 * failed, we know there are no more running.
			 */
			result = dbfind_name(name, now, use_hints,
					     dns_rdatatype_aaaa);
			if (result == ISC_R_SUCCESS) {
				DP(DEF_LEVEL,
				   "name %p: callback_a6: Found AAAA for",
				   name);
				name->flags |= NAME_NEEDS_POKE;
				goto out;
			}

			/*
			 * Listen to negative cache hints, and don't start
			 * another query.
			 */
			if (NCACHE_RESULT(result) || AUTH_NX(result))
				goto out;

			/*
			 * Try to start fetches for AAAA.
			 */
			result = fetch_name_aaaa(name);
			if (result == ISC_R_SUCCESS) {
				DP(DEF_LEVEL,
				   "name %p: callback_a6: Started AAAA fetch",
				   name);
				goto out;
			}
		}

		goto out;
	}

	/*
	 * We got something potentially useful.  Run the A6 chain
	 * follower on this A6 rdataset.
	 */

	fetch->a6ctx.chains = name->chains;
	(void)dns_a6_foreach(&fetch->a6ctx, dev->rdataset, now);

 out:
	free_adbfetch6(adb, &fetch);
	isc_event_free(&ev);

	if (NAME_NEEDSPOKE(name))
		clean_finds_at_name(name, DNS_EVENT_ADBMOREADDRESSES,
				    DNS_ADBFIND_INET6);
	else if (!NAME_FETCH_V6(name))
		clean_finds_at_name(name, DNS_EVENT_ADBNOMOREADDRESSES,
				    DNS_ADBFIND_INET6);

	name->flags &= ~NAME_NEEDS_POKE;

	UNLOCK(&adb->namelocks[bucket]);

	return;
}

static isc_result_t
fetch_name_v4(dns_adbname_t *adbname, isc_boolean_t start_at_root)
{
	isc_result_t result;
	dns_adbfetch_t *fetch;
	dns_adb_t *adb;
	dns_name_t *name;

	INSIST(DNS_ADBNAME_VALID(adbname));
	adb = adbname->adb;
	INSIST(DNS_ADB_VALID(adb));

	INSIST(!NAME_FETCH_V4(adbname));

	if (start_at_root) {
		DP(50, "fetch_name_v4: starting at DNS root for name %p",
		   adbname);
		name = dns_rootname;
	} else
		name = &adbname->name;

	fetch = new_adbfetch(adb);
	if (fetch == NULL) {
		result = ISC_R_NOMEMORY;
		goto cleanup;
	}

	result = dns_resolver_createfetch(adb->view->resolver, &adbname->name,
					  dns_rdatatype_a,
					  NULL, NULL, NULL, 0,
					  adb->task, fetch_callback,
					  adbname, &fetch->rdataset, NULL,
					  &fetch->fetch);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	adbname->fetch_a = fetch;
	fetch = NULL;  /* keep us from cleaning this up below */

 cleanup:
	if (fetch != NULL)
		free_adbfetch(adb, &fetch);

	return (result);
}

static isc_result_t
fetch_name_aaaa(dns_adbname_t *adbname)
{
	isc_result_t result;
	dns_adbfetch_t *fetch;
	dns_adb_t *adb;

	INSIST(DNS_ADBNAME_VALID(adbname));
	adb = adbname->adb;
	INSIST(DNS_ADB_VALID(adb));

	INSIST(!NAME_FETCH_AAAA(adbname));

	fetch = new_adbfetch(adb);
	if (fetch == NULL) {
		result = ISC_R_NOMEMORY;
		goto cleanup;
	}

	result = dns_resolver_createfetch(adb->view->resolver, &adbname->name,
					  dns_rdatatype_aaaa,
					  NULL, NULL, NULL, 0,
					  adb->task, fetch_callback,
					  adbname, &fetch->rdataset, NULL,
					  &fetch->fetch);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	adbname->fetch_aaaa = fetch;
	fetch = NULL;  /* keep us from cleaning this up below */

 cleanup:
	if (fetch != NULL)
		free_adbfetch(adb, &fetch);

	return (result);
}

static isc_result_t
fetch_name_a6(dns_adbname_t *adbname, isc_boolean_t use_hints,
	      isc_boolean_t start_at_root)
{
	isc_result_t result;
	dns_adbfetch6_t *fetch;
	dns_adb_t *adb;
	dns_name_t *name;

	INSIST(DNS_ADBNAME_VALID(adbname));
	adb = adbname->adb;
	INSIST(DNS_ADB_VALID(adb));

	INSIST(!NAME_FETCH_V6(adbname));

	if (start_at_root) {
		DP(50, "fetch_name_a6: starting at DNS root for name %p",
		   adbname);
		name = dns_rootname;
	} else
		name = &adbname->name;

	fetch = new_adbfetch6(adb, adbname, NULL);
	if (fetch == NULL) {
		result = ISC_R_NOMEMORY;
		goto cleanup;
	}
	if (use_hints)
		fetch->flags |= FETCH_USE_HINTS;
	fetch->flags |= FETCH_FIRST_A6;

	result = dns_resolver_createfetch(adb->view->resolver, &adbname->name,
					  dns_rdatatype_a6,
					  NULL, NULL, NULL, 0,
					  adb->task, fetch_callback_a6,
					  adbname, &fetch->rdataset, NULL,
					  &fetch->fetch);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	ISC_LIST_APPEND(adbname->fetches_a6, fetch, plink);
	fetch = NULL;  /* keep us from cleaning this up below */

 cleanup:
	if (fetch != NULL)
		free_adbfetch6(adb, &fetch);

	return (result);
}

/* XXXMLG
 * Needs to take a find argument and an address info, no zone or adb, since
 * these can be extracted from the find itself.
 */
isc_result_t
dns_adb_marklame(dns_adb_t *adb, dns_adbaddrinfo_t *addr, dns_name_t *zone,
		 isc_stdtime_t expire_time)
{
	dns_adbzoneinfo_t *zi;
	int bucket;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(DNS_ADBADDRINFO_VALID(addr));
	REQUIRE(zone != NULL);

	zi = new_adbzoneinfo(adb, zone);
	if (zi == NULL)
		return (ISC_R_NOMEMORY);

	zi->lame_timer = expire_time;

	bucket = addr->entry->lock_bucket;
	LOCK(&adb->entrylocks[bucket]);
	ISC_LIST_PREPEND(addr->entry->zoneinfo, zi, plink);
	UNLOCK(&adb->entrylocks[bucket]);

	return (ISC_R_SUCCESS);
}

void
dns_adb_adjustgoodness(dns_adb_t *adb, dns_adbaddrinfo_t *addr,
		       int goodness_adjustment)
{
	int bucket;
	int old_goodness, new_goodness;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(DNS_ADBADDRINFO_VALID(addr));

	if (goodness_adjustment == 0)
		return;

	bucket = addr->entry->lock_bucket;
	LOCK(&adb->entrylocks[bucket]);

	old_goodness = addr->entry->goodness;

	if (goodness_adjustment > 0) {
		if (old_goodness > INT_MAX - goodness_adjustment)
			new_goodness = INT_MAX;
		else
			new_goodness = old_goodness + goodness_adjustment;
	} else {
		if (old_goodness < INT_MIN - goodness_adjustment)
			new_goodness = INT_MAX;
		else
			new_goodness = old_goodness + goodness_adjustment;
	}

	addr->entry->goodness = new_goodness;
	addr->goodness = new_goodness;

	UNLOCK(&adb->entrylocks[bucket]);
}

void
dns_adb_adjustsrtt(dns_adb_t *adb, dns_adbaddrinfo_t *addr,
		   unsigned int rtt, unsigned int factor)
{
	int bucket;
	unsigned int new_srtt;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(DNS_ADBADDRINFO_VALID(addr));
	REQUIRE(factor <= 10);

	bucket = addr->entry->lock_bucket;
	LOCK(&adb->entrylocks[bucket]);

	new_srtt = (addr->entry->srtt / 10 * factor)
		+ (rtt / 10 * (10 - factor));

	addr->entry->srtt = new_srtt;
	addr->srtt = new_srtt;

	UNLOCK(&adb->entrylocks[bucket]);
}

void
dns_adb_changeflags(dns_adb_t *adb, dns_adbaddrinfo_t *addr,
		    unsigned int bits, unsigned int mask)
{
	int bucket;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(DNS_ADBADDRINFO_VALID(addr));

	bucket = addr->entry->lock_bucket;
	LOCK(&adb->entrylocks[bucket]);

	addr->entry->flags = (addr->entry->flags & ~mask) | (bits & mask);
	/*
	 * Note that we do not update the other bits in addr->flags with
	 * the most recent values from addr->entry->flags.
	 *
	 * XXXRTH  I think this is what we want, because otherwise flags
	 *         that the caller didn't ask to change could be updated.
	 */
	addr->flags = (addr->flags & ~mask) | (bits & mask);

	UNLOCK(&adb->entrylocks[bucket]);
}

isc_result_t
dns_adb_findaddrinfo(dns_adb_t *adb, isc_sockaddr_t *sa,
		     dns_adbaddrinfo_t **addrp)
{
	int bucket;
	dns_adbentry_t *entry;
	dns_adbaddrinfo_t *addr;
	isc_result_t result;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(addrp != NULL && *addrp == NULL);

	result = ISC_R_SUCCESS;
	bucket = DNS_ADB_INVALIDBUCKET;
	entry = find_entry_and_lock(adb, sa, &bucket);
	if (adb->entry_sd[bucket]) {
		result = ISC_R_SHUTTINGDOWN;
		goto unlock;
	}
	if (entry == NULL) {
		/*
		 * We don't know anything about this address.
		 */
		entry = new_adbentry(adb);
		if (entry == NULL) {
			result = ISC_R_NOMEMORY;
			goto unlock;
		}
		entry->sockaddr = *sa;
		link_entry(adb, bucket, entry);
		DP(50, "findaddrinfo: new entry %p", entry);
	} else
		DP(50, "findaddrinfo: found entry %p", entry);

	addr = new_adbaddrinfo(adb, entry);
	if (addr != NULL) {
		inc_entry_refcnt(adb, entry, ISC_FALSE);
		*addrp = addr;
	}

 unlock:
	UNLOCK(&adb->entrylocks[bucket]);

	return (result);
}

void
dns_adb_freeaddrinfo(dns_adb_t *adb, dns_adbaddrinfo_t **addrp)
{
	dns_adbaddrinfo_t *addr;
	dns_adbentry_t *entry;
	int bucket;
	isc_stdtime_t now;
	isc_boolean_t want_check_exit = ISC_FALSE;

	REQUIRE(DNS_ADB_VALID(adb));
	REQUIRE(addrp != NULL);
	addr = *addrp;
	REQUIRE(DNS_ADBADDRINFO_VALID(addr));
	entry = addr->entry;
	REQUIRE(DNS_ADBENTRY_VALID(entry));

	isc_stdtime_get(&now);

	*addrp = NULL;
	
	bucket = addr->entry->lock_bucket;
	LOCK(&adb->entrylocks[bucket]);

	entry->expires = now + 1800;		/* XXXRTH */

	dec_entry_refcnt(adb, entry, ISC_FALSE);
	if (adb->entry_sd[bucket] && adb->entry_refcnt[bucket] == 0)
		want_check_exit = ISC_TRUE;

	UNLOCK(&adb->entrylocks[bucket]);

	addr->entry = NULL;
	addr->sockaddr = NULL;
	free_adbaddrinfo(adb, &addr);

	if (want_check_exit) {
		LOCK(&adb->lock);
		check_exit(adb);
		UNLOCK(&adb->lock);
	}
}
