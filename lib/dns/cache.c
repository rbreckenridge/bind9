/*
 * Copyright (C) 1999 Internet Software Consortium.
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

 /* $Id: cache.c,v 1.11 2000/01/25 19:28:59 halley Exp $ */

#include <config.h>
#include <limits.h>

#include <isc/assertions.h>
#include <isc/error.h>
#include <isc/mutex.h>
#include <isc/util.h>

#include <dns/cache.h>
#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/events.h>
#include <dns/log.h>
#include <dns/rdata.h>
#include <dns/types.h>

#define CACHE_MAGIC	0x24242424U 	/* $$$$. */
#define VALID_CACHE(cache) ((cache) != NULL && (cache)->magic == CACHE_MAGIC)

/***
 ***	Types
 ***/

/*
 * A cache_cleaner_t encapsulsates the state of the periodic
 * cache cleaning. 
 */

typedef struct cache_cleaner cache_cleaner_t;

typedef enum {
	cleaner_s_idle,	/* Waiting for cleaning-interval to expire. */
	cleaner_s_busy	/* Currently cleaning. */
} cleaner_state_t;

/* Convenience macros for comprehensive assertion checking. */
#define CLEANER_IDLE(c) ((c)->state == cleaner_s_idle && \
		         (c)->iterator == NULL && \
			 (c)->resched_event != NULL)
#define CLEANER_BUSY(c) ((c)->state == cleaner_s_busy && \
		         (c)->iterator != NULL && \
			 (c)->resched_event == NULL)

struct cache_cleaner {
	dns_cache_t	*cache;
	isc_task_t 	*task;
	unsigned int	cleaning_interval; /* The cleaning-interval from
					      named.conf, in seconds. */
	isc_timer_t 	*cleaning_timer;
	isc_event_t	*resched_event;	/* Sent by cleaner task to 
					   itself to reschedule */

	dns_dbiterator_t *iterator;
	int 		 increment;	/* Number of names to 
					   clean in one increment */
	cleaner_state_t  state;		/* Idle/Busy. */
};

/*
 * The actual cache object. 
 */

struct dns_cache {
	/* Unlocked */
	unsigned int		magic;
	isc_mutex_t		lock;
	isc_mutex_t		filelock;
	isc_mem_t		*mctx;

	/* Locked by 'lock'. */
	int			references;
	int			live_tasks;	
	dns_rdataclass_t	rdclass;
	dns_db_t		*db;
	cache_cleaner_t		cleaner;

	/* Locked by 'filelock'. */	
	char *			filename;
	/* Access to the on-disk cache file is also locked by 'filelock'. */
};

/***
 ***	Functions
 ***/

static isc_result_t
cache_cleaner_init(dns_cache_t *cache,
		   isc_taskmgr_t *taskmgr, isc_timermgr_t *timermgr,
		   cache_cleaner_t *cleaner);

static void
cleaning_timer_action(isc_task_t *task, isc_event_t *event);

static void
incremental_cleaning_action(isc_task_t *task, isc_event_t *event);

static void
cleaner_shutdown_action(isc_task_t *task, isc_event_t *event);

isc_result_t 
dns_cache_create(isc_mem_t *mctx, isc_taskmgr_t *taskmgr,
		 isc_timermgr_t *timermgr, dns_rdataclass_t rdclass,
		 char *db_type, unsigned int db_argc, char **db_argv,
		 dns_cache_t **cachep)
{
	isc_result_t result;
	dns_cache_t *cache;

	REQUIRE(cachep != NULL);
	REQUIRE(*cachep == NULL);
	REQUIRE(mctx != NULL);

	cache = isc_mem_get(mctx, sizeof *cache);
	if (cache == NULL)
		return (ISC_R_NOMEMORY);

	cache->mctx = mctx;
	result = isc_mutex_init(&cache->lock);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed: %s",
				 dns_result_totext(result));
		result = ISC_R_UNEXPECTED;
		goto fail;
	}

	cache->references = 1;
	cache->live_tasks = 0;
	cache->rdclass = rdclass;

	cache->db = NULL;
	result = dns_db_create(cache->mctx, db_type, dns_rootname, ISC_TRUE,
				rdclass, db_argc, db_argv, &cache->db);
	if (result != ISC_R_SUCCESS)
		goto fail;

	cache->filename = NULL;	
	
	cache->magic = CACHE_MAGIC;

	result = cache_cleaner_init(cache, taskmgr, timermgr,
				    &cache->cleaner);
	RUNTIME_CHECK(result == ISC_R_SUCCESS);

	*cachep = cache;
	return (ISC_R_SUCCESS);
 fail:
	isc_mem_put(cache->mctx, cache, sizeof *cache);
	return (result);
}

static void 
cache_free(dns_cache_t *cache) {
	REQUIRE(VALID_CACHE(cache));
	REQUIRE(cache->references == 0);

	if (cache->cleaner.task != NULL)
		isc_task_detach(&cache->cleaner.task);

	if (cache->cleaner.resched_event != NULL)
		isc_event_free(&cache->cleaner.resched_event);

	if (cache->cleaner.iterator != NULL)
		dns_dbiterator_destroy(&cache->cleaner.iterator);

	if (cache->filename) {
		isc_mem_free(cache->mctx, cache->filename);
		cache->filename = NULL;
	}

	if (cache->db)
		dns_db_detach(&cache->db);

	isc_mutex_destroy(&cache->lock);
	cache->magic = 0;	
	isc_mem_put(cache->mctx, cache, sizeof *cache);	
}	


void
dns_cache_attach(dns_cache_t *cache, dns_cache_t **targetp) {

	REQUIRE(VALID_CACHE(cache));
	REQUIRE(targetp != NULL && *targetp == NULL);

	LOCK(&cache->lock);
	cache->references++;
	UNLOCK(&cache->lock);

	*targetp = cache;
}

void
dns_cache_detach(dns_cache_t **cachep) {
	dns_cache_t *cache;
	isc_boolean_t free_cache = ISC_FALSE;

	REQUIRE(cachep != NULL);
	cache = *cachep;
	REQUIRE(VALID_CACHE(cache));

	LOCK(&cache->lock);
	REQUIRE(cache->references > 0);
	cache->references--;
	if (cache->references == 0)
		free_cache = ISC_TRUE;
	UNLOCK(&cache->lock);
	*cachep = NULL;
	if (free_cache) {
		/* XXXRTH  This is not locked! */
		if (cache->live_tasks > 0)
			isc_task_shutdown(cache->cleaner.task);
		else
			cache_free(cache);
	}
}

void
dns_cache_attachdb(dns_cache_t *cache, dns_db_t **dbp) {
	REQUIRE(VALID_CACHE(cache));
	REQUIRE(dbp != NULL && *dbp == NULL);
	REQUIRE(cache->db != NULL);
	LOCK(&cache->lock);
	dns_db_attach(cache->db, dbp);
	UNLOCK(&cache->lock);
}

#ifdef NOTYET

isc_result_t
dns_cache_setfilename(dns_cache_t *cahce, char *filename) /* ARGSUSED */
{
	char *newname = isc_mem_strdup(filename);
	if (newname == NULL)
		return (ISC_R_NOMEMORY);
	LOCK(&cache->filelock);	
	if (cache->filename)
		isc_mem_free(cache->mctx, cache->filename);
	cache->filename = newname;
	UNLOCK(&cache->filelock);	
	return (ISC_R_SUCCESS);
}

isc_result_t
dns_cache_load(dns_cache_t *cache) {
	isc_result_t result;
	if (cache->filename == NULL)
		return (ISC_R_SUCCESS);
	LOCK(&cache->filelock);
	/* XXX handle TTLs in a way appropriate for the cache */
	result = dns_db_load(cache->db, cache->filename);
	UNLOCK(&cache->filelock);	
	return (result);
}

isc_result_t
dns_cache_dump(dns_cache_t *cache) {
	/* XXX to be written */
	return (ISC_R_NOTIMPLEMENTED);
}

#endif

void
dns_cache_setcleaninginterval(dns_cache_t *cache, unsigned int t) {
	LOCK(&cache->lock);
	cache->cleaner.cleaning_interval = t;
	if (t == 0) {
		isc_timer_reset(cache->cleaner.cleaning_timer, isc_timertype_inactive,
				NULL, NULL, ISC_TRUE);
	} else {
		isc_interval_t interval;
		isc_interval_set(&interval, cache->cleaner.cleaning_interval, 0);
		isc_timer_reset(cache->cleaner.cleaning_timer, isc_timertype_ticker,
				NULL, &interval, ISC_FALSE);
	}
	UNLOCK(&cache->lock);	
}

/*
 * Initialize the cache cleaner object at *cleaner.  
 * Space for the object must be allocated by the caller.
 */

static isc_result_t
cache_cleaner_init(dns_cache_t *cache, isc_taskmgr_t *taskmgr, 
		   isc_timermgr_t *timermgr, cache_cleaner_t *cleaner)
{
        isc_result_t result;

	cleaner->increment = 100;
	cleaner->state = cleaner_s_idle;
	cleaner->cache = cache;
	cleaner->iterator = NULL;
	
	cleaner->task = NULL;
	cleaner->cleaning_timer = NULL;
	cleaner->resched_event = NULL;
	
	if (taskmgr != NULL && timermgr != NULL) {
		result = isc_task_create(taskmgr, cache->mctx,
					  1, &cleaner->task);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "isc_task_create() failed: %s",
					 dns_result_totext(result));
			result = ISC_R_UNEXPECTED;
			goto cleanup_dbiterator;
		}
		cleaner->cache->live_tasks++;
		isc_task_setname(cleaner->task, "cachecleaner", cleaner);

		result = isc_task_onshutdown(cleaner->task,
					      cleaner_shutdown_action, cache);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);

		cleaner->cleaning_interval = 0; /* Initially turned off. */
		result = isc_timer_create(timermgr, isc_timertype_inactive,
					   NULL, NULL,
					   cleaner->task,
					   cleaning_timer_action, cleaner,
					   &cleaner->cleaning_timer);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "isc_timer_create() failed: %s",
					 dns_result_totext(result));
			result = ISC_R_UNEXPECTED;
			goto cleanup_task;
		}

		cleaner->resched_event =
			isc_event_allocate(cache->mctx, cleaner,
					   DNS_EVENT_CACHECLEAN,
					   incremental_cleaning_action,
					   cleaner, sizeof(isc_event_t));
		if (cleaner->resched_event == NULL) {
			result = ISC_R_NOMEMORY;
			goto cleanup_timer;
		}
	}

	return (ISC_R_SUCCESS);

    cleanup_timer:
	isc_timer_detach(&cleaner->cleaning_timer);
    cleanup_task:
	isc_task_detach(&cleaner->task);
    cleanup_dbiterator:
	dns_dbiterator_destroy(&cleaner->iterator);
	return (result);
}

static void
begin_cleaning(cache_cleaner_t *cleaner) {
	isc_result_t result;

	REQUIRE(CLEANER_IDLE(cleaner));
	/*
	 * Create an iterator and position it at the beginning of the cache.
	 */
	result = dns_db_createiterator(cleaner->cache->db, ISC_FALSE,
				       &cleaner->iterator);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_CACHE, ISC_LOG_WARNING,
			      "cache cleaner could not create "
			      "iterator: %s", isc_result_totext(result));
		goto idle;
	}
	result = dns_dbiterator_first(cleaner->iterator);
	if (result == ISC_R_NOMORE) {
		/*
		 * The database is empty.  We are done.
		 */
		goto destroyiter;
	}
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "cache cleaner: dns_dbiterator_first() "
				 "failed: %s", dns_result_totext(result));
		goto destroyiter;
	}

	isc_log_write(dns_lctx, DNS_LOGCATEGORY_GENERAL,
		      DNS_LOGMODULE_CACHE, ISC_LOG_DEBUG(1),
		      "begin cache cleaning");
	cleaner->state = cleaner_s_busy;
	isc_task_send(cleaner->task, &cleaner->resched_event);
	ENSURE(CLEANER_BUSY(cleaner));
	return;

 destroyiter:
	dns_dbiterator_destroy(&cleaner->iterator);	
 idle:
	ENSURE(CLEANER_IDLE(cleaner));
	return;
}

static void
end_cleaning(cache_cleaner_t *cleaner, isc_event_t *event) {
	REQUIRE(CLEANER_BUSY(cleaner));
	isc_log_write(dns_lctx, DNS_LOGCATEGORY_GENERAL,
		      DNS_LOGMODULE_CACHE, ISC_LOG_DEBUG(1),
		      "end cache cleaning");
	dns_dbiterator_destroy(&cleaner->iterator);
	cleaner->state = cleaner_s_idle;
	cleaner->resched_event = event;
	ENSURE(CLEANER_IDLE(cleaner));
}

/*
 * This is run once for every cache-cleaning-interval as defined in named.conf.
 */
static void
cleaning_timer_action(isc_task_t *task, isc_event_t *event) {
	cache_cleaner_t *cleaner = event->arg;
	INSIST(task == cleaner->task);
	INSIST(event->type == ISC_TIMEREVENT_TICK);
	if (cleaner->state == cleaner_s_idle) {
		begin_cleaning(cleaner);
	} else {
		INSIST(CLEANER_BUSY(cleaner));
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_CACHE, ISC_LOG_WARNING,
			      "cache cleaner did not finish "
			      "in one cleaning-interval");
	}
	isc_event_free(&event);
}

/*
 * Do incremental cleaning.
 */
static void
incremental_cleaning_action(isc_task_t *task, isc_event_t *event) {
	isc_result_t result;
	cache_cleaner_t *cleaner = event->arg;
	isc_stdtime_t now;
	int n_names;
	INSIST(event->type == DNS_EVENT_CACHECLEAN);
	INSIST(CLEANER_BUSY(cleaner));
	n_names = cleaner->increment;
	isc_stdtime_get(&now);

	REQUIRE(DNS_DBITERATOR_VALID(cleaner->iterator));

	while (n_names-- > 0) {
		dns_dbnode_t *node = NULL;
		result = dns_dbiterator_current(cleaner->iterator, &node,
						 (dns_name_t *) NULL);
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "cache cleaner: dns_dbiterator_current() "
				 "failed: %s", dns_result_totext(result));
			goto idle;
		}
		INSIST(node != NULL);

		/* Check TTLs, mark expired rdatasets stale. */
		result = dns_db_expirenode(cleaner->cache->db, node, now);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);

		/* This is where the actual freeing takes place. */ 
		dns_db_detachnode(cleaner->cache->db, &node);
		
		/* Step to the next node */
		result = dns_dbiterator_next(cleaner->iterator);
		if (result == ISC_R_NOMORE) {
			/* We have successfully cleaned the whole cache. */
			goto idle;
		}
		if (result != ISC_R_SUCCESS) {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "cache cleaner: dns_dbiterator_next() "
				 "failed: %s", dns_result_totext(result));
			goto idle;
		}
	}

	/* We have successfully performed a cleaning increment. */
	result = dns_dbiterator_pause(cleaner->iterator);
	if (result != ISC_R_SUCCESS && result != ISC_R_NOMORE) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "cache cleaner: dns_dbiterator_pause() "
				 "failed: %s", dns_result_totext(result));
		/* Try to continue. */
	}
	/* Still busy, reschedule. */
	isc_task_send(task, &event);
	INSIST(CLEANER_BUSY(cleaner));
	return;

 idle:
	/* No longer busy; save the event for later use. */
	end_cleaning(cleaner, event);
	INSIST(CLEANER_IDLE(cleaner));	
	return;
}

/*
 * Do immediate cleaning. 
 */
isc_result_t
dns_cache_clean(dns_cache_t *cache, isc_stdtime_t now) {
	isc_result_t result;
	dns_dbiterator_t *iterator = NULL;

	result = dns_db_createiterator(cache->db, ISC_FALSE, &iterator);
	if (result != ISC_R_SUCCESS)
		return result;
	
	result = dns_dbiterator_first(iterator);

	while (result == ISC_R_SUCCESS) {
		dns_dbnode_t *node = NULL;
		result = dns_dbiterator_current(iterator, &node,
						 (dns_name_t *) NULL);
		if (result != ISC_R_SUCCESS)
			break;

		/* Check TTLs, mark expired rdatasets stale. */
		result = dns_db_expirenode(cache->db, node, now);
		RUNTIME_CHECK(result == ISC_R_SUCCESS);

		/* This is where the actual freeing takes place. */ 
		dns_db_detachnode(cache->db, &node);

		result = dns_dbiterator_next(iterator);
	}

	dns_dbiterator_destroy(&iterator);

	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;
	
	return result;
}

/*
 * The cleaner task is shutting down; do the necessary cleanup.
 */
static void
cleaner_shutdown_action(isc_task_t *task, isc_event_t *event) {
	dns_cache_t *cache = event->arg;
	isc_boolean_t should_free = ISC_FALSE;	
	UNUSED(task);
	LOCK(&cache->lock);

	INSIST(event->type == ISC_TASKEVENT_SHUTDOWN);
	isc_event_free(&event);
	
	cache->live_tasks--;
	INSIST(cache->live_tasks == 0);

	if (cache->references == 0)
		should_free = ISC_TRUE;		

	/*
	 * By detaching the timer in the context of its task,
	 * we are guaranteed that there will be no further timer
	 * events.
	 */
	if (cache->cleaner.cleaning_timer != NULL)
		isc_timer_detach(&cache->cleaner.cleaning_timer);

	/* Make sure we don't reschedule anymore. */
	isc_task_purge(task, NULL, DNS_EVENT_CACHECLEAN, NULL);
	
	UNLOCK(&cache->lock);

	if (should_free)
		cache_free(cache);
}
