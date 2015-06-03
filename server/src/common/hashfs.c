/*
 *  Copyright (C) 2012-2014 Skylable Ltd. <info-copyright@skylable.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Special exception for linking this software with OpenSSL:
 *
 *  In addition, as a special exception, Skylable Ltd. gives permission to
 *  link the code of this program with the OpenSSL library and distribute
 *  linked combinations including the two. You must obey the GNU General
 *  Public License in all respects for all of the code used other than
 *  OpenSSL. You may extend this exception to your version of the program,
 *  but you are not obligated to do so. If you do not wish to do so, delete
 *  this exception statement from your version.
 */

#include "default.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <fnmatch.h>

#include "sxdbi.h"
#include "hashfs.h"
#include "hdist.h"
#include "../libsx/src/misc.h"

#include "sx.h"
#include "qsort.h"
#include "utils.h"
#include "blob.h"
#include "../libsx/src/vcrypto.h"
#include "../libsx/src/clustcfg.h"
#include "../libsx/src/cluster.h"
#include "../libsx/src/sxreport.h"
#include "job_common.h"

#define HASHDBS 16
#define GCDBS 1
/* NOTE: HASHFS_VERSION must be exactly 14 bytes */
#define HASHFS_VERSION_1_0 "SX-Storage 1.6"
#define HASHFS_VERSION_1_1 "SX-Storage 1.7"
#define HASHFS_VERSION HASHFS_VERSION_1_1
#define SIZES 3
const char sizedirs[SIZES] = "sml";
const char *sizelongnames[SIZES] = { "small", "medium", "large" };
const unsigned int bsz[SIZES] = {SX_BS_SMALL, SX_BS_MEDIUM, SX_BS_LARGE};

#define HDIST_SEED 0x1337
#define MURMUR_SEED 0xacab
#define TOKEN_REPLICA_LEN 8
#define TOKEN_EXPIRE_LEN 16
#define TOKEN_TEXT_LEN (UUID_STRING_SIZE + 1 + TOKEN_RAND_BYTES * 2 + 1 + TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN + 1 + AUTH_KEY_LEN * 2)

#define WARNHASH(MSG, X) do {				\
    char _warnhash[sizeof(sx_hash_t)*2+1];		\
    bin2hex((X)->b, sizeof(*X), _warnhash, sizeof(_warnhash));	\
    WARN("(%s): %s #%s#", __func__, MSG, _warnhash); \
    } while(0)

#define DEBUGHASH(MSG, X) do {				\
    char _debughash[sizeof(sx_hash_t)*2+1];		\
    if (UNLIKELY(sxi_log_is_debug(&logger))) {          \
	bin2hex((X)->b, sizeof(*X), _debughash, sizeof(_debughash));	\
	DEBUG("%s: #%s#", MSG, _debughash);				\
    }\
    } while(0)

rc_ty sx_hashfs_check_blocksize(unsigned int bs) {
    unsigned int hs;
    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    return (hs == SIZES) ? FAIL_BADBLOCKSIZE : OK;
}

static int write_block(int fd, const void *data, uint64_t off, unsigned int data_len) {
    uint8_t *dt = (uint8_t *)data;
    while(data_len) {
	int l = pwrite(fd, dt, data_len, off);
	if(l<0) {
	    if(errno == EINTR)
		continue;
	    msg_set_errno_reason("Failed to write block");
	    return 1;
	}
	data_len -= l;
	dt += l;
	off += l;
    }
    return 0;
}

static int read_block(int fd, uint8_t *dt, uint64_t off, unsigned int buf_len) {
    while(buf_len) {
	int l = pread(fd, dt, buf_len, off);
	if(l<0) {
	    if(errno == EINTR)
		continue;
	    msg_set_errno_reason("Failed to read block");
	    return 1;
	}
	if(!l) {
	    msg_set_reason("Incomplete block read");
	    return 1;
	}
	buf_len -= l;
	dt += l;
	off += l;
    }
    return 0;
}

int sx_hashfs_hash_buf(const void *salt, unsigned int salt_len, const void *buf, unsigned int buf_len, sx_hash_t *hash) {
    return sxi_sha1_calc(salt, salt_len, buf, buf_len, hash->b);
}
#define hash_buf sx_hashfs_hash_buf

#define CREATE_DB(DBTYPE) \
do { \
    sqlite3 *handle = NULL;\
    /* Create the dbatabase */ \
    if(sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)) { \
	CRIT("Failed to create %s database: %s", DBTYPE, sqlite3_errmsg(handle)); \
	goto create_hashfs_fail; \
    } \
    if (!(db = qnew(handle))) \
        goto create_hashfs_fail;\
    if(qprep(db, &q, "PRAGMA synchronous = OFF") || qstep_noret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    if(qprep(db, &q, "PRAGMA journal_mode = WAL") || qstep_ret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    /* Set create the hashfs table which is a generic k/v store for config items */ \
    if(qprep(db, &q, "CREATE TABLE hashfs (key TEXT NOT NULL PRIMARY KEY, value TEXT NOT NULL)") || qstep_noret(q)) \
	goto create_hashfs_fail; \
    qnullify(q); \
    /* Fill in the basic settings */ \
    if(qprep(db, &q, "INSERT INTO hashfs (key, value) VALUES (:k, :v)")) \
	goto create_hashfs_fail; \
    if(qbind_text(q, ":k", "version") || qbind_text(q, ":v", HASHFS_VERSION_1_0) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    if(qbind_text(q, ":k", "dbtype") || qbind_text(q, ":v", DBTYPE) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    if(qbind_text(q, ":k", "cluster") || qbind_blob(q, ":v", cluster->binary, sizeof(cluster->binary)) || qstep_noret(q)) \
	goto create_hashfs_fail; \
    sqlite3_reset(q); \
    DEBUG("creating %s", path);\
} while(0)

static int qlog_set = 0;

static rc_ty sx_storage_create_1_0(const char *dir, sx_uuid_t *cluster, uint8_t *key, int key_size) {
    unsigned int dirlen, i, j;
    sxi_db_t *db = NULL;
    sqlite3_stmt *q = NULL;
    char *path, dbitem[64];
    int ret = FAIL_EINIT;
    sxc_uri_t *uri = NULL;

    if(!dir || !(dirlen = strlen(dir))) {
	CRIT("Bad path");
	return EINVAL;
    }

    if(ssl_version_check())
	return FAIL_EINIT;

    if(access(dir, R_OK | W_OK | X_OK)) {
	PCRIT("Cannot access storage directory %s", dir);
	return FAIL_EINIT;
    }

    if(!(path = wrap_malloc(dirlen + bsz[SIZES-1])))
	goto create_hashfs_fail;

    /* --- HASHFS db --- */
    sqlite3_config(SQLITE_CONFIG_LOG, qlog, NULL);
    qlog_set = 1;
    sprintf(path, "%s/hashfs.db", dir);
    CREATE_DB("hashfs");
    sqlite3_reset(q); /* q is now prepared for hashfs insertions */

    if(qbind_text(q, ":k", "current_dist_rev") || qbind_int64(q, ":v", 0) || qstep_noret(q))
	goto create_hashfs_fail;
    sqlite3_reset(q);
    if(qbind_text(q, ":k", "current_dist") || qbind_blob(q, ":v", "", 0) || qstep_noret(q))
	goto create_hashfs_fail;

    /* Set the path to the file dbs */
    for(i=0; i<METADBS; i++) {
	sprintf(dbitem, "metadb_%08x", i);
	sprintf(path, "f%08x.db", i);
	sqlite3_reset(q);
	if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	    goto create_hashfs_fail;
    }

    /* Set the path to the block dbs */
    for(j = 0; j < SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    sprintf(dbitem, "hashdb_%c_%08x", sizedirs[j], i);
	    sprintf(path, "h%c%08x.db", sizedirs[j], i);
	    sqlite3_reset(q);
	    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
		goto create_hashfs_fail;

	    sprintf(dbitem, "datafile_%c_%08x", sizedirs[j], i);
	    sprintf(path, "h%c%08x.bin", sizedirs[j], i);
	    sqlite3_reset(q);
	    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
		goto create_hashfs_fail;
	}
    }

    /* Set the path to the temp db */
    strcpy(dbitem, "tempdb");
    strcpy(path, "temp.db");
    sqlite3_reset(q);
    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	goto create_hashfs_fail;

    /* Set the path to the event db */
    strcpy(dbitem, "eventdb");
    strcpy(path, "events.db");
    sqlite3_reset(q);
    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	goto create_hashfs_fail;

    /* Set the path to the xfer db */
    strcpy(dbitem, "xferdb");
    strcpy(path, "xfers.db");
    sqlite3_reset(q);
    if(qbind_text(q, ":k", dbitem) || qbind_text(q, ":v", path) || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    /* Create HASHFS tables */
    if(qprep(db, &q, "CREATE TABLE users (uid INTEGER PRIMARY KEY NOT NULL, user BLOB ("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL UNIQUE, name TEXT ("STRIFY(SXLIMIT_MAX_USERNAME_LEN)") NOT NULL UNIQUE, key BLOB ("STRIFY(AUTH_KEY_LEN)") NOT NULL UNIQUE, role INTEGER NOT NULL, enabled INTEGER NOT NULL DEFAULT 0)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
/*    if(qprep(db, &q, "CREATE INDEX users_byname ON users(name, enabled)") || qstep_noret(q))
       goto create_hashfs_fail;
    qnullify(q);*/

    if(qprep(db, &q, "INSERT INTO users(uid, user, name, key, role, enabled) VALUES(0, :userhash, :name, :key, :role, 1)") ||
       qbind_blob(q, ":userhash", CLUSTER_USER, AUTH_UID_LEN) ||
       qbind_text(q, ":name", "rootcluster") || qbind_blob(q,":key",key,key_size) ||
       qbind_int64(q, ":role", ROLE_CLUSTER) ||
       qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE volumes (vid INTEGER PRIMARY KEY NOT NULL, volume TEXT ("STRIFY(SXLIMIT_MAX_VOLNAME_LEN)") NOT NULL UNIQUE, replica INTEGER NOT NULL, revs INTEGER NOT NULL, cursize INTEGER NOT NULL, maxsize INTEGER NOT NULL, enabled INTEGER NOT NULL DEFAULT 0, owner_id INTEGER NOT NULL REFERENCES users(uid), changed INTEGER NOT NULL DEFAULT 0)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE node_volume_updates (node BLOB ("STRIFY(UUID_BINARY_SIZE)") PRIMARY KEY NOT NULL, last_push INTEGER NOT NULL DEFAULT 0)") || qstep_noret(q))
        goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE vmeta (volume_id INTEGER NOT NULL REFERENCES volumes(vid) ON DELETE CASCADE ON UPDATE CASCADE, key TEXT ("STRIFY(SXLIMIT_META_MAX_KEY_LEN)") NOT NULL, value BLOB ("STRIFY(SXLIMIT_META_MAX_VALUE_LEN)") NOT NULL, PRIMARY KEY(volume_id, key))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE privs (volume_id INTEGER NOT NULL REFERENCES volumes(vid) ON DELETE CASCADE ON UPDATE CASCADE, user_id INTEGER NOT NULL REFERENCES users(uid) ON DELETE CASCADE, priv INTEGER NOT NULL, PRIMARY KEY (volume_id, user_id))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE faultynodes (dist INTEGER NOT NULL, node BLOB ("STRIFY(UUID_BINARY_SIZE)"), restored INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (dist, node))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE replaceblocks (node BLOB ("STRIFY(UUID_BINARY_SIZE)") NOT NULL PRIMARY KEY, last_block BLOB (21) NULL)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE replacefiles (vol TEXT NOT NULL PRIMARY KEY, file TEXT NOT NULL DEFAULT '', rev TEXT NOT NULL DEFAULT '', maxrev TEXT NOT NULL)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    qclose(&db);

    /* --- META dbs --- */
    for(i=0; i<METADBS; i++) {
	sprintf(path, "%s/f%08x.db", dir, i);
	sprintf(dbitem, "metadb_%08x", i);
	CREATE_DB(dbitem);
	qnullify(q); /* q is now prepared for hashfs insertions */

	/* Create META tables */
	if(qprep(db, &q, "CREATE TABLE files (fid INTEGER NOT NULL PRIMARY KEY, volume_id INTEGER NOT NULL, name TEXT ("STRIFY(SXLIMIT_MAX_FILENAME_LEN)") NOT NULL, size INTEGER NOT NULL, rev TEXT (56) NOT NULL, content BLOB NOT NULL, UNIQUE(volume_id, name, rev DESC))") || qstep_noret(q))
	    goto create_hashfs_fail;
	qnullify(q);
	if(qprep(db, &q, "CREATE TABLE fmeta (file_id INTEGER NOT NULL REFERENCES files(fid) ON DELETE CASCADE ON UPDATE CASCADE, key TEXT ("STRIFY(SXLIMIT_META_MAX_KEY_LEN)") NOT NULL, value BLOB ("STRIFY(SXLIMIT_META_MAX_VALUE_LEN)") NOT NULL, PRIMARY KEY(file_id, key))") || qstep_noret(q))
	    goto create_hashfs_fail;
	qnullify(q);
	if(qprep(db, &q, "CREATE TABLE relocs (file_id INTEGER NOT NULL PRIMARY KEY, dest BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL)") || qstep_noret(q)) /* NO FK for better normal use performance */
	    goto create_hashfs_fail;
	qnullify(q);

	qclose(&db);
    }

    /* --- HASH dbs --- */
    for(j = 0; j < SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    int fd;

	    sprintf(path, "%s/h%c%08x.db", dir, sizedirs[j], i);
	    sprintf(dbitem, "hashdb_%c_%08x", sizedirs[j], i);
	    CREATE_DB(dbitem);
	    sqlite3_reset(q); /* q is now prepared for hashfs insertions */
	    if(qbind_text(q, ":k", "block_size") || qbind_int(q, ":v", bsz[j]) || qstep_noret(q))
		goto create_hashfs_fail;
	    sqlite3_reset(q);
	    if(qbind_text(q, ":k", "next_blockno") || qbind_int(q, ":v", 1) || qstep_noret(q))
		goto create_hashfs_fail;
	    qnullify(q);

	    /* Create HASH tables */
	    if(qprep(db, &q, "CREATE TABLE blocks (\
                id INTEGER PRIMARY KEY NOT NULL,\
                hash BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                blockno INTEGER NOT NULL,\
                created_at INTEGER NOT NULL,\
                UNIQUE(hash))") || qstep_noret(q))
		goto create_hashfs_fail;
	    qnullify(q);
            /* if(qprep(db, &q, "CREATE INDEX blockno ON blocks(id, blockno)") || qstep_noret(q))
                goto create_hashfs_fail;
            qnullify(q);*/

	    /* Create freelist table */
	    if(qprep(db, &q, "CREATE TABLE avail (blocknumber INTEGER NOT NULL PRIMARY KEY ASC)") || qstep_noret(q))
		goto create_hashfs_fail;
	    qnullify(q);

	    qclose(&db);

	    /* Create DATA files */
	    sprintf(path, "%s/h%c%08x.bin", dir, sizedirs[j], i);
	    fd = creat(path, 0666);
	    if(fd < 0) {
		PCRIT("Cannot create data file %s", path);
		goto create_hashfs_fail;
	    }
	    memset(path, 0, bsz[j]);
	    sprintf(path, "%-16sdatafile_%c_%08x             %08x", HASHFS_VERSION_1_0, sizedirs[j], i, bsz[j]);
	    memcpy(path+64, cluster->binary, sizeof(cluster->binary));
	    if(write_block(fd, path, 0, bsz[j])) {
		close(fd);
		goto create_hashfs_fail;
	    }
	    if(close(fd)) {
		PCRIT("Cannot close data file %s", path);
		goto create_hashfs_fail;
	    }
	}
    }

    /* --- TEMP db --- */
    sprintf(path, "%s/temp.db", dir);
    CREATE_DB("tempdb");
    qnullify(q); /* q is now prepared for hashfs insertions */
    if(qprep(db, &q, "CREATE TABLE tmpfiles (tid INTEGER PRIMARY KEY, token TEXT (32) NULL UNIQUE, volume_id INTEGER NOT NULL, name TEXT ("STRIFY(SXLIMIT_MAX_FILENAME_LEN)") NOT NULL, size INTEGER NOT NULL DEFAULT 0, t TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f')), flushed INTEGER NOT NULL DEFAULT 0, content BLOB, uniqidx BLOB, ttl INTEGER NOT NULL DEFAULT 0, avail BLOB)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if (qprep(db, &q, "CREATE INDEX tmpfiles_ttl ON tmpfiles(ttl) WHERE ttl > 0") || qstep_noret(q))
        goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE tmpmeta (tid INTEGER NOT NULL REFERENCES tmpfiles(tid) ON DELETE CASCADE ON UPDATE CASCADE, key TEXT ("STRIFY(SXLIMIT_META_MAX_KEY_LEN)") NOT NULL, value BLOB ("STRIFY(SXLIMIT_META_MAX_VALUE_LEN)") NOT NULL, PRIMARY KEY (tid, key))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    qclose(&db);

    /* --- EVENT db --- */
    sprintf(path, "%s/events.db", dir);
    CREATE_DB("eventdb");
    qnullify(q); /* q is now prepared for hashfs insertions */
    if(qprep(db, &q, "INSERT INTO hashfs (key, value) VALUES ('next_version_check', datetime(strftime('%s', 'now') + (abs(random()) % 10800), 'unixepoch'))") || qstep_noret(q)) /* Schedule next version check within 3 hours */
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE jobs (job INTEGER NOT NULL PRIMARY KEY, parent INTEGER NULL, type INTEGER NOT NULL, lock TEXT NULL, data BLOB NOT NULL, sched_time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f')), expiry_time TEXT NOT NULL, complete INTEGER NOT NULL DEFAULT 0, result INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL DEFAULT \"\", user INTEGER NULL, UNIQUE(lock))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX jobs_status ON jobs (complete, sched_time)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX jobs_parent ON jobs (parent)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);

    if(qprep(db, &q, "CREATE TABLE actions (id INTEGER NOT NULL PRIMARY KEY, job_id INTEGER NOT NULL REFERENCES jobs(job) ON DELETE CASCADE ON UPDATE CASCADE, phase INTEGER NOT NULL DEFAULT 0, target BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL, addr TEXT NOT NULL, internaladdr TEXT NOT NULL, capacity INTEGER NOT NULL)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX actions_status ON actions (job_id, phase DESC)") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    qclose(&db);

    /* --- XFER db --- */
    sprintf(path, "%s/xfers.db", dir);
    CREATE_DB("xferdb");
    qnullify(q); /* q is now prepared for hashfs insertions */
    if(qprep(db, &q, "CREATE TABLE topush (id INTEGER NOT NULL PRIMARY KEY, block BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL, size INTEGER NOT NULL, node BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL, sched_time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f')), expiry_time TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now', '"STRIFY(TOPUSH_EXPIRE)" seconds')), UNIQUE (block, size, node))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE INDEX topush_sched ON topush(sched_time ASC, expiry_time)") || qstep_noret(q))
        goto create_hashfs_fail;
    qnullify(q);
    if(qprep(db, &q, "CREATE TABLE onhold (hid INTEGER NOT NULL PRIMARY KEY, hblock BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL, hsize INTEGER NOT NULL, hnode BLOB("STRIFY(UUID_BINARY_SIZE)") NOT NULL, UNIQUE (hblock, hsize, hnode))") || qstep_noret(q))
	goto create_hashfs_fail;
    qnullify(q);
    qclose(&db);

    /* do not modify the schema above (except for dropping tables),
     * put all modifications in the sx_storage_upgrade function! */
    ret = OK;

create_hashfs_fail:
    if (ret != OK)
	WARN("failed to create hashfs");
    sqlite3_finalize(q);
    qclose(&db);
    free(path);
    sxc_free_uri(uri);
    if(ret)
	sxi_rmdirs(dir);
    return ret;
}

rc_ty sx_storage_create(const char *dir, sx_uuid_t *cluster, uint8_t *key, int key_size) {
    rc_ty ret = sx_storage_create_1_0(dir, cluster, key, key_size);
    if (ret == OK)
        ret = sx_storage_upgrade(dir);
    if (ret == OK)
        sync();
    return ret;
}

static int qopen(const char *path, sxi_db_t **dbp, const char *dbtype, sx_uuid_t *cluster, const char *version) {
    sqlite3_stmt *q = NULL;
    const char *str;
    sqlite3 *handle = NULL;
    char qstr[1024];

    if(sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL)) {
	CRIT("Failed to open database %s: %s", path, sqlite3_errmsg(handle));
	goto qopen_fail;
    }
    if (!(*dbp = qnew(handle)))
        goto qopen_fail;
    if(sqlite3_busy_timeout(handle, db_busy_timeout * 1000)) {
	CRIT("Failed to set timeout on database %s: %s", path, sqlite3_errmsg(handle));
	goto qopen_fail;
    }
    if(qprep(*dbp, &q, "PRAGMA synchronous = NORMAL") || qstep_noret(q))
	goto qopen_fail;
    qnullify(q);
    /* TODO: pagesize might not always be 1024,
     * limits should be in bytes */
    snprintf(qstr, sizeof(qstr), "PRAGMA journal_size_limit = %d",
             db_max_restart_wal_pages * 1024);
    if(qprep(*dbp, &q, qstr) || qstep_ret(q))
	goto qopen_fail;
    qnullify(q);

    if(qprep(*dbp, &q, "SELECT value FROM hashfs WHERE key = :k"))
	goto qopen_fail;

    if(qbind_text(q, ":k", "version") || qstep_ret(q))
	goto qopen_fail;
    str = (const char *)sqlite3_column_text(q, 0);
    if(version && (!str || strcmp(str, version))) {
	msg_set_reason("Version mismatch on db %s: expected %s, found %s", path, HASHFS_VERSION, str ? str : "none");
        CRIT("%s", msg_get_reason());
	goto qopen_fail;
    }

    sqlite3_reset(q);
    if(qbind_text(q, ":k", "dbtype") || qstep_ret(q))
	goto qopen_fail;
    str = (const char *)sqlite3_column_text(q, 0);
    if(!str || (dbtype && strcmp(str, dbtype))) {
	CRIT("Type mismatch on db %s: expected %s, found %s", path, dbtype, str ? str : "none");
	goto qopen_fail;
    }

    if(cluster) {
	sqlite3_reset(q);
	if(qbind_text(q, ":k", "cluster") || qstep_ret(q))
	    goto qopen_fail;
	str = (const char *)sqlite3_column_blob(q, 0);
	if(!str || sqlite3_column_bytes(q, 0) != sizeof(cluster->binary) || memcmp(str, cluster->binary, sizeof(cluster->binary))) {
	    if(str) {
		sx_uuid_t wrong;
		uuid_from_binary(&wrong, str);
		CRIT("Cluster UUID mismatch on db %s: expected %s, found %s", path, cluster->string, wrong.string);
	    } else
		CRIT("Cluster UUID mismatch on db %s: expected %s, found NULL", path, cluster->string);
	    goto qopen_fail;
	}
    }

    sqlite3_finalize(q);

    return 0;

qopen_fail:
    WARN("failed to open '%s'", path);
    sqlite3_finalize(q);
    qclose(dbp);
    return 1;
}

struct rebalance_iter {
    unsigned sizeidx;
    unsigned ndbidx;
    unsigned rebalance_ver;
    int retry_mode;
    sqlite3_stmt *q[SIZES][HASHDBS];
    sqlite3_stmt *q_add;
    sqlite3_stmt *q_sel;
    sqlite3_stmt *q_remove;
    sqlite3_stmt *q_reset;
    sqlite3_stmt *q_count;
};

typedef struct {
    int64_t file_size;
    unsigned int block_size;
    unsigned int nblocks;
    unsigned int created_at;
    char name[SXLIMIT_MAX_FILENAME_LEN+2];
    char revision[REV_LEN+1];
} list_entry_t;

struct _sx_hashfs_t {
    uint8_t *blockbuf;

    sxi_db_t *db;
    sqlite3_stmt *q_getval;
    sqlite3_stmt *q_setval;
    sqlite3_stmt *q_delval;
    sqlite3_stmt *q_gethdrev;
    sqlite3_stmt *q_getuser;
    sqlite3_stmt *q_getuserbyid;
    sqlite3_stmt *q_getuserbyname;
    sqlite3_stmt *q_listusers;
    sqlite3_stmt *q_listusersbycid;
    sqlite3_stmt *q_listacl;
    sqlite3_stmt *q_createuser;
    sqlite3_stmt *q_createuser_meta;
    sqlite3_stmt *q_deleteuser;
    sqlite3_stmt *q_user_newkey;
    sqlite3_stmt *q_onoffuser;
    sqlite3_stmt *q_onoffuserclones;
    sqlite3_stmt *q_grant;
    sqlite3_stmt *q_getuid;
    sqlite3_stmt *q_getuidname;
    sqlite3_stmt *q_revoke;
    sqlite3_stmt *q_volbyname;
    sqlite3_stmt *q_volbyid;
    sqlite3_stmt *q_metaget;
    sqlite3_stmt *q_nextvol;
    sqlite3_stmt *q_getaccess;
    sqlite3_stmt *q_addvol;
    sqlite3_stmt *q_addvolmeta;
    sqlite3_stmt *q_addvolprivs;
    sqlite3_stmt *q_chprivs;
    sqlite3_stmt *q_dropvolprivs;
    sqlite3_stmt *q_onoffvol;
    sqlite3_stmt *q_getvolstate;
    sqlite3_stmt *q_delvol;
    sqlite3_stmt *q_chownvol;
    sqlite3_stmt *q_chownvolbyid;
    sqlite3_stmt *q_resizevol;
    sqlite3_stmt *q_changerevs;
    sqlite3_stmt *q_minreqs;
    sqlite3_stmt *q_updatevolcursize;
    sqlite3_stmt *q_setvolcursize;
    sqlite3_stmt *q_getnodepushtime;
    sqlite3_stmt *q_setnodepushtime;
    sqlite3_stmt *q_userisowner;
    sqlite3_stmt *q_getprivholder;

    sxi_db_t *tempdb;
    sqlite3_stmt *qt_new;
    sqlite3_stmt *qt_new4del;
    sqlite3_stmt *qt_update;
    sqlite3_stmt *qt_updateuniq;
    sqlite3_stmt *qt_extend;
    sqlite3_stmt *qt_addmeta;
    sqlite3_stmt *qt_delmeta;
    sqlite3_stmt *qt_getmeta;
    sqlite3_stmt *qt_countmeta;
    sqlite3_stmt *qt_gettoken;
    sqlite3_stmt *qt_tokendata;
    sqlite3_stmt *qt_tmpbyrev;
    sqlite3_stmt *qt_tmpdata;
    sqlite3_stmt *qt_delete;
    sqlite3_stmt *qt_flush;
    sqlite3_stmt *qt_gc_revisions;

    sxi_db_t *metadb[METADBS];
    sqlite3_stmt *qm_ins[METADBS];
    sqlite3_stmt *qm_list[METADBS];
    sqlite3_stmt *qm_list_eq[METADBS];
    sqlite3_stmt *qm_listrevs[METADBS];
    sqlite3_stmt *qm_listrevs_rev[METADBS];
    unsigned char qm_list_done[METADBS];
    uint64_t qm_list_queries;
    sqlite3_stmt *qm_get[METADBS];
    sqlite3_stmt *qm_getrev[METADBS];
    sqlite3_stmt *qm_findrev[METADBS];
    sqlite3_stmt *qm_oldrevs[METADBS];
    sqlite3_stmt *qm_metaget[METADBS];
    sqlite3_stmt *qm_metaset[METADBS];
    sqlite3_stmt *qm_metadel[METADBS];
    sqlite3_stmt *qm_delfile[METADBS];
    sqlite3_stmt *qm_wiperelocs[METADBS];
    sqlite3_stmt *qm_addrelocs[METADBS];
    sqlite3_stmt *qm_getreloc[METADBS];
    sqlite3_stmt *qm_delreloc[METADBS];
    sqlite3_stmt *qm_delbyvol[METADBS];
    sqlite3_stmt *qm_sumfilesizes[METADBS];
    sqlite3_stmt *qm_newest[METADBS];
    sqlite3_stmt *qm_count[METADBS];
    sqlite3_stmt *qm_list_rev_dec[METADBS];
    sqlite3_stmt *qm_list_file[METADBS];
    sqlite3_stmt *qm_add_heal[METADBS];
    sqlite3_stmt *qm_del_heal[METADBS];
    sqlite3_stmt *qm_get_rb[METADBS];
    sqlite3_stmt *qm_count_rb[METADBS];
    sqlite3_stmt *qm_add_heal_volume[METADBS];
    sqlite3_stmt *qm_sel_heal_volume[METADBS];
    sqlite3_stmt *qm_upd_heal_volume[METADBS];
    sqlite3_stmt *qm_del_heal_volume[METADBS];
    sqlite3_stmt *qm_needs_upgrade[METADBS];

    sxi_db_t *datadb[SIZES][HASHDBS];
    sqlite3_stmt *qb_nextavail[SIZES][HASHDBS];
    sqlite3_stmt *qb_nextalloc[SIZES][HASHDBS];
    sqlite3_stmt *qb_add[SIZES][HASHDBS];
    sqlite3_stmt *qb_setfree[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc1[SIZES][HASHDBS];
    sqlite3_stmt *qb_get[SIZES][HASHDBS];
    sqlite3_stmt *qb_bumpavail[SIZES][HASHDBS];
    sqlite3_stmt *qb_bumpalloc[SIZES][HASHDBS];
    sqlite3_stmt *qb_addtoken[SIZES][HASHDBS];
    sqlite3_stmt *qb_moduse[SIZES][HASHDBS];
    sqlite3_stmt *qb_reserve[SIZES][HASHDBS];
    sqlite3_stmt *qb_get_meta[SIZES][HASHDBS];
    sqlite3_stmt *qb_del_reserve[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_unused_revision[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_unused_block[SIZES][HASHDBS];
    sqlite3_stmt *qb_deleteold[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_expired_reservation[SIZES][HASHDBS];
    sqlite3_stmt *qb_find_expired_reservation2[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc_revision_blocks[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc_revision[SIZES][HASHDBS];
    sqlite3_stmt *qb_gc_reserve[SIZES][HASHDBS];

    sxi_db_t *eventdb;
    sqlite3_stmt *qe_getjob;
    sqlite3_stmt *qe_getfiledeljob;
    sqlite3_stmt *qe_addjob;
    sqlite3_stmt *qe_addact;
    sqlite3_stmt *qe_countjobs;
    sqlite3_stmt *qe_islocked;
    sqlite3_stmt *qe_hasjobs;
    sqlite3_stmt *qe_lock;
    sqlite3_stmt *qe_unlock;
    sqlite3_stmt *qe_gc;
    sqlite3_stmt *qe_count_upgradejobs;
    int addjob_begun;

    sxi_db_t *xferdb;
    sqlite3_stmt *qx_add;
    sqlite3_stmt *qx_wipehold;
    sqlite3_stmt *qx_hold;
    sqlite3_stmt *qx_isheld;
    sqlite3_stmt *qx_release;
    sqlite3_stmt *qx_hasheld;

    struct timeval volsizes_push_timestamp;

    char *ssl_ca_file;
    char *cluster_name;
    uint16_t http_port;

    sxi_hdist_t *hd;
    sx_nodelist_t *prev_dist, *next_dist, *nextprev_dist, *prevnext_dist, *faulty_nodes, *ignored_nodes, *effprev_dist, *effnext_dist, *effnextprev_dist, *effprevnext_dist;
    int64_t hd_rev;
    unsigned int have_hd, is_rebalancing, is_orphan;
    time_t last_dist_change;

    sx_hashfs_volume_t curvol;
    const uint8_t *curvoluser;

    sx_hashfs_user_t curclone;
    int listinactiveclones;

    sx_hashfs_file_t list_file;
    int list_recurse;
    int64_t list_volid;
    char list_pattern[SXLIMIT_MAX_FILENAME_LEN+2]; /* +2 -> NUL byte plus possibly added glob if pattern ends with slash */
    list_entry_t list_cache[METADBS];
    unsigned int list_pattern_slashes; /* Number of slashes in pattern */
    int list_pattern_end_with_slash; /* 1 if pattern ends with slash */

    /* fields below are used during iteration */
    /* No need to append byte for asterisk, because that limit is up to first NUL byte */
    char list_lower_limit[SXLIMIT_MAX_FILENAME_LEN+1];
    char list_upper_limit[SXLIMIT_MAX_FILENAME_LEN+1];
    int list_limit_len; /* Both itername and itername_limit will have the same length */

    int64_t get_id;
    const sx_hash_t *get_content;
    unsigned int get_nblocks;
    unsigned int get_replica;
    int get_ndb;
    int rev_ndb;


    int64_t put_id;
    int64_t put_extendsize;
    unsigned int put_extendfrom;
    unsigned int put_putblock;
    unsigned int put_getblock;
    unsigned int put_checkblock;
    unsigned int put_singlecheck;
    unsigned int put_replica;
    int64_t upload_minspeed;
    unsigned int put_hs;
    unsigned int put_success;
    sx_hash_t *put_blocks;
    sx_hash_t put_reserve_id;
    sx_hash_t put_revision_id;
    unsigned int *put_nidxs;
    unsigned int *put_hashnos;
    unsigned int put_nblocks;
    char put_token[TOKEN_TEXT_LEN + 1];
    struct {
	char key[SXLIMIT_META_MAX_KEY_LEN+1];
	uint8_t value[SXLIMIT_META_MAX_VALUE_LEN];
	int value_len;
    } meta[SXLIMIT_META_MAX_ITEMS];
    unsigned int nmeta;

    unsigned int relocdb_start, relocdb_cur;
    int64_t relocid;


    int datafd[SIZES][HASHDBS];
    sx_uuid_t cluster_uuid, node_uuid; /* MODHDIST: store sx_node_t instead - see sx_hashfs_self */
    char version[16];
    sx_hash_t tokenkey;

    sxc_client_t *sx;
    sxi_conns_t *sx_clust;
    sxi_hashop_t hc;
    char root_auth[AUTHTOK_ASCII_LEN+1];

    int job_trigger, xfer_trigger, gc_trigger, gc_expire_trigger;
    char job_message[JOB_FAIL_REASON_SIZE];

    char *dir;
    int gcver;
    int gc_wal_pages;
    struct rebalance_iter rit;

    int readonly;
    int lockfd;
};

static void close_all_dbs(sx_hashfs_t *h) {
    unsigned int i, j;

    sqlite3_finalize(h->qx_add);
    sqlite3_finalize(h->qx_hold);
    sqlite3_finalize(h->qx_isheld);
    sqlite3_finalize(h->qx_release);
    sqlite3_finalize(h->qx_hasheld);
    sqlite3_finalize(h->qx_wipehold);
    qclose(&h->xferdb);

    sqlite3_finalize(h->qe_getjob);
    sqlite3_finalize(h->qe_getfiledeljob);
    sqlite3_finalize(h->qe_addjob);
    sqlite3_finalize(h->qe_addact);
    sqlite3_finalize(h->qe_countjobs);
    sqlite3_finalize(h->qe_islocked);
    sqlite3_finalize(h->qe_hasjobs);
    sqlite3_finalize(h->qe_lock);
    sqlite3_finalize(h->qe_unlock);
    sqlite3_finalize(h->qe_gc);
    sqlite3_finalize(h->qe_count_upgradejobs);

    sqlite3_finalize(h->rit.q_add);
    sqlite3_finalize(h->rit.q_sel);
    sqlite3_finalize(h->rit.q_remove);
    sqlite3_finalize(h->rit.q_reset);
    sqlite3_finalize(h->rit.q_count);

    qclose(&h->eventdb);

    for(j=0; j<SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    sqlite3_finalize(h->qb_nextavail[j][i]);
	    sqlite3_finalize(h->qb_nextalloc[j][i]);
	    sqlite3_finalize(h->qb_add[j][i]);
	    sqlite3_finalize(h->qb_setfree[j][i]);
	    sqlite3_finalize(h->qb_gc1[j][i]);
	    sqlite3_finalize(h->qb_get[j][i]);
	    sqlite3_finalize(h->qb_bumpavail[j][i]);
	    sqlite3_finalize(h->qb_bumpalloc[j][i]);
            sqlite3_finalize(h->qb_addtoken[j][i]);
            sqlite3_finalize(h->qb_moduse[j][i]);
            sqlite3_finalize(h->qb_reserve[j][i]);
            sqlite3_finalize(h->qb_get_meta[j][i]);
            sqlite3_finalize(h->qb_del_reserve[j][i]);
            sqlite3_finalize(h->qb_find_unused_revision[j][i]);
            sqlite3_finalize(h->qb_find_unused_block[j][i]);
            sqlite3_finalize(h->qb_deleteold[j][i]);
            sqlite3_finalize(h->rit.q[j][i]);
            sqlite3_finalize(h->qb_find_expired_reservation[j][i]);
            sqlite3_finalize(h->qb_find_expired_reservation2[j][i]);
            sqlite3_finalize(h->qb_gc_revision_blocks[j][i]);
            sqlite3_finalize(h->qb_gc_revision[j][i]);
            sqlite3_finalize(h->qb_gc_reserve[j][i]);
	    qclose(&h->datadb[j][i]);

	    if(h->datafd[j][i] >= 0)
		close(h->datafd[j][i]);
	}
    }
    for(i=0; i<METADBS; i++) {
	sqlite3_finalize(h->qm_ins[i]);
	sqlite3_finalize(h->qm_list[i]);
        sqlite3_finalize(h->qm_list_eq[i]);
	sqlite3_finalize(h->qm_listrevs[i]);
        sqlite3_finalize(h->qm_listrevs_rev[i]);
	sqlite3_finalize(h->qm_get[i]);
	sqlite3_finalize(h->qm_getrev[i]);
	sqlite3_finalize(h->qm_findrev[i]);
	sqlite3_finalize(h->qm_oldrevs[i]);
	sqlite3_finalize(h->qm_metaget[i]);
	sqlite3_finalize(h->qm_metaset[i]);
	sqlite3_finalize(h->qm_metadel[i]);
	sqlite3_finalize(h->qm_delfile[i]);
	sqlite3_finalize(h->qm_wiperelocs[i]);
	sqlite3_finalize(h->qm_addrelocs[i]);
	sqlite3_finalize(h->qm_getreloc[i]);
	sqlite3_finalize(h->qm_delreloc[i]);
	sqlite3_finalize(h->qm_delbyvol[i]);
        sqlite3_finalize(h->qm_sumfilesizes[i]);
        sqlite3_finalize(h->qm_newest[i]);
        sqlite3_finalize(h->qm_count[i]);
        sqlite3_finalize(h->qm_list_rev_dec[i]);
        sqlite3_finalize(h->qm_list_file[i]);
        sqlite3_finalize(h->qm_del_heal[i]);
        sqlite3_finalize(h->qm_add_heal[i]);
        sqlite3_finalize(h->qm_get_rb[i]);
        sqlite3_finalize(h->qm_count_rb[i]);
        sqlite3_finalize(h->qm_add_heal_volume[i]);
        sqlite3_finalize(h->qm_sel_heal_volume[i]);
        sqlite3_finalize(h->qm_upd_heal_volume[i]);
        sqlite3_finalize(h->qm_del_heal_volume[i]);
        sqlite3_finalize(h->qm_needs_upgrade[i]);
	qclose(&h->metadb[i]);
    }

    sqlite3_finalize(h->q_addvol);
    sqlite3_finalize(h->q_addvolmeta);
    sqlite3_finalize(h->q_addvolprivs);
    sqlite3_finalize(h->q_dropvolprivs);
    sqlite3_finalize(h->q_chprivs);
    sqlite3_finalize(h->q_onoffvol);
    sqlite3_finalize(h->q_getvolstate);
    sqlite3_finalize(h->q_delvol);
    sqlite3_finalize(h->q_chownvol);
    sqlite3_finalize(h->q_chownvolbyid);
    sqlite3_finalize(h->q_resizevol);
    sqlite3_finalize(h->q_changerevs);
    sqlite3_finalize(h->q_minreqs);
    sqlite3_finalize(h->q_updatevolcursize);
    sqlite3_finalize(h->q_setvolcursize);
    sqlite3_finalize(h->q_getnodepushtime);
    sqlite3_finalize(h->q_setnodepushtime);
    sqlite3_finalize(h->q_onoffuser);
    sqlite3_finalize(h->q_onoffuserclones);
    sqlite3_finalize(h->q_gethdrev);
    sqlite3_finalize(h->q_getuser);
    sqlite3_finalize(h->q_getuserbyid);
    sqlite3_finalize(h->q_getuserbyname);
    sqlite3_finalize(h->q_listusers);
    sqlite3_finalize(h->q_listusersbycid);
    sqlite3_finalize(h->q_listacl);
    sqlite3_finalize(h->q_getaccess);
    sqlite3_finalize(h->q_createuser);
    sqlite3_finalize(h->q_createuser_meta);
    sqlite3_finalize(h->q_deleteuser);
    sqlite3_finalize(h->q_user_newkey);
    sqlite3_finalize(h->q_grant);
    sqlite3_finalize(h->q_getuid);
    sqlite3_finalize(h->q_getuidname);
    sqlite3_finalize(h->q_revoke);
    sqlite3_finalize(h->q_nextvol);
    sqlite3_finalize(h->q_userisowner);
    sqlite3_finalize(h->q_getprivholder);

    sqlite3_finalize(h->qt_new);
    sqlite3_finalize(h->qt_new4del);
    sqlite3_finalize(h->qt_update);
    sqlite3_finalize(h->qt_updateuniq);
    sqlite3_finalize(h->qt_extend);
    sqlite3_finalize(h->qt_addmeta);
    sqlite3_finalize(h->qt_delmeta);
    sqlite3_finalize(h->qt_getmeta);
    sqlite3_finalize(h->qt_countmeta);
    sqlite3_finalize(h->qt_gettoken);
    sqlite3_finalize(h->qt_tmpdata);
    sqlite3_finalize(h->qt_tokendata);
    sqlite3_finalize(h->qt_tmpbyrev);
    sqlite3_finalize(h->qt_delete);
    sqlite3_finalize(h->qt_flush);
    sqlite3_finalize(h->qt_gc_revisions);

    sqlite3_finalize(h->q_volbyname);
    sqlite3_finalize(h->q_volbyid);
    sqlite3_finalize(h->q_metaget);
    sqlite3_finalize(h->q_delval);
    sqlite3_finalize(h->q_setval);
    sqlite3_finalize(h->q_getval);
    qclose(&h->tempdb);
    qclose(&h->db);
}

/* TODO: shouldn't use hidden variables */
#define OPEN_DB(DBNAME, DBHANDLE) \
do {\
    sqlite3_reset(h->q_getval); \
    if(qbind_text(h->q_getval, ":k", DBNAME) || qstep_ret(h->q_getval)) {\
	WARN("Couldn't find DB '%s'", DBNAME);				\
	goto open_hashfs_fail; \
    }\
    str = (const char *)sqlite3_column_text(h->q_getval, 0); \
    if(!str) \
	goto open_hashfs_fail; \
    if(*str != '/') { \
	unsigned int subpathlen = strlen(str) + 1; \
	if(subpathlen > pathlen) { \
	    pathlen = subpathlen; \
	    if(!(path = wrap_realloc_or_free(path, dirlen + pathlen))) \
		goto open_hashfs_fail; \
	} \
	memcpy(path + dirlen, str, subpathlen); \
	str = path; \
    } \
    if(qopen(str, DBHANDLE, DBNAME, &h->cluster_uuid, HASHFS_VERSION)) \
	goto open_hashfs_fail; \
} while(0)


void sx_hashfs_checkpoint_passive(sx_hashfs_t *h)
{
    unsigned i, j;
    /* PASSIVE: doesn't block writers/readers
     * FULL/RESTART: blocks writers (not reader)
     * by default sqlite performs a PASSIVE checkpoint every 1000 pages,
     * and ignores errors
     * */
    qcheckpoint(h->db);
    qcheckpoint(h->tempdb);
    for (i=0;i<METADBS;i++)
        qcheckpoint(h->metadb[i]);
    for (i=0;i<SIZES;i++)
        for (j=0;j<HASHDBS;j++)
            qcheckpoint(h->datadb[i][j]);
    qcheckpoint(h->eventdb);
    qcheckpoint(h->xferdb);
}

void sx_hashfs_checkpoint_gc(sx_hashfs_t *h)
{
}

void sx_hashfs_checkpoint_xferdb(sx_hashfs_t *h)
{
    qcheckpoint_idle(h->xferdb);
}

void sx_hashfs_checkpoint_eventdb(sx_hashfs_t *h)
{
    qcheckpoint_idle(h->eventdb);
    qcheckpoint_idle(h->tempdb);
}

static int load_config(sx_hashfs_t *h, sxc_client_t *sx) {
    const void *p;
    int r, load_faulty = 0, ret = -1;
    unsigned int i;

    DEBUG("Reloading cluster configuration");

    free(h->cluster_name);
    h->cluster_name = NULL;
    free(h->ssl_ca_file);
    h->ssl_ca_file = NULL;
    if(h->have_hd)
	sxi_hdist_free(h->hd);
    h->have_hd = 0;
    h->hd_rev = 0;
    sx_nodelist_delete(h->next_dist);
    h->next_dist = NULL;
    sx_nodelist_delete(h->prev_dist);
    h->prev_dist = NULL;
    sx_nodelist_delete(h->nextprev_dist);
    h->nextprev_dist = NULL;
    sx_nodelist_delete(h->prevnext_dist);
    h->prevnext_dist = NULL;
    sx_nodelist_delete(h->effprev_dist);
    h->effprev_dist = NULL;
    sx_nodelist_delete(h->effnext_dist);
    h->effnext_dist = NULL;
    sx_nodelist_delete(h->effnextprev_dist);
    h->effnextprev_dist = NULL;
    sx_nodelist_delete(h->effprevnext_dist);
    h->effprevnext_dist = NULL;
    sx_nodelist_delete(h->faulty_nodes);
    h->faulty_nodes = NULL;
    sx_nodelist_delete(h->ignored_nodes);
    h->ignored_nodes = NULL;
    h->is_rebalancing = 0;
    h->sx = sx;
    h->last_dist_change = time(NULL);

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "cluster_name"))
	goto load_config_fail;
    switch(qstep(h->q_getval)) {
    case SQLITE_ROW:
	/* MODHDIST: this is an operational node */
	h->cluster_name = wrap_strdup((const char*)sqlite3_column_text(h->q_getval, 0));
	if (!h->cluster_name)
	    goto load_config_fail;

	h->http_port = 0;
	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "http_port")) {
	    CRIT("Failed to retrieve network settings from database");
	    goto load_config_fail;
	}
	r = qstep(h->q_getval);
	if(r == SQLITE_ROW)
	    h->http_port = sqlite3_column_int(h->q_getval, 0);
	else if(r != SQLITE_DONE) {
	    CRIT("Failed to retrieve network settings from database");
	    goto load_config_fail;
	}

	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "ssl_ca_file")) {
	    CRIT("Failed to retrieve security certificate from database");
	    goto load_config_fail;
	}

	r = qstep(h->q_getval);
	if(r == SQLITE_ROW) {
            const char *relpath = (const char*)sqlite3_column_text(h->q_getval, 0);
            unsigned cafilen;

	    cafilen = strlen(relpath);
	    if(cafilen) {
		cafilen += strlen(h->dir) + 2;
		h->ssl_ca_file = wrap_malloc(cafilen);
		if(!h->ssl_ca_file)
		    goto load_config_fail;
		if (*relpath == '/')
		    snprintf(h->ssl_ca_file, cafilen, "%s", relpath);
		else
		    snprintf(h->ssl_ca_file, cafilen, "%s/%s", h->dir, relpath);
	    }
	} else if(r != SQLITE_DONE) {
	    CRIT("Failed to retrieve node CA certificate file from database");
	    goto load_config_fail;
	}

	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "node") || qstep_ret(h->q_getval)) {
	    CRIT("Failed to retrieve node UUID from database");
	    goto load_config_fail;
	}
	p = sqlite3_column_blob(h->q_getval, 0);
	if(!p || sqlite3_column_bytes(h->q_getval, 0) != sizeof(h->node_uuid.binary)) {
	    CRIT("Bad node UUID retrieved from database");
	    goto load_config_fail;
	}
	uuid_from_binary(&h->node_uuid, p);

	sqlite3_reset(h->q_getval);
	if(qbind_text(h->q_getval, ":k", "current_dist_rev")) {
	    CRIT("Failed to retrieve cluster distribution from database");
	    goto load_config_fail;
	}

	r = qstep(h->q_getval);
	if(r == SQLITE_ROW) {
	    h->hd_rev = sqlite3_column_int64(h->q_getval, 0);

	    sqlite3_reset(h->q_getval);
	    if(qbind_text(h->q_getval, ":k", "current_dist") || qstep_ret(h->q_getval)) {
		CRIT("Failed to retrieve cluster distribution from database");
		goto load_config_fail;
	    }
	    if(!sqlite3_column_bytes(h->q_getval, 0)) {
		/* MODHDIST: this node has received its configuration but the hdist model wasn't
		 * enabled yet so we consider it effectively the same as a bare node */
		sqlite3_reset(h->q_getval);
		free(h->cluster_name);
		h->cluster_name = NULL;
		h->hd_rev = 0;
		break;
	    }
	} else if(r == SQLITE_DONE) {
	    if(qbind_text(h->q_getval, ":k", "dist_rev") || qstep_ret(h->q_getval)) {
		CRIT("Failed to retrieve cluster distribution from database");
		goto load_config_fail;
	    }
	    h->hd_rev = sqlite3_column_int64(h->q_getval, 0);

	    sqlite3_reset(h->q_getval);
	    if(qbind_text(h->q_getval, ":k", "dist") || qstep_ret(h->q_getval)) {
		CRIT("Failed to retrieve cluster distribution from database");
		goto load_config_fail;
	    }
	    load_faulty = 1;
	} else {
	    CRIT("Failed to retrieve cluster distribution from database");
	    goto load_config_fail;
	}
	p = sqlite3_column_blob(h->q_getval, 0);
	if(!p) {
	    CRIT("Bad cluster distribution retrieved from database");
	    goto load_config_fail;
	}
	if(!(h->hd = sxi_hdist_from_cfg(p, sqlite3_column_bytes(h->q_getval, 0)))) {
	    CRIT("Failed to load cluster distribution");
	    goto load_config_fail;
	}

	h->next_dist = sx_nodelist_dup(sxi_hdist_nodelist(h->hd, 0));
	if(sxi_hdist_buildcnt(h->hd) == 1) {
	    h->prev_dist = sx_nodelist_dup(h->next_dist);
	    if(!sx_nodelist_lookup(h->next_dist, &h->node_uuid)) {
		INFO("THIS NODE IS NO LONGER A CLUSTER MEMBER");
		h->is_orphan = 1;
	    }
	} else if(sxi_hdist_buildcnt(h->hd) == 2) {
	    h->prev_dist = sx_nodelist_dup(sxi_hdist_nodelist(h->hd, 1));
	    h->is_rebalancing = 1;
	} else {
	    CRIT("Failed to load cluster distribution: too many models");
	    goto load_config_fail;
	}

	h->nextprev_dist = sx_nodelist_dup(h->next_dist);
	h->prevnext_dist = sx_nodelist_dup(h->prev_dist);
	if(!h->next_dist || !h->prev_dist ||
	   sx_nodelist_addlist(h->nextprev_dist, h->prev_dist) ||
	   sx_nodelist_addlist(h->prevnext_dist, h->next_dist)) {
	    CRIT("Failed to allocate cluster distribution lists");
	    goto load_config_fail;
	}

	h->faulty_nodes = sx_nodelist_new();
	if(!h->faulty_nodes) {
	    CRIT("Failed to retrieve list of faulty nodes from database");
	    goto load_config_fail;
	}
	if(load_faulty) {
	    sqlite3_stmt *qfaulty = NULL;
	    if(qprep(h->db, &qfaulty, "SELECT node FROM faultynodes WHERE dist = :dist") ||
	       qbind_int64(qfaulty, ":dist", h->hd_rev)) {
		CRIT("Failed to retrieve list of faulty nodes from database");
		qnullify(qfaulty);
		goto load_config_fail;
	    }
	    while((r = qstep(qfaulty)) == SQLITE_ROW) {
		sx_uuid_t faultyid;
		const sx_node_t *faultynode;
		p = sqlite3_column_blob(qfaulty, 0);
		if(!p || sqlite3_column_bytes(qfaulty, 0) != sizeof(faultyid.binary))
		    break;
		uuid_from_binary(&faultyid, p);
		if(!(faultynode = sx_nodelist_lookup(h->next_dist, &faultyid)) ||
		   sx_nodelist_add(h->faulty_nodes, sx_node_dup(faultynode)))
		    break;
	    }
	    qnullify(qfaulty);
	    if(r != SQLITE_DONE) {
		CRIT("Failed to retrieve list of faulty nodes from database");
		goto load_config_fail;
	    }
	}

	sqlite3_reset(h->q_getval);
	if(!h->sx_clust)
	    h->sx_clust = sxi_conns_new(sx);
	if(!h->sx_clust ||
	   sxi_conns_set_uuid(h->sx_clust, h->cluster_uuid.string) ||
	   sxi_conns_set_auth(h->sx_clust, h->root_auth) ||
	   sxi_conns_set_port(h->sx_clust, h->http_port) ||
	   sxi_conns_set_sslname(h->sx_clust, h->cluster_name) ||
	   sxi_conns_disable_proxy(h->sx_clust)) {
	    CRIT("Failed to initialize cluster connectors");
	    goto load_config_fail;
	}
	sxi_conns_set_cafile(h->sx_clust, h->ssl_ca_file);
	sxi_conns_disable_blacklisting(h->sx_clust);
	if(sxi_conns_set_timeouts(h->sx_clust, SXI_CONNS_HARD_TIMEOUT, SXI_CONNS_SOFT_TIMEOUT))
	    WARN("Failed to set connection timeouts");

	h->ignored_nodes = sx_nodelist_new();
	if(h->ignored_nodes) {
	    sqlite3_stmt *qignnodes = NULL;
	    if(qprep(h->db, &qignnodes, "SELECT node FROM ignorednodes WHERE dist = :dist") ||
	       qbind_int64(qignnodes, ":dist", h->hd_rev)) {
		CRIT("Failed to retrieve list of ignored nodes from database");
		qnullify(qignnodes);
		goto load_config_fail;
	    }
	    while((r = qstep(qignnodes)) == SQLITE_ROW) {
		sx_uuid_t ignuuid;
		sx_node_t *ignme;
		p = sqlite3_column_blob(qignnodes, 0);
		if(!p || sqlite3_column_bytes(qignnodes, 0) != sizeof(ignuuid.binary))
		    break;
		uuid_from_binary(&ignuuid, p);
		ignme = sx_node_new(&ignuuid, "127.0.0.1", "127.0.0.1", 1);
		INFO("Node %s is ignored", ignuuid.string);
		if(!ignme || sx_nodelist_add(h->ignored_nodes, ignme))
		    break;
	    }
	    qnullify(qignnodes);
	    if(r != SQLITE_DONE) {
		CRIT("Failed to retrieve list of ignored nodes from database");
		goto load_config_fail;
	    }
	} else {
	    CRIT("Failed to allocate list of ignored nodes from database");
	    goto load_config_fail;
	}

	h->effprev_dist = sx_nodelist_new();
	for(i=0; i<sx_nodelist_count(h->prev_dist); i++) {
	    const sx_node_t *node = sx_nodelist_get(h->prev_dist, i);
	    if(sx_nodelist_lookup(h->ignored_nodes, sx_node_uuid(node)) ||
	       !sx_nodelist_add(h->effprev_dist, sx_node_dup(node)))
		continue;
	    CRIT("Failed to build list of effective nodes");
	    goto load_config_fail;
	}
	h->effnext_dist = sx_nodelist_new();
	for(i=0; i<sx_nodelist_count(h->next_dist); i++) {
	    const sx_node_t *node = sx_nodelist_get(h->next_dist, i);
	    if(sx_nodelist_lookup(h->ignored_nodes, sx_node_uuid(node)) ||
	       !sx_nodelist_add(h->effnext_dist, sx_node_dup(node)))
		continue;
	    CRIT("Failed to build list of effective nodes");
	    goto load_config_fail;
	}
	h->effprevnext_dist = sx_nodelist_new();
	for(i=0; i<sx_nodelist_count(h->prevnext_dist); i++) {
	    const sx_node_t *node = sx_nodelist_get(h->prevnext_dist, i);
	    if(sx_nodelist_lookup(h->ignored_nodes, sx_node_uuid(node)) ||
	       !sx_nodelist_add(h->effprevnext_dist, sx_node_dup(node)))
		continue;
	    CRIT("Failed to build list of effective nodes");
	    goto load_config_fail;
	}
	h->effnextprev_dist = sx_nodelist_new();
	for(i=0; i<sx_nodelist_count(h->nextprev_dist); i++) {
	    const sx_node_t *node = sx_nodelist_get(h->nextprev_dist, i);
	    if(sx_nodelist_lookup(h->ignored_nodes, sx_node_uuid(node)) ||
	       !sx_nodelist_add(h->effnextprev_dist, sx_node_dup(node)))
		continue;
	    CRIT("Failed to build list of effective nodes");
	    goto load_config_fail;
	}


	h->have_hd = 1;
	break;

    case SQLITE_DONE:
	/* MODHDIST: this is a bare node which cannot operate until programmed */
	h->cluster_name = NULL;
	break;

    default:
	CRIT("Failed to retrieve cluster name from database");
	goto load_config_fail;
    }

    ret = 0;
 load_config_fail:
    sqlite3_reset(h->q_getval);
    return ret;
}

sx_hashfs_t *sx_hashfs_open(const char *dir, sxc_client_t *sx) {
    unsigned int dirlen, pathlen, i, j;
    sqlite3_stmt *q = NULL;
    char *path, dbitem[64], qrybuff[128];
    const char *str;
    sx_hashfs_t *h;
    struct flock fl;

    if(!dir || !(dirlen = strlen(dir))) {
	CRIT("Bad path");
	return NULL;
    }
    if (ssl_version_check())
	return NULL;

    if(!(h = wrap_calloc(1, sizeof(*h))))
	return NULL;
    memset(h->datafd, -1, sizeof(h->datafd));
    h->lockfd = -1;
    h->sx = NULL;
    h->job_trigger = h->xfer_trigger = h->gc_trigger = h->gc_expire_trigger = -1;
    /* TODO: read from hashfs kv store */
    h->upload_minspeed = GC_UPLOAD_MINSPEED;

    dirlen++;
    pathlen = 1024;
    if(!(path = wrap_malloc(dirlen + pathlen)))
	goto open_hashfs_fail;
    h->dir = strdup(dir);
    if (!h->dir)
        goto open_hashfs_fail;

    sprintf(path, "%s/hashfs.lock", dir);
    h->lockfd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if(h->lockfd < 0) {
        WARN("Failed to open %s lockfile: %s", path, strerror(errno));
        goto open_hashfs_fail;
    }

    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    if(fcntl(h->lockfd, F_SETLK, &fl) == -1) {
        if(errno == EACCES || errno == EAGAIN)
            INFO("Failed lock HashFS: Storage is locked for maintenance");
        else
            WARN("Failed to acquire read lock: %s", strerror(errno));
        goto open_hashfs_fail;
    }

    if (!qlog_set) {
	sqlite3_config(SQLITE_CONFIG_LOG, qlog, NULL);
	qlog_set = 1;
    }

    gettimeofday(&h->volsizes_push_timestamp, NULL);

    /* reset sqlite3's PRNG, to avoid generating colliding tempfile names in
     * forked processes */
    sqlite3_initialize();
    sqlite3_test_control(SQLITE_TESTCTRL_PRNG_RESET);
    /* reset OpenSSL's PRNG otherwise it'll share state after a fork */
    sxi_rand_cleanup();

    sprintf(path, "%s/hashfs.db", dir);
    if(qopen(path, &h->db, "hashfs", NULL, HASHFS_VERSION))
	goto open_hashfs_fail;
    if(qprep(h->db, &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
	goto open_hashfs_fail;
    qnullify(q);
    if(qprep(h->db, &h->q_getval, "SELECT value FROM hashfs WHERE key = :k"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_setval, "INSERT OR REPLACE INTO hashfs (key,value) VALUES (:k, :v)"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_delval, "DELETE FROM hashfs WHERE key = :k"))
        goto open_hashfs_fail;
    if(qbind_text(h->q_getval, ":k", "cluster") || qstep_ret(h->q_getval))
	goto open_hashfs_fail;
    str = (const char *)sqlite3_column_blob(h->q_getval, 0);
    if(!str || sqlite3_column_bytes(h->q_getval, 0) != sizeof(h->cluster_uuid.binary)) {
	CRIT("Failed to retrieve cluster UUID from database");
	goto open_hashfs_fail;
    }
    uuid_from_binary(&h->cluster_uuid, str);

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "version") || qstep_ret(h->q_getval))
	goto open_hashfs_fail;
    str = (const char *)sqlite3_column_text(h->q_getval, 0);
    if(!str || strlen(str) >= sizeof(h->version)) {
	CRIT("Failed to retrieve HashFS version from database");
	goto open_hashfs_fail;
    }
    strcpy(h->version, str);

    if(qprep(h->db, &q, "SELECT key FROM users WHERE uid = 0 AND role = "STRIFY(ROLE_CLUSTER)" AND enabled = 1") || qstep_ret(q)) {
	CRIT("Failed to retrieve cluster key from database");
	goto open_hashfs_fail;
    }
    if(!(str = sqlite3_column_blob(q, 0)) || sqlite3_column_bytes(q, 0) != AUTH_KEY_LEN) {
	CRIT("Bad cluster key retrieved from database");
	goto open_hashfs_fail;
    }
    if(encode_auth_bin(CLUSTER_USER, (const unsigned char *)str, AUTH_KEY_LEN, h->root_auth, sizeof(h->root_auth))) {
	CRIT("Failed to encode cluster key");
	goto open_hashfs_fail;
    }
    if(hash_buf("", 0, str, AUTH_KEY_LEN, &h->tokenkey)) {
	CRIT("Failed to generate token key");
	goto open_hashfs_fail;
    }
    qnullify(q);
    if(load_config(h, sx))
	goto open_hashfs_fail;

    if(qprep(h->db, &h->q_gethdrev, "SELECT MIN(value) FROM hashfs WHERE key IN ('current_dist_rev','dist_rev')"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuser, "SELECT uid, key, role, desc FROM users LEFT JOIN usermeta ON users.uid=usermeta.userid WHERE user = :user AND enabled=1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuserbyid, "SELECT user FROM users WHERE uid = :uid AND (:inactivetoo OR enabled=1)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuserbyname, "SELECT user FROM users WHERE name = :name AND (:inactivetoo OR enabled=1)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_listusers, "SELECT uid, name, user, key, role, desc FROM users LEFT JOIN usermeta ON users.uid=usermeta.userid WHERE uid > :lastuid AND enabled=1 ORDER BY uid ASC LIMIT 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_listusersbycid, "SELECT uid, name, user, key, role, desc FROM users LEFT JOIN usermeta ON users.uid=usermeta.userid WHERE uid > :lastuid AND (:inactivetoo OR enabled=1) AND SUBSTR(user, 0, "STRIFY(AUTH_CID_LEN)") = SUBSTR(:common_id, 0, "STRIFY(AUTH_CID_LEN)") ORDER BY uid ASC LIMIT 1"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_listacl, "SELECT name, priv, uid, owner_id FROM privs, volumes INNER JOIN users ON user_id=uid WHERE volume_id=:volid AND vid=:volid AND volumes.enabled = 1 AND users.enabled = 1 AND (priv <> 0 OR owner_id=uid) AND user_id > :lastuid ORDER BY user_id ASC LIMIT 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getaccess, "SELECT privs.priv, volumes.owner_id FROM privs, volumes WHERE privs.volume_id = :volume AND privs.user_id IN (SELECT uid FROM users WHERE SUBSTR(user,0,"STRIFY(AUTH_CID_LEN)")=SUBSTR(:user,0,"STRIFY(AUTH_CID_LEN)") AND enabled=1) AND volumes.vid = :volume AND volumes.enabled = 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_createuser, "INSERT INTO users(user, name, key, role) VALUES(:userhash,:name,:key,:role)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_createuser_meta, "INSERT INTO usermeta(userid, desc) SELECT uid, :desc FROM users WHERE user=:userhash"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_deleteuser, "DELETE FROM users WHERE uid = :uid"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_onoffuser, "UPDATE users SET enabled = :enable WHERE name = :username"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_onoffuserclones, "UPDATE users SET enabled = :enable WHERE SUBSTR(user,0,"STRIFY(AUTH_CID_LEN)") = SUBSTR(:user,0,"STRIFY(AUTH_CID_LEN)") AND uid <> 0"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_user_newkey, "UPDATE users SET key=:key WHERE name = :username AND uid <> 0"))
	goto open_hashfs_fail;
    /* update if present otherwise insert:
     * note: the read and write has to be in same transaction otherwise
     * there'd be race conditions.
     * */
    if(qprep(h->db, &h->q_grant, "INSERT OR REPLACE INTO privs(volume_id, user_id, priv)\
	     VALUES(:volid, :uid,\
		    COALESCE((SELECT priv FROM privs WHERE volume_id=:volid AND user_id=:uid), 0)\
		    | :priv)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuid, "SELECT uid, role FROM users WHERE name = :name AND (:inactivetoo OR enabled=1)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getuidname, "SELECT name FROM users WHERE uid = :uid AND enabled=1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_revoke, "REPLACE INTO privs(volume_id, user_id, priv)\
	     VALUES(:volid, :uid,\
		    COALESCE((SELECT priv FROM privs WHERE volume_id=:volid AND user_id=:uid), 0)\
		    & :privmask)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_dropvolprivs, "DELETE FROM privs WHERE volume_id=:volid AND user_id=:uid"))
        goto open_hashfs_fail;
    /* To keep the next query simple we do not check if the user is enabled
     * This is preliminary enforced in auth_begin */
    if(qprep(h->db, &h->q_nextvol, "SELECT volumes.vid, volumes.volume, volumes.replica, volumes.cursize, volumes.maxsize, volumes.owner_id, volumes.revs, volumes.changed FROM volumes LEFT JOIN privs ON privs.volume_id = volumes.vid WHERE volumes.volume > :previous AND volumes.enabled = 1 AND (:user IS NULL OR (privs.priv > 0 AND privs.user_id IN (SELECT uid FROM users WHERE SUBSTR(user,0,"STRIFY(AUTH_CID_LEN)")=SUBSTR(:user,0,"STRIFY(AUTH_CID_LEN)")))) ORDER BY volumes.volume ASC LIMIT 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_volbyname, "SELECT vid, volume, replica, cursize, maxsize, owner_id, revs, changed FROM volumes WHERE volume = :name AND enabled = 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_volbyid, "SELECT vid, volume, replica, cursize, maxsize, owner_id, revs, changed FROM volumes WHERE vid = :volid AND enabled = 1"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_metaget, "SELECT key, value FROM vmeta WHERE volume_id = :volume"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_addvol, "INSERT INTO volumes (volume, replica, revs, cursize, maxsize, owner_id) VALUES (:volume, :replica, :revs, 0, :size, :owner)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_addvolmeta, "INSERT INTO vmeta (volume_id, key, value) VALUES (:volume, :key, :value)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_addvolprivs, "INSERT INTO privs (volume_id, user_id, priv) VALUES (:volume, :user, :priv)"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_chprivs, "UPDATE privs SET user_id = :new WHERE user_id IN (SELECT uid FROM users WHERE SUBSTR(user,0,"STRIFY(AUTH_CID_LEN)")=SUBSTR(:user,0,"STRIFY(AUTH_CID_LEN)"))"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_onoffvol, "UPDATE volumes SET enabled = :enable WHERE volume = :volume AND volume NOT LIKE '.BAD%'"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getvolstate, "SELECT enabled FROM volumes WHERE volume = :volume AND volume NOT LIKE '.BAD%'"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_delvol, "DELETE FROM volumes WHERE volume = :volume AND enabled = 0 AND volume NOT LIKE '.BAD%'"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_chownvol, "UPDATE volumes SET owner_id = :new WHERE owner_id = :old"))
	goto open_hashfs_fail;
    if(qprep(h->db, &h->q_chownvolbyid, "UPDATE volumes SET owner_id = :owner WHERE vid = :volid AND enabled = 1"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_resizevol, "UPDATE volumes SET maxsize = :size WHERE vid = :volid AND enabled = 1"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_changerevs, "UPDATE volumes SET revs = :revs WHERE vid = :volid AND enabled = 1"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_minreqs, "SELECT COALESCE(MAX(replica), 1), COALESCE(SUM(maxsize*replica), 0) FROM volumes"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_updatevolcursize, "UPDATE volumes SET cursize = cursize + :size, changed = :now WHERE vid = :volume AND enabled = 1"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_setvolcursize, "UPDATE volumes SET cursize = :size, changed = :now WHERE vid = :volume AND enabled = 1"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getnodepushtime, "SELECT last_push FROM node_volume_updates WHERE node = :node"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_setnodepushtime, "INSERT OR REPLACE INTO node_volume_updates VALUES (:node, :now)"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_userisowner, "SELECT 1 FROM users u1 JOIN users u2 ON SUBSTR(u1.user, 0, "STRIFY(AUTH_CID_LEN)") = SUBSTR(u2.user, 0, "STRIFY(AUTH_CID_LEN)") WHERE u1.uid = :owner_id AND u2.uid = :uid AND u1.enabled = 1 AND u2.enabled = 1"))
        goto open_hashfs_fail;
    if(qprep(h->db, &h->q_getprivholder, "SELECT uid FROM users JOIN privs ON user_id = uid WHERE SUBSTR(user, 0, "STRIFY(AUTH_CID_LEN)") = SUBSTR(:user, 0, "STRIFY(AUTH_CID_LEN)") AND enabled = 1"))
        goto open_hashfs_fail;

    OPEN_DB("tempdb", &h->tempdb);
    /* needed for ON DELETE CASCADE to work */
    if(qprep(h->tempdb, &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
	goto open_hashfs_fail;
    qnullify(q);

    if(qprep(h->tempdb, &h->qt_new, "INSERT INTO tmpfiles (volume_id, name, token) VALUES (:volume, :name, lower(hex(:random)))"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_new4del, "INSERT INTO tmpfiles (volume_id, name, size, token, t, content, avail, ttl, uniqidx, flushed) VALUES (:volume, :name, :size, :token, :time, :content, :avail, :expires, x'', 1)"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_update, "UPDATE tmpfiles SET size = :size, content = :all, uniqidx = :uniq, ttl = :expiry WHERE tid = :id AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_extend, "UPDATE tmpfiles SET content = cast((content || :all) as blob), uniqidx = cast((uniqidx || :uniq) as blob) WHERE tid = :id AND length(content) = :size AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_addmeta, "INSERT OR REPLACE INTO tmpmeta (tid, key, value) VALUES (:id, :key, :value)"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_delmeta, "DELETE FROM tmpmeta WHERE tid = :id AND key = :key"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_getmeta, "SELECT key, value FROM tmpmeta WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_countmeta, "SELECT COUNT(*) FROM tmpmeta WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_gettoken, "SELECT token, ttl, volume_id, name, t || ':' || token AS revision FROM tmpfiles WHERE tid = :id AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_tokendata, "SELECT tid, size, volume_id, name, content FROM tmpfiles WHERE token = :token AND flushed = 0"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_tmpbyrev, "SELECT tid, name, size, volume_id, content, uniqidx, flushed, avail, token FROM tmpfiles WHERE t = :rev_time AND token = :rev_token AND flushed = 1"))
        goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_tmpdata, "SELECT t || ':' || token AS revision, name, size, volume_id, content, uniqidx, flushed, avail, token FROM tmpfiles WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_updateuniq, "UPDATE tmpfiles SET uniqidx = :uniq, avail = :avail WHERE tid = :id AND flushed = 1"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_flush, "UPDATE tmpfiles SET flushed = 1 WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_delete, "DELETE FROM tmpfiles WHERE tid = :id"))
	goto open_hashfs_fail;
    if(qprep(h->tempdb, &h->qt_gc_revisions, "DELETE FROM tmpfiles WHERE ttl < :now AND ttl > 0"))
	goto open_hashfs_fail;

    if(!(h->blockbuf = wrap_malloc(bsz[SIZES-1])))
	goto open_hashfs_fail;

    for(j=0; j<SIZES; j++) {
	char hexsz[9];
	sprintf(hexsz, "%08x", bsz[j]);
	for(i=0; i<HASHDBS; i++) {
	    sprintf(dbitem, "hashdb_%c_%08x", sizedirs[j], i);
	    OPEN_DB(dbitem, &h->datadb[j][i]);
	    if(qprep(h->datadb[j][i], &h->qb_nextavail[j][i], "SELECT blocknumber FROM avail ORDER BY blocknumber ASC LIMIT 1"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_nextalloc[j][i], "SELECT value FROM hashfs WHERE key = 'next_blockno'"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_add[j][i], "INSERT OR IGNORE INTO blocks(hash, blockno, created_at) VALUES(:hash, :next, :now)"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_setfree[j][i], "INSERT OR IGNORE INTO avail VALUES(:blockno)"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_gc1[j][i], "DELETE FROM blocks WHERE id = :blockid"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_get[j][i], "SELECT blockno FROM blocks WHERE hash = :hash AND blockno IS NOT NULL"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_bumpavail[j][i], "DELETE FROM avail WHERE blocknumber = :next"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_bumpalloc[j][i], "UPDATE hashfs SET value = value + 1 WHERE key = 'next_blockno'"))
		goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_addtoken[j][i], "INSERT OR IGNORE INTO revision_ops(revision_id, op, age) VALUES(:revision_id, :op, :age)"))
                goto open_hashfs_fail;
            /* OR IGNORE to avoid subjournal */
            if(qprep(h->datadb[j][i], &h->qb_moduse[j][i], "INSERT OR IGNORE INTO revision_blocks(revision_id, blocks_hash, age, replica) VALUES(:revision_id, :hash, :age, :replica)"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_reserve[j][i], "INSERT OR IGNORE INTO reservations(reservations_id, revision_id, ttl) VALUES(:reserve_id, :revision_id, :ttl)"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_get_meta[j][i], "SELECT replica, op, revision_id FROM revision_blocks NATURAL INNER JOIN revision_ops NATURAL LEFT JOIN reservations WHERE blocks_hash=:hash AND age < :current_age AND reservations_id IS NULL"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->rit.q[j][i], "SELECT hash FROM blocks WHERE hash > :prevhash AND blockno IS NOT NULL"))
                goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_del_reserve[j][i], "DELETE FROM reservations WHERE reservations_id=:reserve_id"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_find_unused_revision[j][i], "SELECT revision_id FROM revision_ops WHERE revision_id IN (SELECT revision_id FROM revision_ops NATURAL LEFT JOIN reservations WHERE op <= 0 AND age <= :age AND revision_id > :last_revision_id AND reservations_id IS NULL) GROUP BY revision_id HAVING SUM(op)=0 ORDER BY revision_id"))
		goto open_hashfs_fail;
	    if(qprep(h->datadb[j][i], &h->qb_find_unused_block[j][i], "SELECT id, blockno, hash FROM blocks LEFT JOIN revision_blocks ON blocks.hash=blocks_hash WHERE id  > :last AND revision_id IS NULL ORDER BY id"))
		goto open_hashfs_fail;
            /* hash moved,
             * hashes that are not moved don't have the old counters deleted,
             * and must be taken into account when GCing!
             * this is to avoid updating the entire table during rebalance
             * */
            if(qprep(h->datadb[j][i], &h->qb_deleteold[j][i], "DELETE FROM revision_blocks WHERE blocks_hash=:hash AND age < :current_age"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_find_expired_reservation[j][i], "SELECT reservations_id, revision_id FROM reservations NATURAL INNER JOIN revision_blocks INNER JOIN blocks ON blocks.hash = blocks_hash WHERE reservations_id > :lastreserve_id GROUP BY reservations_id HAVING created_at < :expires ORDER BY reservations_id LIMIT 1"))
               goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_find_expired_reservation2[j][i], "SELECT revision_id from reservations WHERE ttl < :now LIMIT 1"))
               goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_gc_revision_blocks[j][i], "DELETE FROM revision_blocks WHERE revision_id=:revision_id"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_gc_revision[j][i], "DELETE FROM revision_ops WHERE revision_id=:revision_id"))
                goto open_hashfs_fail;
            if(qprep(h->datadb[j][i], &h->qb_gc_reserve[j][i], "DELETE FROM reservations WHERE revision_id=:revision_id"))
                goto open_hashfs_fail;

	    sprintf(dbitem, "datafile_%c_%08x", sizedirs[j], i);
	    sqlite3_reset(h->q_getval);
	    if(qbind_text(h->q_getval, ":k", dbitem) || qstep_ret(h->q_getval))
		goto open_hashfs_fail;

	    str = (const char *)sqlite3_column_text(h->q_getval, 0);
	    if(!str || !*str)
		goto open_hashfs_fail;
	    if(*str != '/') {
		unsigned int subpathlen = strlen(str) + 1;
		if(subpathlen > pathlen) {
		    pathlen = subpathlen;
		    if(!(path = wrap_realloc_or_free(path, dirlen + pathlen)))
			goto open_hashfs_fail;
		}
		memcpy(path + dirlen, str, subpathlen);
		str = path;
	    }

	    h->datafd[j][i] = open(str, O_RDWR);
	    if(h->datafd[j][i] < 0) {
		perror("open");
		goto open_hashfs_fail;
	    }
	    if(read_block(h->datafd[j][i], h->blockbuf, 0, bsz[j]))
		goto open_hashfs_fail;
	    if(memcmp(h->blockbuf, HASHFS_VERSION, strlen(HASHFS_VERSION)) ||
	       memcmp(h->blockbuf + 16, dbitem, strlen(dbitem)) ||
	       memcmp(h->blockbuf + 48, hexsz, strlen(hexsz)) ||
	       memcmp(h->blockbuf + 64, h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary))) {
		CRIT("Bad header in datafile %s (version %.*s)", str, (int)strlen(HASHFS_VERSION), h->blockbuf);
		goto open_hashfs_fail;
	    }
	}
    }

    for(i=0; i<METADBS; i++) {
	sprintf(dbitem, "metadb_%08x", i);
	OPEN_DB(dbitem, &h->metadb[i]);
	if(qprep(h->metadb[i], &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
	    goto open_hashfs_fail;
	qnullify(q);
        if(sqlite3_create_function(h->metadb[i]->handle, "pmatch", 4, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, pmatch, NULL, NULL))
            goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_ins[i], "INSERT INTO files (volume_id, name, size, content, rev, revision_id, age) VALUES (:volume, :name, :size, :hashes, :revision, :revision_id, :age)"))
	    goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_list[i], "SELECT name, size, rev FROM files WHERE volume_id = :volume AND name > :previous AND (:limit is NULL OR name < :limit) AND pmatch(name, :pattern, :pattern_slashes, :slash_ending) > 0 GROUP BY name HAVING rev = MAX(rev) ORDER BY name ASC LIMIT 1"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_list_eq[i], "SELECT name, size, rev FROM files WHERE volume_id = :volume AND name >= :previous AND (:limit is NULL OR name < :limit) AND pmatch(name, :pattern, :pattern_slashes, :slash_ending) > 0 GROUP BY name HAVING rev = MAX(rev) ORDER BY name ASC LIMIT 2"))
            goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_listrevs[i], "SELECT size, rev FROM files WHERE volume_id = :volume AND name = :name AND rev > :previous ORDER BY rev ASC LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_get[i], "SELECT fid, size, content, rev FROM files WHERE volume_id = :volume AND name = :name GROUP BY name HAVING rev = MAX(rev) LIMIT 1"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_listrevs_rev[i], "SELECT size, rev FROM files WHERE volume_id = :volume AND name = :name AND (:previous IS NULL OR rev < :previous) ORDER BY rev DESC LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_getrev[i], "SELECT fid, size, content, rev FROM files WHERE volume_id = :volume AND name = :name AND rev = :revision LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_findrev[i], "SELECT volume_id, name, size FROM files WHERE rev = :revision LIMIT 1"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_oldrevs[i], "SELECT rev, size, (SELECT COUNT(*) FROM files AS b WHERE b.volume_id = a.volume_id AND b.name = a.name), fid FROM files AS a WHERE a.volume_id = :volume AND a.name = :name ORDER BY rev ASC"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_metaget[i], "SELECT key, value FROM fmeta WHERE file_id = :file"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_metaset[i], "INSERT OR REPLACE INTO fmeta (file_id, key, value) VALUES (:file, :key, :value)"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_metadel[i], "DELETE FROM fmeta WHERE file_id = :file AND key = :key"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_delfile[i], "DELETE FROM files WHERE fid = :file"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_wiperelocs[i], "DELETE FROM relocs"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_addrelocs[i], "INSERT INTO relocs (file_id, dest) SELECT fid, :node FROM files WHERE volume_id = :volid"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_getreloc[i], "SELECT file_id, dest, volume_id, name, size, rev, content FROM relocs LEFT JOIN files ON relocs.file_id = files.fid WHERE file_id > :prev"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_delreloc[i], "DELETE FROM relocs WHERE file_id = :fileid"))
	    goto open_hashfs_fail;
	if(qprep(h->metadb[i], &h->qm_delbyvol[i], "DELETE FROM files WHERE volume_id = :volid"))
	    goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_sumfilesizes[i], "SELECT SUM(x) FROM (SELECT files.size + LENGTH(CAST(files.name AS BLOB)) + SUM(COALESCE(LENGTH(CAST(fmeta.key AS BLOB)) + LENGTH(fmeta.value),0)) AS x FROM files LEFT JOIN fmeta ON files.fid = fmeta.file_id WHERE files.volume_id = :volid GROUP BY files.fid)"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_newest[i], "SELECT MAX(rev) FROM files WHERE volume_id = :volid"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_count[i], "SELECT COUNT(rev) FROM files WHERE volume_id = :volid"))
	    goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_list_rev_dec[i], "SELECT size, rev, content FROM files WHERE volume_id=:volid AND name = :name AND rev < :maxrev ORDER BY rev DESC LIMIT 1"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_list_file[i], "SELECT size, rev, content, name FROM files WHERE volume_id=:volid AND name > :previous AND rev < :maxrev ORDER BY name ASC, rev DESC LIMIT 1"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_del_heal[i], "DELETE FROM heal WHERE revision_id=:revision_id"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_add_heal[i], "INSERT OR IGNORE INTO heal(revision_id, remote_volume, blocks, blocksize, replica_count) VALUES(:revision_id, :remote_volid, :blocks, :blocksize, :replica_count)"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_get_rb[i], "SELECT size, revision_id, content, name FROM files WHERE volume_id=:volume_id AND age < :age_limit AND revision_id > :min_revision_id ORDER BY revision_id"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_count_rb[i], "SELECT COUNT(revision_id) FROM files WHERE volume_id=:volume_id AND age < :age_limit AND revision_id > :min_revision_id ORDER BY revision_id"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_add_heal_volume[i], "INSERT OR REPLACE INTO heal_volume(name, max_age, min_revision) VALUES(:name,:max_age,:min_revision_id)"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_sel_heal_volume[i], "SELECT name, max_age, min_revision FROM heal_volume WHERE name > :prev"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_upd_heal_volume[i], "UPDATE heal_volume SET min_revision=:min_revision_id WHERE name=:name"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_del_heal_volume[i], "DELETE FROM heal_volume WHERE name=:name"))
            goto open_hashfs_fail;
        if(qprep(h->metadb[i], &h->qm_needs_upgrade[i], "SELECT fid, volume_id, name, rev, size FROM files WHERE revision_id IS NULL"))
            goto open_hashfs_fail;
    }

    OPEN_DB("eventdb", &h->eventdb);
    if(qprep(h->eventdb, &q, "PRAGMA foreign_keys = ON") || qstep_noret(q))
	goto open_hashfs_fail;
    qnullify(q);
    if(qprep(h->eventdb, &h->qe_getjob, "SELECT complete, result, reason FROM jobs WHERE job = :id AND :owner IN (user, 0)"))
	goto open_hashfs_fail;
    snprintf(qrybuff, sizeof(qrybuff), "SELECT job FROM jobs WHERE type = %d AND data = :data", JOBTYPE_DELETE_FILE);
    if(qprep(h->eventdb, &h->qe_getfiledeljob, qrybuff))
        goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_addjob, "INSERT INTO jobs (parent, type, lock, expiry_time, data, user) SELECT :parent, :type, :lock, datetime(:expiry + strftime('%s', COALESCE((SELECT expiry_time FROM jobs WHERE job = :parent), 'now')), 'unixepoch'), :data, :uid"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_addact, "INSERT INTO actions (job_id, target, addr, internaladdr, capacity) VALUES (:job, :node, :addr, :int_addr, :capa)"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_countjobs, "SELECT COUNT(*) FROM jobs WHERE user = :uid AND complete = 0"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_islocked, "SELECT value from hashfs WHERE key = 'lockedby'"))
	goto open_hashfs_fail;
    snprintf(qrybuff, sizeof(qrybuff), "SELECT 1 FROM jobs WHERE complete = 0 AND type NOT IN (%d, %d, %d, %d, %d) LIMIT 1", JOBTYPE_DISTRIBUTION, JOBTYPE_JLOCK, JOBTYPE_STARTREBALANCE, JOBTYPE_FINISHREBALANCE, JOBTYPE_REPLACE);
    if(qprep(h->eventdb, &h->qe_hasjobs, qrybuff))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_lock, "INSERT INTO hashfs (key, value) VALUES ('lockedby', :node)"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_unlock, "DELETE FROM hashfs WHERE key = 'lockedby'"))
	goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_gc, "DELETE FROM jobs WHERE complete=1 AND sched_time <= datetime('now','-1 month')"))
        goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->qe_count_upgradejobs, "SELECT COUNT(*) FROM jobs WHERE complete=0 AND lock='$UPGRADE$UPGRADE'"))
        goto open_hashfs_fail;
    if(qprep(h->eventdb, &q, "CREATE TEMP TABLE hash_retry(hash BLOB("STRIFY(SXI_SHA1_BIN_LEN)") PRIMARY KEY NOT NULL, blocksize INTEGER NOT NULL, id INTEGER NOT NULL)") ||
        qstep_noret(q))
        goto open_hashfs_fail;
    qnullify(q);
    if(qprep(h->eventdb, &h->rit.q_add, "INSERT OR IGNORE INTO hash_retry(hash, blocksize, id) VALUES(:hash, :blocksize, :hash)"))
        goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->rit.q_sel, "SELECT hash, blocksize FROM hash_retry WHERE hash > :prevhash"))
        goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->rit.q_remove, "DELETE FROM hash_retry WHERE hash=:hash"))
        goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->rit.q_reset, "DELETE FROM hash_retry"))
        goto open_hashfs_fail;
    if(qprep(h->eventdb, &h->rit.q_count, "SELECT COUNT(*) FROM hash_retry"))
        goto open_hashfs_fail;

    OPEN_DB("xferdb", &h->xferdb);
    if(qprep(h->xferdb, &h->qx_add, "INSERT INTO topush (block, size, node) VALUES (:b, :s, :n)"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_hold, "INSERT OR IGNORE INTO onhold (hblock, hsize, hnode) VALUES (:b, :s, :n)"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_isheld, "SELECT 1 FROM onhold WHERE hblock = :b AND hsize = :s"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_release, "DELETE FROM onhold WHERE hid IN (SELECT hid FROM onhold, topush WHERE id = :pushid AND hblock = block AND hsize = size AND hnode = node)"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_hasheld, "SELECT 1 FROM onhold LIMIT 1"))
       goto open_hashfs_fail;
    if(qprep(h->xferdb, &h->qx_wipehold, "DELETE FROM onhold"))
       goto open_hashfs_fail;


    qnullify(q);
    sqlite3_reset(h->q_getval);

    free(path);
    return h;

open_hashfs_fail:
    free(path);
    sqlite3_finalize(q);

    close_all_dbs(h);
    sqlite3_shutdown();
    free(h->dir);
    free(h->blockbuf);
    if(h->lockfd >= 0)
        close(h->lockfd);
    free(h);
    return NULL;
}

int sx_hashfs_distcheck(sx_hashfs_t *h) {
    int ret = 0;

    if(!h)
	return 0;

    sqlite3_reset(h->q_gethdrev);
    switch(qstep(h->q_gethdrev)) {
    case SQLITE_DONE:
	break;
    case SQLITE_ROW:
	if(sqlite3_column_int64(h->q_gethdrev, 0) != h->hd_rev)
	    ret = 1;
	break;
    default:
	WARN("Failed to check distribution version, assuming unchanged");
    }
    sqlite3_reset(h->q_gethdrev);

    if(ret && load_config(h, h->sx))
	ret = -1;

    return ret; /* return 0 = no change, 1 = hdist-change, -1 = error */
}

time_t sx_hashfs_disttime(sx_hashfs_t *h) {
    return h->last_dist_change;
}

const char *sx_hashfs_cluster_name(sx_hashfs_t *h) {
    return h ? h->cluster_name : NULL;
}

uint16_t sx_hashfs_http_port(sx_hashfs_t *h) {
    return h ? h->http_port : 0;
}

const char *sx_hashfs_ca_file(sx_hashfs_t *h) {
    return h ? h->ssl_ca_file : NULL;
}

int sx_hashfs_uses_secure_proto(sx_hashfs_t *h) {
    if(!h)
	return -1;
    return (h->ssl_ca_file != NULL);
}

void sx_hashfs_set_triggers(sx_hashfs_t *h, int job_trigger, int xfer_trigger, int gc_trigger, int gc_expire_trigger) {
    if(!h)
	return;
    h->job_trigger = job_trigger;
    h->xfer_trigger = xfer_trigger;
    h->gc_trigger = gc_trigger;
    h->gc_expire_trigger = gc_expire_trigger;
}

void sx_hashfs_close(sx_hashfs_t *h) {
    if(!h)
	return;
    if(h->have_hd)
	sxi_hdist_free(h->hd);
    sx_nodelist_delete(h->prev_dist);
    sx_nodelist_delete(h->next_dist);
    sx_nodelist_delete(h->prevnext_dist);
    sx_nodelist_delete(h->nextprev_dist);
    sx_nodelist_delete(h->effprev_dist);
    sx_nodelist_delete(h->effnext_dist);
    sx_nodelist_delete(h->effprevnext_dist);
    sx_nodelist_delete(h->effnextprev_dist);
    sx_nodelist_delete(h->faulty_nodes);
    sx_nodelist_delete(h->ignored_nodes);

    close_all_dbs(h);

    free(h->blockbuf);
/*    if(h->sx)
	sx_shutdown(h->sx, 0);
    do not free sx here: it is not owned by hashfs.c!
    freeing it here would cause use-after-free if sx_done() is called
    */
    if(h->sx_clust)
	sxi_conns_free(h->sx_clust);
    sqlite3_shutdown();
    free(h->ssl_ca_file);
    free(h->cluster_name);
    free(h->dir);

    if(h->lockfd >= 0)
        close(h->lockfd);
    free(h);
}

int sx_storage_is_bare(sx_hashfs_t *h) {
    return (h != NULL) && (h->cluster_name == NULL);
}

rc_ty sx_storage_activate(sx_hashfs_t *h, const char *name, const sx_uuid_t *node_uuid, uint8_t *admin_uid, unsigned int uid_size, uint8_t *admin_key, int key_size, uint16_t port, const char *ssl_ca_file, const sx_nodelist_t *allnodes) {
    rc_ty r, ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    const sx_node_t *self;
    unsigned int nodeidx;
    sx_hash_t hash;

    if(!h || !name || !node_uuid || !admin_key) {
	NULLARG();
	return EFAULT;
    }
    if(!sx_storage_is_bare(h)) {
	msg_set_reason("Storage was already activated");
	return EINVAL;
    }

    self = sx_nodelist_lookup_index(allnodes, node_uuid, &nodeidx);
    if(!self) {
	msg_set_reason("Failed to find node uuid in node list");
	return EINVAL;
    }

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)"))
	goto storage_activate_fail;

    admin_uid = hash.b;
    if(sx_hashfs_hash_buf(NULL, 0, "admin", 5, &hash)) {
        CRIT("Failed to initialize admin user ID hash");
        goto storage_activate_fail;
    }

    r = sx_hashfs_create_user(h, "admin", admin_uid, uid_size, admin_key, key_size, ROLE_ADMIN, "");
    if(r != OK) {
	ret = r;
	goto storage_activate_fail;
    }
    r = sx_hashfs_user_onoff(h, "admin", 1, 0);
    if(r != OK) {
	ret = r;
	goto storage_activate_fail;
    }

    if(ssl_ca_file) {
	if(qbind_text(q, ":k", "ssl_ca_file") || qbind_text(q, ":v", ssl_ca_file) || qstep_noret(q))
	    goto storage_activate_fail;
    }
    if(qbind_text(q, ":k", "cluster_name") || qbind_text(q, ":v", name) || qstep_noret(q))
	goto storage_activate_fail;
    if(qbind_text(q, ":k", "node") || qbind_blob(q, ":v", node_uuid->binary, sizeof(node_uuid->binary)) || qstep_noret(q))
	goto storage_activate_fail;
    if(qbind_text(q, ":k", "http_port") || qbind_int(q, ":v", port) || qstep_noret(q))
	goto storage_activate_fail;

    if(sx_hashfs_hdist_change_commit(h))
	goto storage_activate_fail;

    if(sx_hashfs_modhdist(h, allnodes))
	goto storage_activate_fail;

    if(qcommit(h->db))
	goto storage_activate_fail;

    ret = OK;
 storage_activate_fail:
    if(ret != OK) {
	qrollback(h->db);
	sxc_clearerr(h->sx);
	sxi_seterr(h->sx, SXE_EARG, "%s", msg_get_reason());
    }

    sqlite3_finalize(q);
    return ret;
}

static unsigned int size_to_blocks(uint64_t size, unsigned int *size_type, unsigned int *block_size) {
    unsigned int ret, sizenum = 1, bs;
    if(size > 128*1024*1024)
	sizenum = 2;
    else if(size < 128*1024)
	sizenum = 0;
    bs = bsz[sizenum];
    ret = size / bs;
    if(size % bs)
	ret++;
    if(size_type)
	*size_type = sizenum;
    if(block_size)
	*block_size = bs;
    return ret;
}

static int cmphash(const void *a, const void *b) {
    return memcmp(a, b, sizeof(sx_hash_t));
}

static unsigned int gethashdb(const sx_hash_t *hash) {
    return MurmurHash64(hash, sizeof(*hash), MURMUR_SEED) & (HASHDBS-1);
}

static int getmetadb(const char *filename) {
    sx_hash_t hash;
    if(hash_buf(NULL, 0, filename, strlen(filename), &hash)) {
        WARN("hash_buf failed");
	return 0;
    }

    return MurmurHash64(&hash, sizeof(hash), MURMUR_SEED) & (METADBS-1);
}

static rc_ty check_path_element(const char *name, unsigned name_min, unsigned name_max, const char *what)
{
    unsigned int namelen;
    if (!name) {
	NULLARG();
	return EFAULT;
    }
    if(*name=='.') {
	msg_set_reason("Invalid %s name '%s': must not start with a '.'", what, name);
	return EINVAL;
    }
    namelen = strlen(name);
    if(namelen < name_min || namelen > name_max) {
	msg_set_reason("Invalid %s name '%s': must be between %d and %d bytes",
                       what, name, name_min, name_max);
	return EINVAL;
    }
    if(utf8_validate_len(name) < 0) {
	msg_set_reason("Invalid %s name '%s': must be valid UTF8", what, name);
	return EINVAL;
    }
    unsigned i;
    for (i=0;i<namelen;i++) {
        if ((unsigned)name[i] < ' ' || name[i] == '/' || name[i] == '\\') {
            msg_set_reason("Invalid %s name '%s': contains forbidden characters (\\n/\\)", what, name);
            return EINVAL;
        }
    }
    return OK;
}

/* Returns 0 if the volume name is valid,
 * sets reason otherwise */
rc_ty sx_hashfs_check_volume_name(const char *name) {
    return check_path_element(name, SXLIMIT_MIN_VOLNAME_LEN, SXLIMIT_MAX_VOLNAME_LEN, "volume");
}

static int check_file_name(const char *name) {
    unsigned int namelen;

    if(!name) {
        NULLARG();
        return -1;
    }
    namelen = strlen(name);
    if(namelen < SXLIMIT_MIN_FILENAME_LEN || namelen > SXLIMIT_MAX_FILENAME_LEN) {
	msg_set_reason("Invalid file name '%s': must be between %d and %d bytes",
		       name, SXLIMIT_MIN_FILENAME_LEN, SXLIMIT_MAX_FILENAME_LEN);
	return -1;
    }
    if(utf8_validate_len(name) < 0) {
	msg_set_reason("Invalid file name '%s': must be valid UTF8", name);
	return -1;
    }
    return namelen;
}

rc_ty sx_hashfs_check_meta(const char *key, const void *value, unsigned int value_len) {
    unsigned int key_len;

    if(!key || !value) {
	NULLARG();
	return EFAULT;
    }

    key_len = strlen(key);
    if(key_len < SXLIMIT_META_MIN_KEY_LEN) {
	msg_set_reason("Invalid metadata key length %d: must be between %d and %d",
		       key_len, SXLIMIT_META_MIN_KEY_LEN, SXLIMIT_META_MAX_KEY_LEN);
	return EINVAL;
    }
    if(key_len > SXLIMIT_META_MAX_KEY_LEN) {
	msg_set_reason("Invalid metadata key length %d: must be between %d and %d",
		       key_len, SXLIMIT_META_MIN_KEY_LEN, SXLIMIT_META_MAX_KEY_LEN);
	return EMSGSIZE;
    }
    if (SXLIMIT_META_MIN_VALUE_LEN > 0 && value_len < SXLIMIT_META_MIN_VALUE_LEN) {
	msg_set_reason("Invalid metadata value length %d: must be between %d and %d",
		       value_len, SXLIMIT_META_MIN_VALUE_LEN, SXLIMIT_META_MAX_VALUE_LEN);
	return EINVAL;
    }
    if (value_len > SXLIMIT_META_MAX_VALUE_LEN) {
	msg_set_reason("Invalid metadata value length %d: must be between %d and %d",
		       value_len, SXLIMIT_META_MIN_VALUE_LEN, SXLIMIT_META_MAX_VALUE_LEN);
	return EMSGSIZE;
    }

    if (utf8_validate_len(key) < 0) {
	msg_set_reason("Invalid metadata key '%s': must be valid UTF8", key);
	return EINVAL;
    }
    return 0;
}

int sx_hashfs_check_username(const char *name) {
    return check_path_element(name, SXLIMIT_MIN_USERNAME_LEN, SXLIMIT_MAX_USERNAME_LEN, "username");
}


static int parse_revision(const char *revision, unsigned int *revtime) {
    const char *eod;
    time_t t;

    if(!revision)
	return -1;
    if(strlen(revision) != REV_LEN)
	return -1;
    if(revision[REV_TIME_LEN] != ':')
	return -1;

    eod = strptimegm(revision, "%Y-%m-%d %H:%M:%S", &t);
    if(eod != &revision[REV_TIME_LEN - 4])
	return -1;
    if(revtime)
	*revtime = (unsigned int)t;
    return 0;
}

static int check_revision(const char *revision) {
    return parse_revision(revision, NULL);
}

#define TOKEN_SIGNED_LEN UUID_STRING_SIZE + 1 + TOKEN_RAND_BYTES * 2 + 1 + TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN + 1
rc_ty sx_hashfs_make_token(sx_hashfs_t *h, const uint8_t *user, const char *rndhex, unsigned int replica, int64_t expires_at, const char **token) {
    sxi_hmac_sha1_ctx *hmac_ctx;
    uint8_t md[SXI_SHA1_BIN_LEN], rndbin[TOKEN_RAND_BYTES];
    char rndhexbuf[TOKEN_RAND_BYTES * 2 + 1], replicahex[2 + TOKEN_REPLICA_LEN + 1], expirehex[TOKEN_EXPIRE_LEN + 1];
    sx_uuid_t node_uuid;
    unsigned int len;
    rc_ty ret;

    if(!h || !user) {
	NULLARG();
	return EINVAL;
    }

    if(rndhex) {
	if(strlen(rndhex) != TOKEN_RAND_BYTES * 2 || hex2bin(rndhex, TOKEN_RAND_BYTES * 2, rndbin, sizeof(rndbin))) {
	    msg_set_reason("Invalid random string");
	    return EINVAL;
	}
    } else {
	/* non-blocking pseudo-random bytes, i.e. we don't want to block or deplete
	 * entropy as we only need a unique sequence of bytes, not a secret one as
	 * it is sent in plaintext anyway, and signed with an HMAC */
	if(sxi_rand_pseudo_bytes(rndbin, sizeof(rndbin)) == -1) {
	    /* can also return 0 or 1 but that doesn't matter here */
	    WARN("Cannot generate random bytes");
	    msg_set_reason("Failed to generate random string");
	    return FAIL_EINTERNAL;
	}
	if (bin2hex(rndbin, sizeof(rndbin), rndhexbuf, sizeof(rndhexbuf)))
            WARN("bin2hex failed");
	rndhex = rndhexbuf;
    }

    ret = sx_hashfs_self_uuid(h, &node_uuid);
    if(ret) {
        WARN("self_uuid failed");
	return ret;
    }

    snprintf(replicahex, sizeof(replicahex), "%010x", replica);
    snprintf(expirehex, sizeof(expirehex), "%016llx", (long long)expires_at);
    snprintf(h->put_token, sizeof(h->put_token), "%s:%s:%s:%s:", node_uuid.string, rndhex, replicahex+2, expirehex);
    len = strlen(h->put_token);
    if(len != TOKEN_SIGNED_LEN) {
	msg_set_reason("Generated token with bad length");
	return EINVAL;
    }

    hmac_ctx = sxi_hmac_sha1_init();
    if(!sxi_hmac_sha1_init_ex(hmac_ctx, &h->tokenkey, sizeof(h->tokenkey)) ||
       !sxi_hmac_sha1_update(hmac_ctx, (unsigned char *)h->put_token, len) ||
       !sxi_hmac_sha1_final(hmac_ctx, md, &len) ||
       len != AUTH_KEY_LEN) {
	msg_set_reason("Failed to compute token hmac");
	CRIT("Cannot genearate token hmac");
	ret = FAIL_EINTERNAL;
    } else {
	bin2hex(md, AUTH_KEY_LEN, &h->put_token[TOKEN_SIGNED_LEN], AUTH_KEY_LEN * 2 + 1);
	h->put_token[sizeof(h->put_token)-1] = '\0';
	*token = h->put_token;
    }
    sxi_hmac_sha1_cleanup(&hmac_ctx);

    return ret;
}


struct token_data {
    sx_uuid_t uuid;
    char token[TOKEN_RAND_BYTES*2+1];
    unsigned int replica;
    int64_t expires_at;
};

static int parse_token(sxc_client_t *sx, const uint8_t *user, const char *token, const sx_hash_t *tokenkey, struct token_data *td) {
    char uuid_str[UUID_STRING_SIZE+1], hmac[AUTH_KEY_LEN*2+1];
    char *eptr;
    uint8_t md[SXI_SHA1_BIN_LEN];
    sxi_hmac_sha1_ctx *hmac_ctx;
    unsigned int ml;

    if(!user || !token || !td) {
	NULLARG();
	return 1;
    }
    if(strlen(token) != TOKEN_TEXT_LEN) {
	msg_set_reason("Invalid token length: expected %d, got %u", TOKEN_TEXT_LEN, (unsigned)strlen(token));
	return 1;
    }
    if(token[UUID_STRING_SIZE] != ':' ||
       token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2] != ':' ||
       token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN] != ':' ||
       token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN] != ':') {
	msg_set_reason("Invalid token format");
	return 1;
    }

    hmac_ctx = sxi_hmac_sha1_init();
    if(!sxi_hmac_sha1_init_ex(hmac_ctx, tokenkey, sizeof(*tokenkey)) ||
       !sxi_hmac_sha1_update(hmac_ctx, (unsigned char *)token, TOKEN_SIGNED_LEN) ||
       !sxi_hmac_sha1_final(hmac_ctx, md, &ml) ||
       ml != AUTH_KEY_LEN) {
	sxi_hmac_sha1_cleanup(&hmac_ctx);
	CRIT("Cannot generate token hmac");
	return 1;
    }
    sxi_hmac_sha1_cleanup(&hmac_ctx);
    bin2hex(md, AUTH_KEY_LEN, hmac, sizeof(hmac));
    if(hmac_compare((const unsigned char *)&token[TOKEN_SIGNED_LEN], (const unsigned char *)hmac, AUTH_KEY_LEN*2)) {
	msg_set_reason("Token signature does not match");
	return 1;
    }

    memcpy(uuid_str, token, UUID_STRING_SIZE);
    uuid_str[UUID_STRING_SIZE] = '\0';
    if(uuid_from_string(&td->uuid, uuid_str)) {
	msg_set_reason("Invalid token format");
	return 1;
    }

    memcpy(td->token, &token[UUID_STRING_SIZE+1], sizeof(td->token));
    td->token[sizeof(td->token)-1] = '\0';

    td->replica = strtol(&token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1], &eptr, 16);
    if(eptr != &token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN]) {
	msg_set_reason("Invalid token format");
	return 1;
    }

    td->expires_at = strtol(&token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1 + TOKEN_REPLICA_LEN + 1], &eptr, 16);
    if(eptr != &token[UUID_STRING_SIZE+1+TOKEN_RAND_BYTES*2+1+TOKEN_REPLICA_LEN + 1 + TOKEN_EXPIRE_LEN]) {
	msg_set_reason("Invalid token format");
	return 1;
    }

    return 0;
}

rc_ty sx_hashfs_token_get(sx_hashfs_t *h, const uint8_t *user, const char *token, unsigned int *replica_count, int64_t *expires_at) {
    struct token_data tkdt;
    if(parse_token(h->sx, user, token, &h->tokenkey, &tkdt))
	return EINVAL;
    *replica_count = tkdt.replica;
    if (expires_at)
        *expires_at = tkdt.expires_at;
    return OK;
}

static long long get_count(sxi_db_t *db, const char *table)
{
    long long ret = 0;
    char query[128];
    sqlite3_stmt *q = NULL;
    snprintf(query, sizeof(query), "SELECT COUNT(*) FROM %s", table);
    if(!qprep(db, &q, query) && !qstep_ret(q))
	ret = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return ret;
}

void sx_hashfs_stats(sx_hashfs_t *h)
{
    int i, j;
    INFO("User#: %lld", get_count(h->db, "users"));
    INFO("Volume#: %lld", get_count(h->db, "volumes"));
    INFO("Volume metadata#: %lld", get_count(h->db, "vmeta"));
    long long files = 0, fmeta = 0;
    for(i=0; i<METADBS; i++) {
	files += get_count(h->metadb[i], "files");
	fmeta += get_count(h->metadb[i], "fmeta");
    }
    INFO("File#: %lld", files);
    INFO("File metadata#: %lld", fmeta);
    INFO("Block counts:");
    for (j=0; j<SIZES; j++) {
	long long blocks = 0;
	for(i=0;i<HASHDBS;i++)
	    blocks += get_count(h->datadb[j][i], "blocks");
	INFO("\t%-8s (%8d byte) block#: %lld", sizelongnames[j], bsz[j], blocks);
    }
}

/* Return number of encountered errors, -1 if failed */
static int analyze_db(sxi_db_t *db, int verbose)
{
    int ret = 0, r;
    const char *name = sqlite3_db_filename(db->handle, "main");
    sqlite3_stmt *qint = NULL, *qfk = NULL;
    if(name && verbose)
        INFO("%s:", name);
    if (qprep(db, &qint, "PRAGMA integrity_check;") || qprep(db, &qfk, "PRAGMA foreign_key_check")) {
        ret = -1;
        WARN("Failed to prepare query");
        goto analyze_db_err;
    }

    /* Check first row for "ok" string */
    if((r = qstep(qint)) == SQLITE_ROW) {
        const char *line = (const char*)sqlite3_column_text(qint, 0);
        if(line && !strcmp(line, "ok")) {
            if(verbose)
                INFO("\tintegrity_check: %s", line);
        } else {
            WARN("\tintegrity_check: %s", line);
            ret++;
        }
    }

    /* Get errors, if there was "ok" in the first row, we shouldn't receive more rows, but its ok to check all */
    while((r = qstep(qint)) == SQLITE_ROW) {
        const char *line = (const char*)sqlite3_column_text(qint, 0);
        WARN("\tintegrity_check: %s", line);
        ret++;
    }

    if(r != SQLITE_DONE) {
        WARN("Failed to perform PRAGMA integrity_check");
        ret = -1;
        goto analyze_db_err;
    }

    char last[1024];
    last[0] = '\0';
    /* Check first row for "ok" string */
    if((r = qstep(qfk)) == SQLITE_ROW) {
        const char *line = (const char*)sqlite3_column_text(qfk, 0);
        if(line) {
            if (!strcmp(line, "ok")) {
                if(verbose)
                    INFO("\tforeign_key_check: %s", line);
                strncpy(last, line, sizeof(last)-1);
            }
        } else {
            WARN("\tforeign_key_check: %s", line);
            ret++;
        }
        last[sizeof(last)-1] = '\0';
    }

    /* Get errors, if there was "ok" in the first row, we shouldn't receive more rows, but its ok to check all */
    while((r = qstep(qfk)) == SQLITE_ROW) {
        const char *line = (const char*)sqlite3_column_text(qfk,0);
        ret++;
        if (strcmp(last, line)) {
            WARN("\tforeign_key_check: %s", line);
            strncpy(last, line, sizeof(last)-1);
            last[sizeof(last)-1] = '\0';
        }
    }

    if(r != SQLITE_DONE) {
        WARN("Failed to perform PRAGMA foreign_key_check");
        ret = -1;
    }

analyze_db_err:
    sqlite3_finalize(qint);
    sqlite3_finalize(qfk);
    return ret;
}

int sx_hashfs_analyze(sx_hashfs_t *h, int verbose)
{
    int ret = 0, r;
    if(verbose)
        INFO("Analyzing databases...");
    r = analyze_db(h->db, verbose);
    if(r == -1)
        return -1;
    ret += r;
    unsigned i, j;
    for(i=0; i<METADBS; i++)
	r = analyze_db(h->metadb[i], verbose);
        if(r == -1)
            return -1;
        ret += r;
    for (j=0; j<SIZES; j++) {
	for(i=0;i<HASHDBS;i++) {
	    r = analyze_db(h->datadb[j][i], verbose);
            if(r == -1)
                return -1;
            ret += r;
	}
    }
    r = analyze_db(h->tempdb, verbose);
    if(r == -1)
        return -1;
    ret += r;
    r = analyze_db(h->eventdb, verbose);
    if(r == -1)
        return -1;
    ret += r;
    r = analyze_db(h->xferdb, verbose);
    if(r == -1)
        return -1;
    ret += r;
    return ret;
}

static int check_warn_printed = 0;
static int check_info_printed = 0;

static void check_print_pgrs(void) {
    static char chars[] = { '-', '\\', '|', '/'};
    static int idx = -1;
    static struct timeval last_time;
    struct timeval now;

    if(idx == -1) {
        idx = 0;
        gettimeofday(&last_time, NULL);
    }

    gettimeofday(&now, NULL);
    if(sxi_timediff(&now, &last_time) > 0.1) {
        idx = (idx + 1) % 4;
        memcpy(&last_time, &now, sizeof(now));
        fprintf(stderr,"\b%c", chars[idx]);
    }
}

#define CHECK_PRINT_WARN(...) do { CHECK_LOG_INTERNAL(SX_LOG_WARNING, __VA_ARGS__, "   "); } while(0)
#define CHECK_ERROR(...) do { ret++; CHECK_PRINT_WARN(__VA_ARGS__); } while(0)
#define CHECK_FATAL(...) CHECK_PRINT_WARN(__VA_ARGS__)
#define CHECK_INFO(...) do { CHECK_LOG_INTERNAL(SX_LOG_INFO, __VA_ARGS__, "   "); } while(0)
#define CHECK_START do { CHECK_PRINT_WARN("Integrity check started"); } while(0)
#define CHECK_FINISH do { fprintf(stderr, "%s", check_warn_printed ? "\b \n" : "\b"); printf("%s", check_info_printed ? "\n" : ""); } while(0)
#define CHECK_PGRS do { check_print_pgrs(); } while(0)

#define CHECK_LOG_INTERNAL(lvl, msg, ...) do { \
                                            if((lvl) == SX_LOG_WARNING) { \
                                                fprintf(stderr, "%s[%s]: ", (check_warn_printed || check_info_printed) ? "\b \n" : "\b", __func__); \
                                                fprintf(stderr, msg"%s", __VA_ARGS__); \
                                                check_warn_printed = 1; \
                                            } else { \
                                                fprintf(stderr, "\b%s", check_info_printed ? " " : ""); \
                                                fprintf(stdout, "%s[%s]: "msg"%s", check_info_printed ? "\n" : "", __func__, __VA_ARGS__); \
                                                fflush(stdout); \
                                                check_info_printed = 1; \
                                            } \
                                         } while(0)


static int check_file_hashes(sx_hashfs_t *h, int debug, const sx_hash_t *hashes, unsigned int hashes_count, unsigned int block_size, unsigned int replica_count) {
    int ret = 0;
    unsigned int j, bs;
    unsigned int processed_hashes = 0;
    const sx_uuid_t *self;
    sqlite3_stmt *q = NULL;

    /* If cluster is rebalancing skip checking blocks existence */
    if(sx_hashfs_is_rebalancing(h)) {
        if(debug)
            CHECK_INFO("Cluster is rebalancing, skipping hash existence check");
        return ret;
    }

    /* Get block size index */
    for(j = 0; j < SIZES; j++) {
        if(bsz[j] == block_size) {
            bs = j;
            break;
        }
    }

    if(j >= SIZES) {
        ret = -1;
        goto check_file_hashes_err;
    }

    self = sx_node_uuid(sx_hashfs_self(h));
    if(!self) {
        CHECK_FATAL("Failed to get node UUID");
        ret = -1;
        goto check_file_hashes_err;
    }

    for(j = 0; j < hashes_count; j++) {
        const sx_hash_t* hash = hashes + j;
        unsigned int ndb = gethashdb(hash);
        int r;
        sxi_db_t *db = h->datadb[bs][ndb];
        q = h->qb_get[bs][ndb];
        char hex[SXI_SHA1_TEXT_LEN+1];
        sx_nodelist_t *hashnodes;

        CHECK_PGRS;

        if(!db || !q) {
            ret = -1;
            goto check_file_hashes_err;
        }

        /* Get hash text representation for debugging*/
        if(bin2hex(hash->b, SXI_SHA1_BIN_LEN, hex, SXI_SHA1_TEXT_LEN+1)) {
            ret = -1;
            goto check_file_hashes_err;
        }

        /* Distribution doesn't matter, we skip blocks existence during rebalance */
        hashnodes = sx_hashfs_all_hashnodes(h, NL_PREV, hash, replica_count);
        if(hashnodes) {
            int skip = 0;
            /* Skip hashes stored on remote nodes */
            if(!sx_nodelist_lookup(hashnodes, self))
                skip = 1;
            sx_nodelist_delete(hashnodes);
            if(skip)
                continue;
        } else {
            ret = -1;
            goto check_file_hashes_err;
        }

        if(debug)
            CHECK_INFO("-> Checking hash %.8s existence [%u]", hex, ndb);

        sqlite3_reset(q);
        if(qbind_blob(q, ":hash", hash, sizeof(*hash))) {
            ret = -1;
            goto check_file_hashes_err;
        }

        r = qstep(q);
        if(r == SQLITE_DONE) {
            CHECK_ERROR("Hash %s could not be found in database", hex);
            sqlite3_reset(q);
        } else if(r != SQLITE_ROW) {
            ret = -1;
            goto check_file_hashes_err;
        }

        processed_hashes++;
    }

    if(debug)
        CHECK_INFO("Checked hashes stored locally: %u/%u", processed_hashes, hashes_count);

check_file_hashes_err:
    sqlite3_reset(q);
    if(ret == -1)
        WARN("Failed to check file hashes existence due to error");
    return ret;
}

static int is_new_volnode(sx_hashfs_t *h, const sx_hashfs_volume_t *vol);
static int check_file_sizes(sx_hashfs_t *h, const sx_hashfs_volume_t *vol) {
    int ret = 0, r;
    unsigned int i;
    sqlite3_stmt *q = NULL;
    int64_t size = 0;

    if(!vol)
        return -1;

    /* If this node becomes a new volnode for vol, the volume will restore correct values after rebalance is finished */
    if(is_new_volnode(h, vol))
        return 0;

    /* Sum up all files */
    for(i = 0; i < METADBS; i++) {
        q = h->qm_sumfilesizes[i];

        sqlite3_reset(q);
        if(qbind_int64(q, ":volid", vol->id)) {
            ret = -1;
            goto check_file_sizes_err;
        }

        r = qstep(q);
        if(r == SQLITE_ROW) {
            size += sqlite3_column_int64(q, 0);
            sqlite3_reset(q);
        } else if(r != SQLITE_DONE) {
            CHECK_FATAL("Failed to query sum of file sizes for volume %s at meta db %d", vol->name, i);
            ret = -1;
            goto check_file_sizes_err;
        }
    }

    /* Check if current volume size is same as sum of all its files */
    if(size != vol->cursize)
        CHECK_ERROR("Volume %s usage %lld is different than sum of all files: %lld", vol->name, (long long)vol->cursize, (long long)size);

check_file_sizes_err:
    sqlite3_reset(q);
    return ret;
}

/* Check one volume correctness */
static int check_volume(sx_hashfs_t *h, int debug, const sx_hashfs_volume_t *vol) {
    int ret = 0;
    int r;

    if(!vol) {
        CHECK_FATAL("Failed to check volume: NULL argument given");
        return -1;
    }

    CHECK_PGRS;

    /* If this volume is mine, sum up all stored files and check if they match vol->cursize field */
    if(sx_hashfs_is_or_was_my_volume(h, vol)) {
        r = check_file_sizes(h, vol);
        if(r == -1) {
            ret = -1;
            goto check_volume_err;
        }
        ret += r;
    } else {
        if(debug)
            CHECK_INFO("Skipping volume %s - it does not belong to this node", vol->name);
    }

check_volume_err:
    if(ret == -1)
        CHECK_FATAL("Failed to check volume %s due to error", vol->name);
    return ret;
}

static int check_meta(sx_hashfs_t *h, sxi_db_t *db, const char *table, const char *id_field) {
    int ret = 0, r, len;
    sqlite3_stmt *q = NULL, *qcount = NULL;
    char *qry;

    if(!h || !db || !table || !*table || !id_field || !*id_field) {
        CHECK_FATAL("Failed to check meta table: NULL argument");
        return -1;
    }

    /* Can't use :table marker in query for table name, need to create string */
    len = strlen("SELECT key, value FROM ") + strlen(table) + 1;
    qry = malloc(len);
    if(!qry) {
        CHECK_FATAL("Failed to allocate memory");
        return -1;
    }
    snprintf(qry, len, "SELECT key, value FROM %s", table);

    if(qprep(db, &q, qry)) {
        CHECK_FATAL("Failed to prepare query");
        ret = -1;
        goto check_meta_err;
    }

    while((r = qstep(q)) == SQLITE_ROW) {
        const char *key = (const char*)sqlite3_column_text(q, 0);
        const void *value = sqlite3_column_blob(q, 1);
        unsigned int value_len = sqlite3_column_bytes(q, 1);
        if(sx_hashfs_check_meta(key, value, value_len) != OK)
            CHECK_ERROR("Bad meta entry found: %s", msg_get_reason());
    }

    if(r != SQLITE_DONE) {
        CHECK_FATAL("Failed to query meta value");
        ret = -1;
        goto check_meta_err;
    }

    free(qry);
    len = strlen("SELECT , COUNT(*) as count FROM  GROUP BY  HAVING count > :limit") + 2 * strlen(id_field) + strlen(table) + 1;
    qry = malloc(len);
    if(!qry) {
        CHECK_FATAL("Failed to allocate memory");
        ret = -1;
        goto check_meta_err;
    }
    snprintf(qry, len, "SELECT %s, COUNT(*) as count FROM %s GROUP BY %s HAVING count > :limit", id_field, table, id_field);
    if(qprep(db, &qcount, qry)|| qbind_int64(qcount, ":limit", SXLIMIT_META_MAX_ITEMS)) {
        CHECK_FATAL("Failed to prepare meta limits query");
        ret = -1;
        goto check_meta_err;
    }

    while((r = qstep(qcount)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(qcount, 0);
        int count = sqlite3_column_int(qcount, 1);
        CHECK_ERROR("Bad number of meta values for ID %lld: %d", (long long)id, count);
    }

    if(r != SQLITE_DONE) {
        CHECK_FATAL("Failed to query invalid meta values number");
        ret = -1;
        goto check_meta_err;
    }

check_meta_err:
    sqlite3_finalize(q);
    sqlite3_finalize(qcount);
    free(qry);
    return ret;
}

/* Iterate over volumes and report errors. Return number of errors or -1 if checking failed */
static int check_volumes(sx_hashfs_t *h, int debug) {
    int ret = 0;
    int r, s;
    const sx_hashfs_volume_t *vol = NULL;

    for(s = sx_hashfs_volume_first(h, &vol, 0); s == OK; s = sx_hashfs_volume_next(h)) {
        if(debug && vol)
            CHECK_INFO("Checking volume %s", vol->name);
        CHECK_PGRS;

        r = check_volume(h, debug, vol);
        if(r == -1) {
            ret = -1;
            goto check_volumes_err;
        }
        ret += r;
    }

    if(s != ITER_NO_MORE) {
        ret = -1;
        goto check_volumes_err;
    }

    r = check_meta(h, h->db, "vmeta", "volume_id");
    if(r == -1) {
        ret = -1;
        goto check_volumes_err;
    }
    ret += r;

check_volumes_err:
    return ret;
}

static rc_ty volume_get_common(sx_hashfs_t *h, const char *name, int64_t volid, const sx_hashfs_volume_t **volume);

/* Iterate over meta databases and check all files for their correctness */
static int check_files(sx_hashfs_t *h, int debug) {
    int ret = 0, r;
    unsigned int i;
    const sx_hashfs_volume_t *vol = NULL;

    for(i=0; i<METADBS; i++) {
        sqlite3_stmt *list = NULL;
        int rows = 0;

        if(qprep(h->metadb[i], &list, "SELECT fid, volume_id, name, size, content FROM files ORDER BY name ASC")) {
            ret = -1;
            CHECK_FATAL("Failed to prepare queries");
            goto check_files_itererr;
        }

        if(debug) {
            rows = get_count(h->metadb[i], "files");
            CHECK_INFO("Checking consistency of %lld files in metadata database %u / %u...", (long long int)rows, i+1, METADBS);
        }

        while(1) {
            const char *name;
            int64_t size, row;
            unsigned int listlen;
            r = qstep(list);
            unsigned int block_size, blocks;
            const sx_hash_t *hashes;
            int64_t volid;

            if(r == SQLITE_DONE)
                break;
            if(r != SQLITE_ROW) {
                ret = -1;
                goto check_files_itererr;
            }

            CHECK_PGRS;
            row = sqlite3_column_int64(list, 0);
            name = (const char *)sqlite3_column_text(list, 2);

            /* Check if file name is correct */
            if(check_file_name(name) < 0)
                CHECK_ERROR("Found invalid name on row %lld in metadata database %08x: %s", (long long int)row, i, msg_get_reason());

            size = sqlite3_column_int64(list, 3);
            hashes = sqlite3_column_blob(list, 4);
            if(size && !hashes) {
                CHECK_ERROR("Empty list of hashes for non-empty file %s", name);
                continue;
            }
            listlen = sqlite3_column_bytes(list, 4);
            blocks = size_to_blocks(size, NULL, &block_size);
            if(size < 0 || (listlen % SXI_SHA1_BIN_LEN) || blocks != listlen / SXI_SHA1_BIN_LEN)
                CHECK_ERROR("Invalid size for file %s (row %lld) in metadata database %08x", name, (long long int)row, i);

            vol = NULL;
            volid = sqlite3_column_int64(list, 1);
            if(volid <= 0) {
                CHECK_ERROR("Invalid volume id for file %s", name);
                continue; /* Stop checking current file now, volume reference is needed for blocks checking */
            } else {
                rc_ty rc = sx_hashfs_volume_by_id(h, volid, &vol);
                if(rc == ENOENT) {
                    if(debug)
                        CHECK_INFO("Volume with ID %lld does not exist or is disabled, but file %s references to it", (long long)volid, name);
                    continue; /* Stop checking current file now, volume reference is needed for blocks checking */
                } else if(rc || !vol) {
                    ret = -1;
                    goto check_files_itererr;
                }
            }

            /* Check if all hashes for given file are stored in database */
            if(debug)
                CHECK_INFO("Checking existence of hashes for file %s: %u", name, blocks);
            r = check_file_hashes(h, debug, hashes, listlen / SXI_SHA1_BIN_LEN, block_size, vol->max_replica);
            if(r == -1) {
                ret = -1;
                goto check_files_itererr;
            } else if(r) {
                ret += r;
                CHECK_PRINT_WARN("%d hashes were not found in database", r);
            }
        }

        r = check_meta(h, h->metadb[i], "fmeta", "file_id");
        if(r == -1) {
            ret = -1;
            goto check_files_itererr;
        }
        ret += r;

    check_files_itererr:
        sqlite3_finalize(list);

        if(ret == -1) {
            CHECK_FATAL("Verification of files in metadata database %08x aborted due to errors", i);
            goto check_files_err;
        }
    }

check_files_err:
    return ret;
}

/* Check if all blocks stored in database are also stored in binary files */
static int check_blocks_existence(sx_hashfs_t *h, int debug, unsigned int hs, unsigned int ndb) {
    int ret = 0, r;
    sqlite3_stmt *q = NULL;
    sxi_db_t *db;

    if(hs >= SIZES || ndb >= HASHDBS) {
        ret = -1;
        goto check_blocks_existence_err;
    }

    db = h->datadb[hs][ndb];

    if(debug) {
        CHECK_INFO("Checking consistency of %lld blocks in %s hash database %u / %u...",
             (long long int)get_count(db, "blocks"), sizelongnames[hs], ndb+1, HASHDBS);
    }

    if(qprep(db, &q, "SELECT id, hash, blockno FROM blocks WHERE blockno IS NOT NULL ORDER BY blockno ASC")) {
        ret = -1;
        goto check_blocks_existence_err;
    }

    while(1) {
        char h1[SXI_SHA1_BIN_LEN * 2 + 1], h2[SXI_SHA1_BIN_LEN * 2 + 1];
        const sx_hash_t *refhash;
        sx_hash_t comphash;
        int64_t off, row;
        r = qstep(q);
        if(r == SQLITE_DONE)
            break;
        if(r != SQLITE_ROW) {
            ret = -1;
            goto check_blocks_existence_err;
        }

        CHECK_PGRS;

        row = sqlite3_column_int64(q, 0);

        refhash = (const sx_hash_t *)sqlite3_column_blob(q, 1);
        if(!refhash || sqlite3_column_bytes(q, 1) != SXI_SHA1_BIN_LEN) {
            CHECK_ERROR("Found invalid hash on row %lld in %s hash database %08x", (long long int)row, sizelongnames[hs], ndb);
            continue;
        }

        off = sqlite3_column_int64(q, 2);
        if(off <= 0) {
            bin2hex(refhash->b, sizeof(*refhash), h1, sizeof(h1));
            CHECK_ERROR("Invalid offset (%lld) found for hash %s (row %lld) in %s data file %08x",
                (long long)off, h1, (long long int)row, sizelongnames[hs], ndb);
            continue;
        }
        off *= bsz[hs];

        if(read_block(h->datafd[hs][ndb], h->blockbuf, off, bsz[hs])) {
            bin2hex(refhash->b, sizeof(*refhash), h1, sizeof(h1));
            CHECK_ERROR("Failed to read hash %s (row %lld) from %s data file %08x at offset %lld", h1, (long long int)row, sizelongnames[hs], ndb, (long long int)off);
            continue;
        }

        if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), h->blockbuf, bsz[hs], &comphash)) {
            ret = -1;
            goto check_blocks_existence_err;
        }

        if(cmphash(refhash, &comphash)) {
            bin2hex(refhash->b, sizeof(*refhash), h1, sizeof(h1));
            bin2hex(comphash.b, sizeof(comphash), h2, sizeof(h2));
            CHECK_ERROR("Mismatch %s (row %lld) vs %s on %s data file %08x at offset %lld", h1, (long long int)row, h2, sizelongnames[hs], ndb, (long long int)off);
        }
    }

check_blocks_existence_err:
    sqlite3_finalize(q);
    return ret;
}

/* Check if there exist any duplicated hashes in block databases */
static int check_blocks_dups(sx_hashfs_t *h, int debug, unsigned int hs, unsigned int ndb) {
    int ret = 0, r;
    sqlite3_stmt *q = NULL;
    sxi_db_t *db;

    if(hs >= SIZES || ndb >= HASHDBS) {
        ret = -1;
        goto check_blocks_dups_err;
    }

    db = h->datadb[hs][ndb];

    if(debug) {
        CHECK_INFO("Checking duplicates within %lld blocks in %s hash database %u / %u...",
            (long long int)get_count(db, "blocks"), sizelongnames[hs], ndb+1, HASHDBS);
    }

    if(qprep(db, &q, "SELECT b1.id, b1.hash, b2.id, b2.hash, b1.blockno FROM blocks AS b1 LEFT JOIN blocks AS b2 ON b1.id < b2.id WHERE b1.blockno = b2.blockno"))
        goto check_blocks_dups_err;

    while(1) {
        char h1[SXI_SHA1_BIN_LEN * 2 + 1], h2[SXI_SHA1_BIN_LEN * 2 + 1];
        const sx_hash_t *hash1, *hash2;
        int64_t row1, row2;
        r = qstep(q);
        if(r == SQLITE_DONE)
            break;
        if(r != SQLITE_ROW) {
            ret = -1;
            goto check_blocks_dups_err;
        }

        CHECK_PGRS;

        row1 = sqlite3_column_int64(q, 0);
        row2 = sqlite3_column_int64(q, 2);
        if(row1 > row2) /* Filtering out half of the set i.e. we report (A, B) but not (B, A) */
            continue;   /* For some reasons doing this in sql is very slow */

        hash1 = (const sx_hash_t *)sqlite3_column_blob(q, 1);
        hash2 = (const sx_hash_t *)sqlite3_column_blob(q, 3);
        if(sqlite3_column_bytes(q, 1) != sizeof(*hash1))
            strcpy(h1, "<INVALID HASH>");
        else
            bin2hex(hash1->b, sizeof(*hash1), h1, sizeof(h1));
        if(sqlite3_column_bytes(q, 3) != sizeof(*hash2))
            strcpy(h2, "<INVALID HASH>");
        else
            bin2hex(hash2->b, sizeof(*hash2), h2, sizeof(h2));

        CHECK_ERROR("Hash %s (row %lld) and hash %s (row %lld) in %s hash database %08x share the same block number %lld",
            h1, (long long int)row1, h2, (long long int)row2, sizelongnames[hs], ndb, sqlite3_column_int64(q, 4));
    }

check_blocks_dups_err:
    sqlite3_finalize(q);
    return ret;
}

/* Check if blocks stored in binary files correspond to hases stored in database */
static int check_blocks(sx_hashfs_t *h, int debug) {
    int ret = 0, r;
    unsigned int i, j;

    for(j = 0; j < SIZES; j++) {
        for(i = 0; i < HASHDBS; i++) {
            sqlite3_stmt *index = NULL;

            /* Create index on blockno field to prevent sqlite from doning this */
            if(qprep(h->datadb[j][i], &index, "CREATE INDEX blocknoidx ON blocks(blockno)") || qstep_noret(index)) {
                ret = -1;
                goto check_blocks_itererr;
            }

            CHECK_PGRS;
            /* Look for non-existing blocks */
            r = check_blocks_existence(h, debug, j, i);
            if(r == -1) {
                ret = -1;
                goto check_blocks_itererr;
            }
            ret += r;

            /* Look for duplicated hashes */
            r = check_blocks_dups(h, debug, j, i);
            if(r == -1) {
                ret = -1;
                goto check_blocks_itererr;
            }
            ret += r;

            check_blocks_itererr:
            sqlite3_finalize(index);

            if(ret == -1) {
                CHECK_FATAL("Verification of hashes in %s hash database %08x aborted due to errors", sizelongnames[j], i);
                goto check_blocks_err;
            }
        }
    }

check_blocks_err:
    return ret;
}

static int check_users(sx_hashfs_t *h, int debug) {
    int ret = 0, r;
    unsigned int admin_found = 0;
    sqlite3_stmt *q = NULL;

    if(qprep(h->db, &q, "SELECT uid, user, name, role, enabled FROM users")) {
        ret = -1;
        goto check_users_err;
    }

    while((r = qstep(q)) == SQLITE_ROW) {
        int64_t userid = sqlite3_column_int64(q, 0);
        int uid_len = sqlite3_column_bytes(q, 1);
        const char *name = (const char*)sqlite3_column_text(q, 2);
        int role = sqlite3_column_int(q, 3);

        CHECK_PGRS;
        if(uid_len != AUTH_UID_LEN)
            CHECK_ERROR("User with ID %lld has bad UID assigned", (long long)userid);

        if(sx_hashfs_check_username(name))
            CHECK_ERROR("Bad name for user with ID %lld", (long long)userid);

        /* Check special admin user existence */
        if(name && !strcmp(name, "admin")) {
            int enabled = sqlite3_column_int(q, 4);

            if(role != ROLE_ADMIN)
                CHECK_ERROR("admin user has bad role: %d", role);

            if(!enabled)
                CHECK_ERROR("admin user is disabled");

            admin_found = 1;
            continue; /* Skip roles checking, already done */
        }

        if(role != ROLE_CLUSTER && role != ROLE_ADMIN && role != ROLE_USER)
            CHECK_ERROR("User %s has incorrect role", name);

        if(name && strcmp(name, "rootcluster") && role == ROLE_CLUSTER)
            CHECK_ERROR("User %s has CLUSTER role", name);
    }

    if(!admin_found && h->have_hd)
        CHECK_ERROR("admin user was not found");

    if(r != SQLITE_DONE)
        ret = -1;

check_users_err:
    sqlite3_finalize(q);
    return ret;
}

/* Check all entries in tmp db for correct names, meta values and volume references */
static int check_tmpdb(sx_hashfs_t *h, int debug) {
    int ret = 0, r;
    sqlite3_stmt *q = NULL;

    if(qprep(h->tempdb, &q, "SELECT tid, volume_id, name FROM tmpfiles")) {
        CHECK_FATAL("Failed to prepare query for temp db");
        ret = -1;
        goto check_tmpdb_err;
    }

    while((r = qstep(q)) == SQLITE_ROW) {
        int64_t tid = sqlite3_column_int64(q, 0);
        int64_t volid = sqlite3_column_int64(q, 1);
        const char *name = (const char*)sqlite3_column_text(q, 2);
        const sx_hashfs_volume_t *vol = NULL;

        if(check_file_name(name) < 0)
            CHECK_ERROR("Bad file name for ID %lld", (long long)tid);

        if(sx_hashfs_volume_by_id(h, volid, &vol) != OK)
            CHECK_ERROR("Volume with ID %lld is referenced by tempfile %s, but it does not exist or is disabled", (long long)volid, name);
    }

    r = check_meta(h, h->tempdb, "tmpmeta", "tid");
    if(r == -1) {
        ret = -1;
        goto check_tmpdb_err;
    }
    ret += r;

check_tmpdb_err:
    sqlite3_finalize(q);
    return ret;
}

/* Check for broken parent IDs and user IDs in jobs table */
static int check_jobs(sx_hashfs_t *h, int debug) {
    int ret = 0, r;
    sqlite3_stmt *q = NULL;

    if(qprep(h->eventdb, &q, "SELECT j1.job, j2.job, j1.user, j1.parent FROM jobs j1 LEFT JOIN jobs j2 ON j1.parent = j2.job WHERE j1.parent IS NOT NULL AND j1.user IS NOT NULL")) {
        CHECK_FATAL("Failed to prepare query");
        return -1;
    }

    while((r = qstep(q)) == SQLITE_ROW) {
        int64_t job = sqlite3_column_int64(q, 0);
        int64_t userid = sqlite3_column_int64(q, 2);
        uint8_t useruid[AUTH_UID_LEN];
        if(sx_hashfs_get_user_by_uid(h, (sx_uid_t)userid, useruid, 0) != OK)
            CHECK_ERROR("User with ID %lld is job %lld owner but does not exist or is disabled", (long long)userid, (long long)job);

        if(sqlite3_column_type(q, 1) == SQLITE_NULL) {
            int64_t parent = sqlite3_column_int64(q, 3);
            CHECK_ERROR("Job with ID %lld is a parent for job with ID %lld but it could not be found", (long long)parent, (long long)job);
        }
    }

    sqlite3_finalize(q);
    return ret;
}

/* Lock given database exclusively and initialize locking and unlocking queries */
static int lock_db(sxi_db_t *db, sqlite3_stmt **lock, sqlite3_stmt **unlock) {
    const char *l = "BEGIN EXCLUSIVE TRANSACTION", *ul = "ROLLBACK";

    if(!db || !lock || !unlock) {
        CHECK_FATAL("Failed to prepare locks: NULL argument");
        return -1;
    }

    if(qprep(db, lock, l) || qprep(db, unlock, ul)) {
        CHECK_FATAL("Failed to prepare locks queries");
        goto lock_db_err;
    }

    if(qstep_noret(*lock)) {
        CHECK_FATAL("Failed to lock database: query failed");
        goto lock_db_err;
    }

    return 0;
lock_db_err:
    sqlite3_finalize(*lock);
    *lock = NULL;
    sqlite3_finalize(*unlock);
    *unlock = NULL;
    return -1;
}

#define RUN_CHECK(func) do { r = func(h, debug); if(r == -1) { ret = -1; goto sx_hashfs_check_err; } ret += r; } while(0)
int sx_hashfs_check(sx_hashfs_t *h, int debug) {
    int ret = -1, r = 0, i, j;
    sqlite3_stmt *locks[METADBS + SIZES * HASHDBS + 4], *unlocks[METADBS + SIZES * HASHDBS + 4];
    int readonly = 0, hashfs_locked = 0;
    struct flock fl;

    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;

    memset(locks, 0, sizeof(locks));
    memset(unlocks, 0, sizeof(unlocks));

    for(i = 0; i < METADBS; i++, r++) {
        if(lock_db(h->metadb[i], locks + r, unlocks + r)) {
            CHECK_FATAL("Failed to lock database meta database");
            goto sx_hashfs_check_err;
        }
    }

    for(i = 0; i < SIZES; i++) {
        for(j = 0; j < HASHDBS; j++, r++) {
            if(lock_db(h->datadb[i][j], locks + r, unlocks + r)) {
                CHECK_FATAL("Failed to lock database hash database");
                goto sx_hashfs_check_err;
            }
        }
    }

    if(lock_db(h->db, locks + r, unlocks + r) || lock_db(h->tempdb, locks + r + 1, unlocks + r + 1)
       || lock_db(h->xferdb, locks + r + 2, unlocks + r + 2) || lock_db(h->eventdb, locks + r + 3, unlocks + r + 3)) {
        CHECK_FATAL("Failed to lock database");
        goto sx_hashfs_check_err;
    }

    if(sx_hashfs_cluster_get_mode(h, &readonly)) {
        CHECK_FATAL("Failed to check cluster operating mode");
        goto sx_hashfs_check_err;
    }

    if(!readonly) {
        if(fcntl(h->lockfd, F_SETLK, &fl) == -1) {
            if(errno == EAGAIN || errno == EACCES)
                CHECK_FATAL("Cluster needs to be in read-only mode or this node should be stopped in order to perform storage check");
            else
                CHECK_FATAL("Failed to lock HashFS storage: %s", strerror(errno));
            goto sx_hashfs_check_err;
        }
        hashfs_locked = 1;
    }
    /* Cluster is in read-only mode or node is stopped, we can perform HashFS check */

    ret = 0;
    CHECK_START;

    /* Analyze databases using PRAGMA integrity_check query */
    RUN_CHECK(sx_hashfs_analyze);
    /* Check volumes correctness */
    RUN_CHECK(check_volumes);
    /* Check files correctness */
    RUN_CHECK(check_files);
    /* Check blocks sanity */
    RUN_CHECK(check_blocks);
    /* Check users table */
    RUN_CHECK(check_users);
    /* Check tempfiles database for IDs and meta correectness */
    RUN_CHECK(check_tmpdb);
    /* Check jobs correctness */
    RUN_CHECK(check_jobs);

sx_hashfs_check_err:
    CHECK_FINISH;

    for(i = 0; i < SIZES * HASHDBS + METADBS + 4; i++) {
        if(unlocks[i] && qstep_noret(unlocks[i]))
	    CHECK_FATAL("Failed to unlock database");
        sqlite3_finalize(locks[i]);
        sqlite3_finalize(unlocks[i]);
    }

    if(hashfs_locked) {
        fl.l_type = F_RDLCK; /* Downgrade to read lock */
        if(fcntl(h->lockfd, F_SETLK, &fl) == -1)
            CHECK_FATAL("Failed to release HashFS lock: %s", strerror(errno));
    }

    return ret;
}

typedef struct {
    sxi_db_t *hashfs;
    sxi_db_t *meta[METADBS];
    sxi_db_t *data[SIZES][HASHDBS];
    sxi_db_t *temp;
    sxi_db_t *event;
    sxi_db_t *xfer;
} sxi_all_db_t;

typedef rc_ty (*sx_db_upgrade_fn)(sxi_db_t *db);
typedef struct {
    const char *from;
    const char *to;
    sx_db_upgrade_fn upgrade_hashfsdb;
    sx_db_upgrade_fn upgrade_metadb;
    sx_db_upgrade_fn upgrade_datadb;
    sx_db_upgrade_fn upgrade_tempdb;
    sx_db_upgrade_fn upgrade_eventsdb;
    sx_db_upgrade_fn upgrade_xfersdb;
    rc_ty (*upgrade_alldb)(sxi_all_db_t *alldb);
} sx_upgrade_t;

static rc_ty upgrade_db_prepare(const char *path, const char *dbitem, sxi_db_t **dbp)
{
    sqlite3_stmt *qlock = NULL, *qunlock = NULL, *q = NULL;

    DEBUG("Opening %s", path);
    if (!path || !dbitem || !dbp) {
        NULLARG();
        return EFAULT;
    }
    if (qopen(path, dbp, dbitem, NULL, NULL))
        return FAIL_EINTERNAL;
    if (qprep(*dbp, &q, "PRAGMA foreign_keys=ON") || qstep_noret(q))
        return FAIL_EINTERNAL;
    qnullify(q);
    if (lock_db(*dbp, &qlock, &qunlock) ||
        analyze_db(*dbp, 0)) {
        CRIT("Integrity check failed for %s", path);
        if (qunlock)
            qstep_noret(qunlock);
        qnullify(qunlock);
        qnullify(qlock);
        qclose(dbp);
        return FAIL_EINTERNAL;
    }
    qnullify(qunlock);
    qnullify(qlock);
    return OK;
}

static int ensure_not_running(int lockfd, const char *path)
{
    struct flock fl;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if(fcntl(lockfd, F_SETLK, &fl) == -1) {
        if(errno == EAGAIN || errno == EACCES)
            WARN("Cannot acquire write lock on storage %s: node still running?", path);
        else
            PWARN("Failed to lock HashFS storage: %s", path);
        return -1;
    }
    unsigned n = strlen(path) + sizeof("/hashfs.db");
    char *dbpath = wrap_malloc(n);
    if (!dbpath)
        return -1;
    snprintf(dbpath, n, "%s/hashfs.db", path);
    int ret = 0;
    int dbf = open(dbpath, O_RDWR, 0);
    if (dbf < 0) {
        PWARN("Cannot open database %s", dbpath);
        ret = -1;
    }
    free(dbpath);
    if(!ret) {
        if (fcntl(dbf, F_SETLK, &fl) == -1) {
            if(errno == EAGAIN || errno == EACCES)
                WARN("Cannot acquire write lock on storage db %s: node still running?", path);
            else
                PWARN("Failed to lock HashFS storage db: %s", path);
            ret = -1;
        }
        close(dbf);
    }
    return ret;
}


static rc_ty upgrade_db(int lockfd, const char *path, sxi_db_t *db, const char *from_version, const char *to_version, sx_db_upgrade_fn fn)
{
    sqlite3_stmt *q = NULL;
    rc_ty ret = FAIL_EINTERNAL;

    do {
        if(qprep(db, &q, "SELECT value FROM hashfs WHERE key = :k") ||
           qbind_text(q, ":k", "version") || qstep_ret(q))
            break;
        const unsigned char *str = sqlite3_column_text(q, 0);
        if (!str) {
            CRIT("Unable to determine database version at %s", path);
            break;
        }
        int cmp = strcmp((const char*)str, from_version);
        if (cmp < 0) {
            WARN("Database schema too old: %s, expected >= %s", str, from_version);
            ret = EINVAL;
        }
        if (cmp > 0) {
            ret = OK;
            DEBUG("Upgrade not needed on %s", path);
        }
        if (cmp)
            break;
        if (ensure_not_running(lockfd, path)) {
            ret = FAIL_LOCKED;
            break;
        }
        /* old version matches, update it */
        qnullify(q);
        DEBUG("Upgrading DB %s: %s -> %s", path, from_version, to_version);

        if (fn && fn(db)) {
            CRIT("Upgrade callback failed for %s (%s -> %s)", path, from_version, to_version);
            break;
        }

        if(qprep(db, &q, "UPDATE hashfs SET value=:v WHERE key=:k") ||
           qbind_text(q, ":k", "version") ||
           qbind_text(q, ":v", to_version) ||
           qstep_noret(q))
            break;
        INFO("Upgraded DB %s from %s to %s", path, from_version, to_version);
        ret = OK;
    } while(0);
    if (ret)
        WARN("Failed to upgrade %s", path);
    qnullify(q);
    return ret;
}

static rc_ty hashfs_1_0_to_1_1(sxi_db_t *db)
{
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    do {
        if(qprep(db, &q, "CREATE TABLE IF NOT EXISTS usermeta (userid INTEGER PRIMARY KEY NOT NULL REFERENCES users(uid) ON DELETE CASCADE ON UPDATE CASCADE, desc TEXT("STRIFY(SXLIMIT_META_MAX_VALUE_LEN)"))") || qstep_noret(q))
            break;
	qnullify(q);

	if(qprep(db, &q, "CREATE TABLE ignorednodes (dist INTEGER NOT NULL, node BLOB ("STRIFY(UUID_BINARY_SIZE)") NOT NULL, PRIMARY KEY (dist, node))") || qstep_noret(q))
	    break;
	qnullify(q);

        ret = OK;
    } while(0);
    qnullify(q);
    return ret;
}

static rc_ty metadb_1_0_to_1_1(sxi_db_t *db)
{
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    do {
        /* add column age ... */
        if(qprep(db, &q, "ALTER TABLE files ADD COLUMN revision_id BLOB("STRIFY(SXI_SHA1_BIN_LEN)")") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "ALTER TABLE files ADD COLUMN age NULL DEFAULT 0") ||
           qstep_noret(q))
        /* TODO: for new files insert correct one already */
        if(qprep(db, &q, "CREATE UNIQUE INDEX IF NOT EXISTS file_rev_id ON files(revision_id)") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE INDEX IF NOT EXISTS file_vol_revs ON files(volume_id, revision_id, age)") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE TABLE IF NOT EXISTS heal(\
            revision_id BLOB("STRIFY(SXI_SHA1_BIN_LEN)") PRIMARY KEY NOT NULL,\
            remote_volume INT,\
            blocks INT,\
            blocksize INT,\
            replica_count INT)") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE INDEX IF NOT EXISTS heal_vol ON heal(blocks, remote_volume)") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE TABLE IF NOT EXISTS heal_volume(\
            name TEXT NOT NULL,\
            max_age INTEGER NOT NULL,\
            min_revision BLOB NOT NULL)") || qstep_noret(q))
            break;
        qnullify(q);
        ret = OK;
    } while(0);
    qnullify(q);
    return ret;
}

static rc_ty datadb_1_0_to_1_1(sxi_db_t *db)
{
    /* TODO: disable GC until upgrade job is run */
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    do {
        if(qprep(db, &q, "DELETE FROM blocks WHERE blockno IS NULL OR created_at IS NULL") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "DROP TABLE IF EXISTS operations") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "DROP TABLE IF EXISTS use") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "DROP TABLE IF EXISTS reservations") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE TABLE IF NOT EXISTS reservations (\
            reservations_id BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                    revision_id BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                    ttl INTEGER NOT NULL,\
                    PRIMARY KEY(reservations_id, revision_id))") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE INDEX IF NOT EXISTS idx_res ON reservations(revision_id, reservations_id)") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE INDEX IF NOT EXISTS idx_res_ttl ON reservations(ttl, revision_id)") || qstep_noret(q))
            break;
        qnullify(q);
        /* age = rebalance/hdist version
         * */
        if(qprep(db, &q, "CREATE TABLE IF NOT EXISTS revision_blocks (\
            revision_id BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                    blocks_hash BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                    age INTEGER NOT NULL,\
                    replica INTEGER NOT NULL,\
                    PRIMARY KEY(revision_id, blocks_hash, age))") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE INDEX IF NOT EXISTS idx_revmap ON revision_blocks(blocks_hash, revision_id)") || qstep_noret(q))
            break;
        qnullify(q);

        /* op:
         * +1: inuse
         *  0: reserved
         * -1: delete */
        if(qprep(db, &q, "CREATE TABLE IF NOT EXISTS revision_ops(\
            revision_id BLOB("STRIFY(SXI_SHA1_BIN_LEN)") NOT NULL,\
                    op INTEGER NOT NULL,\
                    age INTEGER NOT NULL,\
                    PRIMARY KEY(revision_id, op))") || qstep_noret(q))
            break;
        qnullify(q);
        if(qprep(db, &q, "CREATE INDEX IF NOT EXISTS idx_op_revision ON revision_ops(op, revision_id, age)") || qstep_noret(q))
            break;
        qnullify(q);
        /* TODO: how to add NOT NULL constraint to 1.0 blocks schema without
         * reimporting table? */
        ret = OK;
    } while(0);
    qnullify(q);
    return ret;
}

static rc_ty eventsdb_1_0_to_1_1(sxi_db_t *db)
{
    char qrybuff[128];
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    do {
        /* delete duplicate delete jobs so we can add unique constraint */
        snprintf(qrybuff, sizeof(qrybuff), "DELETE FROM jobs WHERE type=%d AND rowid NOT IN (SELECT MAX(rowid) FROM jobs WHERE type=%d GROUP BY data)", JOBTYPE_DELETE_FILE, JOBTYPE_DELETE_FILE);
        if (qprep(db, &q, qrybuff) || qstep_noret(q))
            break;
        qnullify(q);
        snprintf(qrybuff, sizeof(qrybuff), "CREATE UNIQUE INDEX IF NOT EXISTS jobs_data ON jobs(data) WHERE type = %d", JOBTYPE_DELETE_FILE);
        if (qprep(db, &q, qrybuff) || qstep_noret(q))
            break;
        qnullify(q);
        /* TODO: upgrade jobs */
        ret = OK;
    } while(0);
    qnullify(q);
    return ret;
}

static rc_ty tempdb_1_0_to_1_1(sxi_db_t *db)
{
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    do {
        qnullify(q);
        ret = OK;
    } while(0);
    qnullify(q);
    return ret;
}

static rc_ty xfersdb_1_0_to_1_1(sxi_db_t *db)
{
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    do {
        qnullify(q);
        ret = OK;
    } while(0);
    qnullify(q);
    return ret;
}

static rc_ty alldb_1_0_to_1_1(sxi_all_db_t *alldb)
{
    /* running this function again must succeed and be a noop */
    rc_ty ret = FAIL_EINTERNAL;
    char qrybuff[128];
    sqlite3_stmt *q = NULL, *tq = NULL, *jdq = NULL;
    do {
        int64_t tmpid;
        int r;

        /* Update file delete jobs having tempfile ID as job data to store file revision instead */
        snprintf(qrybuff, sizeof(qrybuff), "SELECT job, data FROM jobs WHERE type = %d AND LENGTH(data) == %ld AND complete = 0 LIMIT 1", JOBTYPE_DELETE_FILE, sizeof(tmpid));
        if (qprep(alldb->event, &q, qrybuff))
            break;
        if (qprep(alldb->temp, &tq, "SELECT t || ':' || token FROM tmpfiles WHERE tid = :id"))
            break;
        if (qprep(alldb->event, &jdq, "UPDATE jobs SET data = :data WHERE job = :id"))
            break;

        r = qstep(q);
        while(r == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(q, 0);
            const void *data = sqlite3_column_blob(q, 1);
            const char *rev;
            int rt;

            /* Set tempfile ID from current job data */
            memcpy(&tmpid, data, sizeof(tmpid));
            sqlite3_reset(q);
            sqlite3_reset(tq);
            sqlite3_reset(jdq);

            /* Get tempfile entry */
            if(qbind_int64(tq, ":id", tmpid)) {
                WARN("Failed to prepare tempfile picking query for ID %lld", (long long)tmpid);
                break;
            }

            rt = qstep(tq);
            if(rt == SQLITE_DONE) {
                DEBUG("Tempfile %lld does not exist for job %lld", (long long)tmpid, (long long)id);
                r = qstep(q);
                continue;
            } else if(rt != SQLITE_ROW) {
                WARN("Failed to get tempfile %lld", (long long)tmpid);
                break;
            }

            /* Get tempfile revision */
            rev = (const char *)sqlite3_column_text(tq, 0);
            if(!rev || strlen(rev) != REV_LEN) {
                WARN("Invalid tempfile revision");
                break;
            }

            /* Update existing job data */
            if(qbind_blob(jdq, ":data", rev, REV_LEN) || qbind_int64(jdq, ":id", id) || qstep_noret(jdq)) {
                WARN("Failed to perform job data update query");
                break;
            }
            DEBUG("Upgraded job %lld data from tempfile ID %lld to revision %s", (long long)id, (long long)tmpid, rev);

            sqlite3_reset(tq);
            sqlite3_reset(jdq);
            r = qstep(q);
        }

        if(r != SQLITE_DONE) {
            WARN("Failed to finish filedelete jobs upgrade");
            break;
        }
        ret = OK;
    } while(0);
    qnullify(jdq);
    qnullify(tq);
    qnullify(q);
    return ret;
}

static const sx_upgrade_t upgrade_sequence[] = {
    {
        HASHFS_VERSION_1_0,
        HASHFS_VERSION_1_1,
        .upgrade_hashfsdb = hashfs_1_0_to_1_1,
        .upgrade_metadb = metadb_1_0_to_1_1,
        .upgrade_datadb = datadb_1_0_to_1_1,
        .upgrade_tempdb = tempdb_1_0_to_1_1,
        .upgrade_eventsdb = eventsdb_1_0_to_1_1,
        .upgrade_xfersdb = xfersdb_1_0_to_1_1,
        .upgrade_alldb = alldb_1_0_to_1_1
    }
};

static rc_ty upgrade_bin(int lockfd, const char *dir, const char *path, const char *from, const char *to, unsigned j, unsigned i)
{
    rc_ty ret = FAIL_EINTERNAL;
    char old_header[128];
    uint8_t *blockbuf = NULL;

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("open");
        return ret;
    }
    do {
        blockbuf = wrap_malloc(bsz[j]);
        if (!blockbuf)
            break;
        snprintf(old_header, sizeof(old_header), "%-16sdatafile_%c_%08x             %08x", from, sizedirs[j], i, bsz[j]);
        if(read_block(fd, blockbuf, 0, bsz[j]))
            break;
        if(!memcmp(blockbuf, old_header, strlen(old_header))) {
            /* old version matches, update it */
            if (ensure_not_running(lockfd, dir)) {
                ret = FAIL_LOCKED;
                break;
            }
            snprintf((char*)blockbuf, bsz[j], "%-16sdatafile_%c_%08x             %08x", to, sizedirs[j], i, bsz[j]);
            if(write_block(fd, blockbuf, 0, bsz[j]))
                break;
        }
        ret = OK;
    } while(0);
    free(blockbuf);
    if (close(fd) < 0) {
        perror("close");
        return FAIL_EINTERNAL;
    }

    return ret;
}

static int qcommit_alldb(sxi_all_db_t *alldb) {
    unsigned i,j;
    if(qcommit(alldb->hashfs))
	return -1;
    for(i=0;i<METADBS;i++)
        if(qcommit(alldb->meta[i]))
            return -1;
    for(j=0; j<SIZES; j++)
        for(i=0; i<HASHDBS; i++)
            if(qcommit(alldb->data[j][i]))
                return -1;
    if(qcommit(alldb->temp))
	return -1;
    if(qcommit(alldb->event))
	return -1;
    if(qcommit(alldb->xfer))
	return -1;
    return 0;
}

static void qrollback_alldb(sxi_all_db_t *alldb)
{
    unsigned i, j;
    qrollback(alldb->hashfs);
    for(i=0;i<METADBS;i++)
        qrollback(alldb->meta[i]);
    for(j=0; j<SIZES; j++)
        for(i=0; i<HASHDBS; i++)
            qrollback(alldb->data[j][i]);
    qrollback(alldb->temp);
    qrollback(alldb->event);
    qrollback(alldb->xfer);
}

static void qclose_alldb(sxi_all_db_t *alldb)
{
    unsigned i, j;
    qclose(&alldb->hashfs);
    for(i=0;i<METADBS;i++)
        qclose(&alldb->meta[i]);
    for(j=0; j<SIZES; j++)
        for(i=0; i<HASHDBS; i++)
            qclose(&alldb->data[j][i]);
    qclose(&alldb->temp);
    qclose(&alldb->event);
    qclose(&alldb->xfer);
}

rc_ty sx_storage_upgrade(const char *dir) {
    sxi_all_db_t alldb;
    unsigned i,j,pathlen;
    char *path;
    char dbitem[64];
    struct timeval tv_start, tv_integrity_done, tv_upgrade_done, tv_close;
    int lockfd = -1;

    pathlen = strlen(dir) + 1024;
    rc_ty ret = FAIL_EINTERNAL;
    memset(&alldb, 0, sizeof(alldb));

    if(!(path = wrap_malloc(pathlen)))
        return FAIL_EINTERNAL;
    if (!qlog_set) {
        sqlite3_config(SQLITE_CONFIG_LOG, qlog, NULL);
        qlog_set = 1;
    }

    INFO("Performing integrity check on %s", dir);
    gettimeofday(&tv_start, NULL);
    {
        snprintf(path, pathlen, "%s/hashfs.db", dir);
        if ((ret = upgrade_db_prepare(path, "hashfs", &alldb.hashfs)))
            goto upgrade_fail;

        for(i=0; i<METADBS; i++) {
            snprintf(path, pathlen, "%s/f%08x.db", dir, i);
            snprintf(dbitem, sizeof(dbitem), "metadb_%08x", i);
            if ((ret = upgrade_db_prepare(path, dbitem, &alldb.meta[i])))
                goto upgrade_fail;
        }

        for(j=0; j<SIZES; j++) {
            for(i=0; i<HASHDBS; i++) {
                snprintf(path, pathlen, "%s/h%c%08x.db", dir, sizedirs[j], i);
                snprintf(dbitem, sizeof(dbitem), "hashdb_%c_%08x", sizedirs[j], i);
                if ((ret = upgrade_db_prepare(path, dbitem, &alldb.data[j][i])))
                    goto upgrade_fail;
            }
        }

        snprintf(path, pathlen, "%s/temp.db", dir);
        if((ret = upgrade_db_prepare(path, "tempdb", &alldb.temp)))
            goto upgrade_fail;

        snprintf(path, pathlen, "%s/events.db", dir);
        if((ret = upgrade_db_prepare(path, "eventdb", &alldb.event)))
            goto upgrade_fail;

        snprintf(path, pathlen, "%s/xfers.db", dir);
        if((ret = upgrade_db_prepare(path, "xferdb", &alldb.xfer)))
            goto upgrade_fail;

        snprintf(path, pathlen, "%s/hashfs.lock", dir);
        lockfd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (lockfd < 0) {
            PWARN("Failed to open %s lockfile", path);
            goto upgrade_fail;
        }
        struct flock fl;
        fl.l_start = 0;
        fl.l_len = 0;
        fl.l_type = F_RDLCK;
        fl.l_whence = SEEK_SET;
        if(fcntl(lockfd, F_SETLK, &fl) == -1) {
            if(errno == EACCES || errno == EAGAIN)
                INFO("Failed lock HashFS: Storage is locked for maintenance");
            else
                PWARN("Failed to acquire read lock");
            goto upgrade_fail;
        }
        snprintf(path, pathlen, "%s", dir);
    }
    gettimeofday(&tv_integrity_done, NULL);
    INFO("Integrity check completed in %.fs", timediff(&tv_start, &tv_integrity_done));

    for(i=0;i<sizeof(upgrade_sequence)/sizeof(upgrade_sequence[0]);i++) {
        sx_upgrade_t desc = upgrade_sequence[i];
        if ((ret = upgrade_db(lockfd, path, alldb.hashfs, desc.from, desc.to, desc.upgrade_hashfsdb)))
            goto upgrade_fail;
        for(i=0; i<METADBS; i++) {
            if ((ret = upgrade_db(lockfd, path, alldb.meta[i], desc.from, desc.to, desc.upgrade_metadb)))
                goto upgrade_fail;
        }

        for(j=0; j<SIZES; j++) {
            for(i=0; i<HASHDBS; i++) {
                snprintf(path, pathlen, "%s", dir);
                if ((ret = upgrade_db(lockfd, path, alldb.data[j][i], desc.from, desc.to, desc.upgrade_datadb)))
                    goto upgrade_fail;
                snprintf(path, pathlen, "%s/h%c%08x.bin", dir, sizedirs[j], i);
                if (upgrade_bin(lockfd, dir, path, desc.from, desc.to, j, i))
                    goto upgrade_fail;
            }
        }
        snprintf(path, pathlen, "%s", dir);

        if((ret = upgrade_db(lockfd, path, alldb.temp, desc.from, desc.to, desc.upgrade_tempdb)))
            goto upgrade_fail;

        if((ret = upgrade_db(lockfd, path, alldb.event, desc.from, desc.to, desc.upgrade_eventsdb)))
            goto upgrade_fail;

        if((ret = upgrade_db(lockfd, path, alldb.xfer, desc.from, desc.to, desc.upgrade_xfersdb)))
            goto upgrade_fail;

        if (desc.upgrade_alldb(&alldb) || desc.upgrade_alldb(&alldb))
            goto upgrade_fail;
        INFO("Successfully upgraded all DBs");
    }
    INFO("Committing changes");
    if (qcommit_alldb(&alldb))
        goto upgrade_fail;
    gettimeofday(&tv_upgrade_done, NULL);
    INFO("Schema upgrade completed in %.fs", timediff(&tv_integrity_done, &tv_upgrade_done));
    ret = OK;

upgrade_fail:
    free(path);
    if (ret)
        qrollback_alldb(&alldb);
    qclose_alldb(&alldb);
    gettimeofday(&tv_close, NULL);
    if (ret == OK)
        INFO("Storage closed in %.fs", timediff(&tv_upgrade_done, &tv_close));
    if (lockfd != -1)
        close(lockfd);
    return ret;
}

static rc_ty datadb_begin(sx_hashfs_t *h, unsigned int hs);
static rc_ty datadb_commit(sx_hashfs_t *h, unsigned int hs);
static void datadb_rollback(sx_hashfs_t *h, unsigned int hs);
static rc_ty datadb_beginall(sx_hashfs_t *h);
static rc_ty datadb_commitall(sx_hashfs_t *h);
static void datadb_rollbackall(sx_hashfs_t *h);
static rc_ty sx_hashfs_revision_op_internal(sx_hashfs_t *h, unsigned int hs, const sx_hash_t *revision_id, int op, int age);

struct bulk_save {
    int db_min_passive_wal_pages;
    int db_max_passive_wal_pages;
    int db_max_restart_wal_pages;
    int gc_max_batch;
};

static void bulk_start(struct bulk_save *save)
{
    save->db_min_passive_wal_pages = db_min_passive_wal_pages;
    save->db_max_passive_wal_pages = db_max_passive_wal_pages;
    save->db_max_restart_wal_pages = db_max_restart_wal_pages;
    save->gc_max_batch = gc_max_batch;
    db_min_passive_wal_pages = db_max_passive_wal_pages = db_max_restart_wal_pages = 50000;
    gc_max_batch = 1000;
}

static void bulk_done(const struct bulk_save *save)
{
    db_min_passive_wal_pages = save->db_min_passive_wal_pages;
    db_max_passive_wal_pages = save->db_max_passive_wal_pages;
    db_max_restart_wal_pages = save->db_max_restart_wal_pages;
    gc_max_batch = save->gc_max_batch;
}

rc_ty sx_hashfs_upgrade_1_0_prepare(sx_hashfs_t *h)
{
    sxi_db_t *db = NULL;
    sqlite3_stmt *q = NULL;
    rc_ty rc = FAIL_EINTERNAL;
    unsigned i;
    int64_t heal_required =  0;
    int64_t heal_done = 0;
    int64_t total = 0;
    struct timeval tv0, tv1;
    struct bulk_save bulk;

    bulk_start(&bulk);
    gettimeofday(&tv0, NULL);
    sx_hashfs_set_progress_info(h, INPRG_UPGRADE_RUNNING, "Upgrade: preparing");
    for(i=0;i<METADBS;i++) {
        db = h->metadb[i];
        if(qprep(db, &q, "SELECT COUNT(*) FROM files WHERE revision_id IS NULL") ||
           qstep_ret(q))
            break;
        heal_required += sqlite3_column_int64(q, 0);
        qnullify(q);
        if(qprep(db, &q, "SELECT COUNT(*) FROM files") ||
           qstep_ret(q))
            break;
        total += sqlite3_column_int64(q, 0);
        qnullify(q);
    }
    qnullify(q);
    if (i < METADBS)
        return FAIL_EINTERNAL;

    int age = sxi_hdist_version(h->hd);
    for(i=0;i<METADBS;i++) {
        sqlite3_stmt *qsel = h->qm_needs_upgrade[i], *qupd = NULL;
        int ret;
        db = h->metadb[i];
        rc = FAIL_EINTERNAL;
        /* OR FAIL - avoid creation of temp file, it can only affect one row,
         * since fid is a primary key */
        if(qprep(db, &qupd, "UPDATE OR FAIL files SET revision_id=:revision_id WHERE fid=:fid")) {
            qnullify(qupd);
            break;
        }
        do {
            unsigned int k = 0;
            rc = FAIL_EINTERNAL;
            if (qbegin(db) || datadb_beginall(h))
                break;
            sqlite3_reset(qsel);
            while ((ret = qstep(qsel)) == SQLITE_ROW && (k < gc_max_batch)) {
                sx_hash_t revision_id;
                const sx_hashfs_volume_t *volume;
                unsigned int bsize;

                int64_t fid = sqlite3_column_int64(qsel, 0);
                int64_t volid = sqlite3_column_int64(qsel, 1);
                const unsigned char *name = sqlite3_column_text(qsel, 2);
                const unsigned char *rev = sqlite3_column_text(qsel, 3);
                int64_t size = sqlite3_column_int64(qsel, 4);
                unsigned blocks = size_to_blocks(size, NULL, &bsize);
                ret = -1;
                if ((rc = sx_hashfs_volume_by_id(h, volid, &volume))) {
                    WARN("volume_by_id failed");
                    break;
                }
                if (sx_unique_fileid(sx_hashfs_client(h), volume, (const char*)name, (const char*)rev, &revision_id)) {
                    WARN("unique_fileid failed");
                    break;
                }
                sqlite3_reset(qupd);
                sqlite3_reset(h->qm_add_heal[i]);
                DEBUGHASH("preparing for upgrade", &revision_id);
                unsigned int hs;
                for(hs = 0; hs < SIZES; hs++)
                    if(bsz[hs] == bsize)
                        break;
                if (hs == SIZES) {
                    WARN("bad blocksize: %d", bsize);
                    break;
                }
                if (qbind_blob(h->qm_add_heal[i], ":revision_id", revision_id.b, sizeof(revision_id.b)) ||
                    qbind_null(h->qm_add_heal[i], ":remote_volid") ||
                    qbind_int(h->qm_add_heal[i], ":blocks", blocks) ||
                    qbind_int(h->qm_add_heal[i], ":blocksize", bsize) ||
                    qbind_int(h->qm_add_heal[i], ":replica_count", volume->max_replica) ||
                    qstep_noret(h->qm_add_heal[i]) ||
                    sx_hashfs_revision_op_internal(h, hs, &revision_id, 1, age) ||
                    qbind_int64(qupd, ":fid", fid) ||
                    qbind_blob(qupd, ":revision_id", revision_id.b, sizeof(revision_id.b)) ||
                    qstep_noret(qupd)) {
                    WARN("rev upgrade failed");
                    break;
                }
                k++;
            }
            sqlite3_reset(qsel);
            sqlite3_reset(qupd);
            sqlite3_reset(h->qm_add_heal[i]);
            if (ret != SQLITE_ROW && ret != SQLITE_DONE) {
                rc = FAIL_EINTERNAL;
                datadb_rollbackall(h);
                break;
            }
            if (ret == SQLITE_DONE) {
              if(qprep(db, &q, "SELECT * FROM files"))
                  break;
              int has_dummy = 0;
              for (unsigned i=0;i<sqlite3_column_count(q);i++)
                  if (!strcmp(sqlite3_column_name(q, i), "dummy")) {
                      has_dummy = 1;
                      break;
                  }
              qnullify(q);
              if (!has_dummy) {
                  if (qprep(db, &q, "ALTER TABLE files ADD dummy CHECK(revision_id IS NOT NULL)") ||
                      qstep_noret(q))
                      break;
                  qnullify(q);
              }
            }
            if (datadb_commitall(h) || qcommit(db))
                break;
            if (k) {
                char msg[128];
                heal_done += k;
                snprintf(msg, sizeof(msg), "Upgrade - preparing local files: %lld/%lld remaining",
                        (long long)heal_required - heal_done, (long long)total);
                sx_hashfs_set_progress_info(h, INPRG_UPGRADE_RUNNING, msg);
            }
            rc = OK;
        } while(ret == SQLITE_ROW);
        sqlite3_reset(qsel);
        qnullify(qupd);
        if (rc) {
            qrollback(db);
            datadb_rollbackall(h);
            break;
        }
    }
    if (rc == OK)
        sx_hashfs_set_progress_info(h, INPRG_UPGRADE_RUNNING, "Upgrade - local files prepared");

    gettimeofday(&tv1, NULL);
    INFO("Local upgrade prepared in %.fs: %s", timediff(&tv0, &tv1), rc2str(rc));
    bulk_done(&bulk);
    qnullify(q);

    return rc;
}

static rc_ty sx_hashfs_hashop_moduse(sx_hashfs_t *h, const sx_hash_t *reserve_id, const sx_hash_t *revision_id, unsigned int hs, const sx_hash_t *hash, unsigned replica, int64_t op, uint64_t op_expires_at);
static rc_ty sx_hashfs_hashop_moduse_internal(sx_hashfs_t *h, const sx_hash_t *revision_id, unsigned int hs, unsigned int ndb, const sx_hash_t *hash, unsigned replica, unsigned age);
static rc_ty hash_of_blob_result(sx_hash_t *hash, sqlite3_stmt *stmt, int col);

static rc_ty datadb_begin(sx_hashfs_t *h, unsigned int hs)
{
    for(int ndb=0;ndb<HASHDBS;ndb++)
        if (qbegin(h->datadb[hs][ndb])) {
            while (ndb-->0)
                qrollback(h->datadb[hs][ndb]);
            return FAIL_EINTERNAL;
        }
    return OK;
}

static void datadb_rollback(sx_hashfs_t *h, unsigned int hs)
{
    for(int ndb=0;ndb<HASHDBS;ndb++)
        qrollback(h->datadb[hs][ndb]);
}

static rc_ty datadb_commit(sx_hashfs_t *h, unsigned int hs)
{
    for(int ndb=0;ndb<HASHDBS;ndb++)
        if (qcommit(h->datadb[hs][ndb])) {
            datadb_rollback(h, hs);
            return FAIL_EINTERNAL;
        }
    return OK;
}

static rc_ty datadb_beginall(sx_hashfs_t *h)
{
    for (int i=0;i<SIZES;i++)
        if (datadb_begin(h, i)) {
            while(i-- > 0)
                datadb_rollback(h, i);
            return FAIL_EINTERNAL;
        }
    return OK;
}

static rc_ty datadb_commitall(sx_hashfs_t *h)
{
    for (int i=0;i<SIZES;i++)
        if (datadb_commit(h, i)) {
            while(i-- > 0)
                datadb_rollback(h, i);
            return FAIL_EINTERNAL;
        }
    return OK;
}

static void datadb_rollbackall(sx_hashfs_t *h)
{
    for (int i=0;i<SIZES;i++)
        datadb_rollback(h, i);
}

static rc_ty gc_eventdb(sx_hashfs_t *h)
{
    int64_t n;
    sqlite3_reset(h->qe_gc);
    if (qstep_noret(h->qe_gc))
        return FAIL_EINTERNAL;
    n = sqlite3_changes(h->eventdb->handle);
    if (n > 0)
        INFO("GCed jobs: %lld", (long long)n);
    return OK;
}

rc_ty sx_hashfs_upgrade_1_0_local(sx_hashfs_t *h)
{
    struct timeval tv0, tv1, tv2, tv3;
    sqlite3_stmt *q = NULL;
    rc_ty rc = FAIL_EINTERNAL;
    sxi_db_t *db = NULL;
    unsigned i;
    int64_t heal_required = 0;
    char msg[128];
    struct bulk_save bulk;

    bulk_start(&bulk);
    gettimeofday(&tv0, NULL);

    sx_hashfs_set_progress_info(h, INPRG_UPGRADE_RUNNING, "Upgrade - local file blocks");
    for(i=0;i<METADBS;i++) {
        db = h->metadb[i];
        if(qprep(db, &q, "SELECT SUM(blocks) FROM heal WHERE remote_volume IS NULL") ||
           qstep_ret(q))
            break;
        heal_required += sqlite3_column_int64(q, 0);
        db = h->metadb[i];
        qnullify(q);
    }
    qnullify(q);
    for(i=0;i<METADBS;i++) {
        sqlite3_stmt *qsel = NULL;
        int ret;
        db = h->metadb[i];
        if(qprep(db, &qsel, "SELECT heal.revision_id, blocks, blocksize, content, replica_count, name, rev FROM heal INNER JOIN files ON heal.revision_id=files.revision_id WHERE remote_volume IS NULL"))
            break;
        do {
            int64_t heal_done = 0;
            unsigned k=0;
            rc = FAIL_EINTERNAL;
            snprintf(msg, sizeof(msg), "Upgrade - local file blocks: %lld remaining", (long long)heal_required);
            sx_hashfs_set_progress_info(h, INPRG_UPGRADE_RUNNING, msg);
            if (qbegin(db) || datadb_beginall(h))
                break;
            sqlite3_reset(qsel);
            while ((ret = qstep(qsel)) == SQLITE_ROW && (k++ < gc_max_batch)) {
                unsigned int hs;
                sx_hash_t revision_id;
                int64_t blocks = sqlite3_column_int64(qsel, 1);
                unsigned blocksize = sqlite3_column_int64(qsel, 2);
                const sx_hash_t *content = sqlite3_column_blob(qsel, 3);
                unsigned int replica = sqlite3_column_int64(qsel, 4);
                ret = -1;
                int64_t j;

                if (hash_of_blob_result(&revision_id, qsel, 0))
                    break;
                if (sqlite3_column_bytes(qsel, 3) != blocks*sizeof(*content)) {
                    CRIT("corrupt file blob: %s (%s)", sqlite3_column_text(qsel,5), sqlite3_column_text(qsel, 6));
                    continue;
                }
                for(hs = 0; hs < SIZES; hs++)
            	    if(bsz[hs] == blocksize)
        	        break;
                if (hs == SIZES) {
                    CRIT("corrupt file blocksize: %d", blocksize);
                    continue;
                }
                unsigned int age = sxi_hdist_version(h->hd);
                for (j=0;j<blocks;j++) {
                    const sx_hash_t *hash = &content[j];
                    if (sx_hashfs_hashop_moduse_internal(h, &revision_id, hs, gethashdb(hash), hash, replica, age))
                        break;
                }
                if (j != blocks)
                    break;
                DEBUG("rebuilt revmap for %lld blocks", (long long)blocks);
                if (qbind_blob(h->qm_del_heal[i],":revision_id", revision_id.b, sizeof(revision_id.b)) ||
                    qstep_noret(h->qm_del_heal[i]))
                    break;
                heal_done += blocks;
            }
            sqlite3_reset(qsel);
            if (ret != SQLITE_ROW && ret != SQLITE_DONE) {
                datadb_rollbackall(h);
                break;
            }
            if (datadb_commitall(h) || qcommit(db))
                break;
            rc = OK;
            heal_required -= heal_done;
        } while(ret == SQLITE_ROW);
        qnullify(qsel);
        if (rc)
            qrollback(db);
    }
    bulk_done(&bulk);
    if (rc == OK) {
        sx_hashfs_set_progress_info(h, INPRG_UPGRADE_RUNNING, "Upgrade - local file blocks checkpointing");
        gettimeofday(&tv1, NULL);
        for(i=0;i<METADBS;i++)
            qcheckpoint_idle(h->metadb[i]);
        for(unsigned j=0;j<SIZES;j++)
            for(i=0;i<HASHDBS;i++)
                qcheckpoint_idle(h->datadb[j][i]);
        sx_hashfs_set_progress_info(h, INPRG_UPGRADE_RUNNING, "Upgrade - local file blocks done");
        gettimeofday(&tv2, NULL);
        INFO("Checkpoint finished in %.fs", timediff(&tv1, &tv2));
        const sx_hashfs_volume_t *volume;
        int max_age = sxi_hdist_version(h->hd);
        unsigned n = 0;
        for(rc = sx_hashfs_volume_first(h, &volume, 0);rc == OK;rc = sx_hashfs_volume_next(h)) {
            if (sx_hashfs_is_or_was_my_volume(h, volume))
                continue;/* we've already imported this data from the local volnode */
            for(i=0;i<METADBS;i++) {
                sqlite3_stmt *q = h->qm_add_heal_volume[i];
                sqlite3_reset(q);
                if(qbind_text(q,":name", volume->name) ||
                   qbind_int(q,":max_age", max_age) ||
                   qbind_blob(q,":min_revision_id","",0) ||
                   qstep_noret(q)) {
                    rc = FAIL_EINTERNAL;
                    break;
                }
            }
            if (i != METADBS) {
                rc = FAIL_EINTERNAL;
                break;
            }
            n++;
        }
        if (rc == ITER_NO_MORE) {
            snprintf(msg, sizeof(msg), "Local upgrade completed - %d remote volume names listed", n);
            sx_hashfs_set_progress_info(h, INPRG_UPGRADE_COMPLETE, msg);
            rc = OK;

            INFO("Cleaning events.db");
            if (gc_eventdb(h) ||
                qprep(h->eventdb, &q, "VACUUM") || qstep_noret(q))
                WARN("Clean failed on events.db");/* not critical */
            qnullify(q);
        }
    }
    if (rc)
        return rc;
    gettimeofday(&tv3, NULL);
    INFO("Local upgrade finished in %.fs: %s", timediff(&tv0, &tv3) - timediff(&tv1, &tv2), rc2str(rc));
    return OK;
}

/* Create temporary file inside given path.
 * Return file name or NULL in case of error. File descriptor is set via fd argument. */
static char *create_partfile(sx_hashfs_t *h, const char *destpath, const char *volname, const char *name, int64_t size, int *fd) {
    int ret = -1;
    unsigned int len;
    char *fname = NULL;
    mode_t mask = 0;

    if(!h || !destpath || !volname || !name || !fd) {
        WARN("Failed to create partfile: NULL argument");
        return NULL;
    }

    len = strlen(destpath) + strlen(volname) + 10; /* 6 chars for tempfile, 2 slashes, dot and nulbyte */
    fname = malloc(len);
    if(!fname) {
        WARN("Failed to allocate memory for file name");
        goto create_partfile_err;
    }

    snprintf(fname, len, "%s/.%s/XXXXXX", destpath, volname);

    mask = umask(0);
    umask(077);
    if((*fd = mkstemp(fname)) < 0 || ftruncate(*fd, size)) {
        WARN("Failed to create file %s", fname);
        goto create_partfile_err;
    }

    ret = 0;
create_partfile_err:
    umask(mask);
    if(ret) {
        free(fname);
        return NULL;
    }
    return fname;
}

static void close_partfile(sx_hashfs_t *h, const char *destpath, const char *volname, const char *name, char *partname, int fd, int64_t nhashes, int64_t restored_hashes) {
    char *newname = NULL;

    if(!destpath || !volname || !name) {
        WARN("Failed to close partflie: NULL argument");
        goto close_partfile_err;
    }

    if(!partname)
        goto close_partfile_err;

    /* Rename partfile only if all blocks were restored properly */
    if(nhashes == restored_hashes) {
        unsigned int len = strlen(destpath) + strlen(volname) + strlen(name) + 3;
        char *tmp;

        /* Allocate mem for new file name */
        newname = malloc(len);
        if(!newname) {
            WARN("Failed to allocate memory for file name");
            goto close_partfile_err;
        }

        snprintf(newname, len, "%s/%s/%s", destpath, volname, name);

        /* Make needed directories */
        if((tmp = strrchr(newname, '/'))) {
            *tmp = '\0';
            if(access(newname, R_OK) && sxi_mkdir_hier(h->sx, newname, 0700)) {
                WARN("Failed to create directory %s for file %s: Move partial file %s manually", newname, name, partname);
                goto close_partfile_err;
            }
            *tmp = '/';
        }

        if(rename(partname, newname)) {
            WARN("Failed to rename partial file %s to %s: Move it manually", partname, newname);
            goto close_partfile_err;
        }
    }

close_partfile_err:
    if(fd > 0)
        close(fd);
    if(partname && volname && name) {
        /* If no hashes were restored, remove partfile */
        if(restored_hashes == 0 && nhashes != 0)
            unlink(partname);

        if(restored_hashes != nhashes && restored_hashes > 0)
            WARN("File %s/%s was restored to partial file %s: saved %lld of %lld blocks", volname, name, partname, (long long)restored_hashes, (long long)nhashes);
    }
    free(newname);
    free(partname);
}

static int extract_file(sx_hashfs_t *h, const char *destpath, const char *volname, const char *name, int64_t size, const sx_hash_t *hashes, int64_t nhashes, int64_t *restored_hashes) {
    int ret = -1;
    sqlite3_stmt *q = NULL;
    unsigned int blocksize = 0, hs = 0;
    int64_t i;
    int fd = -1;
    char *fname = NULL;
    sx_hash_t zerohash;

    if(!destpath || !volname || !name || !restored_hashes || (nhashes > 0 && !hashes)) {
        WARN("Failed to extract file: bad argument");
        return -1;
    }

    /* Get block size for file */
    size_to_blocks(size, NULL, &blocksize);

    if(!blocksize) {
        WARN("Failed to compute block size for file %s", name);
        goto extract_file_err;
    }

    /* Get hash size index */
    for(hs = 0; hs < SIZES; hs++) {
        if(blocksize == bsz[hs])
            break;
    }

    if(hs == SIZES) {
        WARN("Failed to get hash size database, blocksize: %u", blocksize);
        goto extract_file_err;
    }

    memset(h->blockbuf, 0, blocksize);
    /* Calculate zerohash */
    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), h->blockbuf, blocksize, &zerohash))
        goto extract_file_err;

    /* Create part file */
    if(!(fname = create_partfile(h, destpath, volname, name, size, &fd)) || fd < 0) {
        WARN("Failed to create part file");
        goto extract_file_err;
    }

    /* Iterate over all hashes */
    for(i = 0LL; i < nhashes; i++) {
        int64_t hoff = i * blocksize;
        const sx_hash_t *hash = hashes + i;
        unsigned int hdb;
        int r;
        int64_t to_write;
        char hex[SXI_SHA1_TEXT_LEN+1];

        if(!memcmp(zerohash.b, hash->b, sizeof(sx_hash_t))) {
            /* This hash correspond to block filled with zeroes, skip it */
            if(restored_hashes)
                (*restored_hashes)++;
            continue;
        }

        /* Used for printing hash, can be remove if not needed */
        bin2hex(hash->b, sizeof(sx_hash_t), hex, SXI_SHA1_TEXT_LEN+1);

        hdb = gethashdb(hash);
        q = h->qb_get[hs][hdb];

        sqlite3_reset(q);
        if(qbind_blob(q, ":hash", hash->b, sizeof(sx_hash_t))) {
            WARN("Failed to bind hash to query");
            goto extract_file_err;
        }

        /* Check whether we want to write whole block or its file last block */
        if(hoff + blocksize < size)
            to_write = blocksize;
        else
            to_write = size - hoff;

        /* Get offset */
        r = qstep(q);
        if(r == SQLITE_ROW) {
            /* Hash was found in database, now get its offset */
            int64_t offset = sqlite3_column_int64(q, 0);
            offset *= blocksize;
            if(read_block(h->datafd[hs][hdb], h->blockbuf, offset, to_write)) /* Block was not found in storage, but continue extraction */
                continue;

            if(lseek(fd, hoff, SEEK_SET) != hoff) {
                WARN("Failed to seek file to %lld", (long long)hoff);
                goto extract_file_err;
            }

            if(write_block(fd, h->blockbuf, hoff, to_write)) {
                WARN("Failed to write block to part file");
                goto extract_file_err;
            }
            if(restored_hashes)
                (*restored_hashes)++;
        } else if(r != SQLITE_DONE) {
            WARN("Failed to query database for block offset");
            goto extract_file_err;
        }
    }

    ret = 0;
extract_file_err:
    sqlite3_reset(q);
    close_partfile(h, destpath, volname, name, fname, fd, nhashes, *restored_hashes);
    return ret;
}

static int extract_volume_files(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, const char *destpath, int64_t *restored, int64_t *nfiles) {
    int ret = -1, r, i;
    sqlite3_stmt *list[METADBS];

    if(!vol || !destpath || !restored || !nfiles) {
        NULLARG();
        return ret;
    }

    memset(list, 0, sizeof(list));

    for(i = 0; i < METADBS; i++) {
        if(qprep(h->metadb[i], &list[i], "SELECT name, size, content FROM files WHERE volume_id = :volid")
           || qbind_int64(list[i], ":volid", vol->id)) {
            WARN("Failed to prepare files list query for volume %s", vol->name);
            goto extract_volume_files_err;
        }
    }

    /* Iterate over meta databases
     * This loop is not going to be broken when error occurs - this is a backup routine */
    ret = 0; /* Start counting errors during extraction */
    *restored = 0;
    *nfiles = 0;
    for(i = 0; i < METADBS; i++) {
        while((r = qstep(list[i])) == SQLITE_ROW) {
            const char *name = (const char*)sqlite3_column_text(list[i], 0);
            int64_t size = sqlite3_column_int64(list[i], 1);
            const sx_hash_t *hashes = (const sx_hash_t*)sqlite3_column_blob(list[i], 2);
            int64_t nhashes = sqlite3_column_bytes(list[i], 2);
            int64_t restored_hashes = 0;

            if(nhashes % sizeof(sx_hash_t)) {
                WARN("Bad list of hashes for file: %s", name);
                continue;
            }
            nhashes /= sizeof(sx_hash_t);

            if(extract_file(h, destpath, vol->name, name, size, hashes, nhashes, &restored_hashes) < 0) {
                ret++;
                WARN("Failed to extract file %s/%s", vol->name, name);
            } else
                (*restored)++;
            (*nfiles)++;
        }

        if(r != SQLITE_DONE) {
            ret++;
            WARN("Failed to list files from meta database %d / %d for volume %s", i, METADBS, vol->name);
        }
    }

extract_volume_files_err:
    for(i = 0; i < METADBS; i++)
        sqlite3_finalize(list[i]);

    return ret;
}

static rc_ty volume_next_common(sx_hashfs_t *h);

int sx_hashfs_extract(sx_hashfs_t *h, const char *destpath) {
    int ret = -1, s, r, i;
    sqlite3_stmt *locks[METADBS+1], *unlocks[METADBS+1];
    const sx_hashfs_volume_t *vol = &h->curvol;

    if(!destpath || !*destpath) {
        WARN("Failed to extract files: Bad output path");
        return -1;
    }

    memset(locks, 0, sizeof(locks));
    memset(unlocks, 0, sizeof(unlocks));

    /* Lock meta databases */
    for(i = 0; i < METADBS; i++) {
        if(qprep(h->metadb[i], &locks[i], "BEGIN EXCLUSIVE TRANSACTION") || qprep(h->metadb[i], &unlocks[i], "ROLLBACK") || qstep_noret(locks[i])) {
            WARN("Failed to lock meta database at index %d", i);
            goto sx_hashfs_extract_err;
        }
    }

    /* Lock hahsfs database */
    if(qprep(h->db, &locks[METADBS], "BEGIN EXCLUSIVE TRANSACTION") || qprep(h->db, &unlocks[METADBS], "ROLLBACK") || qstep_noret(locks[METADBS])) {
        WARN("Failed to lock volumes database");
        goto sx_hashfs_extract_err;
    }

    h->curvol.name[0] = '\0';
    h->curvoluser = 0;

    /* Iterate over all volumes */
    for(s = volume_next_common(h); s == OK; s = volume_next_common(h)) {
        int64_t restored = 0, nfiles = 0;
        unsigned int len;
        char *partdir = NULL;

        if(!vol) {
            WARN("Failed to iterate volumes: bad volume reference");
            goto sx_hashfs_extract_err;
        }

        /* Part files directory should be empty now, clean up now */
        len = strlen(destpath) + strlen(vol->name) + 3;
        partdir = malloc(len);
        if(!partdir) {
            WARN("Failed to allocate memory");
            goto sx_hashfs_extract_err;
        }
        snprintf(partdir, len, "%s/.%s", destpath, vol->name);

        /* Make needed directories */
        if(access(partdir, R_OK) && sxi_mkdir_hier(h->sx, partdir, 0700)) {
            WARN("Failed to create directory %s for volume %s files", partdir, vol->name);
            free(partdir);
            goto sx_hashfs_extract_err;
        }

        if((r = extract_volume_files(h, vol, destpath, &restored, &nfiles)) < 0) {
            WARN("Failed to restore files for volume %s", vol->name);
        } else {
            if(r > 0) {
                WARN("Encountered %d error(s) during volume %s files extraction, restored %lld of %lld files ", r, vol->name,
                    (long long)restored, (long long)nfiles);
            } else if(nfiles > 0) {
                INFO("All %lld files for volume %s were extracted successfully", (long long)nfiles, vol->name);
            }
        }

        if(rmdir(partdir) && errno != EEXIST && errno != ENOTEMPTY)
            WARN("Failed to remove part files directory: %s", partdir);

        free(partdir);
    }

    if(s != ITER_NO_MORE) {
        WARN("Failed to iterate volumes");
        goto sx_hashfs_extract_err;
    }

    ret = 0;

sx_hashfs_extract_err:
    /* Unlock all databases */
    for(i = METADBS; i >= 0; i--) {
        if(unlocks[i] && qstep_noret(unlocks[i]))
	    WARN("Failed to unlock database");
        sqlite3_finalize(locks[i]);
        sqlite3_finalize(unlocks[i]);
    }

    return ret;
}

const char *sx_hashfs_version(sx_hashfs_t *h) {
    return h->version;
}

const sx_uuid_t *sx_hashfs_uuid(sx_hashfs_t *h) {
    return &h->cluster_uuid;
}

int sx_hashfs_is_rebalancing(sx_hashfs_t *h) {
    return h ? h->is_rebalancing : 0;
}

int sx_hashfs_is_orphan(sx_hashfs_t *h) {
    return h ? h->is_orphan : 0;
}

/* MODHDIST: this was forked off into sx_hashfs_hdist_change_add
 * it should be simplified to only handle local activation (sxadm cluster --new) */
rc_ty sx_hashfs_modhdist(sx_hashfs_t *h, const sx_nodelist_t *list) {
    sxi_hdist_t *newmod = NULL;
    unsigned int nnodes, i, blob_size;
    sqlite3_stmt *q = NULL;
    const void *blob;
    rc_ty ret = OK;

    if(!h || !list) {
	NULLARG();
	return EINVAL;
    }

    nnodes = sx_nodelist_count(list);
    if(nnodes < 1) {
	msg_set_reason("Called with empty distribution list");
	return EINVAL;
    }

    if(h->have_hd == 0) {
	newmod = sxi_hdist_new(HDIST_SEED, 2, NULL);
    } else if(!h->is_rebalancing) {
	ret = sxi_hdist_get_cfg(h->hd, &blob, &blob_size);
	if(ret == OK) {
	    newmod = sxi_hdist_from_cfg(blob, blob_size);
	    if(newmod)
		ret = sxi_hdist_newbuild(newmod);
	}
    } else
	ret = EEXIST;

    if(ret == OK && !newmod) /* Either _new() or _from_cfg() failed above */
	ret = ENOMEM;

    if(ret != OK) {
	msg_set_reason("Failed to prepare the distribution update");
	sxi_hdist_free(newmod);
	return ret;
    }

    for(i=0; i<nnodes; i++) {
	const sx_node_t *n = sx_nodelist_get(list, i);
	if(sx_node_capacity(n) < SXLIMIT_MIN_NODE_SIZE) {
	    sxi_hdist_free(newmod);
	    msg_set_reason("Invalid capacity: Node %s cannot be smaller than %u bytes", sx_node_uuid_str(n), SXLIMIT_MIN_NODE_SIZE);
	    return EINVAL;
	}
	ret = sxi_hdist_addnode(newmod, sx_node_uuid(n), sx_node_addr(n), sx_node_internal_addr(n), sx_node_capacity(n), NULL);
	if(ret == OK)
	    continue;
	msg_set_reason("Failed to add the distribution node");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    ret = sxi_hdist_build(newmod);
    if(ret) {
	msg_set_reason("Failed to update the distribution model");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    ret = sxi_hdist_get_cfg(newmod, &blob, &blob_size);
    if(ret) {
	msg_set_reason("Failed to update the distribution model");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)") ||
       qbind_text(q, ":k", "dist") ||
       qbind_blob(q, ":v", blob, blob_size) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save the updated distribution model");
	ret = FAIL_EINTERNAL;
    } else {
	sqlite3_reset(q);
	if(qbind_text(q, ":k", "dist_rev") ||
	   qbind_int64(q, ":v", sxi_hdist_version(newmod)) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save the updated distribution model");
	    ret = FAIL_EINTERNAL;
	}
    }
    qnullify(q);
    if(ret)
	return ret;

    if(h->have_hd)
	sxi_hdist_free(h->hd);
    h->hd = newmod;
    h->have_hd = 1;
    return OK;
}


const sx_nodelist_t *sx_hashfs_all_nodes(sx_hashfs_t *h, sx_hashfs_nl_t which) {
    if(!h)
	return NULL;
    switch(which) {
    case NL_PREV:
	return h->prev_dist;
    case NL_NEXT:
	return h->next_dist;
    case NL_PREVNEXT:
	return h->prevnext_dist;
    case NL_NEXTPREV:
	return h->nextprev_dist;
    default:
	return NULL;
    }
}

const sx_nodelist_t *sx_hashfs_effective_nodes(sx_hashfs_t *h, sx_hashfs_nl_t which) {
    if(!h)
	return NULL;
    switch(which) {
    case NL_PREV:
	return h->effprev_dist;
    case NL_NEXT:
	return h->effnext_dist;
    case NL_PREVNEXT:
	return h->effprevnext_dist;
    case NL_NEXTPREV:
	return h->effnextprev_dist;
    default:
	return NULL;
    }
}


/* always builds the table with required_replica nodes
 * ignored nodes are placed at the end of the array
 * returns the number of non ignored nodes in the array */
static int hash_nidx_tobuf(sx_hashfs_t *h, const sx_hash_t *hash, unsigned int required_replica, unsigned int max_volume_replica, unsigned int *nidx) {
    const sx_nodelist_t *nodes;
    sx_nodelist_t *belongsto;
    unsigned int i, start, nnodes;
    int ret;

    if(!h || !hash) {
	NULLARG();
	return -1;
    }

    if(!h->have_hd) {
	BADSTATE("Called before initialization");
	return -1;
    }

    nodes = sx_hashfs_all_nodes(h, NL_NEXT);
    nnodes = sx_nodelist_count(nodes);

    if(max_volume_replica < 1 || max_volume_replica > nnodes ||
       required_replica < 1 || required_replica > max_volume_replica ) {
	msg_set_reason("Bad replica count: %d must be between %d and %d", max_volume_replica, 1, nnodes);
	return -1;
    }

    /* MODHDIST: using _next set - see rant under are_blocks_available() */
    belongsto = sxi_hdist_locate(h->hd, MurmurHash64(hash, sizeof(*hash), HDIST_SEED), max_volume_replica, 0);
    if(!belongsto) {
	WARN("Cannot get nodes for volume");
	return -1;
    }

    start = 0;
    for(i=0; i<max_volume_replica && start < required_replica; i++) {
	const sx_node_t *node = sx_nodelist_get(belongsto, i);
	const sx_uuid_t *uuid = sx_node_uuid(node);
	unsigned int nodeidx;

	if(!sx_nodelist_lookup_index(nodes, uuid, &nodeidx)) {
	    CRIT("node id %s from hdist is unknown to us", uuid->string);
	    sx_nodelist_delete(belongsto);
	    return -1;
	}

	if(!sx_nodelist_lookup(h->ignored_nodes, uuid))
	    nidx[start++] = nodeidx;
    }
    ret = start;

    for(i=0; i<max_volume_replica && start < required_replica; i++) {
	const sx_node_t *node = sx_nodelist_get(belongsto, i);
	const sx_uuid_t *uuid = sx_node_uuid(node);
	unsigned int nodeidx;

	if(!sx_nodelist_lookup_index(nodes, uuid, &nodeidx)) {
	    CRIT("node id %s from hdist is unknown to us", uuid->string);
	    sx_nodelist_delete(belongsto);
	    return -1;
	}

	if(sx_nodelist_lookup(h->ignored_nodes, uuid))
	    nidx[start++] = nodeidx;
    }

    sx_nodelist_delete(belongsto);

    return ret;
}

int sx_hashfs_is_node_volume_owner(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_node_t *n, const sx_hashfs_volume_t *vol) {
    sx_nodelist_t *volnodes;
    sx_hash_t hash;
    int ret = 0;

    if(!h || !vol || !h->have_hd || !n)
        return 0;

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), vol->name, strlen(vol->name), &hash)) {
        WARN("hashing volume name failed");
        return 0;
    }

    volnodes = sx_hashfs_all_hashnodes(h, which, &hash, vol->max_replica);
    if(volnodes) {
        if(sx_nodelist_lookup(volnodes, sx_node_uuid(n)))
            ret = 1;
        sx_nodelist_delete(volnodes);
    }

    return ret;
}

/* Return 1 only if node is not a volnode on PREV but is on NEXT */
static int is_new_volnode(sx_hashfs_t *h, const sx_hashfs_volume_t *vol) {
    if(!h || !vol) {
        NULLARG();
        return -1;
    }

    if(!h->have_hd || !sx_hashfs_is_rebalancing(h))
        return 0;

    if(!sx_hashfs_is_node_volume_owner(h, NL_PREV, sx_hashfs_self(h), vol) && sx_hashfs_is_node_volume_owner(h, NL_NEXT, sx_hashfs_self(h), vol))
        return 1;

    return 0;
}

int sx_hashfs_is_or_was_my_volume(sx_hashfs_t *h, const sx_hashfs_volume_t *vol) {
    return sx_hashfs_is_node_volume_owner(h, NL_NEXTPREV, sx_hashfs_self(h), vol);
}

int sx_hashfs_is_node_faulty(sx_hashfs_t *h, const sx_uuid_t *node_uuid) {
    return sx_nodelist_lookup(h->faulty_nodes, node_uuid) != NULL;
}

int sx_hashfs_is_node_ignored(sx_hashfs_t *h, const sx_uuid_t *node_uuid) {
    return sx_nodelist_lookup(h->ignored_nodes, node_uuid) != NULL;
}

rc_ty sx_hashfs_revision_first(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *name, const sx_hashfs_file_t **file, int reversed) {
    sqlite3_stmt *q;
    if(!volume || !file) {
	NULLARG();
	return EINVAL;
    }

    if(!sx_hashfs_is_or_was_my_volume(h, volume)) {
	msg_set_reason("Wrong node for volume '%s': ...", volume->name);
	return ENOENT;
    }

    if(check_file_name(name)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    h->rev_ndb = getmetadb(name);
    if(h->rev_ndb < 0)
	return FAIL_EINTERNAL;

    q = (reversed ? h->qm_listrevs_rev[h->rev_ndb] : h->qm_listrevs[h->rev_ndb]);
    sqlite3_reset(q);

    if(qbind_int64(q, ":volume", volume->id) ||
       qbind_text(q, ":name", name))
	return FAIL_EINTERNAL;

    sxi_strlcpy(h->list_file.name, name, sizeof(h->list_file.name));
    h->list_file.revision[0] = '\0';
    h->list_volid = volume->id;
    *file = &h->list_file;

    return sx_hashfs_revision_next(h, reversed);
}


rc_ty sx_hashfs_revision_next(sx_hashfs_t *h, int reversed) {
    sqlite3_stmt *q = (reversed ? h->qm_listrevs_rev[h->rev_ndb] : h->qm_listrevs[h->rev_ndb]);
    const char *revision;
    int r;

    sqlite3_reset(q);
    if(qbind_int64(q, ":volume", h->list_volid) ||
       qbind_text(q, ":name", h->list_file.name))
        return FAIL_EINTERNAL;

    if(reversed && h->list_file.revision[0] == '\0') {
        if(qbind_null(q, ":previous"))
	    return FAIL_EINTERNAL;
    } else if(qbind_text(q, ":previous", h->list_file.revision))
        return FAIL_EINTERNAL;

    r = qstep(q);
    if(r == SQLITE_DONE) {
	sqlite3_reset(q);
	return h->list_file.revision[0] ? ITER_NO_MORE : ENOENT;
    }
    if(r != SQLITE_ROW) {
	sqlite3_reset(q);
	return FAIL_EINTERNAL;
    }

    h->list_file.file_size = sqlite3_column_int64(q, 0);
    revision = (const char *)sqlite3_column_text(q, 1);
    if(parse_revision(revision, &h->list_file.created_at)) {
	WARN("Found bad revision %s", revision ? revision : "(NULL)");
	sqlite3_reset(q);
	return FAIL_EINTERNAL;
    }
    sxi_strlcpy(h->list_file.revision, revision, sizeof(h->list_file.revision));
    size_to_blocks(h->list_file.file_size, NULL, &h->list_file.block_size);

    sqlite3_reset(q);

    return OK;
}

rc_ty sx_hashfs_list_etag(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *pattern, int8_t recurse, sx_hash_t *etag)
{
    sxi_md_ctx *hash_ctx = sxi_md_init();
    rc_ty rc = OK;
    unsigned i;
    char *vol_newest;
    int64_t total = 0;

    if (!h || !volume || !pattern || !etag) {
        NULLARG();
        return EFAULT;
    }
    if (!(vol_newest = wrap_strdup("")))
        rc = ENOMEM;
    for (i=0;i<METADBS && !rc;i++) {
        sqlite3_reset(h->qm_newest[i]);
        sqlite3_reset(h->qm_count[i]);
        if (qbind_int(h->qm_newest[i], ":volid", volume->id) ||
            qbind_int(h->qm_count[i], ":volid", volume->id) ||
            qstep_ret(h->qm_newest[i]) ||
            qstep_ret(h->qm_count[i])) {
            rc = FAIL_EINTERNAL;
        } else {
            /* detects newly created or updated files */
            const char *newest = (const char*)sqlite3_column_text(h->qm_newest[i], 0);
            /* detects deleted files */
            total += sqlite3_column_int64(h->qm_count[i], 0);
            if (newest) {
                if (strcmp(newest, vol_newest) > 0) {
                    free(vol_newest);
                    if (!(vol_newest = wrap_strdup(newest)))
                        rc = ENOMEM;
                }
            }
        }
        sqlite3_reset(h->qm_newest[i]);
        sqlite3_reset(h->qm_count[i]);
    }
    if (!hash_ctx) {
        WARN("failed to initialize etag hash");
        rc = FAIL_EINTERNAL;
    }
    if (rc == OK) {
        /* must be same on all volnodes */
        DEBUG("%d: newest: %s, total: %lld", i, vol_newest, (long long)total);
        if (!sxi_sha1_init(hash_ctx) ||
            !sxi_sha1_update(hash_ctx, h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary)) ||
            !sxi_sha1_update(hash_ctx, pattern, strlen(pattern)+1) ||
            !sxi_sha1_update(hash_ctx, &recurse, sizeof(recurse)) ||
            !sxi_sha1_update(hash_ctx, vol_newest, strlen(vol_newest)+1) ||
            !sxi_sha1_update(hash_ctx, &total, sizeof(total)) ||
            !sxi_sha1_final(hash_ctx, etag->b, NULL)) {
            WARN("failed to calculate etag hash");
            rc = FAIL_EINTERNAL;
        }
    }
    free(vol_newest);
    sxi_md_cleanup(&hash_ctx);
    return rc;
}

static unsigned int slashes_in(const char *s) {
    unsigned int l = strlen(s), found = 0;
    const char *sl;
    while(l && (sl = memchr(s, '/', l))) {
        found++;
        sl++;
        l -= sl -s;
        s = sl;
    }
    return found;
}

static const char *ith_slash(const char *s, unsigned int i) {
    unsigned found = 0;
    while ((s = strchr(s, '/'))) {
        found++;
        if (found == i)
            return s;
        s++;
    }
    return NULL;
}

/* Compare paths */
static int pcmp(sx_hashfs_t *h, const char *p1, const char *p2, unsigned int maxlen) {
    int r = 0;

    if(!p1 || !p2) {
        WARN("Null argument");
        return 0;
    }

    if(!h->list_recurse) {
        const char *s1 = ith_slash(p1, h->list_pattern_slashes + 1);
        const char *s2 = ith_slash(p2, h->list_pattern_slashes + 1);
        if(!s1 || !s2 || s1 - p1 != s2 - p2)
            r = strncmp(p1, p2, maxlen);
        else
            r = strncmp(p1, p2, MIN(s1 - p1, maxlen)); /* Slash in the same place*/
    } else
        r = strncmp(p1, p2, maxlen);

    return r;
}

static rc_ty lookup_file_name(sx_hashfs_t *h, int db_idx, int update_cache) {
    int r;
    rc_ty ret = FAIL_EINTERNAL;
    const char *n, *revision;
    sqlite3_stmt *stmt;
    list_entry_t *e = &h->list_cache[db_idx];
    const char *q = NULL;

    if(!h->list_recurse && (q = ith_slash(e->name, h->list_pattern_slashes + 1))) {
        /* We are not searching recursively and next slash was found in pervious name,
         * we can skip all files that are prefixed by that dir. To achieve that we can simply move
         * starting name to the next one, but we will have to also be careful and use >= for name matching
         */
        e->name[q - e->name]++;
        e->name[q - e->name + 1] = '\0';
    }

    /* Use statement with > or >= regarding to previous q assignment (or if using h->list_lower_limit) */
    if(q || !update_cache)
        stmt = h->qm_list_eq[db_idx];
    else
        stmt = h->qm_list[db_idx];
    sqlite3_reset(stmt);
    if(qbind_int64(stmt, ":volume", h->list_volid) ||
       qbind_text(stmt, ":previous", update_cache ? e->name : h->list_lower_limit) ||
       qbind_text(stmt, ":pattern", h->list_pattern) ||
       qbind_int(stmt, ":pattern_slashes", h->list_pattern_slashes) ||
       qbind_int(stmt, ":slash_ending", h->list_pattern_end_with_slash)) {
        WARN("Failed to bind list query values");
        goto lookup_file_name_err;
    }

    if(h->list_limit_len) {
        if(qbind_text(stmt, ":limit", h->list_upper_limit)) {
            WARN("Failed to bind upper limit");
            goto lookup_file_name_err;
        }
    } else {
        if(qbind_null(stmt, ":limit")) {
            WARN("Failed to bind upper limit (null)");
            goto lookup_file_name_err;
        }
    }

    r = qstep(stmt);
    h->qm_list_queries++;
    if(r == SQLITE_DONE) {
        e->name[0] = '\0';
        /* Done for this db, no more reading needed */
        ret = OK;
        goto lookup_file_name_err;
    }

    if(r != SQLITE_ROW)
        goto lookup_file_name_err;

    n = (const char *)sqlite3_column_text(stmt, 0);
    if(!n) {
        WARN("Cannot list NULL filename on meta database %u", db_idx);
        goto lookup_file_name_err;
    }

    e->file_size = sqlite3_column_int64(stmt, 1);
    e->nblocks = size_to_blocks(e->file_size, NULL, &e->block_size);

    revision = (const char *)sqlite3_column_text(stmt, 2);
    if(!revision || parse_revision(revision, &e->created_at)) {
        WARN("Bad revision found on file %s, volid %lld", h->list_file.name, (long long)h->list_volid);
        goto lookup_file_name_err;
    } else {
        strncpy(e->revision, revision, sizeof(e->revision));
        e->revision[sizeof(e->revision)-1] = '\0';
    }

    strncpy(e->name, n, sizeof(e->name));
    e->name[sizeof(e->name)-1] = '\0';

    ret = OK;
    lookup_file_name_err:
    /* Always reset statement to avoid locking server */
    sqlite3_reset(stmt);

    return ret;
}

static int parse_pattern(sx_hashfs_t *h, const char *pattern) {
    int plen;
    unsigned int l, r;

    /* If pattern is empty, make it a single slash */
    if(!pattern)
        pattern = "/";

    while(pattern[0] == '/')
        pattern++;

    if (!*pattern)
        pattern = "/";

    /* Pattern length is at least 1, so this call is OK */
    plen = check_file_name(pattern);
    if(plen < 0) {
        WARN("Could not get pattern length");
        return 1;
    }

    /* Reset globbing character position */
    h->list_limit_len = -1;

    /* Clone pattern, plen is up to SXLIMIT_MAX_FILE_NAME_LEN */
    memcpy(h->list_pattern, pattern, plen);
    h->list_pattern[plen] = '\0';

    /* Check if first character is a globbing one. */
    if(strchr("*?[\\", h->list_pattern[0]))
        h->list_limit_len = 0;

    /*
     * Iterate over pattern and remove multiplied slashes from that
     * l points to chars in new pattern and r points to char in old one
     */
    for(l = 0, r = 1; r < (unsigned int)plen; r++) {
        if(h->list_pattern[l] != '/' || h->list_pattern[r] != '/') {
            l++;
            if(l!=r)
                h->list_pattern[l] = h->list_pattern[r];
        }
        /* Check if character is a globbing one and then set first globbing char position if not set yet */
        if(h->list_limit_len == -1 && strchr("*?[\\", h->list_pattern[l]))
            h->list_limit_len = l;
    }

    /* Update pattern length */
    plen = l + 1;
    h->list_pattern[plen] = '\0';

    /* Check if pattern ends with slash, then we want to list fake dir contents, append asterisk at the end */
    if (h->list_pattern[plen-1] == '/') {
        h->list_pattern_end_with_slash = 1;
        if (plen > 1) {
            memcpy(&h->list_pattern[plen], "*", 2);
            /* Set position of first globbin character */
            if(h->list_limit_len == -1)
                h->list_limit_len = plen;
        } else {
            /* Pattern is only one character, so it is a slash, it should become just asterisk */
            memcpy(h->list_pattern, "*", 2);
            /* Set position of first globbing character */
            if(h->list_limit_len == -1)
                h->list_limit_len = 0;
        }
    } else {
        h->list_pattern_end_with_slash = 0;
        if(h->list_limit_len == -1) /* Exact file name match */
            h->list_limit_len = plen;
    }

    /* Check number of slashes stored in pattern */
    h->list_pattern_slashes = slashes_in(h->list_pattern);

    return 0;
}

rc_ty sx_hashfs_list_first(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *pattern, const sx_hashfs_file_t **file, int recurse, const char *after) {
    unsigned int l = 0;

    if(!h || !volume) {
        NULLARG();
        return EINVAL;
    }

    if(!sx_hashfs_is_or_was_my_volume(h, volume)) {
        /* TODO: got, expected: */
        msg_set_reason("Wrong node for volume '%s': ...", volume->name);
        return ENOENT;
    }

    /* Check given search pattern for globbing characters */
    if(parse_pattern(h, pattern)) {
        WARN("Failed to parse listing pattern");
        return EINVAL;
    }

    /* If 'after' parameter is bigger than pattern prefix, then there is no need to query database */
    if(after && strncmp(h->list_pattern, after, h->list_limit_len) < 0)
        return ITER_NO_MORE;

    /* Store number of slashes in listing pattern, used for comparing file names modulo fake directory name */
    h->list_recurse = recurse;
    h->list_volid = volume->id;
    if(file)
        *file = &h->list_file;

    /* If after given, try to start from later position if after is greater or equal to truncated search pattern */
    if (after && strncmp(h->list_pattern, after, h->list_limit_len) >= 0)
        sxi_strlcpy(h->list_lower_limit, after, strlen(after)+1);
    else /* Copy pattern to list_itername and list_itername_limit variables to limit search boundaries */
        sxi_strlcpy(h->list_lower_limit, h->list_pattern, h->list_limit_len+1);

    sxi_strlcpy(h->list_upper_limit, h->list_pattern, h->list_limit_len+1);

    /* Increment upper limit that file names */
    if(h->list_limit_len > 0)
        h->list_upper_limit[h->list_limit_len-1]++;

    /* For debugging */
    h->qm_list_queries = 0;

    /* Store first file names in cache */
    for(l = 0; l < METADBS; l++) {
        h->list_cache[l].name[0] = '\0';
        if(lookup_file_name(h, l, 0) != OK) {
            WARN("Failed fetching file name from db %d", l);
            return FAIL_EINTERNAL;
        }
    }

    return sx_hashfs_list_next(h);
}

rc_ty sx_hashfs_list_next(sx_hashfs_t *h) {
    int i, min_idx = -1;
    const char *q = NULL;

    if(!h || !*h->list_pattern)
        return EINVAL;

    for(i=0; i < METADBS; i++) {
        int comp;
        if(h->list_cache[i].name[0] == '\0')
            continue; /* No more file names found in that db */

        if(min_idx == -1)
            min_idx = i;
        else {
            /* In case this db has stored file name that is equal (modulo directory name) to current name, lookup next one */
            if(!(comp = pcmp(h, h->list_cache[i].name, h->list_cache[min_idx].name, sizeof(h->list_cache[i].name)))) {
                if(lookup_file_name(h, i, 1)) {
                    WARN("Could not lookup file name");
                    return FAIL_EINTERNAL;
                }
                /* No more in that db */
                if(h->list_cache[i].name[0] == '\0')
                    continue;

                comp = pcmp(h, h->list_cache[i].name, h->list_cache[min_idx].name, sizeof(h->list_cache[i].name));
            }

            if(comp < 0) /* Found smaller file name */
                min_idx = i;
        }
    }

    if(min_idx < 0) {
        DEBUG("Queried %lld times", (long long)h->qm_list_queries);
        return ITER_NO_MORE;
    }

    h->list_file.file_size = h->list_cache[min_idx].file_size;
    h->list_file.nblocks = h->list_cache[min_idx].nblocks;
    h->list_file.block_size = h->list_cache[min_idx].block_size;

    strncpy(h->list_file.revision, h->list_cache[min_idx].revision, sizeof(h->list_file.revision));
    h->list_file.revision[sizeof(h->list_file.revision)-1] = '\0';
    h->list_file.created_at = h->list_cache[min_idx].created_at;

    h->list_file.name[0] = '/';
    /* Truncate dir file name */
    if(!h->list_recurse && (q = ith_slash(h->list_cache[min_idx].name, h->list_pattern_slashes + 1))) {
        /* Truncate file name */
        strncpy(h->list_file.name + 1, h->list_cache[min_idx].name, q - h->list_cache[min_idx].name + 1);
        h->list_file.name[q - h->list_cache[min_idx].name + 2] = '\0';
        /* This is a fake dir, all unrelated items are zeroed */
        h->list_file.file_size = 0;
        h->list_file.block_size = 0;
        h->list_file.nblocks = 0;
        h->list_file.revision[0] = '\0';
        /*
          h->list_file.created_at = 0;

          The created_at value here is not the fakedir mtime, but rather the mtime of
          some random file inside it.
          This value is not reported back to the client because it is incorrect
          (the correct dir mtime would be the max(mtime) of all the files inside the fakedir and all its children).
          This value is only used internally to adjust the Last-Modified header.
        */
    } else {
        strncpy(h->list_file.name + 1, h->list_cache[min_idx].name, sizeof(h->list_file.name)-1);
        h->list_file.name[sizeof(h->list_file.name)-1] = '\0';
    }

    /* Lookup next file name for this database */
    if(lookup_file_name(h, min_idx, 1) != OK) {
        WARN("Could not lookup next file name");
        return FAIL_EINTERNAL;
    }

    /* If path comparison returned same string, query again, applicable only when not listing recursively */
    if(h->list_cache[min_idx].name[0] != '\0' && !h->list_recurse &&
       !pcmp(h, h->list_cache[min_idx].name, h->list_file.name + 1, sizeof(h->list_cache[min_idx].name))) {
        if(lookup_file_name(h, min_idx, 1)) {
            WARN("Could not lookup file name");
            return FAIL_EINTERNAL;
        }
    }

    return OK;
}

sx_nodelist_t *sx_hashfs_all_hashnodes(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hash_t *hash, unsigned int replica_count) {
    sx_nodelist_t *prev = NULL, *next = NULL;
    unsigned int nnodes;
    int64_t mh;

    if(!h || !hash) {
	NULLARG();
	return NULL;
    }

    if(replica_count < 1) {
	msg_set_reason("Bad replica count: %d", replica_count);
	return NULL;
    }

    if(!h->have_hd) {
	BADSTATE("Called before initialization");
	return NULL;
    }

    mh = MurmurHash64(hash, sizeof(*hash), HDIST_SEED);

    if(h->is_rebalancing && (which == NL_PREV || which == NL_PREVNEXT || which == NL_NEXTPREV)) {
	nnodes = sx_nodelist_count(h->prev_dist);
	if(replica_count <= nnodes) {
	    prev = sxi_hdist_locate(h->hd, mh, replica_count, 1);
	    if(!prev) {
		msg_set_reason("Failed to locate hash");
		return NULL;
	    }
	} else if(which == NL_PREV)
	    msg_set_reason("Bad replica count: %d should be below %d", replica_count, nnodes);

	/* MODHDIST: over replica request is only fatal if we don't have a NEXT part */
	if(which == NL_PREV)
	    return prev;
    }

    nnodes = sx_nodelist_count(h->next_dist);
    if(replica_count > nnodes) {
	/* MODHDIST: over replica request is always fatal (replica can't have decreased) */
	msg_set_reason("Bad replica count: %d should be below %d", replica_count, nnodes);
	sx_nodelist_delete(prev);
	return NULL;
    }
    next = sxi_hdist_locate(h->hd, mh, replica_count, 0);
    if(!next) {
	msg_set_reason("Failed to locate hash");
	sx_nodelist_delete(prev);
	return NULL;
    }

    if(prev) {
	sx_nodelist_t *ret, *del;
	rc_ty r;
	if(which == NL_NEXTPREV) {
	    ret = next;
	    del = prev;
	} else {
	    ret = prev;
	    del = next;
	}
	r = sx_nodelist_addlist(ret, del);
	sx_nodelist_delete(del);
	if(r) {
	    sx_nodelist_delete(ret);
	    ret = NULL;
	}
	return ret;
    }

    return next;
}

sx_nodelist_t *sx_hashfs_effective_hashnodes(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hash_t *hash, unsigned int replica_count) {
    sx_nodelist_t *effnodes, *allnodes;
    unsigned int nnodes, i;

    nnodes = sx_nodelist_count(h->ignored_nodes);
    if(replica_count <= nnodes)
	return NULL;

    allnodes = sx_hashfs_all_hashnodes(h, which, hash, replica_count);
    if(!nnodes)
	return allnodes;

    effnodes = sx_nodelist_new();
    if(!effnodes) {
	sx_nodelist_delete(allnodes);
	return NULL;
    }

    nnodes = sx_nodelist_count(allnodes);
    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(allnodes, i);
	if(sx_nodelist_lookup(h->ignored_nodes, sx_node_uuid(node)))
	    continue;
	if(sx_nodelist_add(effnodes, sx_node_dup(node)))
	    break;
    }

    sx_nodelist_delete(allnodes);
    if(i<nnodes) {
	sx_nodelist_delete(effnodes);
	return NULL;
    }
    return effnodes;
}

sx_nodelist_t *sx_hashfs_putfile_hashnodes(sx_hashfs_t *h, const sx_hash_t *hash) {
    return sx_hashfs_effective_hashnodes(h, NL_NEXT, hash, h->put_replica);
}

static rc_ty get_volnodes_common(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hashfs_volume_t *volume, int64_t size, sx_nodelist_t **nodes, unsigned int *block_size, int effective_only) {
    sx_hash_t hash;

    if(!h || !volume || !nodes) {
	NULLARG();
	return EFAULT;
    }
    if (size < SXLIMIT_MIN_FILE_SIZE || size > SXLIMIT_MAX_FILE_SIZE) {
	msg_set_reason("Invalid size %lld: must be between %lld and %lld",
		       (long long)size, (long long)SXLIMIT_MIN_FILE_SIZE,
		       (long long)SXLIMIT_MAX_FILE_SIZE);
	return EINVAL;
    }

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), volume->name, strlen(volume->name), &hash))
	return FAIL_EINTERNAL;

    *nodes = effective_only ?
	sx_hashfs_effective_hashnodes(h, which, &hash, volume->max_replica) :
	sx_hashfs_all_hashnodes(h, which, &hash, volume->max_replica);
    if(!*nodes)
	return FAIL_EINTERNAL;

    size_to_blocks(size, NULL, block_size);
    return OK;
}

rc_ty sx_hashfs_effective_volnodes(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hashfs_volume_t *volume, int64_t size, sx_nodelist_t **nodes, unsigned int *block_size) {
    return get_volnodes_common(h, which, volume, size, nodes, block_size, 1);
}

rc_ty sx_hashfs_all_volnodes(sx_hashfs_t *h, sx_hashfs_nl_t which, const sx_hashfs_volume_t *volume, int64_t size, sx_nodelist_t **nodes, unsigned int *block_size) {
    return get_volnodes_common(h, which, volume, size, nodes, block_size, 0);
}

/* MODHDIST: there might now be 0, 1 or 2 selves. which do we return? */
const sx_node_t *sx_hashfs_self(sx_hashfs_t *h) {
    if(!h || sx_storage_is_bare(h))
	return NULL;
    return sx_nodelist_lookup(h->nextprev_dist, &h->node_uuid);
}

rc_ty sx_hashfs_self_uuid(sx_hashfs_t *h, sx_uuid_t *uuid) {
    if(!h || !uuid) {
	NULLARG();
	return EFAULT;
    }
    if(sx_storage_is_bare(h))
	return FAIL_EINIT;

    memcpy(uuid, &h->node_uuid, sizeof(*uuid));
    return OK;
}

const char *sx_hashfs_self_unique(sx_hashfs_t *h) {
    char *r = (char *)h->blockbuf;
    uint64_t a, b;

    if(sx_storage_is_bare(h))
	a = 0;
    else
	a = MurmurHash64(h->node_uuid.binary, sizeof(h->node_uuid.binary), HDIST_SEED);

    b = MurmurHash64(h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary), HDIST_SEED);
    sprintf(r, "%016llx%016llx", (long long)a, (long long)b);

    return r;
}

const char *sx_hashfs_authtoken(sx_hashfs_t *h) {
    return (h && strlen(h->root_auth) == AUTHTOK_ASCII_LEN) ? h->root_auth : NULL;
}

char *sxi_hashfs_admintoken(sx_hashfs_t *h) {
    uint8_t key[AUTH_KEY_LEN];
    char auth[AUTHTOK_ASCII_LEN + 1];

    if(!h) {
	NULLARG();
	return NULL;
    }

    if(sx_hashfs_get_user_info(h, ADMIN_USER, NULL, key, NULL, NULL))
	return NULL;

    if(encode_auth_bin(ADMIN_USER, (const unsigned char *) key, AUTH_KEY_LEN, auth, sizeof(auth))) {
	CRIT("Failed to encode cluster key");
	fprintf(stderr, "encode_auth_bin failed\n");
	return NULL;
    }

    return strdup(auth);
}

rc_ty sx_hashfs_derive_key(sx_hashfs_t *h, unsigned char *key, int len, const char *info)
{
    if (derive_key(h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary),
		   (const unsigned char*)h->root_auth, strlen(h->root_auth), info,
		   key, len))
	return FAIL_EINTERNAL;
    return OK;
}

rc_ty sx_hashfs_create_user(sx_hashfs_t *h, const char *user, const uint8_t *uid, unsigned uid_size, const uint8_t *key, unsigned key_size, int role, const char *desc)
{
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !user || !uid || !key) {
	NULLARG();
	return EFAULT;
    }

    if(sx_hashfs_check_username(user)) {
	msg_set_reason("Invalid user");
	return EINVAL;
    }

    if(key_size != AUTH_KEY_LEN) {
	msg_set_reason("Invalid key");
	return EINVAL;
    }

    if(role != ROLE_ADMIN && role != ROLE_USER) {
	msg_set_reason("Invalid role");
	return EINVAL;
    }

    sqlite3_stmt *q = h->q_createuser;
    sqlite3_reset(q);
    do {
	if(qbind_blob(q, ":userhash", uid, AUTH_UID_LEN))
	    break;
	if (qbind_text(q, ":name", user))
	    break;
	if (qbind_blob(q, ":key", key, key_size))
	    break;
	if (qbind_int64(q, ":role", role))
	    break;
	int ret = qstep(q);
	if (ret == SQLITE_CONSTRAINT) {
	    rc = EEXIST;
	    break;
	}
	if (ret != SQLITE_DONE)
	    break;
        INFO("User '%s' created", user);
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    INFO("desc: %s", desc);
    if (desc && *desc) {
        sqlite3_stmt *q = h->q_createuser_meta;
        if (qbind_blob(q, ":userhash", uid, AUTH_UID_LEN) ||
            qbind_text(q, ":desc", desc) ||
            qstep_noret(q)) {
            WARN("failed to set user desc");
            rc = FAIL_EINTERNAL;
        }
        sqlite3_reset(q);
    }
    return rc;
}

rc_ty sx_hashfs_user_newkey(sx_hashfs_t *h, const char *user, const uint8_t *key, unsigned key_size)
{
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !user || !key) {
	NULLARG();
	return EFAULT;
    }

    if(sx_hashfs_check_username(user)) {
	msg_set_reason("Invalid user");
	return EINVAL;
    }

    if(key_size != AUTH_KEY_LEN) {
	msg_set_reason("Invalid key");
	return EINVAL;
    }

    sqlite3_stmt *q = h->q_user_newkey;
    sqlite3_reset(q);
    do {
	if (qbind_text(q, ":username", user))
	    break;
	if (qbind_blob(q, ":key", key, key_size))
	    break;
	int ret = qstep(q);
	if (ret != SQLITE_DONE)
	    break;
        INFO("Key changed for user '%s' ", user);
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}

int encode_auth(const char *user, const unsigned char *key, unsigned key_size, char *auth, unsigned auth_size)
{
    sx_hash_t h;
    if (!user || !key || !auth) {
	NULLARG();
	return -1;
    }
    if (key_size != AUTH_KEY_LEN) {
	msg_set_reason("Key of wrong size: %d != %d", key_size, AUTH_KEY_LEN);
	return -1;
    }
    if (auth_size < AUTHTOK_ASCII_LEN + 1) {
	msg_set_reason("Auth of wrong size: %d != %d",
		       auth_size, AUTHTOK_ASCII_LEN+1);
	return -1;
    }
    if (hash_buf(NULL, 0, user, strlen(user), &h)) {
	WARN("hashing username failed");
	return -1;
    }
    return encode_auth_bin(h.b, key, key_size, auth, auth_size);
}

int encode_auth_bin(const uint8_t *userhash, const unsigned char *key, unsigned key_size, char *auth, unsigned auth_size)
{
    uint8_t buf[AUTHTOK_BIN_LEN];
    if (!userhash) {
	WARN("NULL userhash");
	return -1;
    }
    if (!key) {
	WARN("NULL key");
	return -1;
    }
    if (!auth) {
	WARN("NULL auth");
	return -1;
    }

    if (key_size != AUTH_KEY_LEN) {
	msg_set_reason("bad key size: %d != %d",
		       key_size, AUTH_KEY_LEN);
	return -1;
    }
    if (auth_size < AUTHTOK_ASCII_LEN + 1) {
	msg_set_reason("bad auth token size: %d != %d",
		       auth_size, AUTHTOK_ASCII_LEN+1);
	return -1;
    }

    memset(buf, 0, sizeof(buf));
    memcpy(buf, userhash, AUTH_UID_LEN);
    memcpy(buf + AUTH_UID_LEN, key, AUTH_KEY_LEN);
    char *a = sxi_b64_enc_core(buf, sizeof(buf));
    sxi_strlcpy(auth, a, auth_size);
    free(a);
    return 0;
}

rc_ty sx_hashfs_list_users(sx_hashfs_t *h, const uint8_t *list_clones, user_list_cb_t cb, int desc, void *ctx) {
    rc_ty rc = FAIL_EINTERNAL;
    uint64_t lastuid = 0;
    sqlite3_stmt *q;

    if (!h || !cb) {
	NULLARG();
	return EFAULT;
    }

    if(!list_clones)
        q = h->q_listusers;
    else
        q = h->q_listusersbycid;
    while(1) {
        int ret;
        sx_uid_t uid;
        const char *name;
        const uint8_t *user;
        const uint8_t *key;
        int is_admin = 0;

        sqlite3_reset(q);
        if(qbind_int64(q, ":lastuid", lastuid)) {
            WARN("Failed to bind uid to users listing query");
            break;
        }

        if(list_clones && (qbind_blob(q, ":common_id", list_clones, AUTH_CID_LEN) || qbind_int(q, ":inactivetoo", 0))) {
            WARN("Failed to bind common id to q_listusersbycid query");
            break;
        }
        ret = qstep(q);
	if(ret == SQLITE_DONE)
	    rc = OK;
	if(ret != SQLITE_ROW)
            break;
	uid = sqlite3_column_int64(q, 0);
	name = (const char *)sqlite3_column_text(q, 1);
	user = sqlite3_column_blob(q, 2);
	key = sqlite3_column_blob(q, 3);
	is_admin = sqlite3_column_int64(q, 4) == ROLE_ADMIN;
        lastuid = uid;

	if(sqlite3_column_bytes(q, 2) != SXI_SHA1_BIN_LEN || sqlite3_column_bytes(q, 3) != AUTH_KEY_LEN) {
	    WARN("User %s (%lld) is invalid", name, (long long)uid);
	    continue;
	}
	if(cb(uid, name, user, key, is_admin, desc ? (const char*)sqlite3_column_text(q, 5) : NULL, ctx)) {
	    rc = EINTR;
	    break;
	}
    }
    sqlite3_reset(q);
    return rc;
}

/* Check if user with given uid is a volume owner */
static int uid_is_volume_owner(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, sx_uid_t id) {
    sqlite3_stmt *q = h->q_userisowner;
    int r, ret = -1;

    if(!vol || !id) {
        WARN("NULL argument");
        return EINVAL;
    }
    sqlite3_reset(q);

    if(qbind_int64(q, ":owner_id", vol->owner) || qbind_int64(q, ":uid", id)) {
        WARN("Failed to bind IDs to volume ownership check query");
        goto uid_is_volume_owner_err;
    }

    r = qstep(q);
    if(r == SQLITE_DONE)
        ret = 0;
    else if(r == SQLITE_ROW)
        ret = 1;
    else
        WARN("Failed to check volume %s ownership for ID %lld", vol->name, (long long)id);

uid_is_volume_owner_err:
    sqlite3_reset(q);
    return ret;
}

rc_ty sx_hashfs_list_clones_first(sx_hashfs_t *h, sx_uid_t id, const sx_hashfs_user_t **user, int inactivetoo) {
    rc_ty s;

    if(!user) {
        NULLARG();
        return EINVAL;
    }

    memset(&h->curclone, 0, sizeof(h->curclone));
    h->listinactiveclones = inactivetoo;

    /* Get user UID */
    if((s = sx_hashfs_get_user_by_uid(h, id, h->curclone.uid, inactivetoo)) != OK) {
        WARN("Failed to get user by UID: %d", s);
        if(s == ENOENT)
            return ITER_NO_MORE;
        else
            return s;
    }

    *user = &h->curclone;
    return sx_hashfs_list_clones_next(h);
}

rc_ty sx_hashfs_list_clones_next(sx_hashfs_t *h) {
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = h->q_listusersbycid;
    int r;
    const char *name;
    const uint8_t *uid, *key;
    sx_hashfs_user_t *u = &h->curclone;

    sqlite3_reset(q);
    if(qbind_int64(q, ":lastuid", h->curclone.id) || qbind_blob(q, ":common_id", h->curclone.uid, AUTH_CID_LEN) ||
       qbind_int(q, ":inactivetoo", h->listinactiveclones)) {
        WARN("Failed to bind user IDs");
        goto sx_hashfs_list_volume_owners_next_err;
    }

    r = qstep(q);
    if(r == SQLITE_DONE) {
        ret = ITER_NO_MORE;
        goto sx_hashfs_list_volume_owners_next_err;
    } else if(r != SQLITE_ROW) {
        WARN("Failed to iterate user clones");
        goto sx_hashfs_list_volume_owners_next_err;
    }

    u->id = sqlite3_column_int64(q, 0);
    name = (const char *)sqlite3_column_text(q, 1);
    if(!name || sx_hashfs_check_username(name)) {
        WARN("Invalid user name");
        goto sx_hashfs_list_volume_owners_next_err;
    }
    sxi_strlcpy(u->name, name, sizeof(u->name));

    uid = sqlite3_column_blob(q, 2);
    if(!uid || sqlite3_column_bytes(q, 2) != AUTH_UID_LEN) {
        WARN("Invalid user ID");
        goto sx_hashfs_list_volume_owners_next_err;
    }
    memcpy(u->uid, uid, AUTH_UID_LEN);

    key = sqlite3_column_blob(q, 3);
    if(!uid || sqlite3_column_bytes(q, 3) != AUTH_KEY_LEN) {
        WARN("Invalid user key");
        goto sx_hashfs_list_volume_owners_next_err;
    }
    memcpy(u->key, key, AUTH_KEY_LEN);

    u->role = sqlite3_column_int(q, 4);

    ret = OK;
sx_hashfs_list_volume_owners_next_err:
    sqlite3_reset(q);
    return ret;
}

rc_ty sx_hashfs_list_acl(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, sx_uid_t uid, int uid_priv, acl_list_cb_t cb, void *ctx)
{
    char user[SXLIMIT_MAX_USERNAME_LEN+1];
    int64_t lastuid = 0;
    int is_owner;
    sx_priv_t priv = PRIV_NONE;
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !cb || !ctx)
	return EINVAL;

    /* list privileges for self */
    priv = uid_priv;
    if (uid > 0 ) {
        if ((rc = sx_hashfs_uid_get_name(h, uid, user, sizeof(user))))
            return rc;

        rc = FAIL_EINTERNAL;
        is_owner = uid_is_volume_owner(h, vol, uid);
        if(is_owner < 0)
            return FAIL_EINTERNAL;
        if (cb(user, priv, is_owner, ctx))
            return rc;
    }

    if (!(priv & (PRIV_ADMIN | PRIV_ACL))) {
        DEBUG("Not an owner/admin: printed only self privileges");
        return OK;
    }
    /* admin and owner can see full ACL list */

    sqlite3_stmt *q = h->q_listacl;
    do {
        int ret = SQLITE_ROW;
	if (qbind_int64(q, ":volid", vol->id))
            break;
        while (1) {
            rc_ty s;
            const sx_hashfs_user_t *u = NULL;
            sqlite3_reset(q);
            if (qbind_int64(q, ":lastuid", lastuid))
                break;
            ret = qstep(q);
            if (ret != SQLITE_ROW)
               break;
            int64_t list_uid = sqlite3_column_int64(q, 2);
            lastuid = list_uid;
	    int perm = sqlite3_column_int64(q, 1);
            is_owner = uid_is_volume_owner(h, vol, list_uid);
            if(is_owner < 0)
                return FAIL_EINTERNAL;
            for(s = sx_hashfs_list_clones_first(h, list_uid, &u, 0); s == OK; s = sx_hashfs_list_clones_next(h)) {
                if (u->id == uid)
                    continue;/* we've already printed permissions for self */
                if (cb(u->name, perm, is_owner, ctx))
                    break;
            }

            if(s != ITER_NO_MORE) {
                WARN("Failed to iterate over all %lld clones", (long long)list_uid);
                ret = s;
                break;
            }
	}
	if (ret != SQLITE_DONE)
	    break;
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}


static rc_ty get_uid_role(sx_hashfs_t *h, const char *username, int64_t *uid, int *role, int inactivetoo) {
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !username || sx_hashfs_check_username(username))
	return EINVAL;
    sqlite3_stmt *q = h->q_getuid;
    sqlite3_reset(q);
    do {
	if(qbind_text(q, ":name", username) ||
	   qbind_int(q, ":inactivetoo", (inactivetoo != 0)))
	    break;

	int ret = qstep(q);
	if (ret == SQLITE_DONE) {
	    rc = ENOENT;
	    break;
	}
	if (ret != SQLITE_ROW)
	    break;
	if(uid)
	    *uid = sqlite3_column_int64(q, 0);
        if(role)
            *role = sqlite3_column_int(q, 1);
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}

rc_ty sx_hashfs_get_uid_role(sx_hashfs_t *h, const char *user, int64_t *uid, int *role) {
    return get_uid_role(h, user, uid, role, 0);
}

rc_ty sx_hashfs_get_uid(sx_hashfs_t *h, const char *user, int64_t *uid)
{
    return sx_hashfs_get_uid_role(h, user, uid, NULL);
}

rc_ty sx_hashfs_uid_get_name(sx_hashfs_t *h, uint64_t uid, char *name, unsigned len)
{
    rc_ty rc = FAIL_EINTERNAL;
    if (!h || !name || !len)
	return EINVAL;
    sqlite3_stmt *q = h->q_getuidname;
    sqlite3_reset(q);
    do {
	if (qbind_int64(q, ":uid", uid))
	    break;
	int ret = qstep(q);
	if (ret == SQLITE_DONE) {
	    rc = ENOENT;
	    break;
	}
	if (ret != SQLITE_ROW)
	    break;
	sxi_strlcpy(name, (const char*)sqlite3_column_text(q, 0), len);
	rc = OK;
    } while(0);
    sqlite3_reset(q);
    return rc;
}


rc_ty sx_hashfs_delete_user(sx_hashfs_t *h, const char *username, const char *new_owner, int all_clones) {
    rc_ty rc, ret = FAIL_EINTERNAL;
    sx_uid_t old, new;
    int has_clone = 0;
    const sx_hashfs_user_t *u = NULL;

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    rc = get_uid_role(h, username, &old, NULL, 1);
    if(rc != OK) {
	ret = rc;
	goto delete_user_err;
    }

    rc = sx_hashfs_get_uid(h, new_owner, &new);
    if(rc != OK) {
	/* FIXME: shall i rather fall back to any admin user? */
	ret = rc;
	goto delete_user_err;
    }

    rc = sx_hashfs_list_clones_first(h, old, &u, 1);
    if(rc != OK || !u) {
        WARN("Failed to list clones of %s: %d", username, rc);
        ret = rc;
        goto delete_user_err;
    }

    if(u->id != old) { /* If returned user ID is differend, then clone exists */
        has_clone = 1;
    } else if((rc = sx_hashfs_list_clones_next(h)) != ITER_NO_MORE) { /* If got user with same id, then need to check against next one */
        if(rc != OK || !u) { /* Check for errors again */
            WARN("Failed to list clones of %s", username);
            ret = rc;
            goto delete_user_err;
        }
        has_clone = 1; /* Clone exists, got him in second try */
    }

    if(!all_clones && has_clone) {
        /* When a clone exists new volume owner will become one of deleted user clones */
        new = u->id;
    }

    sqlite3_reset(h->q_chownvol);
    sqlite3_reset(h->q_deleteuser);
    sqlite3_reset(h->q_chprivs);

    /* First, change volume ownership to a new owner */
    if(qbind_int64(h->q_chownvol, ":new", new) || 
       qbind_int64(h->q_chownvol, ":old", old) ||
       qstep_noret(h->q_chownvol))
        goto delete_user_err;

    if(all_clones) {
        /* When deleting all users, drop them all in a loop */
        for(rc = sx_hashfs_list_clones_first(h, old, &u, 1); rc == OK; rc = sx_hashfs_list_clones_next(h)) {
            if(qbind_int64(h->q_deleteuser, ":uid", u->id) ||
               qstep_noret(h->q_deleteuser)) {
                WARN("Failed to drop user %s [%lld]", u->name, (long long)u->id);
                goto delete_user_err;
            }

            INFO("User %s deleted", u->name);
        }
    } else {
        if(has_clone) {
            /* In this case we have to grant privileges to given user clone in order to allow all clones still access their vols */
            if(qbind_int64(h->q_chprivs, ":new", new) || qbind_blob(h->q_chprivs, ":user", u->uid, AUTH_UID_LEN) || qstep_noret(h->q_chprivs)) {
                WARN("Failed to prepare and evaluate privs change query");
                goto delete_user_err;
            }
        }
        /* Drop that one particular user */
        if(qbind_int64(h->q_deleteuser, ":uid", old) ||
           qstep_noret(h->q_deleteuser)) {
            WARN("Failed to drop user %s [%lld]", u->name, (long long)u->id);
	    goto delete_user_err;
        }
    }

    if(qcommit(h->db))
	goto delete_user_err;

    ret = OK;
    if(all_clones || !has_clone)
        INFO("User %s removed (and replaced by %s)", username, new_owner);
    else
        INFO("User %s removed", username);

 delete_user_err:
    sqlite3_reset(h->q_chownvol);
    sqlite3_reset(h->q_deleteuser);
    sqlite3_reset(h->q_chprivs);

    if(ret != OK)
	qrollback(h->db);

    return ret;
}


void sx_hashfs_volume_new_begin(sx_hashfs_t *h) {
    h->nmeta = 0;
}

rc_ty sx_hashfs_volume_new_addmeta(sx_hashfs_t *h, const char *key, const void *value, unsigned int value_len) {
    if(!h)
	return FAIL_EINTERNAL;

    rc_ty rc;
    if((rc = sx_hashfs_check_meta(key, value, value_len)))
	return rc;

    if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	return EOVERFLOW;

    memcpy(h->meta[h->nmeta].key, key, strlen(key)+1);
    memcpy(h->meta[h->nmeta].value, value, value_len);
    h->meta[h->nmeta].value_len = value_len;
    h->nmeta++;
    return OK;
}

static rc_ty get_min_reqs(sx_hashfs_t *h, unsigned int *min_nodes, int64_t *min_capa) {
    sqlite3_reset(h->q_minreqs);
    if(qstep_ret(h->q_minreqs))
        return FAIL_EINTERNAL;

    if(min_nodes)
        *min_nodes = sqlite3_column_int(h->q_minreqs, 0);
    if(min_capa)
        *min_capa = sqlite3_column_int64(h->q_minreqs, 1);

    sqlite3_reset(h->q_minreqs);
    return OK;
}

static int64_t get_cluster_capacity(sx_hashfs_t *h, sx_hashfs_nl_t which) {
    int64_t size = 0;
    unsigned int i, nnodes;

    nnodes = sx_nodelist_count(sx_hashfs_all_nodes(h, which));
    for(i = 0; i < nnodes; i++)
        size += sx_node_capacity(sx_nodelist_get(sx_hashfs_all_nodes(h, which), i));

    return size;
}

/* Return error if given size is incorrect */
static rc_ty sx_hashfs_check_volume_size(sx_hashfs_t *h, int64_t size, unsigned int replica) {
    int64_t vols_size;
    int64_t nodes_size = 0;

    if(size < SXLIMIT_MIN_VOLUME_SIZE || size > SXLIMIT_MAX_VOLUME_SIZE) {
        msg_set_reason("Invalid volume size %lld: must be between %lld and %lld",
                       (long long)size,
                       (long long)SXLIMIT_MIN_VOLUME_SIZE,
                       (long long)SXLIMIT_MAX_VOLUME_SIZE);
        return EINVAL;
    }

    if(!h->have_hd) /* No hdist, so skip checking if sum of volumes fits in cluster capacity */
        return OK;

    if(get_min_reqs(h, NULL, &vols_size)) {
        msg_set_reason("Failed to get volume sizes");
        return FAIL_EINTERNAL;
    }

    if(sx_hashfs_is_rebalancing(h)) /* NL_PREV will differ from NL_NEXT */
        nodes_size = MIN(get_cluster_capacity(h, NL_PREV), get_cluster_capacity(h, NL_NEXT));
    else
        nodes_size = get_cluster_capacity(h, NL_PREV); /* All dists are equal, doesn't matter which one will we take here */

    if(!nodes_size) {
        msg_set_reason("Failed to get node sizes");
        return FAIL_EINTERNAL;
    }

    /* Check if cluster capacity is not reached yet (better error message) */
    if(SXLIMIT_MIN_VOLUME_SIZE > nodes_size - vols_size) {
        msg_set_reason("Invalid volume size %lld: reached cluster capacity", (long long)size);
        return EINVAL;
    }

    /* Total volumes size is greater than cluster capacity */
    if(vols_size + size * (int64_t)replica > nodes_size) {
        msg_set_reason("Invalid volume size %lld - with replica %u and current cluster usage it must be between %lld and %lld",
                       (long long)size, replica,
                       (long long)SXLIMIT_MIN_VOLUME_SIZE,
                       (long long)(nodes_size - vols_size) / replica);
        return EINVAL;
    }

    return OK;
}

rc_ty sx_hashfs_check_volume_settings(sx_hashfs_t *h, const char *volume, int64_t size, unsigned int replica, unsigned int revisions) {
    rc_ty ret;

    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if((ret = sx_hashfs_check_volume_name(volume)))
	return ret;

    if(h->have_hd) {
	unsigned int minreplica = sx_nodelist_count(h->ignored_nodes) + 1;
	unsigned int maxreplica = MIN(sx_nodelist_count(sx_hashfs_all_nodes(h, NL_PREV)), sx_nodelist_count(sx_hashfs_all_nodes(h, NL_NEXT)));
	if(replica < minreplica || replica > maxreplica) {
	    if(maxreplica == 1)
		msg_set_reason("Invalid replica count %d: must be 1 for a single-node cluster", replica);
	    else
		msg_set_reason("Invalid replica count %d: must be between %d and %d", replica, minreplica, maxreplica);
	    return EINVAL;
	}
    }

    if(revisions < SXLIMIT_MIN_REVISIONS || revisions > SXLIMIT_MAX_REVISIONS) {
	msg_set_reason("Invalid volume revisions: must be between %u and %u", SXLIMIT_MIN_REVISIONS, SXLIMIT_MAX_REVISIONS);
	return EINVAL;
    }

    return sx_hashfs_check_volume_size(h, size, replica);
}

rc_ty sx_hashfs_volume_new_finish(sx_hashfs_t *h, const char *volume, int64_t size, unsigned int replica, unsigned int revisions, sx_uid_t uid) {
    unsigned int reqlen = 0;
    rc_ty ret = FAIL_EINTERNAL;
    int64_t volid;
    int r;

    if(!h) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_reset(h->q_addvol);
    sqlite3_reset(h->q_addvolmeta);
    sqlite3_reset(h->q_addvolprivs);

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    /* Check volume size inside transaction to not fall into race */
    if((ret = sx_hashfs_check_volume_settings(h, volume, size, replica, revisions)) != OK)
        goto volume_new_err;
    ret = FAIL_EINTERNAL;

    if(qbind_text(h->q_addvol, ":volume", volume) ||
       qbind_int(h->q_addvol, ":replica", replica) ||
       qbind_int(h->q_addvol, ":revs", revisions) ||
       qbind_int64(h->q_addvol, ":size", size) ||
       qbind_int64(h->q_addvol, ":owner", uid))
	goto volume_new_err;

    r = qstep(h->q_addvol);
    if(r == SQLITE_CONSTRAINT) {
	const sx_hashfs_volume_t *vol;
	if(sx_hashfs_volume_by_name(h, volume, &vol) == OK)
	    ret = FAIL_VOLUME_EEXIST;
	else
	    ret = FAIL_LOCKED;
    }

    if(r != SQLITE_DONE)
	goto volume_new_err;

    volid = sqlite3_last_insert_rowid(sqlite3_db_handle(h->q_addvol));

    if(h->nmeta) {
	unsigned int nmeta = h->nmeta;
	if(qbind_int64(h->q_addvolmeta, ":volume", volid))
	    goto volume_new_err;

	while(nmeta--) {
	    reqlen += strlen(h->meta[nmeta].key) + 3 + h->meta[nmeta].value_len * 2 + 3; /* "key":"hex(value)", */
	    sqlite3_reset(h->q_addvolmeta);
	    if(qbind_text(h->q_addvolmeta, ":key", h->meta[nmeta].key) ||
	       qbind_blob(h->q_addvolmeta, ":value", h->meta[nmeta].value, h->meta[nmeta].value_len) ||
	       qstep_noret(h->q_addvolmeta))
		goto volume_new_err;
	}
    }

    if(qbind_int64(h->q_addvolprivs, ":volume", volid) ||
       qbind_int64(h->q_addvolprivs, ":user", uid) ||
       qbind_int(h->q_addvolprivs, ":priv", PRIV_READ | PRIV_WRITE) ||
       qstep_noret(h->q_addvolprivs))
	goto volume_new_err;

    if(qcommit(h->db))
	goto volume_new_err;

    ret = OK;

    volume_new_err:
    sqlite3_reset(h->q_addvol);
    sqlite3_reset(h->q_addvolmeta);
    sqlite3_reset(h->q_addvolprivs);

    if(ret != OK)
	qrollback(h->db);

    h->nmeta = 0;

    return ret;
}

rc_ty sx_hashfs_volume_enable(sx_hashfs_t *h, const char *volume) {
    int ret = OK;

    if(qbind_text(h->q_onoffvol, ":volume", volume) ||
       qbind_int(h->q_onoffvol, ":enable", 1) ||
       qstep_noret(h->q_onoffvol))
	ret = FAIL_EINTERNAL;

    return ret;
}

rc_ty sx_hashfs_volume_disable(sx_hashfs_t *h, const char *volume) {
    const sx_hashfs_volume_t *vol;
    unsigned int mdb = 0;
    rc_ty ret;

    if(!h) {
	NULLARG();
	return EFAULT;
    }
    if((ret = sx_hashfs_check_volume_name(volume)))
	return ret;

    ret = sx_hashfs_volume_by_name(h, volume, &vol);
    if(ret != OK)
	return ret;

    sqlite3_reset(h->q_onoffvol);

    /* If not a volnode, then disable right away */
    if(!sx_hashfs_is_or_was_my_volume(h, vol)) {
	if(qbind_text(h->q_onoffvol, ":volume", volume) ||
	   qbind_int(h->q_onoffvol, ":enable", 0) ||
	   qstep_noret(h->q_onoffvol))
	    return FAIL_EINTERNAL;
	return OK;
    }

    /* Otherwise make sure the volume is empty */
    if(qbegin(h->db)) {
	ret = FAIL_EINTERNAL;
	goto volume_disable_err;
    }
    for(mdb=0; mdb<METADBS; mdb++) {
	if(qbegin(h->metadb[mdb])) {
	    ret = FAIL_EINTERNAL;
	    goto volume_disable_err;
	}
    }

    ret = sx_hashfs_list_first(h, vol, NULL, NULL, 1, NULL);
    if(ret == OK) {
	msg_set_reason("Cannot disable non empty volume");
	ret = ENOTEMPTY;
    }
    if(ret != ITER_NO_MORE)
	goto volume_disable_err;
    ret = OK;

    if(qbind_text(h->q_onoffvol, ":volume", volume) ||
       qbind_int(h->q_onoffvol, ":enable", 0) ||
       qstep_noret(h->q_onoffvol)) {
	ret = FAIL_EINTERNAL;
	goto volume_disable_err;
    }

    if(qcommit(h->db))
	ret = FAIL_EINTERNAL;

 volume_disable_err:
    if(ret != OK)
	qrollback(h->db);

    while(mdb--)
	qrollback(h->metadb[mdb]);

    sqlite3_reset(h->q_onoffvol);

    return ret;
}

rc_ty sx_hashfs_volume_delete(sx_hashfs_t *h, const char *volume, int force) {
    rc_ty ret;
    int r;

    if(!h) {
	NULLARG();
	return EFAULT;
    }
    if((ret = sx_hashfs_check_volume_name(volume)))
	return ret;

    sqlite3_reset(h->q_getvolstate);
    sqlite3_reset(h->q_delvol);

    if(qbegin(h->db) ||
       qbind_text(h->q_getvolstate, ":volume", volume)) {
	ret = FAIL_EINTERNAL;
	goto volume_delete_err;
    }

    r = qstep(h->q_getvolstate);
    if(r == SQLITE_DONE) {
	ret = ENOENT;
	goto volume_delete_err;
    }
    if(r != SQLITE_ROW) {
	ret = FAIL_EINTERNAL;
	goto volume_delete_err;
    }
    if(!force) {
	r = sqlite3_column_int(h->q_getvolstate, 0);
	if(r) {
	    ret = EPERM;
	    msg_set_reason("Cannot delete an enabled volume");
	    goto volume_delete_err;
	}
    }
    if(qbind_text(h->q_delvol, ":volume", volume) ||
       qstep_noret(h->q_delvol) ||
       qcommit(h->db))
	ret = FAIL_EINTERNAL;
    else
	ret = OK;

 volume_delete_err:
    if(ret != OK)
	qrollback(h->db);

    sqlite3_reset(h->q_getvolstate);
    sqlite3_reset(h->q_delvol);

    return ret;
}

rc_ty sx_hashfs_user_onoff(sx_hashfs_t *h, const char *user, int enable, int all_clones) {
    if(!all_clones) {
        if(qbind_text(h->q_onoffuser, ":username", user) ||
           qbind_int(h->q_onoffuser, ":enable", enable) ||
           qstep_noret(h->q_onoffuser))
            return FAIL_EINTERNAL;
        INFO("User '%s' %s", user, enable ? "enabled" : "disabled");
    } else {
        uint8_t user_uid[AUTH_UID_LEN];
        rc_ty s;

        if((s = sx_hashfs_get_user_by_name(h, user, user_uid, 1)) != OK) {
            WARN("Failed to get user by name in order to disable/enable all its clones");
            return s;
        }
        if(qbind_blob(h->q_onoffuserclones, ":user", user_uid, AUTH_UID_LEN) ||
           qbind_int(h->q_onoffuserclones, ":enable", enable) ||
           qstep_noret(h->q_onoffuserclones)) {
            WARN("Failed to disable all %s clones", user);
            return FAIL_EINTERNAL;
        }

        INFO("User '%s' and all his clones were %s", user, enable ? "enabled" : "disabled");
    }
    return OK;
}

rc_ty sx_hashfs_volume_first(sx_hashfs_t *h, const sx_hashfs_volume_t **volume, const uint8_t *uid) {
    if(!h || !volume) {
	WARN("Called with invalid arguments");
	return EINVAL;
    }

    h->curvol.name[0] = '\0';
    h->curvoluser = uid;
    *volume = &h->curvol;
    return sx_hashfs_volume_next(h);
}

static rc_ty volume_next_common(sx_hashfs_t *h) {
    const char *name;
    rc_ty res = FAIL_EINTERNAL;
    int r;

    if(!h) {
        WARN("Called with invalid arguments");
        return EINVAL;
    }

    sqlite3_reset(h->q_nextvol);
    if(qbind_text(h->q_nextvol, ":previous", h->curvol.name))
        goto volume_next_common_err;
    if(h->curvoluser) {
        if(qbind_blob(h->q_nextvol, ":user", h->curvoluser, AUTH_UID_LEN))
            goto volume_next_common_err;
    } else {
        if(qbind_null(h->q_nextvol, ":user"))
            goto volume_next_common_err;
    }

    r = qstep(h->q_nextvol);
    if(r == SQLITE_DONE)
        res = ITER_NO_MORE;
    if(r != SQLITE_ROW)
        goto volume_next_common_err;

    name = (const char *)sqlite3_column_text(h->q_nextvol, 1);
    if(!name)
        goto volume_next_common_err;

    sxi_strlcpy(h->curvol.name, name, sizeof(h->curvol.name));
    h->curvol.id = sqlite3_column_int64(h->q_nextvol, 0);
    h->curvol.max_replica = sqlite3_column_int(h->q_nextvol, 2);
    h->curvol.cursize = sqlite3_column_int64(h->q_nextvol, 3);
    h->curvol.size = sqlite3_column_int64(h->q_nextvol, 4);
    h->curvol.owner = sqlite3_column_int64(h->q_nextvol, 5);
    h->curvol.revisions = sqlite3_column_int(h->q_nextvol, 6);
    h->curvol.changed = sqlite3_column_int64(h->q_nextvol, 7);

    res = OK;
volume_next_common_err:
    sqlite3_reset(h->q_nextvol);
    return res;
}

rc_ty sx_hashfs_volume_next(sx_hashfs_t *h) {
    rc_ty res = volume_next_common(h);
    if(res != OK)
        goto volume_list_err;

    if(sx_nodelist_count(h->ignored_nodes) >= h->curvol.max_replica)
	return sx_hashfs_volume_next(h);
    h->curvol.effective_replica = h->curvol.max_replica - sx_nodelist_count(h->ignored_nodes);

    res = OK;
    volume_list_err:
    return res;
}


static rc_ty volume_get_common(sx_hashfs_t *h, const char *name, int64_t volid, const sx_hashfs_volume_t **volume) {
    sqlite3_stmt *q;
    rc_ty res = FAIL_EINTERNAL;
    int r;

    if(!h || !volume) {
	WARN("Called with invalid arguments");
	return EINVAL;
    }

    if(name) {
	q = h->q_volbyname;
	sqlite3_reset(q);
	if(qbind_text(q, ":name", name))
	    goto volume_err;
    } else {
	q = h->q_volbyid;
	sqlite3_reset(q);
	if(qbind_int64(q, ":volid", volid))
	    goto volume_err;
    }

    r = qstep(q);
    if(r == SQLITE_DONE)
	res = ENOENT;
    if(r != SQLITE_ROW)
	goto volume_err;

    name = (const char *)sqlite3_column_text(q, 1);
    if(!name)
	goto volume_err;

    sxi_strlcpy(h->curvol.name, name, sizeof(h->curvol.name));
    h->curvol.id = sqlite3_column_int64(q, 0);
    h->curvol.max_replica = sqlite3_column_int(q, 2);
    h->curvol.cursize = sqlite3_column_int64(q, 3);
    h->curvol.size = sqlite3_column_int64(q, 4);
    h->curvol.owner = sqlite3_column_int64(q, 5);
    h->curvol.revisions = sqlite3_column_int(q, 6);
    h->curvol.changed = sqlite3_column_int64(q, 7);

    if(sx_nodelist_count(h->ignored_nodes) >= h->curvol.max_replica) {
	res = ENOENT;
	goto volume_err;
    }
    h->curvol.effective_replica = h->curvol.max_replica - sx_nodelist_count(h->ignored_nodes);

    *volume = &h->curvol;
    res = OK;

    volume_err:
    sqlite3_reset(q);
    return res;
}

static rc_ty get_priv_holder(sx_hashfs_t *h, uint64_t uid, const char *volume, int64_t *holder) {
    uint8_t user[AUTH_UID_LEN];
    rc_ty s;
    int r;

    if(!volume || !holder) {
        NULLARG();
        return EINVAL;
    }

    if((s = sx_hashfs_get_user_by_uid(h, uid, user, 1)) != OK) {
        WARN("Failed to get priv holder: %s", rc2str(s));
        return s;
    }

    sqlite3_reset(h->q_getprivholder);
    if(qbind_blob(h->q_getprivholder, ":user", user, AUTH_UID_LEN)) {
        WARN("Failed to prepare query");
        sqlite3_reset(h->q_getprivholder);
        return FAIL_EINTERNAL;
    }

    r = qstep(h->q_getprivholder);
    if(r == SQLITE_DONE) {
        *holder = -1;
        s = ENOENT;
    } else if(r == SQLITE_ROW) {
        *holder = sqlite3_column_int64(h->q_getprivholder, 0);
        s = OK;
    } else {
        WARN("Failed to get existing priv holder");
        *holder = -1;
        s = FAIL_EINTERNAL;
    }
    sqlite3_reset(h->q_getprivholder);
    return s;
}

rc_ty sx_hashfs_grant(sx_hashfs_t *h, uint64_t uid, const char *volume, int priv)
{
    if (!h || !volume)
	return EINVAL;

    rc_ty rc = FAIL_EINTERNAL;
    sqlite3_stmt *q = h->q_grant;
    sqlite3_reset(q);
    const sx_hashfs_volume_t *vol = NULL;
    do {
        int64_t privholder = -1;
        char name[SXLIMIT_MAX_USERNAME_LEN+1];

	rc = volume_get_common(h, volume, -1, &vol);
	if (rc) {
	    WARN("Cannot retrieve volume id for '%s': %s", volume, rc2str(rc));
	    break;
	}

        rc = get_priv_holder(h, uid, volume, &privholder);
        if(rc != OK && rc != ENOENT) {
            WARN("Failed to get priv holder");
            break;
        }
        if(rc == ENOENT)
            privholder = uid;
        rc = OK;
	if (qbind_int64(q,":uid", privholder))
	    break;
	if (qbind_int64(q,":volid", vol->id))
	    break;
	if (qbind_int64(q,":priv", priv))
	    break;
	if (qstep_noret(q))
	    break;

        if(sx_hashfs_uid_get_name(h, uid, name, SXLIMIT_MAX_USERNAME_LEN+1)) {
            WARN("Failed to get user %lld name", (long long)uid);
            INFO("Granted '%s' permission to %lld on volume %s", (priv & PRIV_READ ? "read" : "write"), (long long)uid, volume);
        } else
            INFO("Granted '%s' permission to '%s' on volume %s", (priv & PRIV_READ ? "read" : "write"), name, volume);
    } while(0);
    sqlite3_reset(q);
    return rc;
}

rc_ty sx_hashfs_revoke(sx_hashfs_t *h, uint64_t uid, const char *volume, int privmask)
{
    if (!h || !volume)
	return EINVAL;
    rc_ty rc = FAIL_EINTERNAL;
    sqlite3_stmt *q = h->q_revoke;
    sqlite3_reset(q);
    const sx_hashfs_volume_t *vol = NULL;
    do {
        int64_t privholder;
        char name[SXLIMIT_MAX_USERNAME_LEN+1];

	rc = volume_get_common(h, volume, -1, &vol);
	if (rc) {
	    WARN("Cannot retrieve volume id for '%s': %s", volume, rc2str(rc));
	    break;
	}
        rc = get_priv_holder(h, uid, volume, &privholder);
        if(rc != OK && rc != ENOENT) {
            WARN("Failed to get priv holder");
            break;
        }
        if(rc == ENOENT)
            privholder = uid;
	if (qbind_int64(q,":uid", privholder))
	    break;
	if (qbind_int64(q,":volid", vol->id))
	    break;
	if (qbind_int64(q,":privmask", privmask))
	    break;
	if (qstep_noret(q))
	    break;

        if(sx_hashfs_uid_get_name(h, uid, name, SXLIMIT_MAX_USERNAME_LEN+1)) {
            WARN("Failed to get user %lld name", (long long)uid);
            INFO("Revoked '%s' permission from %lld on volume %s", (~privmask & PRIV_READ ? "read" : "write"), (long long)uid, volume);
        } else
	    INFO("Revoked '%s' permission from '%s' on volume %s", (~privmask & PRIV_READ ? "read" : "write"), name, volume);
    } while(0);
    sqlite3_reset(q);
    return rc;
}

rc_ty sx_hashfs_volume_by_name(sx_hashfs_t *h, const char *name, const sx_hashfs_volume_t **volume) {
    if(sx_hashfs_check_volume_name(name)) {
	WARN("Called with invalid arguments");
	return EINVAL;
    }

    return volume_get_common(h, name, 0, volume);
}

rc_ty sx_hashfs_volume_by_id(sx_hashfs_t *h, int64_t volid, const sx_hashfs_volume_t **volume) {
    return volume_get_common(h, NULL, volid, volume);
}

static void sx_hashfs_getfile_reset(sx_hashfs_t *h)
{
    if(h->get_ndb < METADBS) {
	sqlite3_reset(h->qm_get[h->get_ndb]);
	sqlite3_reset(h->qm_getrev[h->get_ndb]);
    }
}

rc_ty sx_hashfs_getfile_begin(sx_hashfs_t *h, const char *volume, const char *filename, const char *revision, sx_hashfs_file_t *filedata, sx_hash_t *etag) {
    const sx_hashfs_volume_t *vol;
    unsigned int content_len, created_at, bsize;
    const char *rev;
    sqlite3_stmt *q;
    int64_t size;
    rc_ty res;
    int r;

    /* reset previous getfile queries */
    sx_hashfs_getfile_end(h);
    res = sx_hashfs_volume_by_name(h, volume, &vol);
    if(res)
	return res;

    if(check_file_name(filename)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    h->get_ndb = getmetadb(filename);
    if(h->get_ndb < 0)
	return FAIL_EINTERNAL;
    /* reset current getfile queries */
    sx_hashfs_getfile_reset(h);

    if(revision) {
	if(check_revision(revision)) {
	    msg_set_reason("Invalid file name");
	    return EINVAL;
	}
	q = h->qm_getrev[h->get_ndb];
	if(qbind_text(q, ":revision", revision))
	    return FAIL_EINTERNAL;
    } else
	q = h->qm_get[h->get_ndb];

    if(qbind_int64(q, ":volume", vol->id) || qbind_text(q, ":name", filename))
	return FAIL_EINTERNAL;

    r = qstep(q);
    if(r == SQLITE_DONE) {
	DEBUG("No such file: %s/%s", volume, filename);
	sx_hashfs_getfile_end(h);
	return ENOENT;
    }
    if(r != SQLITE_ROW) {
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }

    h->get_id = sqlite3_column_int64(q, 0);
    size = sqlite3_column_int64(q, 1);
    h->get_nblocks = size_to_blocks(size, NULL, &bsize);
    h->get_content = sqlite3_column_blob(q, 2);
    content_len = sqlite3_column_bytes(q, 2);

    rev = (const char *)sqlite3_column_text(q, 3);
    if(!rev ||
       parse_revision(rev, &created_at) ||
       (etag && hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), rev, strlen(rev), etag))) {
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }
    if(content_len != sizeof(sx_hash_t) * h->get_nblocks) {
	WARN("Inconsistent entry for %s:%s", volume, filename);
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }

    h->get_replica = vol->max_replica;

    if(filedata) {
        filedata->volume_id = vol->id;
	filedata->file_size = size;
	filedata->block_size = bsize;
	filedata->nblocks = h->get_nblocks;
	filedata->created_at = created_at;
	sxi_strlcpy(filedata->name, filename, sizeof(filedata->name));
	sxi_strlcpy(filedata->revision, rev, sizeof(filedata->revision));
    }
    return OK;
}

uint64_t sx_hashfs_getfile_count(sx_hashfs_t *h)
{
    return h->get_nblocks;
}

rc_ty sx_hashfs_getfile_block(sx_hashfs_t *h, const sx_hash_t **hash, sx_nodelist_t **nodes) {
    if(!h || !hash || !nodes || (h->get_nblocks && !h->get_content))
	return EINVAL;

    if(!h->get_nblocks)
	return ITER_NO_MORE;

    /* NEXTPREV would be more efficient
     * (because it's pointless to lookup new blocks in PREV)
     * but PREVNEXT is not prone to the following race condition:
     * 1. client -> next: not found
     * 2. prev -> next: move block
     * 3. client -> prev: not found */
    *nodes = sx_hashfs_effective_hashnodes(h, NL_PREVNEXT, h->get_content, h->get_replica);
    if(!*nodes) {
	sx_hashfs_getfile_end(h);
	return FAIL_EINTERNAL;
    }

    *hash = h->get_content;
    h->get_content++;
    h->get_nblocks--;
    return OK;
}

void sx_hashfs_getfile_end(sx_hashfs_t *h) {
    sx_hashfs_getfile_reset(h);
    h->get_content = NULL;
    h->get_nblocks = 0;
    h->get_ndb = METADBS;
}

rc_ty sx_hashfs_block_get(sx_hashfs_t *h, unsigned int bs, const sx_hash_t *hash, const uint8_t **block) {
    unsigned int ndb = gethashdb(hash), hs;
    uint64_t dboff;
    int r;

    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    if(hs == SIZES) {
	WARN("bad blocksize: %d", bs);
	return FAIL_BADBLOCKSIZE;
    }

    sqlite3_reset(h->qb_get[hs][ndb]);
    if(qbind_blob(h->qb_get[hs][ndb], ":hash", hash, sizeof(*hash)))
	return FAIL_EINTERNAL;

    r = qstep(h->qb_get[hs][ndb]);
    if(r == SQLITE_DONE) {
	char thash[41];
	DEBUG("Hash not in database");
	sqlite3_reset(h->qb_get[hs][ndb]);
	bin2hex(hash->b, 20, thash, 41);
/*        WARN("{%s}: hash %s missing",
	     sx_node_internal_addr(sx_nodelist_get(h->nodes, h->thisnode)), thash);*/
	return ENOENT;
    }
    if(r != SQLITE_ROW) {
	sqlite3_reset(h->qb_get[hs][ndb]);
	return FAIL_EINTERNAL;
    }
    if(!block) {
	sqlite3_reset(h->qb_get[hs][ndb]);
	return OK;
    }
    dboff = sqlite3_column_int64(h->qb_get[hs][ndb], 0);
    sqlite3_reset(h->qb_get[hs][ndb]);
    dboff *= bs;

    if(read_block(h->datafd[hs][ndb], h->blockbuf, dboff, bs))
	return FAIL_EINTERNAL;

    *block = h->blockbuf;
    return OK;
}

static rc_ty sx_hashfs_hashop_ishash(sx_hashfs_t *h, unsigned hs, const sx_hash_t *hash)
{
    rc_ty ret;
    unsigned ndb;
    ndb = gethashdb(hash);
    sqlite3_reset(h->qb_get[hs][ndb]);
    if(qbind_blob(h->qb_get[hs][ndb], ":hash", hash, sizeof(*hash)))
        return FAIL_EINTERNAL;
    switch (qstep(h->qb_get[hs][ndb])) {
        case SQLITE_ROW:
            ret = OK;
            break;
        case SQLITE_DONE:
            ret = ENOENT;
            break;
        default:
            ret = FAIL_EINTERNAL;
            break;
    }
    sqlite3_reset(h->qb_get[hs][ndb]);
    return ret;
}

static rc_ty sx_hashfs_revision_op_internal(sx_hashfs_t *h, unsigned int hs, const sx_hash_t *revision_id, int op, int age)
{
    for (unsigned ndb=0;ndb<HASHDBS;ndb++) {
        sqlite3_stmt *q = h->qb_addtoken[hs][ndb];
        sqlite3_reset(q);
        if (qbind_blob(q, ":revision_id", revision_id->b, sizeof(revision_id->b)) ||
            qbind_int(q, ":op", op) ||
            qbind_int(q, ":age", age) ||
            qstep_noret(q)) {
            sqlite3_reset(q);
            return FAIL_EINTERNAL;
        }
        sqlite3_reset(q);
    }
    return OK;
}

rc_ty sx_hashfs_revision_op(sx_hashfs_t *h, unsigned blocksize, const sx_hash_t *revision_id, int op)
{
    unsigned hs;
    int age;
    for(hs = 0; hs < SIZES; hs++) {
        if(blocksize == bsz[hs])
            break;
    }

    if(hs == SIZES) {
        WARN("Failed to get hash size database, blocksize: %u", blocksize);
        return EINVAL;
    }
    age = sxi_hdist_version(h->hd);
    if (revision_id)
        DEBUGHASH("revision_op on", revision_id);
    if (datadb_begin(h, hs))
        return FAIL_EINTERNAL;
    if (sx_hashfs_revision_op_internal(h, hs, revision_id, op, age)) {
        datadb_rollback(h, hs);
        return FAIL_EINTERNAL;
    }
    if (datadb_commit(h, hs))
        return FAIL_EINTERNAL;
    DEBUG("ok");
    return OK;
}

static rc_ty sx_hashfs_hashop_moduse_internal(sx_hashfs_t *h, const sx_hash_t *revision_id, unsigned int hs, unsigned int ndb, const sx_hash_t *hash, unsigned replica, unsigned age)
{
    sqlite3_reset(h->qb_moduse[hs][ndb]);
    if (qbind_blob(h->qb_moduse[hs][ndb], ":hash", hash, sizeof(*hash)) ||
            qbind_int(h->qb_moduse[hs][ndb], ":replica", replica) ||
            qbind_int(h->qb_moduse[hs][ndb], ":age", age) ||
            qbind_blob(h->qb_moduse[hs][ndb], ":revision_id", revision_id, sizeof(*revision_id)) ||
            qstep_noret(h->qb_moduse[hs][ndb]))
        return FAIL_EINTERNAL;
    sqlite3_reset(h->qb_moduse[hs][ndb]);
    return OK;
}

static rc_ty sx_hashfs_hashop_moduse(sx_hashfs_t *h, const sx_hash_t *reserve_id, const sx_hash_t *revision_id, unsigned int hs, const sx_hash_t *hash, unsigned replica, int64_t op, uint64_t op_expires_at)
{
    unsigned ndb;
    unsigned int age;
    rc_ty ret = FAIL_EINTERNAL;
    /* FIXME: maybe enable this
    if(!is_hash_local(h, hash, replica_count))
	return ENOENT;*/

    if (!h || !hash) {
        NULLARG();
        return EFAULT;
    }
    if (hs >= SIZES) {
        WARN("invalid hash size: %d", hs);
        return EINVAL;
    }
    if (!replica) {
        msg_set_reason("replica zero is not valid");
        return EINVAL;
    }

    if (!revision_id) {
        msg_set_reason("missing revision id");
        return EINVAL;
    }
    ndb = gethashdb(hash);

    sqlite3_reset(h->qb_addtoken[hs][ndb]);
    sqlite3_reset(h->qb_reserve[hs][ndb]);
    sqlite3_reset(h->qb_moduse[hs][ndb]);
    DEBUG("moduse %lld", (long long)op);
    if (reserve_id)
        DEBUGHASH("reserve_id", reserve_id);
    if (revision_id)
        DEBUGHASH("revision_id", revision_id);
    if (qbegin(h->datadb[hs][ndb]))
        return FAIL_EINTERNAL;
    do {
        if (op) { /* +N or -N */
            if (reserve_id) {
                if (qbind_blob(h->qb_del_reserve[hs][ndb], ":reserve_id", reserve_id, sizeof(*reserve_id)) ||
                    qstep_noret(h->qb_del_reserve[hs][ndb]))
                   break;
            }
        } else {
            /* reserve */
            if (!reserve_id) {
                WARN("reserve without id!");
                break;
            }
            if (qbind_blob(h->qb_reserve[hs][ndb], ":revision_id", revision_id, sizeof(*revision_id)) ||
                qbind_blob(h->qb_reserve[hs][ndb], ":reserve_id", reserve_id, sizeof(*reserve_id)) ||
                qbind_int64(h->qb_reserve[hs][ndb], ":ttl", op_expires_at) ||
                qstep_noret(h->qb_reserve[hs][ndb]))
                break;
            sqlite3_reset(h->qb_reserve[hs][ndb]);
        }

        age = sxi_hdist_version(h->hd);
        if (qbind_blob(h->qb_addtoken[hs][ndb], ":revision_id", revision_id, sizeof(*revision_id)) ||
            qbind_int(h->qb_addtoken[hs][ndb], ":op", op) ||
            qbind_int(h->qb_addtoken[hs][ndb], ":age", age) ||
            qstep_noret(h->qb_addtoken[hs][ndb]))
            break;
        sqlite3_reset(h->qb_addtoken[hs][ndb]);
        DEBUGHASH("moduse on", hash);
        DEBUG("op: %ld, replica: %d, age: %d", op, replica, age);
        if (sx_hashfs_hashop_moduse_internal(h, revision_id, hs, ndb, hash, replica, age))
            break;
        if (qcommit(h->datadb[hs][ndb]))
            break;
        ret = OK;
    } while(0);
    if (ret)
        qrollback(h->datadb[hs][ndb]);
    return ret;
}

rc_ty sx_hashfs_hashop_perform(sx_hashfs_t *h, unsigned int block_size, unsigned replica_count, enum sxi_hashop_kind kind, const sx_hash_t *hash, const sx_hash_t *reserve_id, const sx_hash_t *revision_id, uint64_t op_expires_at, int *present)
{
    unsigned int hs;
    rc_ty rc;

    if (UNLIKELY(sxi_log_is_debug(&logger))) {
        char debughash[sizeof(sx_hash_t)*2+1];		\
        bin2hex(hash->b, sizeof(*hash), debughash, sizeof(debughash));	\
        DEBUG("processing %s, #%s#",
              kind == HASHOP_RESERVE ? "reserve" :
              kind == HASHOP_INUSE ? "inuse" :
              kind == HASHOP_DELETE ? "decuse" : "??",
              debughash);
        if (reserve_id)
            DEBUGHASH("reserve_id: ", reserve_id);
        if (revision_id)
            DEBUGHASH("revision_id: ", revision_id);
    }

    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == block_size)
	    break;
    if(hs == SIZES)
	return FAIL_BADBLOCKSIZE;

    switch (kind) {
        case HASHOP_CHECK:
            rc = sx_hashfs_hashop_ishash(h, hs, hash);
            break;
        case HASHOP_RESERVE:
            /* we must always reserve, even if ENOENT */
            rc = sx_hashfs_hashop_moduse(h, reserve_id, revision_id, hs, hash, replica_count, 0, op_expires_at);
            if (rc == OK)
                rc = sx_hashfs_hashop_ishash(h, hs, hash);
            break;
        case HASHOP_INUSE:
            rc = sx_hashfs_hashop_mod(h, hash, reserve_id, revision_id, block_size, replica_count, 1, op_expires_at);
            break;
        case HASHOP_DELETE:
            rc = sx_hashfs_hashop_mod(h, hash, reserve_id, revision_id, block_size, replica_count, -1, op_expires_at);
            break;

        default:
            msg_set_reason("Invalid hashop");
            return EINVAL;
    }
    DEBUG("result: %s", rc2str(rc));
    if(present)
	*present = rc == OK;
    if(rc == ENOENT)
	rc = OK;
    return rc;
}

rc_ty sx_hashfs_hashop_mod(sx_hashfs_t *h, const sx_hash_t *hash, const sx_hash_t *reserve_id, const sx_hash_t *revision_id, unsigned int bs, unsigned replica, int count, uint64_t op_expires_at)
{
    unsigned int hs;
    rc_ty rc;
    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    if(hs == SIZES) {
	WARN("bad blocksize: %d", bs);
	return FAIL_BADBLOCKSIZE;
    }
    rc = sx_hashfs_hashop_moduse(h, reserve_id, revision_id, hs, hash, replica, count, op_expires_at);
    if (rc == OK)
        rc = sx_hashfs_hashop_ishash(h, hs, hash);
    return rc;
}

rc_ty sx_hashfs_block_put(sx_hashfs_t *h, const uint8_t *data, unsigned int bs, unsigned int replica_count, int propagate) {
    sx_nodelist_t *belongsto;
    unsigned int ndb, hs;
    sx_hash_t hash;
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h->have_hd) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }

    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == bs)
	    break;
    if(hs == SIZES)
	return FAIL_BADBLOCKSIZE;

    if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), data, bs, &hash)) {
	WARN("hashing failed");
	return FAIL_EINTERNAL;
    }

    DEBUGHASH("Block uploaded by user", &hash);

    /* MODHDIST: lookup is strictly on bidx 0 */
    belongsto = sxi_hdist_locate(h->hd, MurmurHash64(&hash, sizeof(hash), HDIST_SEED), replica_count, 0);
    r = sx_nodelist_lookup(belongsto, &h->node_uuid) == NULL;
    sx_nodelist_delete(belongsto);
    if(r) {
	DEBUGHASH("Block doesn't belong to this node", &hash);
	return ENOENT;
    }

    ndb = gethashdb(&hash);

    sqlite3_reset(h->qb_get[hs][ndb]);
    if(qbind_blob(h->qb_get[hs][ndb], ":hash", &hash, sizeof(hash))) {
	WARN("binding hash failed");
	return FAIL_EINTERNAL;
    }

    r = qstep(h->qb_get[hs][ndb]);
    sqlite3_reset(h->qb_get[hs][ndb]);
    if(r == SQLITE_DONE) {
	int64_t dsto, next;

	if(qbegin(h->datadb[hs][ndb])) {
	    WARN("begin failed");
	    return FAIL_EINTERNAL;
	}

	sqlite3_reset(h->qb_nextavail[hs][ndb]);
	sqlite3_reset(h->qb_nextalloc[hs][ndb]);
	sqlite3_reset(h->qb_bumpavail[hs][ndb]);
	sqlite3_reset(h->qb_bumpalloc[hs][ndb]);
	sqlite3_reset(h->qb_add[hs][ndb]);

	r = qstep(h->qb_nextavail[hs][ndb]);
	if(r == SQLITE_ROW) {
	    next = sqlite3_column_int64(h->qb_nextavail[hs][ndb], 0);
	    sqlite3_reset(h->qb_nextavail[hs][ndb]);

	    if(qbind_int64(h->qb_bumpavail[hs][ndb], ":next", next) || qstep_noret(h->qb_bumpavail[hs][ndb])) {
		qrollback(h->datadb[hs][ndb]);
		WARN("bumpavail failed");
		return FAIL_EINTERNAL;
	    }
	} else if(r == SQLITE_DONE) {
	    r = qstep(h->qb_nextalloc[hs][ndb]);
	    if(r == SQLITE_ROW) {
		next = sqlite3_column_int64(h->qb_nextalloc[hs][ndb], 0);
		sqlite3_reset(h->qb_nextalloc[hs][ndb]);

		if(qstep_noret(h->qb_bumpalloc[hs][ndb])) {
		    qrollback(h->datadb[hs][ndb]);
		    WARN("bumpalloc failed");
		    return FAIL_EINTERNAL;
		}
	    }
	}

	if(r != SQLITE_ROW || qcommit(h->datadb[hs][ndb])) {
	    qrollback(h->datadb[hs][ndb]);
	    WARN("nextavail failed");
	    return FAIL_EINTERNAL;
	}

	dsto = next * bs;
	DEBUG("Block stored @%d/%d/%ld", hs, ndb, dsto);

	if(write_block(h->datafd[hs][ndb], data, dsto, bs)) {
	    WARN("write failed");
	    return FAIL_EINTERNAL;
	}

	/* insert it now */
	if(qbind_blob(h->qb_add[hs][ndb], ":hash", &hash, sizeof(hash)) ||
           qbind_int64(h->qb_add[hs][ndb], ":now", time(NULL)) ||
	   qbind_int64(h->qb_add[hs][ndb], ":next", next)) {
	    WARN("add failed");
	    return FAIL_EINTERNAL;
	}
	r = qstep(h->qb_add[hs][ndb]);
        DEBUG("r: %d, changes: %d", r, sqlite3_changes(h->datadb[hs][ndb]->handle));
        if (r == SQLITE_DONE && !sqlite3_changes(h->datadb[hs][ndb]->handle)) {
            DEBUG("checking for race condition");
            /* race condition */
            r = qstep(h->qb_get[hs][ndb]);
            sqlite3_reset(h->qb_get[hs][ndb]);
            if (r == SQLITE_ROW) {
		DEBUG("Race in block_store, falling back");
                ret = EAGAIN;
            } else if (r == SQLITE_DONE) {
                WARNHASH("Impossible: hash cannot be added but is not present either", &hash);
                ret = FAIL_EINTERNAL;
            } else
                ret = FAIL_EINTERNAL;
        } else
            ret = OK;
    } else if(r == SQLITE_ROW)
        ret = EAGAIN;
    if(ret != OK && ret != EAGAIN)
	return FAIL_EINTERNAL;

    if(propagate && replica_count > 1) {
	sx_nodelist_t *targets = sx_hashfs_effective_hashnodes(h, NL_NEXT, &hash, replica_count);
	rc_ty ret = sx_hashfs_xfer_tonodes(h, &hash, bs, targets);
	sx_nodelist_delete(targets);
	return ret;
    }
    return OK;
}

static void putfile_reinit(sx_hashfs_t *h) {
    if(!h)
	return;

    h->put_id = 0;
    h->put_putblock = 0;
    h->put_getblock = 0;
    h->put_checkblock = 0;
    h->put_replica = 0;
    h->put_hs = 0;
    h->put_success = 0;
    h->put_token[0] = '\0';
    h->put_blocks = NULL;
    h->put_nidxs = NULL;
    h->put_nblocks = 0;
    h->put_extendsize = -1LL;
    h->put_extendfrom = 0;
    h->nmeta = 0;
}

const char *sx_hashfs_geterrmsg(sx_hashfs_t *h)
{
    return sxc_geterrmsg(h->sx);
}

rc_ty sx_hashfs_createfile_begin(sx_hashfs_t *h) {
    if(!h) {
	NULLARG();
	return EFAULT;
    }

    putfile_reinit(h);

    h->put_id = -1; /* Fake id so that the blocks are actually accepted */
    return OK;
}

static rc_ty fill_filemeta(sx_hashfs_t *h, unsigned int metadb, int64_t file_id);
static rc_ty get_file_metasize(sx_hashfs_t *h, int64_t file_id, int64_t ndb, int64_t *size) {
    rc_ty s;
    int64_t metasize = 0;
    unsigned int i;

    if(!size) {
        NULLARG();
        return EFAULT;
    }

    if(ndb >= METADBS) {
        msg_set_reason("Failed to compute file meta size: invalid argument");
        return EFAULT;
    }

    if((s = fill_filemeta(h, ndb, file_id)) != OK) {
        msg_set_reason("Failed to compute file meta size");
        return s;
    }

    for(i=0; i<h->nmeta; i++)
        metasize += strlen(h->meta[i].key) + h->meta[i].value_len;

    *size = metasize;
    return OK;
}

rc_ty sx_hashfs_check_file_size(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, const char *filename, int64_t size) {
    int64_t revsz = 0;
    sqlite3_stmt *q;
    int mdb, r, nrevs;
    rc_ty ret = OK;

    if(!vol || size < 0 || !filename) {
        NULLARG();
        return FAIL_EINTERNAL;
    }

    if(vol->size < size) { /* Avoid subsequent int wraps */
	msg_set_reason("Quota exceeded");
	return ENOSPC;
    }

    if(vol->cursize <= vol->size - size)
	return OK;

    mdb = getmetadb(filename);
    if(mdb < 0) {
        WARN("Failed to get meta db for file name: %s", filename);
        msg_set_reason("Failed to get meta db for file name: %s", filename);
        return FAIL_EINTERNAL;
    }

    q = h->qm_oldrevs[mdb];
    sqlite3_reset(q);

    if(qbind_int64(q, ":volume", vol->id) || qbind_text(q, ":name", filename))
        return FAIL_EINTERNAL;

    r = qstep(q);
    if(r == SQLITE_DONE) {
	sqlite3_reset(q);
	msg_set_reason("Quota exceeded");
	return ENOSPC;
    }
    if(r != SQLITE_ROW) {
	msg_set_reason("Failed to check volume quota");
	return FAIL_EINTERNAL;
    }

    nrevs = sqlite3_column_int(q, 2);
    while(nrevs >= vol->revisions) {
        int64_t metasize = 0;

	revsz += sqlite3_column_int64(q, 1);
        if(get_file_metasize(h, sqlite3_column_int64(q, 3), mdb, &metasize)) {
            sqlite3_reset(q);
            WARN("Failed to get file meta size");
            msg_set_reason("Failed to check volume quota");
            return FAIL_EINTERNAL;
        }
        revsz += metasize + strlen(filename);

	if(revsz >= vol->cursize) {
	    revsz = vol->cursize;
	    break;
	}

	nrevs--;
	if(!nrevs)
	    break;

	r = qstep(q);
	if(r != SQLITE_ROW) {
            sqlite3_reset(q);
	    msg_set_reason("Failed to check volume quota");
	    return FAIL_EINTERNAL;
	}
    }
    sqlite3_reset(q);

    if(ret == OK && vol->cursize - revsz > vol->size - size) {
	msg_set_reason("Quota exceeded");
	ret = ENOSPC;
    }
    return ret;
}

int64_t sx_hashfs_get_node_push_time(sx_hashfs_t *h, const sx_node_t *n) {
    int64_t timestamp;
    int r;

    if(!h || !n)
        return -1;

    sqlite3_reset(h->q_getnodepushtime);
    if(qbind_blob(h->q_getnodepushtime, ":node", sx_node_uuid(n), UUID_BINARY_SIZE)) {
        WARN("Failed to prepare query for getting last push timestamp for node %s", sx_node_addr(n));
        sqlite3_reset(h->q_getnodepushtime);
        return -1;
    }

    r = qstep(h->q_getnodepushtime);
    if(r == SQLITE_DONE) {
        /* No row found, no push yet */
        timestamp = 0;
    } else if(r == SQLITE_ROW) {
        /* Found a push timestamp, get it */
        timestamp = sqlite3_column_int64(h->q_getnodepushtime, 0);
    } else {
        WARN("Failed to get last push timestamp");
        timestamp = -1;
    }

    sqlite3_reset(h->q_getnodepushtime);
    return timestamp;
}

int sx_hashfs_is_volume_to_push(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, const sx_node_t *node) {
    if(!h || !vol || !h->have_hd || !node)
        return 0;

    /* If this is not my volume on PREV distribution, then I should not push volume size to other nodes */
    if(!sx_hashfs_is_node_volume_owner(h, NL_PREV, sx_hashfs_self(h), vol))
        return 0;

    /* If cluster is rebalancing, then we should send volume sizes to new volnodes as well as to non-volnodes */
    if(!sx_hashfs_is_node_volume_owner(h, NL_PREV, node, vol))
        return 1;

    return 0;
}

rc_ty sx_hashfs_update_node_push_time(sx_hashfs_t *h, const sx_node_t *n) {
    rc_ty ret = FAIL_EINTERNAL;

    if(!h || !n)
        return FAIL_EINTERNAL;

    /* Update push time */
    sqlite3_reset(h->q_setnodepushtime);
    if(qbind_int64(h->q_setnodepushtime, ":now", time(NULL))
       || qbind_blob(h->q_setnodepushtime, ":node", sx_node_uuid(n), UUID_BINARY_SIZE)
       || qstep_noret(h->q_setnodepushtime)) {
        WARN("Failed to update node push timestamp");
        goto update_node_push_time_err;
    }

    ret = OK;
    update_node_push_time_err:
    sqlite3_reset(h->q_setnodepushtime);
    return ret;
}

struct timeval* sx_hashfs_volsizes_timestamp(sx_hashfs_t *h) {
    return &h->volsizes_push_timestamp;
}

rc_ty sx_hashfs_putfile_begin(sx_hashfs_t *h, sx_uid_t user_id, const char *volume, const char *file, const sx_hashfs_volume_t **volptr) {
    uint8_t rnd[TOKEN_RAND_BYTES];
    const sx_hashfs_volume_t *vol;
    int flen;
    rc_ty r;

    putfile_reinit(h);

    flen = check_file_name(file);
    if(!h || flen < 0 || file[flen - 1] == '/')
	return EINVAL;

    if((r = sx_hashfs_volume_by_name(h, volume, &vol)))
	return r;

    if(volptr)
        *volptr = vol;

    if(!sx_hashfs_is_or_was_my_volume(h, vol))
	return ENOENT;

    sqlite3_reset(h->qt_new);
    /* non-blocking pseudo-random bytes, i.e. we don't want to block or deplete
     * entropy as we only need a unique sequence of bytes, not a secret one as
     * it is sent in plaintext anyway, and signed with an HMAC */
    if (sxi_rand_pseudo_bytes(rnd, sizeof(rnd)) == -1) {
	/* can also return 0 or 1 but that doesn't matter here */
	WARN("Cannot generate random bytes");
	return FAIL_EINTERNAL;
    }
    if(qbind_int64(h->qt_new, ":volume", vol->id) || qbind_text(h->qt_new, ":name", file) ||
       qbind_blob(h->qt_new, ":random", rnd, sizeof(rnd)) || qstep_noret(h->qt_new)) {
	sqlite3_reset(h->qt_new);
	return FAIL_EINTERNAL;
    }
    sqlite3_reset(h->qt_new);

    h->put_id = sqlite3_last_insert_rowid(sqlite3_db_handle(h->qt_new));
    h->put_replica = vol->max_replica;
    return sx_hashfs_countjobs(h, user_id);
}

rc_ty sx_hashfs_putfile_extend_begin(sx_hashfs_t *h, sx_uid_t user_id, const uint8_t *user, const char *token) {
    const sx_hashfs_volume_t *vol;
    const sx_uuid_t *self_uuid;
    struct token_data tkdt;
    const sx_node_t *self;
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h)
	return EINVAL;

    putfile_reinit(h);

    if(parse_token(h->sx, user, token, &h->tokenkey, &tkdt))
	return EINVAL;

    if(!(self = sx_hashfs_self(h)) || !(self_uuid = sx_node_uuid(self)))
	return FAIL_EINTERNAL;
    if(memcmp(self_uuid->binary, tkdt.uuid.binary, sizeof(tkdt.uuid.binary)))
	return EINVAL;

    sqlite3_reset(h->qt_tokendata);
    if(qbind_text(h->qt_tokendata, ":token", tkdt.token))
	goto putfile_extend_err;

    r = qstep(h->qt_tokendata);
    if(r == SQLITE_DONE)
	ret = ENOENT;
    if(r != SQLITE_ROW)
	goto putfile_extend_err;

    if((ret = sx_hashfs_volume_by_id(h, sqlite3_column_int64(h->qt_tokendata, 2), &vol)))
	goto putfile_extend_err;

    h->put_id = sqlite3_column_int64(h->qt_tokendata, 0);
    h->put_replica = vol->max_replica;
    h->put_extendsize = sqlite3_column_int64(h->qt_tokendata, 1);
    h->put_extendfrom = sqlite3_column_bytes(h->qt_tokendata, 4) / sizeof(sx_hash_t);
    ret = sx_hashfs_countjobs(h, user_id);

    putfile_extend_err:
    sqlite3_reset(h->qt_tokendata);
    return ret;
}

rc_ty sx_hashfs_putfile_putblock(sx_hashfs_t *h, sx_hash_t *hash) {
    if(!h || !hash || !h->put_id)
	return EINVAL;

    if(h->put_putblock >= h->put_nblocks) {
	h->put_nblocks += 128;
	h->put_blocks = wrap_realloc_or_free(h->put_blocks, sizeof(*hash) * h->put_nblocks);
	if(!h->put_blocks)
	    return FAIL_EINTERNAL;
    }
    memcpy(&h->put_blocks[h->put_putblock], hash, sizeof(*hash));
    h->put_putblock++;
    return OK;
}

rc_ty sx_hashfs_putfile_putmeta(sx_hashfs_t *h, const char *key, const void *value, unsigned int value_len) {
    rc_ty rc;

    if(!h)
	return FAIL_EINTERNAL;

    if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	return EOVERFLOW;

    if(!value) {
	/* Delete key: check key (and bogus value) */
	char checkme[SXLIMIT_META_MIN_VALUE_LEN + 1];
	memset(checkme, 0, sizeof(checkme));
	rc = sx_hashfs_check_meta(key, checkme, SXLIMIT_META_MIN_VALUE_LEN);
	if(rc)
	    return rc;
	h->meta[h->nmeta].value_len = -1;
    } else {
	/* Add/replace key: check key and value */
	rc = sx_hashfs_check_meta(key, value, value_len);
	if(rc)
	    return rc;
	memcpy(h->meta[h->nmeta].value, value, value_len);
	h->meta[h->nmeta].value_len = value_len;
    }

    memcpy(h->meta[h->nmeta].key, key, strlen(key)+1);

    h->nmeta++;
    return OK;
}

struct sort_by_node_t {
    sx_hash_t *hashes;
    unsigned int *nidxs;
    unsigned int sort_replica;
    unsigned int replica_count;
};

static int sort_by_node_func(const void *thunk, const void *a, const void *b) {
     unsigned int hashno_a = *(unsigned int *)a;
     unsigned int hashno_b = *(unsigned int *)b;
     const struct sort_by_node_t *support = (const struct sort_by_node_t *)thunk;

     int nidxa = support->nidxs[support->replica_count * hashno_a + support->sort_replica - 1];
     int nidxb = support->nidxs[support->replica_count * hashno_b + support->sort_replica - 1];

     /* Sort by node first */
     if(nidxa < nidxb)
	 return -1;
     if(nidxa > nidxb)
	 return 1;

     /* Then either sort by hash, if so requested */
     if(support->hashes)
	 return memcmp(&support->hashes[hashno_a], &support->hashes[hashno_b], sizeof(support->hashes[0]));

     /* Or alternatively sort by position */
     if(hashno_a < hashno_b)
	 return -1;
     if(hashno_a > hashno_b)
	 return 1;
     return 0;
}

static void sort_by_node_then_hash(sx_hash_t *hashes, unsigned int *hashnos, unsigned int *nidxs, unsigned int items, unsigned int replica, unsigned int replica_count) {
    struct sort_by_node_t sortsupport = {hashes, nidxs, replica, replica_count};
    sx_qsort(hashnos, items, sizeof(*hashnos), &sortsupport, sort_by_node_func);
}

static void sort_by_node_then_position(unsigned int *hashnos, unsigned int *nidxs, unsigned int items, unsigned int replica, unsigned int replica_count) {
    struct sort_by_node_t sortsupport = {NULL, nidxs, replica, replica_count};
    sx_qsort(hashnos, items, sizeof(*hashnos), &sortsupport, sort_by_node_func);
}

static int sort_by_hash_func(const void *thunk, const void *a, const void *b) {
    unsigned int ia = *(const unsigned int *)a;
    unsigned int ib = *(const unsigned int *)b;
    const sx_hash_t *hashes = (const sx_hash_t *)thunk;
    return cmphash(&hashes[ia], &hashes[ib]);
}

static void build_uniq_hash_index(const sx_hash_t *hashes, unsigned *idxs, unsigned *n)
{
    unsigned i, l, r;
    /* Build an index and an array of first nodes */
    for (i=0; i<*n; i++)
        idxs[i] = i;
    /* Sort by hash */
    sx_qsort(idxs, *n, sizeof(*idxs), hashes, sort_by_hash_func);
    /* Remove duplicates */
    for(l = 0, r = 1; r<*n; r++) {
        if(cmphash(&hashes[idxs[l]], &hashes[idxs[r]])) {
            l++;
            if(l!=r)
                idxs[l] = idxs[r];
        }
    }
    *n = l+1;
}

/* TODO: use volume name */
static int reserve_fileid(sx_hashfs_t *h, int64_t volume_id, const char *name, sx_hash_t *hash)
{
    int ret = 0;
    sxi_md_ctx *hash_ctx = sxi_md_init();
    if (!hash_ctx)
        return 1;
    if (!sxi_sha1_init(hash_ctx))
        return 1;

    if (!sxi_sha1_update(hash_ctx, h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary)) ||
        !sxi_sha1_update(hash_ctx, &volume_id, sizeof(volume_id)) ||
        !sxi_sha1_update(hash_ctx, name, strlen(name) + 1) ||
        !sxi_sha1_final(hash_ctx, hash->b, NULL)) {
        ret = 1;
    }

    sxi_md_cleanup(&hash_ctx);
    return ret;
}

int sx_unique_fileid(sxc_client_t *sx, const sx_hashfs_volume_t *volume, const char *name, const char *revision, sx_hash_t *fileid)
{
    int ret = 0;
    if (!name || !revision || !fileid) {
        NULLARG();
        return 1;
    }
    sxi_md_ctx *hash_ctx = sxi_md_init();
    if (!hash_ctx)
        return 1;
    if (!sxi_sha1_init(hash_ctx))
        return 1;

    DEBUG("fileid input: %s, %s, %s", volume->name, name, revision);
    if (!sxi_sha1_update(hash_ctx, volume->name, strlen(volume->name) + 1) ||
        !sxi_sha1_update(hash_ctx, name, strlen(name) + 1) ||
        !sxi_sha1_update(hash_ctx, revision, strlen(revision)) ||
        !sxi_sha1_final(hash_ctx, fileid->b, NULL)) {
        ret = 1;
    }
    DEBUGHASH("FILEID", fileid);

    sxi_md_cleanup(&hash_ctx);
    return ret;
}

rc_ty sx_hashfs_putfile_gettoken(sx_hashfs_t *h, const uint8_t *user, int64_t size_or_seq, const char **token, hash_presence_cb_t hdck_cb, void *hdck_cb_ctx) {
    const char *ptr;
    sqlite3_stmt *q;
    unsigned int i;
    uint64_t total_blocks;
    rc_ty ret = FAIL_EINTERNAL;
    unsigned int blocksize;
    int64_t expires_at;

    if(!h || !h->put_id)
	return EINVAL;

    if(h->put_extendsize < 0) {
	/* creating */
	if(size_or_seq < SXLIMIT_MIN_FILE_SIZE || size_or_seq > SXLIMIT_MAX_FILE_SIZE) {
	    msg_set_reason("Cannot obtain upload token: file size must be between %llu and %llu bytes", SXLIMIT_MIN_FILE_SIZE, SXLIMIT_MAX_FILE_SIZE);
	    return EINVAL;
	}
	q = h->qt_update;
	total_blocks = size_to_blocks(size_or_seq, &h->put_hs, &blocksize);
	/* calculate expiry time of token proportional to the amount of data
	 * uploaded with _this_ token, i.e. we issue a new token for an extend.
	 * */
	sqlite3_reset(q);
	expires_at = time(NULL) + GC_GRACE_PERIOD + blocksize * total_blocks / h->upload_minspeed
            + GC_MIN_LATENCY * size_or_seq / UPLOAD_CHUNK_SIZE / 1000;
	if(qbind_int64(q, ":expiry", expires_at))
	    return FAIL_EINTERNAL;
    } else {
	/* extending */
	if(size_or_seq != h->put_extendfrom) {
	    msg_set_reason("Cannot obtain upload token: out of sequence");
	    return EINVAL;
	}
	q = h->qt_extend;
	total_blocks = size_to_blocks(h->put_extendsize, &h->put_hs, &blocksize);
	size_or_seq *= sizeof(sx_hash_t);
	sqlite3_reset(q);
    }

    if(h->put_putblock + h->put_extendfrom > total_blocks) {
	msg_set_reason("Cannot obtain upload token: cannot extend beyond the file size");
	return EINVAL;
    }

    if(qbind_int64(q, ":id", h->put_id) ||
       qbind_int64(q, ":size", size_or_seq))
	goto gettoken_err;

    if(h->put_putblock) {
	if(qbind_blob(q, ":all", h->put_blocks, sizeof(h->put_blocks[0]) * h->put_putblock))
	    goto gettoken_err;
	h->put_nidxs = wrap_malloc(h->put_putblock * sizeof(h->put_nidxs[0]) * 2);
	if(!h->put_nidxs) {
	    OOM();
	    goto gettoken_err;
	}
	h->put_hashnos = &h->put_nidxs[h->put_putblock];
        build_uniq_hash_index(h->put_blocks, h->put_hashnos, &h->put_putblock);

	/* Lookup first node */
	for(i=0; i<h->put_putblock; i++) {
	    /* We (via hash_nidx_tobuf) check presence on the ultimate target nodes, i.e. NL_NEXT */
	    if(hash_nidx_tobuf(h, &h->put_blocks[h->put_hashnos[i]], 1, h->put_replica, &h->put_nidxs[h->put_hashnos[i]]) < 1)
		goto gettoken_err;
	}
	if(h->put_putblock > 1) {
	    /* Group by node, then by original position
	     * so that we second the readahead instead of fighting it */
	    sort_by_node_then_position(h->put_hashnos, h->put_nidxs, h->put_putblock, 1, 1);
	}
	if(h->put_extendfrom) {
	    for(i=0; i<h->put_putblock; i++)
		h->put_hashnos[i] += h->put_extendfrom;
	}
	if(qbind_blob(q, ":uniq", h->put_hashnos, sizeof(h->put_hashnos[0]) * h->put_putblock))
	    goto gettoken_err;
	if(h->put_extendfrom) {
	    for(i=0; i<h->put_putblock; i++)
		h->put_hashnos[i] -= h->put_extendfrom;
	}
    } else {
	if(qbind_blob(q, ":all", "", 0) || qbind_blob(q, ":uniq", "", 0))
	    goto gettoken_err;
    }

    if(qstep_noret(q))
	goto gettoken_err;
    sqlite3_reset(q);

    if(h->nmeta) {
	int items;
	for(i=0; i<h->nmeta; i++) {
	    sqlite3_stmt *q;
	    if(h->meta[i].value_len < 0)
		q = h->qt_delmeta;
	    else
		q = h->qt_addmeta;

	    sqlite3_reset(q);
	    if(qbind_int64(q, ":id", h->put_id) ||
	       qbind_text(q, ":key", h->meta[i].key) ||
	       (h->meta[i].value_len >=0 && qbind_blob(q, ":value", h->meta[i].value, h->meta[i].value_len)) ||
	       qstep_noret(q))
		goto gettoken_err;
	    sqlite3_reset(q);
	}
	sqlite3_reset(h->qt_countmeta);
	if(qbind_int64(h->qt_countmeta, ":id", h->put_id) ||
	   qstep_ret(h->qt_countmeta)) {
	    sqlite3_reset(h->qt_countmeta);
	    goto gettoken_err;
	}

	items = sqlite3_column_int(h->qt_countmeta, 0);
	sqlite3_reset(h->qt_countmeta);
	if(items > SXLIMIT_META_MAX_ITEMS) {
	    ret = EOVERFLOW;
	    goto gettoken_err;
	}
    }

    sqlite3_reset(h->qt_gettoken);
    if(qbind_int64(h->qt_gettoken, ":id", h->put_id) || qstep_ret(h->qt_gettoken))
	goto gettoken_err;

    ptr = (const char *)sqlite3_column_text(h->qt_gettoken, 0);
    expires_at = sqlite3_column_int64(h->qt_gettoken, 1);
    if(sx_hashfs_make_token(h, user, ptr, h->put_replica, expires_at, token))
	goto gettoken_err;


    int volid = sqlite3_column_int64(h->qt_gettoken, 2);
    const char *name = (const char*)sqlite3_column_text(h->qt_gettoken, 3);
    const char *revision = (const char*)sqlite3_column_text(h->qt_gettoken, 4);
    const sx_hashfs_volume_t *vol;
    if (reserve_fileid(h, volid, name, &h->put_reserve_id) ||
        sx_hashfs_volume_by_id(h, volid, &vol) ||
        sx_unique_fileid(h->sx, vol, name, revision, &h->put_revision_id))
        goto gettoken_err;
    DEBUGHASH("file initial PUT reserve_id", &h->put_reserve_id);
    sxi_hashop_begin(&h->hc, h->sx_clust, hdck_cb, HASHOP_RESERVE, vol->max_replica, &h->put_reserve_id, &h->put_revision_id, hdck_cb_ctx, expires_at);
    sqlite3_reset(h->qt_gettoken);
    return OK;

    gettoken_err:
    sqlite3_reset(h->qt_addmeta);
    sqlite3_reset(h->qt_delmeta);
    sqlite3_reset(q);
    sqlite3_reset(h->qt_gettoken);
    return ret;
}

static rc_ty delete_old_revs_common(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *name, const char *revision, int make_place, unsigned int *deletes_scheduled) {
    int r;
    int mdb;
    unsigned int scheduled = 0;

    if(!volume || !name) {
        NULLARG();
        return EINVAL;
    }

    mdb = getmetadb(name);
    if(mdb < 0 || mdb >= METADBS) {
        WARN("Failed to get meta db index");
        return FAIL_EINTERNAL;
    }

    /* Count old file revisions */
    sqlite3_reset(h->qm_oldrevs[mdb]);
    if(qbind_int64(h->qm_oldrevs[mdb], ":volume", volume->id) ||
       qbind_text(h->qm_oldrevs[mdb], ":name", name))
        return FAIL_EINTERNAL;

    r = qstep(h->qm_oldrevs[mdb]);
    if(r == SQLITE_ROW) {
        unsigned int nrevs = sqlite3_column_int(h->qm_oldrevs[mdb], 2);
        rc_ty rc = OK;
        job_t job = JOB_NOPARENT;

        /* There are some revs */
        while(nrevs >= volume->revisions + (make_place ? 0 : 1)) {
            const char *tooold_rev = (const char *)sqlite3_column_text(h->qm_oldrevs[mdb], 0);

            if(!tooold_rev) {
                WARN("NULL old revision");
                msg_set_reason("Invalid old revision");
                break;
            }

            if(revision) { /* If revision is given, then check if it is not outdated */
                if(strcmp(revision, (const char *)sqlite3_column_text(h->qm_oldrevs[mdb], 0)) < 0) {
                    msg_set_reason("Newer copies of this file already exist");
                    rc = EINVAL;
                    break;
                }
            }

            rc = sx_hashfs_filedelete_job(h, 0, volume, name, tooold_rev, &job);
            if(rc == EEXIST)
                rc = OK;
            else if(rc) {
                msg_set_reason("Failed to mark older file revision '%s' for deletion", tooold_rev);
                break;
            }

            scheduled++;
            nrevs--;
            if(!nrevs)
                break;

            r = qstep(h->qm_oldrevs[mdb]);
            if(r != SQLITE_ROW) {
                msg_set_reason("There was a problem enumerating current revisions of the file");
                rc = FAIL_EINTERNAL;
                break;
            }
        }

        sqlite3_reset(h->qm_oldrevs[mdb]);
        if(rc)
            return rc;
        /* Yay we have a slot now */
    } else {
        sqlite3_reset(h->qm_oldrevs[mdb]);
        if(r != SQLITE_DONE) /* Something didn't quite work */
            return FAIL_EINTERNAL;
        /* There are no existing revs */
    }

    if(deletes_scheduled)
        *deletes_scheduled = scheduled;
    return OK;
}

rc_ty sx_hashfs_delete_old_revs(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *name, unsigned int *deletes_scheduled) {
    return delete_old_revs_common(h, volume, name, NULL, 0, deletes_scheduled);
}

/* WARNING: MUST BE CALLED WITHIN A TANSACTION ON META !!! */
static rc_ty create_file(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *name, const char *revision, sx_hash_t *blocks, unsigned int nblocks, int64_t size, int64_t totalsize, int64_t *file_id) {
    unsigned int nblocks2;
    int r, mdb;
    sqlite3_stmt *q;

    if(!h || !volume || !name || !revision || (!blocks && nblocks)) {
	NULLARG();
	return EFAULT;
    }

    if(check_file_name(name)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    if(check_revision(revision)) {
	msg_set_reason("Invalid revision");
	return EINVAL;
    }

    nblocks2 = size_to_blocks(size, NULL, NULL);
    if(nblocks != nblocks2) {
	WARN("Inconsistent size: %u blocks given, %u expected", nblocks, nblocks2);
	return EFAULT;
    }

    mdb = getmetadb(name);
    if(mdb < 0) {
	msg_set_reason("Failed to locate file database");
	return FAIL_EINTERNAL;
    }

    q = h->qm_getrev[mdb];
    sqlite3_reset(q);
    if(qbind_int64(q, ":volume", volume->id)
       || qbind_text(q, ":name", name)
       || qbind_text(q, ":revision", revision)) {
        msg_set_reason("Failed to check revision existence");
        sqlite3_reset(q);
        return FAIL_EINTERNAL;
    }

    r = qstep(q);
    if(r == SQLITE_ROW) {
        DEBUG("File '%s (%s)' on volume '%s' is already here", name, revision, volume->name);
        sqlite3_reset(q);
        return EEXIST;
    } else if(r != SQLITE_DONE) {
        msg_set_reason("Failed to check revision existence");
        sqlite3_reset(q);
        return FAIL_EINTERNAL;
    }
    sqlite3_reset(q);

    /* Drop old exisiting revisions of the file */
    if(delete_old_revs_common(h, volume, name, revision, 1, NULL) != OK) {
        WARN("Failed to remove old revisions");
        return FAIL_EINTERNAL;
    }

    sx_hash_t revision_id;
    sqlite3_reset(h->qm_ins[mdb]);
    if(qbind_int64(h->qm_ins[mdb], ":volume", volume->id) ||
       qbind_text(h->qm_ins[mdb], ":name", name) ||
       qbind_text(h->qm_ins[mdb], ":revision", revision) ||
       sx_unique_fileid(sx_hashfs_client(h), volume, name, revision, &revision_id) ||
       qbind_blob(h->qm_ins[mdb], ":revision_id", &revision_id, sizeof(revision_id)) ||
       qbind_int64(h->qm_ins[mdb], ":size", size) ||
       qbind_int64(h->qm_ins[mdb], ":age", sxi_hdist_version(h->hd)) ||
       qbind_blob(h->qm_ins[mdb], ":hashes", nblocks ? (const void *)blocks : "", nblocks * sizeof(blocks[0]))) {
	WARN("Failed to create file '%s' on volume '%s'", name, volume->name);
	sqlite3_reset(h->qm_ins[mdb]);
	return FAIL_EINTERNAL;
    }

    if (qstep_noret(h->qm_ins[mdb])) {
	WARN("Failed to create file '%s' on volume '%s'", name, volume->name);
	return FAIL_EINTERNAL;
    }
    sqlite3_reset(h->qm_ins[mdb]);

    if(file_id)
	*file_id = sqlite3_last_insert_rowid(sqlite3_db_handle(h->qm_ins[mdb]));

    /* Update volume size counter only when size is positive and this node is not becoming a volnode */
    if(sx_hashfs_update_volume_cursize(h, volume->id, totalsize)) {
        WARN("Failed to update volume size");
        return FAIL_EINTERNAL;
    }

    return OK;
}

rc_ty sx_hashfs_createfile_commit(sx_hashfs_t *h, const char *volume, const char *name, const char *revision, int64_t size) {
    const sx_hashfs_volume_t *vol;
    unsigned int i, nblocks;
    int64_t file_id, totalsize;
    int mdb, flen;
    rc_ty ret = FAIL_EINTERNAL, ret2;

    if(!h || !name || !revision || h->put_id != -1) {
	NULLARG();
	return EFAULT;
    }

    if(check_revision(revision)) {
	msg_set_reason("Invalid revision");
	return EINVAL;
    }

    flen = check_file_name(name);
    if(flen < 0 || name[flen - 1] == '/') {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    nblocks = size_to_blocks(size, NULL, NULL);
    if(h->put_putblock != nblocks) {
	msg_set_reason("Blocks do not match the file size");
	return EINVAL;
    }

    if((ret2 = sx_hashfs_volume_by_name(h, volume, &vol)))
	return ret2;

    if(!sx_hashfs_is_or_was_my_volume(h, vol)) {
	msg_set_reason("This volume does not belong here");
	return ENOENT;
    }

    mdb = getmetadb(name);
    if(mdb < 0) {
	msg_set_reason("Failed to locate file database");
	return FAIL_EINTERNAL;
    }

    totalsize = size + strlen(name);
    if(qbegin(h->metadb[mdb]))
	return FAIL_EINTERNAL;

    /* Increment file size with size of its meta values and keys */
    for(i=0; i<h->nmeta; i++) {
        totalsize += strlen(h->meta[i].key) + h->meta[i].value_len;
    }

    ret2 = create_file(h, vol, name, revision, h->put_blocks, nblocks, size, totalsize, &file_id);
    if(ret2) {
	ret = ret2;
	goto cretatefile_rollback;
    }

    for(i=0; i<h->nmeta; i++) {
	sqlite3_reset(h->qm_metaset[mdb]);
	if(qbind_int64(h->qm_metaset[mdb], ":file", file_id) ||
	   qbind_text(h->qm_metaset[mdb], ":key", h->meta[i].key) ||
	   qbind_blob(h->qm_metaset[mdb], ":value", h->meta[i].value, h->meta[i].value_len) ||
	   qstep_noret(h->qm_metaset[mdb]))
	    break;
    }
    sqlite3_reset(h->qm_metaset[mdb]);
    if(i != h->nmeta)
	goto cretatefile_rollback;

    if(qcommit(h->metadb[mdb]))
	goto cretatefile_rollback;

    ret = OK;

 cretatefile_rollback:
    if(ret != OK)
	qrollback(h->metadb[mdb]);

    if(ret == EEXIST)
	ret = OK;
    sx_hashfs_createfile_end(h);

    return ret;
}

static rc_ty are_blocks_available(sx_hashfs_t *h, sx_hash_t *hashes,
				  sxi_hashop_t *hdck,
				  unsigned int *hashnos, unsigned int *nidxs,
				  unsigned int *current, unsigned int count,
				  unsigned int hash_size, unsigned int check_replica,
				  unsigned int replica_count) {
    /* h: main hashfs
     * hashes: complete array of hashes, unsorted
     * hashnos: array of hash indexes sorted by node id
     * nidxs: array of node indexes
     * current: current hash to be checked
     * count: number of hashes in the list (also the numer of items in hashnos the number of items per replica in nidxs)
     * hash_size: the size (small, medium, big) of the hashes in hashes
     * check_replica: which set to check (1 <= check_replica <= replica_count)
     * replica_count: the number of replica sets (max replica!)
     */

    /* MODHDIST:
     * this is a major PITA. With the current api (and callbacks) we can only check presence
     * on a single set, which, for functional reasons outta be the _next set.
     * In practice this means that the clients (i.e. cluster users) effectively help the
     * migration process with their bandwith. This also means that the clients (i.e. the users) might
     * experience unexpected spikes in resource usage for reasons which are outside their control.
     * A better approach would be to check presence on _prev, then _next and merge the results.
     * This is however not possible without a major api rework :(
     */
    const sx_nodelist_t *nodes = sx_hashfs_all_nodes(h, NL_NEXT);
    unsigned int check_item = *current;
    unsigned int thisnode, nextnode, prevnode = nidxs[replica_count * hashnos[check_item] + check_replica - 1];

    if(!count)
	return OK;

    if(check_item >= count) {
	WARN("bad check_item: %d >= %d", check_item, count);
	return FAIL_EINTERNAL;
    }

    /* hash is local */
    if(sx_nodelist_lookup_index(nodes, &h->node_uuid, &thisnode) && prevnode == thisnode) {
	sx_hash_t *hash = &hashes[hashnos[check_item]];
	unsigned int ndb = gethashdb(hash);
	int r;
        rc_ty rc;

	sqlite3_reset(h->qb_get[hash_size][ndb]);
	if(qbind_blob(h->qb_get[hash_size][ndb], ":hash", hash, sizeof(*hash))) {
	    WARN("qbind_blob failed");
	    sqlite3_reset(h->qb_get[hash_size][ndb]);
	    return FAIL_EINTERNAL;
	}
	r = qstep(h->qb_get[hash_size][ndb]);
	sqlite3_reset(h->qb_get[hash_size][ndb]);

	*current = check_item+1;
        if (sxi_hashop_batch_flush(hdck))
            WARN("Failed to query hash: %s", sxc_geterrmsg(h->sx));
        rc = sx_hashfs_hashop_perform(h, bsz[hash_size], hdck->replica, hdck->kind, hash, &hdck->reserve_id, &hdck->revision_id, hdck->op_expires_at, NULL);
        if(rc != OK) {
            WARN("hashop_perform/finish failed: %s", rc2str(rc));
            return rc;
        }
	if (hdck->cb) {
	    int code = r == SQLITE_ROW ? 200 : 404;
	    char thash[SXI_SHA1_TEXT_LEN + 1];
	    if (bin2hex(hash->b, SXI_SHA1_BIN_LEN, thash, sizeof(thash))) {
		WARN("bin2hex failed for hash");
		return FAIL_EINTERNAL;
	    }
	    if (hdck->cb(thash, check_item, code, hdck->context) == -1) {
		WARN("callback returned failure");
		return FAIL_EINTERNAL;
	    }
	}
	return OK;
    }

    const sx_node_t *node = sx_nodelist_get(nodes, prevnode);
    if(!node) {
	WARN("failed to get nodelist");
	return FAIL_EINTERNAL;
    }
    if(sx_nodelist_lookup(h->ignored_nodes, sx_node_uuid(node))) {
	sx_hash_t *hash = &hashes[hashnos[check_item]];
	*current = check_item+1;
        if (sxi_hashop_batch_flush(hdck))
            WARN("Failed to query hash: %s", sxc_geterrmsg(h->sx));
	if (hdck->cb) {
	    char thash[SXI_SHA1_TEXT_LEN + 1];
	    if (bin2hex(hash->b, SXI_SHA1_BIN_LEN, thash, sizeof(thash))) {
		WARN("bin2hex failed for hash");
		return FAIL_EINTERNAL;
	    }
	    if (hdck->cb(thash, check_item, 444, hdck->context) == -1) {
		WARN("callback returned failure");
		return FAIL_EINTERNAL;
	    }
	}
	return OK;
    }

    const char *host = sx_node_internal_addr(node);
    DEBUG("preparing request to %s, node #%d, replica count: %d, current replica: %d", host, prevnode, replica_count,
          check_replica);
    DEBUG("nodelist:");
    for (unsigned i=0;i<sx_nodelist_count(nodes);i++) {
        DEBUG("node #%d: %s", i, sx_node_internal_addr(sx_nodelist_get(nodes, i)));
    }
    do {
        if (sxi_hashop_batch_add(hdck, host, check_item, hashes[hashnos[check_item]].b, bsz[hash_size])) {
            WARN("Failed to query hash on %s: %s", host, sxc_geterrmsg(h->sx));
        }
        DEBUGHASH("preparing query for hash", &hashes[hashnos[check_item]]);
        DEBUG("index: %d, blockno: %d", check_item, hashnos[check_item]);
	check_item++;
	if(check_item >= count)
	    break; /* end of set for this replica */
	nextnode = nidxs[replica_count * hashnos[check_item] + check_replica - 1];
    } while (nextnode == prevnode); /* end of current node */

    *current = check_item;
    return OK;
}

static rc_ty reserve_replicas(sx_hashfs_t *h, uint64_t op_expires_at)
{
    /*
     * assign more understandable names to pointers and counters
     */
    rc_ty ret = OK;
    sx_hash_t *all_hashes = h->put_blocks;
    sxi_hashop_t *hashop = &h->hc;
    unsigned int *uniq_hash_indexes = h->put_hashnos;
    unsigned uniq_count = h->put_putblock;
    if (!uniq_count)
        return OK;
    unsigned hash_size = h->put_hs, effective_replica = h->put_replica - sx_nodelist_count(h->ignored_nodes);
    unsigned int *node_indexes = wrap_malloc((1+effective_replica) * h->put_nblocks * sizeof(*node_indexes));

    if (!node_indexes)
        return ENOMEM;

    unsigned i;
    for(i=0; i<uniq_count; i++) {
	/* MODHDIST: pick from _next, bidx=0 */
	if(hash_nidx_tobuf(
			   h,
			   &all_hashes[uniq_hash_indexes[i]],
			   effective_replica,
			   h->put_replica,
			   &node_indexes[uniq_hash_indexes[i]*effective_replica]
			   ) < effective_replica) {
	    WARN("hash_nidx_tobuf failed");
            ret = FAIL_EINTERNAL;
	}
    }
    DEBUG("reserve_replicas begin");
    for(i=2; ret == OK && i<=effective_replica; i++) {
        unsigned int cur_item = 0;
        sort_by_node_then_hash(all_hashes, uniq_hash_indexes, node_indexes, uniq_count, i, effective_replica);
        memset(hashop, 0, sizeof(*hashop));
        DEBUGHASH("reserve_replicas reserve_id", &h->put_reserve_id);
        sxi_hashop_begin(hashop, h->sx_clust, NULL,
                         HASHOP_RESERVE, h->put_replica, &h->put_reserve_id, &h->put_revision_id, NULL, op_expires_at);
        while((ret = are_blocks_available(h, all_hashes, hashop,
                                          uniq_hash_indexes, node_indexes,
                                          &cur_item, uniq_count, hash_size,
                                          i, effective_replica)) == OK) {
            if(cur_item >= uniq_count)
                break;
        }
        if (ret)
            WARN("are_blocks_available failed: %s", rc2str(ret));
        if (!ret && sxi_hashop_end(hashop) == -1) {
            WARN("sxi_hashop_end failed: %s", sxc_geterrmsg(h->sx));
            ret = FAIL_EINTERNAL;
        }
    }
    DEBUG("reserve_replicas end");
    free(node_indexes);
    return ret;
}

rc_ty sx_hashfs_putfile_getblock(sx_hashfs_t *h) {
    rc_ty ret;
    if(!h || !h->put_token[0])
	return EINVAL;

    if(h->put_checkblock >= h->put_putblock) {
        uint64_t op_expires_at = h->hc.op_expires_at;
	if(sxi_hashop_end(&h->hc) == -1) {
	    WARN("hashop_end failed: %s", sxc_geterrmsg(h->sx));
            if(sxc_geterrnum(h->sx) == SXE_ECOMM) {
                msg_set_reason("Remote error: %s", sxc_geterrmsg(h->sx));
                return EAGAIN;
            }
            return FAIL_EINTERNAL;
        } else
	    DEBUG("{%s}: finished:%d, queries:%d, ok:%d, enoent:%d, cbfail:%d",
		  h->node_uuid.string,
		  h->hc.finished, h->hc.queries, h->hc.ok, h->hc.enoent, h->hc.cb_fail);
        ret = reserve_replicas(h, op_expires_at);
        if (ret) {
            WARN("failed to reserve replicas: %s", rc2str(ret));
            return ret;
        }
	h->put_success = 1;
	return ITER_NO_MORE;
    }
    ret = are_blocks_available(h, h->put_blocks, &h->hc, h->put_hashnos, h->put_nidxs, &h->put_checkblock, h->put_putblock, h->put_hs, 1, 1);
    return ret;
}

void sx_hashfs_createfile_end(sx_hashfs_t *h) {
    if(!h)
	return;

    free(h->put_blocks);
    putfile_reinit(h);
}

void sx_hashfs_putfile_end(sx_hashfs_t *h) {
    if(!h)
	return;
    /* ensure no callbacks are running anymore, or they'd access
     * a wrong ctx data */
    if (sxi_hashop_end(&h->hc) == -1)
        WARN("hashop_end failed");
    memset(&h->hc, 0, sizeof(h->hc));

    free(h->put_blocks);
    free(h->put_nidxs);

    if(!h->put_success && h->put_id)
	sx_hashfs_tmp_delete(h, h->put_id);
    putfile_reinit(h);
}

unsigned int sx_hashfs_job_file_timeout(sx_hashfs_t *h, unsigned int ndests, uint64_t size)
{
    unsigned int blocks = size_to_blocks(size, NULL, NULL), timeout = 35;
    timeout += 20 * (ndests-1);
    timeout += 10 * blocks;

    /* Empirically determined very crude cap; to be reworked later */
    if(timeout < 160 * ndests)
	timeout = 160 * ndests;

    if(timeout > JOB_FILE_MAX_TIME)
	timeout = JOB_FILE_MAX_TIME;

    return timeout;
}

rc_ty sx_hashfs_putfile_commitjob(sx_hashfs_t *h, const uint8_t *user, sx_uid_t user_id, const char *token, job_t *job_id) {
    unsigned int expected_blocks, actual_blocks, ndests, blocksize;
    int64_t tmpfile_id, expected_size, volid;
    rc_ty ret;
    sx_nodelist_t *singlenode = NULL, *volnodes = NULL, *revisionnodes = NULL;
    const sx_hashfs_volume_t *vol;
    const sx_uuid_t *self_uuid;
    const sx_node_t *self;
    struct token_data tkdt;
    const char *fname;
    sqlite3_stmt *q;
    int ndb, r, has_begun = 0;

    if(!h || !user || !job_id) {
	NULLARG();
	return EFAULT;
    }
    if(parse_token(h->sx, user, token, &h->tokenkey, &tkdt)) {
	WARN("bad token: %s", token);
	return EINVAL;
    }

    if(!(self = sx_hashfs_self(h)) || !(self_uuid = sx_node_uuid(self)))
	return FAIL_EINTERNAL;
    if(memcmp(self_uuid->binary, tkdt.uuid.binary, sizeof(tkdt.uuid.binary))) {
	WARN("bad token uuid");
	return EINVAL;
    }

    singlenode = sx_nodelist_new();
    if(!singlenode) {
	WARN("Cannot allocate single node nodelist");
	return ENOMEM;
    }
    ret = sx_nodelist_add(singlenode, sx_node_dup(self));
    if(ret) {
	WARN("Cannot add self to nodelist");
	sx_nodelist_delete(singlenode);
	return ret;
    }

    if(qbegin(h->tempdb)) {
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }
    has_begun = 1;

    sqlite3_reset(h->qt_tokendata);
    if(qbind_text(h->qt_tokendata, ":token", tkdt.token)) {
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }
    r = qstep(h->qt_tokendata);
    if(r == SQLITE_DONE) {
        msg_set_reason("Token is unknown or already flushed");
	ret = ENOENT;
	goto putfile_commitjob_err;
    }
    if(r != SQLITE_ROW) {
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }

    tmpfile_id = sqlite3_column_int64(h->qt_tokendata, 0);
    expected_size = sqlite3_column_int64(h->qt_tokendata, 1);
    volid = sqlite3_column_int64(h->qt_tokendata, 2);
    fname = (const char *)sqlite3_column_text(h->qt_tokendata, 3);
    actual_blocks = sqlite3_column_bytes(h->qt_tokendata, 4);
    ret = sx_hashfs_volume_by_id(h, volid, &vol);
    if(ret) {
	WARN("Cannot locate volume %lld for tmp file %lld", (long long)volid, (long long)tmpfile_id);
	goto putfile_commitjob_err;
    }

    ndb = getmetadb(fname);
    q = h->qm_get[ndb];
    sqlite3_reset(q);
    if(qbind_int64(q, ":volume", volid) || qbind_text(q, ":name", fname)) {
	WARN("Failed to lookup latest revision for tmpfile %lld", (long long)tmpfile_id);
	sqlite3_reset(q);
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }
    r = qstep(q);
    if(r == SQLITE_ROW) {
	/* Don't create a new revision if the file hasn't changed */
	if(expected_size == sqlite3_column_int64(q, 1) &&
	   actual_blocks == sqlite3_column_bytes(q, 2) &&
	   (actual_blocks == 0 || !memcmp(sqlite3_column_blob(q, 2), sqlite3_column_blob(h->qt_tokendata, 4), actual_blocks))) {
	    int64_t fid = sqlite3_column_int64(q, 0);
	    sxc_meta_t *fmeta;
	    unsigned int i;

	    sqlite3_reset(q);
	    if((ret = fill_filemeta(h, ndb, fid)))
		goto putfile_commitjob_err;
	    if(!(fmeta = sxc_meta_new(h->sx))) {
		ret = ENOMEM;
		goto putfile_commitjob_err;
	    }

	    if((ret = sx_hashfs_tmp_getmeta(h, tmpfile_id, fmeta))) {
		sxc_meta_free(fmeta);
		goto putfile_commitjob_err;
	    }
	    for(i=0; i < h->nmeta; i++) {
		const void *v;
		unsigned int vlen;

		/* if(!strcmp(h->meta[i].key, "attribsAtime")) continue; */
		if(sxc_meta_getval(fmeta, h->meta[i].key, &v, &vlen))
		    break;
		if(h->meta[i].value_len != vlen || memcmp(h->meta[i].value, v, vlen))
		    break;
	    }
	    sxc_meta_free(fmeta);
	    if(i >= h->nmeta) {
		if((ret = sx_hashfs_tmp_delete(h, tmpfile_id)))
		    goto putfile_commitjob_err;
		if((ret = sx_hashfs_job_new(h, user_id, job_id, JOBTYPE_DUMMY, 3600, NULL, NULL, 0, singlenode)))
		    goto putfile_commitjob_err;
		if(qcommit(h->tempdb))
		    ret = FAIL_EINTERNAL;
		else
		    ret = OK;
		goto putfile_commitjob_err;
	    }
	}
	r = SQLITE_DONE;
    }

    sqlite3_reset(q);
    if(r != SQLITE_DONE) {
	WARN("Failed to lookup latest revision for tmpfile %lld, %d", (long long)tmpfile_id, r);
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }

    if (revision_spec.nodes(h, NULL, &revisionnodes)) {
        WARN("cannot allocate nodes");
        ret = FAIL_EINTERNAL;
        goto putfile_commitjob_err;
    }
    /* Flushed tempfiles are propagated to all volnodes (both PREV and NEXT),
     * in no particular order (NL_PREVNEXT would be fine as well) */
    ret = sx_hashfs_effective_volnodes(h, NL_NEXTPREV, vol, 0, &volnodes, NULL);
    if(ret) {
	WARN("Cannot determine volume nodes for '%s'", vol->name);
	goto putfile_commitjob_err;
    }
    if(actual_blocks % sizeof(sx_hash_t)) {
	msg_set_reason("Corrupted token data");
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }
    actual_blocks /= sizeof(sx_hash_t);
    expected_blocks = size_to_blocks(expected_size, NULL, &blocksize);
    if(actual_blocks != expected_blocks) {
	/* File was not extended enough to match its size */
	msg_set_reason("Token not extended to its final size");
	ret = EINVAL;
	goto putfile_commitjob_err;
    }

    sqlite3_reset(h->qt_flush);
    if(qbind_int64(h->qt_flush, ":id", tmpfile_id) ||
       qstep_noret(h->qt_flush)) {
	/* The job itself will fail in case a token is still present */
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }

    ndests = sx_nodelist_count(volnodes);
    ret = sx_hashfs_job_new_begin(h);
    if(ret) {
        INFO("Failed to begin jobadd");
	goto putfile_commitjob_err;
    }

    ret = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, user_id, job_id, JOBTYPE_REPLICATE_BLOCKS, sx_hashfs_job_file_timeout(h, ndests, expected_size), token, &tmpfile_id, sizeof(tmpfile_id), singlenode);
    if(ret) {
        INFO("job_new (replicate) returned: %s", rc2str(ret));
	goto putfile_commitjob_err;
    }

    /* when flush fails we need to undo the parent job which was targeted to
     * all (revision) nodes, so we need to target the flush jub to all nodes.
     * In the request/commit we'll skip the non-volnode nodes */
    ret = sx_hashfs_job_new_notrigger(h, *job_id, user_id, job_id, JOBTYPE_FLUSH_FILE_REMOTE, 60*ndests, token, &tmpfile_id, sizeof(tmpfile_id), revisionnodes);
    if(ret) {
        INFO("job_new (flush remote) returned: %s", rc2str(ret));
	goto putfile_commitjob_err;
    }
    ret = sx_hashfs_job_new_notrigger(h, *job_id, user_id, job_id, JOBTYPE_FLUSH_FILE_LOCAL, 60, token, &tmpfile_id, sizeof(tmpfile_id), revisionnodes);
    if(ret) {
        INFO("job_new (flush local) returned: %s", rc2str(ret));
	goto putfile_commitjob_err;
    }

    ret = sx_hashfs_job_new_end(h);
    if(ret) {
        INFO("Failed to commit jobadd");
	goto putfile_commitjob_err;
    }

    if(qcommit(h->tempdb)) {
	ret = FAIL_EINTERNAL;
	goto putfile_commitjob_err;
    }

    ret = OK;
    sx_hashfs_job_trigger(h);

 putfile_commitjob_err:
    if(ret != OK && has_begun)
	qrollback(h->tempdb);

    sqlite3_reset(h->qt_tokendata);

    sx_nodelist_delete(revisionnodes);
    sx_nodelist_delete(volnodes);
    sx_nodelist_delete(singlenode);

    return ret;
}

static int tmp_getmissing_cb(const char *hexhash, unsigned int index, int code, void *context) {
    sx_hashfs_tmpinfo_t *mis = (sx_hashfs_tmpinfo_t *)context;
    sx_hash_t binhash;
    unsigned int blockno;

    if(!hexhash || !mis)
	return -1;
    hex2bin(hexhash, SXI_SHA1_TEXT_LEN, binhash.b, sizeof(binhash));
    blockno = mis->uniq_ids[index];
    unsigned int pushingidx = mis->nidxs[blockno * mis->replica_count +mis->current_replica - 1];
    const sx_node_t *pusher = sx_nodelist_get(mis->allnodes, pushingidx);
    DEBUG("nodelist:");

    for (unsigned i=0;i<sx_nodelist_count(mis->allnodes);i++) {
        DEBUG("node #%d: %s", i, sx_node_internal_addr(sx_nodelist_get(mis->allnodes, i)));
    }

    DEBUG("remote hash #%.*s#: %d, index #%d, blockno %d, set %u, node %s (#%d), replica count: %d, current replica: %d", SXI_SHA1_TEXT_LEN, hexhash, code,
          index, blockno,
          mis->current_replica-1, sx_node_internal_addr(pusher), pushingidx, mis->replica_count, mis->current_replica);
    DEBUG("remote hash #%.*s#: %d", SXI_SHA1_TEXT_LEN, hexhash, code);
    if(code != 200 && code != 404 && code != 444)
	return 0;

    if(index >= mis->nuniq) {
	WARN("Index out of bounds");
	return -1;
    }

    hex2bin(hexhash, SXI_SHA1_TEXT_LEN, binhash.b, sizeof(binhash));
    blockno = mis->uniq_ids[index];
    if(memcmp(&mis->all_blocks[blockno], &binhash, sizeof(binhash))) {
	char idxhash[SXI_SHA1_TEXT_LEN + 1];
	bin2hex(&mis->all_blocks[blockno], sizeof(mis->all_blocks[0]), idxhash, sizeof(idxhash));
	WARN("Hash mismatch: called for %.*s but index %d points to %s", SXI_SHA1_TEXT_LEN, hexhash, index, idxhash);
	return -1;
    }

    int changeto = code != 404 ? 1 : -1;
    if(mis->avlblty[blockno * mis->replica_count + mis->current_replica - 1] != changeto) {
	mis->avlblty[blockno * mis->replica_count + mis->current_replica - 1] = changeto;
	mis->somestatechanged = 1;
        DEBUG("(cb): Block %.*s set %u is NOW bumped and %s on node %c",
              SXI_SHA1_TEXT_LEN, hexhash, mis->current_replica - 1,
              changeto == 1 ? "available" : "unavailable",
              'a' +
              mis->nidxs[blockno * mis->replica_count + mis->current_replica - 1]);
    }
    return 0;
}

rc_ty sx_hashfs_tmp_getinfo(sx_hashfs_t *h, int64_t tmpfile_id, sx_hashfs_tmpinfo_t **tmpinfo, int recheck_presence) {
    unsigned int contentsz, nblocks, bs, nuniqs, i, hash_size, navl;
    const unsigned int *uniqs;
    const sx_hashfs_volume_t *volume;
    rc_ty ret = FAIL_EINTERNAL, ret2;
    const sx_hash_t *content;
    sx_hashfs_tmpinfo_t *tbd = NULL;
    const char *name, *revision;
    const int8_t *avl;
    int64_t file_size;
    int r;
    char token[TOKEN_RAND_BYTES*2 + 1];
    sqlite3_stmt *q;

    if(!h || !tmpinfo) {
	NULLARG();
	return EFAULT;
    }
    DEBUG("tmp_getinfo for file %ld", tmpfile_id);

    /* Get tmp data */
    q = h->qt_tmpdata;
    sqlite3_reset(q);
    if(qbind_int64(q, ":id", tmpfile_id))
	goto getmissing_err;

    r = qstep(q);
    if(r == SQLITE_DONE) {
	msg_set_reason("Token not found");
	ret = ENOENT;
    }
    if(r != SQLITE_ROW) {
	WARN("Error looking up token");
	goto getmissing_err;
    }

    if(sqlite3_column_int(q, 6) == 0) {
	/* Not yet flushed, need to retry later */
	msg_set_reason("Token not ready yet");
	ret = EAGAIN;
	goto getmissing_err;
    }

    /* Quickly validate tmp data */
    revision = (const char *)sqlite3_column_text(q, 0);
    if(!revision || strlen(revision) >= sizeof(tbd->revision)) {
	WARN("Tmpfile with %s revision", revision ? "bad" : "NULL");
	msg_set_reason("Internal corruption detected (bad revision)");
	ret = EFAULT;
	goto getmissing_err;
    }

    if((ret2 = sx_hashfs_volume_by_id(h, sqlite3_column_int64(q, 3), &volume))) {
	ret = ret2;
	if(ret2 == ENOENT)
	    msg_set_reason("Volume no longer exists");
	goto getmissing_err;
    }

    name = (const char *)sqlite3_column_text(q, 1);
    if(check_file_name(name) < 0) {
	WARN("Tmpfile with bad name");
	msg_set_reason("Internal corruption detected (bad name)");
	ret = EFAULT;
	goto getmissing_err;
    }

    file_size = sqlite3_column_int64(q, 2);
    nblocks = size_to_blocks(file_size, &hash_size, &bs);

    content = sqlite3_column_blob(q, 4);
    contentsz = sqlite3_column_bytes(q, 4);
    if(contentsz % sizeof(sx_hash_t) || contentsz / sizeof(*content) != nblocks) {
	WARN("Tmpfile with bad content length");
	msg_set_reason("Internal corruption detected (bad content)");
	ret = EFAULT;
	goto getmissing_err;
    }

    uniqs = sqlite3_column_blob(q, 5);
    contentsz = sqlite3_column_bytes(q, 5);
    nuniqs = contentsz / sizeof(*uniqs);
    if(contentsz % sizeof(*uniqs) || nuniqs > nblocks)  {
	WARN("Tmpfile with bad unique length");
	msg_set_reason("Internal corruption detected (bad unique content)");
	ret = EFAULT;
	goto getmissing_err;
    }

    avl = sqlite3_column_blob(q, 7);
    if(avl) {
	navl = sqlite3_column_bytes(q, 7);
	if(navl != nblocks * volume->max_replica) {
	    WARN("Tmpfile with bad availability length");
	    msg_set_reason("Internal corruption detected (bad availability content)");
	    ret = EFAULT;
	    goto getmissing_err;
	}
    } else
	navl = nblocks * volume->max_replica;

    tbd = wrap_malloc(sizeof(*tbd) + /* The struct itself */
		      nblocks * sizeof(sx_hash_t) + /* all_blocks */
		      nuniqs * sizeof(tbd->uniq_ids[0]) + /* uniq_ids */
		      nblocks * sizeof(tbd->nidxs[0]) * volume->max_replica + /* nidxs */
		      navl); /* avlblty */
    if(!tbd) {
	OOM();
	ret = ENOMEM;
	goto getmissing_err;
    }

    tbd->allnodes = sx_hashfs_all_nodes(h, NL_NEXT);

    tbd->volume_id = volume->id;
    tbd->all_blocks = (sx_hash_t *)(tbd+1);
    tbd->uniq_ids = (unsigned int *)&tbd->all_blocks[nblocks];
    tbd->nidxs = &tbd->uniq_ids[nuniqs];
    tbd->avlblty = (int8_t *)&tbd->nidxs[nblocks * volume->max_replica];
    memcpy(tbd->all_blocks, content, nblocks * sizeof(sx_hash_t));
    memcpy(tbd->uniq_ids, uniqs, nuniqs * sizeof(tbd->uniq_ids[0]));
    memset(tbd->nidxs, -1, nblocks * sizeof(tbd->nidxs[0]) * volume->max_replica);
    for(i=0; i<nuniqs; i++) {
	/* MODHDIST: pick from _next, bidx=0 */
	if(hash_nidx_tobuf(h, &tbd->all_blocks[tbd->uniq_ids[i]], volume->max_replica, volume->max_replica, &tbd->nidxs[tbd->uniq_ids[i]*volume->max_replica]) < 0) {
	    WARN("hash_nidx_tobuf failed");
	    goto getmissing_err;
	}
    }
    if(!avl)
	memset(tbd->avlblty, 0, navl);
    else
	memcpy(tbd->avlblty, avl, navl);
    tbd->nall = nblocks;
    tbd->nuniq = nuniqs;
    tbd->replica_count = volume->max_replica;
    tbd->block_size = bs;
    sxi_strlcpy(tbd->revision, revision, sizeof(tbd->revision));
    sxi_strlcpy(tbd->name, name, sizeof(tbd->name));
    tbd->file_size = file_size;
    tbd->tmpfile_id = tmpfile_id;
    tbd->somestatechanged = 0;

    sxi_strlcpy(token, (const char*)sqlite3_column_text(q, 8), sizeof(token));
    sqlite3_reset(q); /* Do not deadlock if we need to update this very entry */

    if(nuniqs && recheck_presence) {
	unsigned int r, l;

	/* For each replica set populate tbd->avlblty via hash_presence callback */
	for(i=1; i<=tbd->replica_count; i++) {
            sx_hash_t revision_id, reserve_id;
	    unsigned int cur_item = 0;
	    sort_by_node_then_hash(tbd->all_blocks, tbd->uniq_ids, tbd->nidxs, tbd->nuniq, i, tbd->replica_count);
            /* revision_id when deleting the file must match
	     * the revision_id used here when creating it */
            if (sx_unique_fileid(h->sx, volume, tbd->name, tbd->revision, &revision_id))
                goto getmissing_err;
            /* reserve_id must match the id used in reserve */
            if (reserve_fileid(h, tbd->volume_id, tbd->name, &reserve_id))
                goto getmissing_err;
            DEBUGHASH("tmp_get_info reserve_id", &reserve_id);
            DEBUGHASH("tmp_get_info revision_id", &revision_id);
            sxi_hashop_begin(&h->hc, h->sx_clust, tmp_getmissing_cb,
                             HASHOP_INUSE, tbd->replica_count, &reserve_id, &revision_id, tbd, 0);
	    tbd->current_replica = i;
            DEBUG("begin queries for replica #%d", i);
	    while((ret2 = are_blocks_available(h,
					       tbd->all_blocks,
					       &h->hc,
					       tbd->uniq_ids,
					       tbd->nidxs,
					       &cur_item,
					       tbd->nuniq,
					       hash_size,
					       i,
					       tbd->replica_count)) == OK) {
		if(cur_item >= nuniqs)
		    break;
	    }
	    if(ret2 != OK)
		ret = ret2;
	    if(sxi_hashop_end(&h->hc) == -1) {
                if (ret2 == OK)
                    ret = ret2 = EAGAIN;
            }
            if (ret2 != OK)
		goto getmissing_err;
            DEBUG("end queries for replica #%d", i);
	}

	/* Drop all hashes which are already fully replicated */
	for(r=0, l=0; r < tbd->nuniq; r++) {
	    for(i=0; i<tbd->replica_count; i++) {
		if(tbd->avlblty[tbd->uniq_ids[r] * tbd->replica_count + i] != 1)
		    break;
	    }
	    if(i == tbd->replica_count) {
		tbd->somestatechanged = 1; /* Should be implied but explicitly setting this won't harm */
		nuniqs--;
		continue;
	    }
	    if(l != r)
		tbd->uniq_ids[l] = tbd->uniq_ids[r];
	    l++;
	}

    }

    *tmpinfo = tbd;
    ret = OK;

 getmissing_err:
    if(tbd && tbd->somestatechanged) {
        /* If we've harvested some hash bring the counter down */
        tbd->nuniq = nuniqs;

        /* and update the db so they won't be hashop'd again on the next run */
        sqlite3_reset(h->qt_updateuniq);
        if(!qbind_int64(h->qt_updateuniq, ":id", tmpfile_id) &&
           !qbind_blob(h->qt_updateuniq, ":uniq", tbd->uniq_ids, sizeof(*tbd->uniq_ids) * tbd->nuniq) &&
           !qbind_blob(h->qt_updateuniq, ":avail", tbd->avlblty, navl))
            qstep_noret(h->qt_updateuniq);
        sqlite3_reset(h->qt_updateuniq);
    }

    if(ret != OK) {
        (void)sxi_hashop_end(&h->hc);
	free(tbd);
    }

    sqlite3_reset(q);

    return ret;
}

rc_ty sx_hashfs_getinfo_by_revision(sx_hashfs_t *h, const char *revision, sx_hashfs_file_t *filerev)
{
    unsigned ndb;
    if(!h || !revision || !filerev) {
	NULLARG();
	return EFAULT;
    }

    DEBUG("looking up revision %s", revision);
    if(check_revision(revision)) {
	msg_set_reason("Invalid file revision");
	return EINVAL;
    }

    for(ndb=0;ndb<METADBS;ndb++) {
        sqlite3_stmt *q = h->qm_findrev[ndb];
        if(qbind_text(q, ":revision", revision))
            return FAIL_EINTERNAL;

        sqlite3_reset(q);
        int r = qstep(q);
        if(r == SQLITE_DONE) {
            sqlite3_reset(q);
            continue;
        }
        if(r != SQLITE_ROW)
            return FAIL_EINTERNAL;

        filerev->volume_id = sqlite3_column_int64(q, 0);
        const unsigned char *name = sqlite3_column_text(q, 1);
        filerev->file_size = sqlite3_column_int64(q, 2);
        size_to_blocks(filerev->file_size, NULL, &filerev->block_size);
        strncpy(filerev->name, (const char*)name, sizeof(filerev->name));
        filerev->name[sizeof(filerev->name)-1] = '\0';
        strncpy(filerev->revision, revision, sizeof(filerev->revision));
        filerev->revision[sizeof(filerev->revision)-1] = '\0';
        sqlite3_reset(q);
        return OK;
    }
    return ENOENT;
}

rc_ty sx_hashfs_tmp_tofile(sx_hashfs_t *h, const sx_hashfs_tmpinfo_t *missing) {
    rc_ty ret = FAIL_EINTERNAL, ret2;
    const sx_hashfs_volume_t *volume;
    int64_t file_id, totalsize = 0;
    int mdb;
    sxc_meta_t *meta;
    unsigned int i;

    if(!h || !missing) {
	NULLARG();
	return EFAULT;
    }

    mdb = getmetadb(missing->name);
    if(mdb < 0) {
	msg_set_reason("Failed to locate file database");
	return FAIL_EINTERNAL;
    }

    ret2 = sx_hashfs_volume_by_id(h, missing->volume_id, &volume);
    if(ret2) {
	WARN("Cannot locate volume %lld", (long long)missing->volume_id);
	return ret2;
    }

    sqlite3_reset(h->qt_getmeta);
    if(!(meta = sxc_meta_new(h->sx)))
        return ENOMEM;

    if((ret2 = sx_hashfs_tmp_getmeta(h, missing->tmpfile_id, meta)) != OK) {
        sxc_meta_free(meta);
        return ret2;
    }

    /* Calculate total file size */
    totalsize = missing->file_size + strlen(missing->name);
    for(i = 0; i < sxc_meta_count(meta); i++) {
        const void *value;
        const char *key;
        unsigned int value_len;

        if(sxc_meta_getkeyval(meta, i, &key, &value, &value_len)) {
            WARN("Failed to get tempfile meta entry");
            sxc_meta_free(meta);
            return FAIL_EINTERNAL;
        }

        totalsize += strlen(key) + value_len;
    }

    if(qbegin(h->metadb[mdb])) {
        sxc_meta_free(meta);
        return FAIL_EINTERNAL;
    }

    ret2 = create_file(h, volume, missing->name, missing->revision, missing->all_blocks, missing->nall, missing->file_size, totalsize, &file_id);
    if(ret2) {
	ret = ret2;
	goto tmp2file_rollback;
    }

    /* Set new file meta */
    for(i = 0; i < sxc_meta_count(meta); i++) {
        const void *value;
        const char *key;
        unsigned int value_len;

        if(sxc_meta_getkeyval(meta, i, &key, &value, &value_len)) {
            WARN("Failed to get tempfile meta entry");
            goto tmp2file_rollback;
        }

        sqlite3_reset(h->qm_metaset[mdb]);
        if(qbind_int64(h->qm_metaset[mdb], ":file", file_id) ||
           qbind_text(h->qm_metaset[mdb], ":key", key) ||
           qbind_blob(h->qm_metaset[mdb], ":value", value, value_len) ||
           qstep_noret(h->qm_metaset[mdb])) {
            sqlite3_reset(h->qm_metaset[mdb]);
            goto tmp2file_rollback;
        }
    }

    sqlite3_reset(h->qm_metaset[mdb]);
    if(qcommit(h->metadb[mdb]))
	goto tmp2file_rollback;

    sx_hashfs_tmp_delete(h, missing->tmpfile_id);
    ret = OK;

 tmp2file_rollback:
    if(ret != OK)
	qrollback(h->metadb[mdb]);

    sqlite3_reset(h->qm_metaset[mdb]);
    sqlite3_reset(h->qt_getmeta);
    sxc_meta_free(meta);

    return ret;
}

static rc_ty get_existing_delete_job(sx_hashfs_t *h, const char *revision, job_t *job_id) {
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!job_id) {
        WARN("NULL argument");
        return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qe_getfiledeljob);
    if(qbind_blob(h->qe_getfiledeljob, ":data", revision, strlen(revision))) {
        WARN("Failed to prepare query with revision %s", revision);
        goto get_existing_delete_job_err;
    }

    r = qstep(h->qe_getfiledeljob);
    if(r == SQLITE_DONE) {
        ret = ENOENT;
        goto get_existing_delete_job_err;
    } else if(r == SQLITE_ROW) {
        /* Set parent ID for current job */
        *job_id = sqlite3_column_int64(h->qe_getfiledeljob, 0);
        ret = OK;
    } else {
        WARN("Failed to get job for revision %s", revision);
        goto get_existing_delete_job_err;
    }

get_existing_delete_job_err:
    sqlite3_reset(h->qe_getfiledeljob);
    return ret;
}

rc_ty sx_hashfs_job_new_2pc(sx_hashfs_t *hashfs, const job_2pc_t *spec, void *yctx, sx_uid_t uid, job_t *job, int execute)
{
    sxc_client_t *sx = sx_hashfs_client(hashfs);
    sx_nodelist_t *nodes = NULL;
    rc_ty rc = FAIL_EINTERNAL;
    sx_blob_t *joblb = NULL;

    do {
        joblb = sx_blob_new();
        if (!joblb) {
            msg_set_reason("Cannot allocate job blob");
            break;
        }

        if (spec->nodes(hashfs, joblb, &nodes))
            break;

        if (spec->to_blob(sx, sx_nodelist_count(nodes), yctx, joblb)) {
            const char *msg = msg_get_reason();
            if(!msg || !*msg)
                msg_set_reason("Cannot create job blob");
            break;
        }

        if(execute) {
            sx_blob_reset(joblb);
            rc = spec->execute_blob(hashfs, joblb, JOBPHASE_REQUEST, 1);
            if (rc != OK) {
                const char *msg = msg_get_reason();
                if (!msg)
                    msg_set_reason("%s", rc2str(rc));
                WARN("Failed to execute job(%d): %s", rc2http(rc), msg);
                break;
            }
        } else {
            const void *job_data;
            unsigned int job_datalen;
            unsigned int job_timeout;

            /* create job, must not reset yet */
            sx_blob_to_data(joblb, &job_data, &job_datalen);
            /* must reset now */
            sx_blob_reset(joblb);
            if (!spec->get_lock) {
                NULLARG();
                rc = EFAULT;
                break;
            }
            const char *lock = spec->get_lock(joblb);
            sx_blob_reset(joblb);
            job_timeout = spec->timeout(sx, sx_nodelist_count(nodes));
            if (*job == JOB_NOPARENT)
                rc = sx_hashfs_job_new(hashfs, uid, job, spec->job_type, job_timeout, lock, job_data, job_datalen, nodes);
            else {
                job_t parent = *job;
                rc = sx_hashfs_job_new_notrigger(hashfs, parent, uid, job, spec->job_type, job_timeout, lock, job_data, job_datalen, nodes);
                DEBUG("job %ld created, parent: %ld", *job, parent);
            }
        }
    } while(0);
    sx_blob_free(joblb);
    sx_nodelist_delete(nodes);
    return rc;
}

/* FIXME: this belongs in fcgi-actions-block.c, but then delete_job, and
 * create_file and all their callers have to be moved to fcgi-actions-file.c too
 * or there would be a linker error */

static int revision_to_blob(sxc_client_t *sx, int nodes, void *yctx, sx_blob_t *blob)
{
    sx_revision_op_t *op = yctx;

    if (!blob) {
        msg_set_reason("cannot allocate job storage");
        return -1;
    }
    if (!op) {
        NULLARG();
        return -1;
    }

    if (sx_blob_add_string(blob, op->lock ? op->lock : "") ||
        sx_blob_add_int32(blob, op->blocksize) ||
        sx_blob_add_blob(blob, op->revision_id.b, sizeof(op->revision_id.b)) ||
        sx_blob_add_int32(blob, op->op)) {
        msg_set_reason("Cannot create job storage");
        return -1;
    }
    return 0;
}

int sx_revision_op_of_blob(sx_blob_t *b, sx_revision_op_t *op)
{
    const char *lock;
    const void *ptr;
    unsigned int len;
    if  (!op) {
        NULLARG();
        return -1;
    }
    if (sx_blob_get_string(b, &lock) ||
        sx_blob_get_int32(b, &op->blocksize) ||
        sx_blob_get_blob(b, &ptr, &len) || len != sizeof(op->revision_id.b) ||
        sx_blob_get_int32(b, &op->op)) {
        msg_set_reason("Corrupt revision blob");
        return -1;
    }
    memcpy(op->revision_id.b, ptr, sizeof(op->revision_id.b));
    op->lock = NULL;
    return 0;
}

static rc_ty revision_execute_blob(sx_hashfs_t *hashfs, sx_blob_t *b, jobphase_t phase, int remote)
{
    sx_revision_op_t op;
    rc_ty rc;

    DEBUG("executing");
    if (remote && phase == JOBPHASE_REQUEST)
        phase = JOBPHASE_COMMIT;
    if (sx_revision_op_of_blob(b, &op))
        return FAIL_EINTERNAL;
    switch (phase) {
        case JOBPHASE_COMMIT:
            rc = sx_hashfs_revision_op(hashfs, op.blocksize, &op.revision_id, op.op);
            if (rc != OK)
                WARN("Failed to (un)bump revision: %s", msg_get_reason());
            return rc;
        case JOBPHASE_ABORT:/* fall-through */
        case JOBPHASE_UNDO:
            rc = sx_hashfs_revision_op(hashfs, op.blocksize, &op.revision_id, -op.op);
            if (rc != OK)
                WARN("Failed to (un)bump revision: %s", msg_get_reason());
            return rc;
        default:
            WARN("Impossible job phase: %d", phase);
            return FAIL_EINTERNAL;
    }
}

static sxi_query_t* revision_proto_from_blob(sxc_client_t *sx, sx_blob_t *b, jobphase_t phase)
{
    sx_revision_op_t op;
    if (sx_revision_op_of_blob(b, &op))
        return NULL;

    if (phase == JOBPHASE_ABORT || phase == JOBPHASE_UNDO)
        op.op = -op.op;
    return sxi_hashop_proto_revision(sx, op.blocksize, &op.revision_id, op.op);
}

static rc_ty revision_nodes(sx_hashfs_t *hashfs, sx_blob_t *blob, sx_nodelist_t **nodes)
{
    /* all nodes */
    if (!nodes)
        return FAIL_EINTERNAL;
    *nodes = sx_nodelist_dup(sx_hashfs_effective_nodes(hashfs, NL_NEXTPREV));
    if (!*nodes)
        return FAIL_EINTERNAL;
    DEBUG("returning nodes");
    return OK;
}

static unsigned revision_timeout(sxc_client_t *sx, int nodes)
{
    return 12 * nodes;
}

static const char *revision_get_lock(sx_blob_t *b)
{
    const char *ret = NULL;
    if (sx_blob_get_string(b, &ret))
        return NULL;
    sx_blob_reset(b);
    return ret;
}

const job_2pc_t revision_spec = {
    NULL,
    JOBTYPE_BLOCKS_REVISION,
    NULL,
    revision_get_lock,
    revision_to_blob,
    revision_execute_blob,
    revision_proto_from_blob,
    revision_nodes,
    revision_timeout
};

rc_ty sx_hashfs_filedelete_job(sx_hashfs_t *h, sx_uid_t user_id, const sx_hashfs_volume_t *vol, const char *name, const char *revision, job_t *job_id) {
    const sx_hashfs_file_t *filerev;
    sx_nodelist_t *targets;
    unsigned int timeout;
    char *lockname;
    rc_ty ret, s;
    int added = 0; /* Set to 1 if already added job for a chain */

    if(!h || !vol || !name || !job_id) {
	NULLARG();
	return EFAULT;
    }

    ret = sx_hashfs_effective_volnodes(h, NL_NEXTPREV, vol, 0, &targets, NULL);
    if(ret)
	return ret;

    if(qbegin(h->tempdb)) {
        sx_nodelist_delete(targets);
        msg_set_reason("Internal error: failed to start database transaction");
        return FAIL_EINTERNAL;
    }

    ret = sx_hashfs_job_new_begin(h);
    if(ret)
        goto sx_hashfs_filedelete_job_err;

    if(revision) {
        sx_hashfs_file_t filerev;
        if (sx_hashfs_getinfo_by_revision(h, revision, &filerev))
            goto sx_hashfs_filedelete_job_err;
        /* Check if delete job for this revision exist and if not, create new one */
        if((ret = get_existing_delete_job(h, revision, job_id)) == ENOENT) {
            lockname = malloc(strlen(name) + 1 + strlen(revision) + 1);
            if(!lockname) {
                msg_set_reason("Internal error: not enough memory to delete the specified file");
                ret = ENOMEM;
                goto sx_hashfs_filedelete_job_err;
            }
            sprintf(lockname, "%s:%s", name, revision);

            timeout = sx_hashfs_job_file_timeout(h, vol->effective_replica, filerev.file_size);
            /* Create a job for newly created tempfile */
            ret = sx_hashfs_job_new_notrigger(h, *job_id, user_id, job_id, JOBTYPE_DELETE_FILE, timeout, lockname, revision, strlen(revision), targets);
            if (ret == OK) {
                sx_revision_op_t revision_op;
                ret = sx_unique_fileid(h->sx, vol, name, revision, &revision_op.revision_id);
                if (ret == OK) {
                    revision_op.lock = lockname;
                    revision_op.op = -1;
                    revision_op.blocksize = filerev.block_size;
                    /* job to unbump revision for blocks */
                    ret = sx_hashfs_job_new_2pc(h, &revision_spec, &revision_op, user_id, job_id, 0);
                    DEBUG("ret3: %d", ret);
                }
            }
            free(lockname);
        } else if(ret != OK) {
            WARN("Failed to get job for revision %s", revision);
            goto sx_hashfs_filedelete_job_err;
        }

	goto sx_hashfs_filedelete_job_err;
    }

    *job_id = JOB_NOPARENT;
    for(s = sx_hashfs_revision_first(h, vol, name, &filerev, 1); s == OK; s = sx_hashfs_revision_next(h, 1)) {
        if((ret = get_existing_delete_job(h, filerev->revision, job_id)) == OK)
            break;
        else if(ret != ENOENT) {
            WARN("Failed to get job for revision %s", filerev->revision);
            s = ret;
            break;
        }
    }

    if(s == OK) /* If locked revisions were found, take next revision in ascending order */
        s = sx_hashfs_revision_next(h, 0);
    else if(s == ITER_NO_MORE) /* If no locked revision was found, restart listing from the beginning */
        s = sx_hashfs_revision_first(h, vol, name, &filerev, 0);

    if(s != OK && s != ITER_NO_MORE) {
        ret = s;
        goto sx_hashfs_filedelete_job_err;
    }

    for(; s == OK; s = sx_hashfs_revision_next(h, 0)) {
	lockname = malloc(strlen(name) + 1 + strlen(filerev->revision) + 1);
	if(!lockname) {
	    msg_set_reason("Internal error: not enough memory to delete the specified file");
	    ret = ENOMEM;
	    goto sx_hashfs_filedelete_job_err;
	}
	sprintf(lockname, "%s:%s", name, filerev->revision);

        timeout = sx_hashfs_job_file_timeout(h, vol->effective_replica, filerev->file_size);
	ret = sx_hashfs_job_new_notrigger(h, *job_id, user_id, job_id, JOBTYPE_DELETE_FILE, timeout, lockname, filerev->revision, REV_LEN, targets);
        if (ret == OK) {
            sx_revision_op_t revision_op;
            ret = sx_unique_fileid(h->sx, vol, name, filerev->revision, &revision_op.revision_id);
            if (ret == OK) {
                revision_op.lock = lockname;
                revision_op.op = -1;
                revision_op.blocksize = filerev->block_size;
                /* job to unbump revision for blocks */
                ret = sx_hashfs_job_new_2pc(h, &revision_spec, &revision_op, user_id, job_id, 0);
                DEBUG("ret5: %d", ret);
            }
        }
	free(lockname);

        if(ret != OK) {
            if(ret != FAIL_ETOOMANY)
                WARN("Failed to add delete job: %s", msg_get_reason());
            DEBUG("jumping, ret: %d", ret);
            goto sx_hashfs_filedelete_job_err;
        }
        added = 1;
    }

    if(s != ITER_NO_MORE) {
        WARN("Failed to finish revisions iteration");
        ret = s;
        goto sx_hashfs_filedelete_job_err;
    }

    if(!added) {
        ret = sx_hashfs_job_new_notrigger(h, *job_id, user_id, job_id, JOBTYPE_DUMMY, 3600, NULL, NULL, 0, targets);
        if(ret != OK) {
            if(ret != FAIL_ETOOMANY)
                WARN("Failed to create dummy job: %s", msg_get_reason());
            goto sx_hashfs_filedelete_job_err;
        }
    }

    ret = OK;
sx_hashfs_filedelete_job_err:
    DEBUG("end ret:%d", ret);
    sx_nodelist_delete(targets);
    if(!ret) {
        if(!qcommit(h->tempdb)) {
            return sx_hashfs_job_new_end(h);
        } else {
            ret = FAIL_EINTERNAL;
            msg_set_reason("Internal error: failed to commit database transaction");
        }
    }

    if(ret) {
        qrollback(h->tempdb);
        sx_hashfs_job_new_abort(h);
    }
    return ret;
}

rc_ty sx_hashfs_tmp_getmeta(sx_hashfs_t *h, int64_t tmpfile_id, sxc_meta_t *metadata) {
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h || !metadata) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_reset(h->qt_getmeta);
    if(qbind_int64(h->qt_getmeta, ":id", tmpfile_id))
	return FAIL_EINTERNAL;
    while((r = qstep(h->qt_getmeta)) == SQLITE_ROW) {
	const char *key = (const char *)sqlite3_column_text(h->qt_getmeta, 0);
	const void *value = sqlite3_column_blob(h->qt_getmeta, 1);
	int value_len = sqlite3_column_bytes(h->qt_getmeta, 1);

	if(sxc_meta_setval(metadata, key, value, value_len)) {
	    msg_set_reason("Not enough memory to collect file metadata");
	    ret = ENOMEM;
	    goto tmpgetmeta_err;
	}
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Database error collecting file metadata");
	goto tmpgetmeta_err;
    }

    ret = OK;

 tmpgetmeta_err:
    sqlite3_reset(h->qt_getmeta);

    return ret;
}

rc_ty sx_hashfs_tmp_delete(sx_hashfs_t *h, int64_t tmpfile_id) {
    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(!qbind_int64(h->qt_delete, ":id", tmpfile_id) &&
       !qstep_noret(h->qt_delete))
	return OK;

    return FAIL_EINTERNAL;
}

static rc_ty get_file_id(sx_hashfs_t *h, const char *volume, const char *filename, const char *revision, int64_t *file_id, int *database_number, unsigned int *created_at, sx_hash_t *etag, int64_t *size) {
    const sx_hashfs_volume_t *vol;
    sqlite3_stmt *q;
    int r, ndb;
    rc_ty res;

    if(!h || !volume || !filename || !file_id) {
	NULLARG();
	return EFAULT;
    }

    if(check_file_name(filename)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    res = sx_hashfs_volume_by_name(h, volume, &vol);
    if(res)
	return res;

    ndb = getmetadb(filename);
    if(ndb < 0)
	return FAIL_EINTERNAL;

    if(revision) {
	if(check_revision(revision)) {
	    msg_set_reason("Invalid revision");
	    return EINVAL;
	}
	q = h->qm_getrev[ndb];
	if(qbind_text(q, ":revision", revision))
	    return FAIL_EINTERNAL;
    } else
	q = h->qm_get[ndb];

    if(qbind_int64(q, ":volume", vol->id) || qbind_text(q, ":name", filename))
	return FAIL_EINTERNAL;

    r = qstep(q);
    if(r == SQLITE_ROW) {
	*file_id = sqlite3_column_int64(q, 0);
	*database_number = ndb;
	res = OK;
	if(created_at || etag) {
	    const char *rev = (const char *)sqlite3_column_text(q, 3);
	    if(!rev ||
	       (created_at && parse_revision(rev, created_at)) ||
	       (etag && hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), rev, strlen(rev), etag)))
		res = FAIL_EINTERNAL;
	}
        if(size) {
            int64_t metasize = 0;
            rc_ty s;
            *size = sqlite3_column_int64(q, 1);
            if((s = get_file_metasize(h, *file_id, ndb, &metasize)) != OK) {
                WARN("Failed to get file meta size");
                res = s;
            } else
                *size += metasize + strlen(filename);
        }
    } else if(r == SQLITE_DONE)
	res = ENOENT;
    else
	res = FAIL_EINTERNAL;

    sqlite3_reset(q);
    return res;
}

rc_ty sx_hashfs_file_delete(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *file, const char *revision) {
    int64_t file_id, size = 0;
    int mdb, deleted;
    rc_ty ret;

    if(!h || !volume) {
	NULLARG();
	return EFAULT;
    }

    if(!h->have_hd) {
        WARN("Called before initialization");
        return FAIL_EINIT;
    }

    if(!sx_hashfs_is_or_was_my_volume(h, volume)) {
	msg_set_reason("Wrong node for volume '%s': ...", volume->name);
	return ENOENT;
    }

    if(check_file_name(file)<0) {
	msg_set_reason("Invalid file name");
	return EINVAL;
    }

    if(check_revision(revision)) {
	msg_set_reason("Invalid file revision");
	return EINVAL;
    }
    DEBUG("Deleting file %s, revision %s", file, revision);

    ret = get_file_id(h, volume->name, file, revision, &file_id, &mdb, NULL, NULL, &size);
    if(ret) {
        DEBUG("get_file_id failed: %s", rc2str(ret));
        return ret;
    }

    if(qbind_int64(h->qm_delfile[mdb], ":file", file_id) ||
       qstep_noret(h->qm_delfile[mdb])) {
	msg_set_reason("Failed to delete file from database");
	return FAIL_EINTERNAL;
    }

    deleted = sqlite3_changes(h->metadb[mdb]->handle);

    /* Update counters only when file deletion succeeded and this node is not becoming a volnode */
    if(ret == OK && deleted && sx_hashfs_update_volume_cursize(h, volume->id, -size)) {
        WARN("Failed to update volume size");
        return FAIL_EINTERNAL;
    }

    return OK;
}

static rc_ty fill_filemeta(sx_hashfs_t *h, unsigned int metadb, int64_t file_id) {
    sqlite3_stmt *q = h->qm_metaget[metadb];
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    sqlite3_reset(q);
    if(qbind_int64(q, ":file", file_id))
	return FAIL_EINTERNAL;

    h->nmeta = 0;
    while((r = qstep(q)) == SQLITE_ROW) {
	const char *key = (const char *)sqlite3_column_text(q, 0);
	const void *value = sqlite3_column_text(q, 1);
	int value_len = sqlite3_column_bytes(q, 1), key_len;
	if(!key)
	    break;
	key_len = strlen(key);
	if(key_len >= sizeof(h->meta[0].key))
	    break;
	if(!value || value_len > sizeof(h->meta[0].value))
	    break;
	memcpy(h->meta[h->nmeta].key, key, key_len+1);
	memcpy(h->meta[h->nmeta].value, value, value_len);
	h->meta[h->nmeta].value_len = value_len;
	h->nmeta++;
	if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	    break;
    }

    if(r == SQLITE_DONE)
	ret = OK;

    sqlite3_reset(q);

    return ret;
}


rc_ty sx_hashfs_getfilemeta_begin(sx_hashfs_t *h, const char *volume, const char *filename, const char *revision, unsigned int *created_at, sx_hash_t *etag) {
    rc_ty ret;
    int metaget_ndb;
    int64_t file_id;

    if(!h)
	return EINVAL;

    ret = get_file_id(h, volume, filename, revision, &file_id, &metaget_ndb, created_at, etag, NULL);
    if(ret)
	return ret;

    return fill_filemeta(h, metaget_ndb, file_id);
}

rc_ty sx_hashfs_getfilemeta_next(sx_hashfs_t *h, const char **key, const void **value, unsigned int *value_len) {
    if(!h || !key || (value && !value_len)) {
	NULLARG();
	return EFAULT;
    }

    if(!h->nmeta || h->nmeta > SXLIMIT_META_MAX_ITEMS)
	return ITER_NO_MORE;

    h->nmeta--;
    *key = h->meta[h->nmeta].key;
    if(value) {
	*value = h->meta[h->nmeta].value;
	*value_len = h->meta[h->nmeta].value_len;
    }

    return OK;
}

rc_ty sx_hashfs_volumemeta_begin(sx_hashfs_t *h, const sx_hashfs_volume_t *volume) {
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h || !volume) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_reset(h->q_metaget);
    if(qbind_int64(h->q_metaget, ":volume", volume->id)) {
	sqlite3_reset(h->q_metaget);
	return FAIL_EINTERNAL;
    }

    h->nmeta = 0;
    while((r = qstep(h->q_metaget)) == SQLITE_ROW) {
	const char *key = (const char *)sqlite3_column_text(h->q_metaget, 0);
	const void *value = sqlite3_column_text(h->q_metaget, 1);
	int value_len = sqlite3_column_bytes(h->q_metaget, 1), key_len;
	if(!key || !value) {
	    OOM();
	    goto getvolumemeta_begin_err;
	}
	key_len = strlen(key);
	if(key_len >= sizeof(h->meta[0].key)) {
	    msg_set_reason("Key '%s' is too long: must be <%ld", key, sizeof(h->meta[0].key));
	    goto getvolumemeta_begin_err;
	}
	if(value_len > sizeof(h->meta[0].value)) {
	    /* Do not log the value, might contain sensitive data */
	    msg_set_reason("Value is too long: %d >= %ld", value_len, sizeof(h->meta[0].key));
	    goto getvolumemeta_begin_err;
	}
	memcpy(h->meta[h->nmeta].key, key, key_len+1);
	memcpy(h->meta[h->nmeta].value, value, value_len);
	h->meta[h->nmeta].value_len = value_len;
	h->nmeta++;
	if(h->nmeta >= SXLIMIT_META_MAX_ITEMS)
	    break;
    }

    if(r != SQLITE_DONE)
	goto getvolumemeta_begin_err;

    ret = OK;

 getvolumemeta_begin_err:
    sqlite3_reset(h->q_metaget);

    return ret;
}

rc_ty sx_hashfs_volumemeta_next(sx_hashfs_t *h, const char **key, const void **value, unsigned int *value_len) {
    return sx_hashfs_getfilemeta_next(h, key, value, value_len);
}

rc_ty sx_hashfs_get_user_info(sx_hashfs_t *h, const uint8_t *user, sx_uid_t *uid, uint8_t *key, sx_priv_t *basepriv, char **desc) {
    const uint8_t *kcol;
    rc_ty ret = FAIL_EINTERNAL;
    sx_priv_t userpriv;
    int r;

    if(!h || !user)
	return EINVAL;
    if (desc)
        *desc = NULL;

    sqlite3_reset(h->q_getuser);
    if(qbind_blob(h->q_getuser, ":user", user, AUTH_UID_LEN))
	goto get_user_info_err;

    r = qstep(h->q_getuser);
    if(r == SQLITE_DONE) {
	ret = ENOENT;
	goto get_user_info_err;
    }
    if(r != SQLITE_ROW)
	goto get_user_info_err;

    switch(sqlite3_column_int(h->q_getuser, 2)) {
    case ROLE_CLUSTER:
	userpriv = PRIV_CLUSTER;
	break;
    case ROLE_ADMIN:
	userpriv = PRIV_ADMIN;
	break;
    case ROLE_USER:
	userpriv = PRIV_NONE;
	break;
    default:
	WARN("Found invalid role");
	goto get_user_info_err;
    }
    if(basepriv)
	*basepriv = userpriv;

    kcol = (const uint8_t *)sqlite3_column_blob(h->q_getuser, 1);
    if(!kcol || sqlite3_column_bytes(h->q_getuser, 1) != AUTH_KEY_LEN) {
	WARN("Found bad key");
	goto get_user_info_err;
    }
    if(key)
	memcpy(key, kcol, AUTH_KEY_LEN);
    if(uid)
	*uid = sqlite3_column_int64(h->q_getuser, 0);
    if (desc) {
        const char *udesc = (const char*)sqlite3_column_text(h->q_getuser, 3);
        *desc = wrap_strdup(udesc ? udesc : "");
        if (!*desc) {
            ret = ENOMEM;
            goto get_user_info_err;
        }
    }
    ret = OK;

get_user_info_err:
    sqlite3_reset(h->q_getuser);
    return ret;
}

static rc_ty get_user_common(sx_hashfs_t *h, sx_uid_t uid, const char *name, uint8_t *user, int inactivetoo) {
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q;
    int r;

    if(!h || !uid) {
	NULLARG();
	return EFAULT;
    }

    if(!name) {
	q = h->q_getuserbyid;
	sqlite3_reset(q);
	if(qbind_int64(q, ":uid", uid) || qbind_int(q, ":inactivetoo", inactivetoo))
	    goto get_user_common_fail;
    } else {
	if(sx_hashfs_check_username(name)) {
	    msg_set_reason("Invalid username");
	    return EINVAL;
	}
	q = h->q_getuserbyname;
	sqlite3_reset(q);
	if(qbind_text(q, ":name", name) || qbind_int(q, ":inactivetoo", inactivetoo))
	    goto get_user_common_fail;
    }

    r = qstep(q);
    if(r == SQLITE_ROW) {
	const void *sqlusr = sqlite3_column_blob(q, 0);
	if(sqlusr && sqlite3_column_bytes(q, 0) == AUTH_UID_LEN) {
	    if(user)
		memcpy(user, sqlusr, AUTH_UID_LEN);
	    ret = OK;
	}
    } else if(r == SQLITE_DONE)
	ret = ENOENT;

 get_user_common_fail:
    sqlite3_reset(q);
    return ret;
}


rc_ty sx_hashfs_get_user_by_uid(sx_hashfs_t *h, sx_uid_t uid, uint8_t *user, int inactivetoo) {
    return get_user_common(h, uid, NULL, user, inactivetoo);
}

rc_ty sx_hashfs_get_user_by_name(sx_hashfs_t *h, const char *name, uint8_t *user, int inactivetoo) {
    return get_user_common(h, -1, name, user, inactivetoo);
}

#define MAX_UID_GEN_TRIES     100
/* TODO: Find a better way to generate unique UIDs */
rc_ty sx_hashfs_generate_uid(sx_hashfs_t *h, uint8_t *uid) {
    unsigned int i = 0;

    if(!uid) {
        NULLARG();
        return FAIL_EINTERNAL;
    }

    if(qbegin(h->db)) {
        WARN("Failed to lock database");
        return FAIL_LOCKED;
    }

    sxi_rand_pseudo_bytes(uid + AUTH_CID_LEN, AUTH_UID_LEN - AUTH_CID_LEN);
    while(sx_hashfs_get_user_info(h, uid, NULL, NULL, NULL, NULL) != ENOENT && i < MAX_UID_GEN_TRIES) {
        i++;
        sxi_rand_pseudo_bytes(uid + AUTH_CID_LEN, AUTH_UID_LEN - AUTH_CID_LEN);
    }

    if(i == MAX_UID_GEN_TRIES) {
        WARN("Failed to generate user ID: reached iteration limit");
        return EEXIST;
    }

    qrollback(h->db);
    return OK;
}

rc_ty sx_hashfs_get_access(sx_hashfs_t *h, const uint8_t *user, const char *volume, sx_priv_t *access) {
    const sx_hashfs_volume_t *vol;
    rc_ty ret, rc;
    int r;
    int64_t owner_id;
    uint8_t owner_uid[AUTH_UID_LEN];

    if(!h || !user || !volume || !access)
	return EINVAL;

    ret = sx_hashfs_volume_by_name(h, volume, &vol);
    if(ret)
	return ret;

    sqlite3_reset(h->q_getaccess);
    if(qbind_int64(h->q_getaccess, ":volume", vol->id) ||
       qbind_blob(h->q_getaccess, ":user", user, AUTH_UID_LEN))
	return FAIL_EINTERNAL;

    r = qstep(h->q_getaccess);
    if(r == SQLITE_DONE) {
	*access = PRIV_NONE;
	return OK;
    }
    if(r != SQLITE_ROW)
	return FAIL_EINTERNAL;

    r = sqlite3_column_int(h->q_getaccess, 0);
    if(!(r & ~(PRIV_READ | PRIV_WRITE))) {
	ret = OK;
	*access = r;
    } else {
        char hex[AUTH_UID_LEN*2+1];

        bin2hex(user, AUTH_UID_LEN, hex, AUTH_UID_LEN*2+1);
	WARN("Found invalid priv for user %s on volume %lld: %d", hex, (long long int)vol->id, r);
    }

    owner_id = sqlite3_column_int64(h->q_getaccess, 1);
    if((rc = sx_hashfs_get_user_by_uid(h, owner_id, owner_uid, 0)) != OK) {
        WARN("Failed to get volume %s owner by ID", volume);
        return rc;
    }

    /* Compare common ID part of UIDs */
    if(!memcmp(owner_uid, user, AUTH_CID_LEN))
        *access |= PRIV_ACL;

    sqlite3_reset(h->q_getaccess);
    return ret;
}

sxi_db_t *sx_hashfs_eventdb(sx_hashfs_t *h) {
    return h->eventdb;
}

sxi_db_t *sx_hashfs_xferdb(sx_hashfs_t *h) {
    return h->xferdb;
}

sxc_client_t *sx_hashfs_client(sx_hashfs_t *h) {
    return h->sx;
}

sxi_conns_t *sx_hashfs_conns(sx_hashfs_t *h) {
    return h->sx_clust;
}


rc_ty sx_hashfs_job_result(sx_hashfs_t *h, job_t job, sx_uid_t uid, job_status_t *status, const char **message) {
    int r;

    if(!h || !status || !message) {
	NULLARG();
	return EFAULT;
    }

    sqlite3_reset(h->qe_getjob);

    if(qbind_int64(h->qe_getjob, ":id", job) ||
       qbind_int64(h->qe_getjob, ":owner", uid))
	return FAIL_EINTERNAL;

    r = qstep(h->qe_getjob);
    if(r == SQLITE_DONE)
	return ENOENT;

    if(r != SQLITE_ROW)
	return FAIL_EINTERNAL;

    if(!sqlite3_column_int(h->qe_getjob, 0)) {
	/* Pending job */
	*status = JOB_PENDING;
	*message = "Job status pending";
    } else {
	/* Completed */
	int result = sqlite3_column_int(h->qe_getjob, 1);
	if(result) {
	    /* Failed */
	    const char *reason = (const char *)sqlite3_column_text(h->qe_getjob, 2);
	    *status = JOB_ERROR;
	    if(!reason || !*reason)
		*message = "Unknown job failure";
	    else {
		sxi_strlcpy(h->job_message, reason, sizeof(h->job_message));
		*message = h->job_message;
	    }
	} else {
	    /* Succeded */
	    *status = JOB_OK;
	    *message = "Job completed successfully";
	}
    }

    sqlite3_reset(h->qe_getjob);
    return OK;
}

static const char *locknames[] = {
    "VOL", /* JOBTYPE_CREATE_VOLUME */
    "USER", /* JOBTYPE_CREATE_USER */
    "ACL", /* JOBTYPE_VOLUME_ACL */
    NULL, /* JOBTYPE_REPLICATE_BLOCKS */
    "TOKEN", /* JOBTYPE_FLUSH_FILE_REMOTE */
    "DELFILE", /* JOBTYPE_DELETE_FILE */
    "*", /* JOBTYPE_DISTRIBUTION */
    "STARTREBALANCE", /* JOBTYPE_STARTREBALANCE */
    "FINISHREBALANCE", /* JOBTYPE_FINISHREBALANCE */
    NULL, /* JOBTYPE_JLOCK */
    "REBALANCE_BLOCKS", /* JOBTYPE_REBALANCE_BLOCKS */
    "REBALANCE_FILES", /* JOBTYPE_REBALANCE_FILES */
    "REBALANCE_CLEANUP", /* JOBTYPE_REBALANCE_CLEANUP */
    "USER", /* JOBTYPE_DELETE_USER */
    "VOL", /* JOBTYPE_DELETE_VOLUME */
    "USER", /* JOBTYPE_NEWKEY_USER */
    "VOL", /* JOBTYPE_MODIFY_VOLUME */
    "*", /* JOBTYPE_REPLACE */
    "REPLACE_BLOCKS", /* JOBTYPE_REPLACE_BLOCKS */
    "REPLACE_FILES", /* JOBTYPE_REPLACE_FILES */
    NULL, /* JOBTYPE_DUMMY */
    NULL, /* JOBTYPE_REVSCLEAN */
    "DISTLOCK", /* JOBTYPE_DISTLOCK */
    "CLUSTER_MODE", /* JOBTYPE_CLUSTER_MODE */
    "IGNNODES", /* JOBTYPE_IGNODES */
    NULL, /* JOBTYPE_BLOCKS_REVISION */
    "TOKEN_LOCAL", /* JOBTYPE_FLUSH_FILE_LOCAL */
    "UPGRADE", /* JOBTYPE_UPGRADE_1_0_TO_1_1 */
};

#define MAX_PENDING_JOBS 128
rc_ty sx_hashfs_countjobs(sx_hashfs_t *h, sx_uid_t user_id) {
    rc_ty ret = FAIL_EINTERNAL;

    sqlite3_reset(h->qe_countjobs);
    if(qbind_int64(h->qe_countjobs, ":uid", user_id) ||
       qstep_ret(h->qe_countjobs)) 
	goto countjobs_out;
    if(sqlite3_column_int64(h->qe_countjobs, 0) > MAX_PENDING_JOBS) {
	ret = FAIL_ETOOMANY;
        DEBUG("too many jobs");
	goto countjobs_out;
    }
    ret = OK;

 countjobs_out:
    sqlite3_reset(h->qe_countjobs);
    return ret;
}

rc_ty sx_hashfs_job_new_begin(sx_hashfs_t *h) {
    int r;

    DEBUG("IN %s", __func__);
    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(h->addjob_begun) {
	msg_set_reason("Internal error: job_new_begin phase error");
	return FAIL_EINTERNAL;
    }

    if(qbegin(h->eventdb)) {
	msg_set_reason("Internal error: failed to start database transaction");
	return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qe_islocked);
    r = qstep(h->qe_islocked);
    if(r == SQLITE_ROW) {
	const char *owner = (const char *)sqlite3_column_text(h->qe_islocked, 0);
	msg_set_reason("The requested action cannot be completed because a complex operation is being executed on the cluster (by node %s). Please try again later.", owner);
	sqlite3_reset(h->qe_islocked);
	qrollback(h->eventdb);
	return FAIL_LOCKED;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to verify job lock");
	qrollback(h->eventdb);
	return FAIL_EINTERNAL;
    }

    h->addjob_begun = 1;
    return OK;
}

rc_ty sx_hashfs_job_new_end(sx_hashfs_t *h) {
    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(!h->addjob_begun) {
	msg_set_reason("Internal error: job_new_end phase error");
	return FAIL_EINTERNAL;
    }

    h->addjob_begun = 0;

    if(!qcommit(h->eventdb))
	return OK;

    msg_set_reason("Internal error: failed to commit new job(s) to database");
    qrollback(h->eventdb);

    return FAIL_EINTERNAL;
}

rc_ty sx_hashfs_job_new_abort(sx_hashfs_t *h) {
    if(!h) {
	NULLARG();
	return EFAULT;
    }
    
    if(!h->addjob_begun)
	return OK;

    qrollback(h->eventdb);
    h->addjob_begun = 0;
    return FAIL_EINTERNAL;
}

rc_ty sx_hashfs_job_new_notrigger(sx_hashfs_t *h, job_t parent, sx_uid_t user_id, job_t *job_id, jobtype_t type, unsigned int timeout_secs, const char *lock, const void *data, unsigned int datalen, const sx_nodelist_t *targets) {
    job_t id = JOB_FAILURE;
    char *lockstr = NULL;
    unsigned int i, ntargets;
    int r;
    rc_ty ret = FAIL_EINTERNAL, ret2;

    if(!h || !job_id) {
	msg_set_reason("Internal error: NULL argument given");
        return FAIL_EINTERNAL;
    }

    if(!h->addjob_begun) {
	msg_set_reason("Internal error: job_new phase error");
	return FAIL_EINTERNAL;
    }

    if((datalen && !data) || !targets) {
	msg_set_reason("Internal error: NULL argument given");
	goto addjob_error;
    }

    ntargets = sx_nodelist_count(targets);
    if(!ntargets) {
	msg_set_reason("Internal error: request with no targets");
	goto addjob_error;
    }

    if(!data)
	data = "";

    if ((unsigned)type >= sizeof(locknames) / sizeof(locknames[0])) {
	msg_set_reason("Internal error: bad action type");
	goto addjob_error;
    }
    if(lock && locknames[type]) {
	if(!(lockstr = malloc(2 + strlen(locknames[type]) + strlen(lock) + 1))) {
	    msg_set_reason("Not enough memory to create job");
	    goto addjob_error;
	}
	sprintf(lockstr, "$%s$%s", locknames[type], lock);
    }

    ret2 = sx_hashfs_countjobs(h, user_id);
    if(ret2 != OK) {
	ret = ret2;
	goto addjob_error;
    }

    if(parent == JOB_NOPARENT) {
	if(qbind_null(h->qe_addjob, ":parent"))
	    goto addjob_error;
    } else {
	if(qbind_int64(h->qe_addjob, ":parent", parent))
	    goto addjob_error;
    }

    if(qbind_int(h->qe_addjob, ":type", type) ||
       qbind_int(h->qe_addjob, ":expiry", timeout_secs) ||
       qbind_blob(h->qe_addjob, ":data", data, datalen)) {
	msg_set_reason("Internal error: failed to add job to database");
	goto addjob_error;
    }
    if(user_id == 0) {
	if(qbind_null(h->qe_addjob, ":uid"))
	    goto addjob_error;
    } else {
	if(qbind_int64(h->qe_addjob, ":uid", user_id))
	    goto addjob_error;
    }

    if(lockstr)
	r = qbind_text(h->qe_addjob, ":lock", lockstr);
    else
	r = qbind_null(h->qe_addjob, ":lock");
    if(r) {
	msg_set_reason("Internal error: failed to add job to database");
	goto addjob_error;
    }

    r = qstep(h->qe_addjob);
    if(r == SQLITE_CONSTRAINT) {
	msg_set_reason("Resource is temporarily locked%s%s", lockstr ? ": " : "", lockstr ? lockstr : "");
	ret = FAIL_LOCKED;
	goto addjob_error;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to add job to database");
	goto addjob_error;
    }

    id = sqlite3_last_insert_rowid(sqlite3_db_handle(h->qe_addjob));

    if(qbind_int64(h->qe_addact, ":job", id)) {
	msg_set_reason("Internal error: failed to add job action to database");
	goto addjob_error;
    }
    for(i=0; i<ntargets; i++) {
	const sx_node_t *node = sx_nodelist_get(targets, i);
	const sx_uuid_t *uuid = sx_node_uuid(node);
	if(qbind_blob(h->qe_addact, ":node", uuid->binary, sizeof(uuid->binary)) ||
	   qbind_text(h->qe_addact, ":addr", sx_node_addr(node)) ||
	   qbind_text(h->qe_addact, ":int_addr", sx_node_internal_addr(node)) ||
	   qbind_int64(h->qe_addact, ":capa", sx_node_capacity(node)) ||
	   qstep_noret(h->qe_addact)) {
	    msg_set_reason("Internal error: failed to add job action to database");
	    goto addjob_error;
	}
    }

    ret = OK;

 addjob_error:
    if(ret != OK) {
	qrollback(h->eventdb);
	h->addjob_begun = 0;
	id = JOB_FAILURE;
    }

    free(lockstr);
    sqlite3_reset(h->qe_addjob);
    sqlite3_reset(h->qe_addact);

    *job_id = id;
    return ret;
}

rc_ty sx_hashfs_job_new(sx_hashfs_t *h, sx_uid_t user_id, job_t *job_id, jobtype_t type, unsigned int timeout_secs, const char *lock, const void *data, unsigned int datalen, const sx_nodelist_t *targets) {
    rc_ty ret;

    if((ret = sx_hashfs_job_new_begin(h)) == OK && 
       (ret = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, user_id, job_id, type, timeout_secs, lock, data, datalen, targets)) == OK &&
       (ret = sx_hashfs_job_new_end(h)) == OK)
	sx_hashfs_job_trigger(h);

    return ret;
}

rc_ty sx_hashfs_job_lock(sx_hashfs_t *h, const char *owner) {
    rc_ty ret = FAIL_EINTERNAL;
    sx_uuid_t node;
    int r;

    if(!h || !owner) {
	NULLARG();
	return EFAULT;
    }

    if(uuid_from_string(&node, owner)) {
	msg_set_reason("Invalid lock owner");
	return EINVAL;
    }

    if(qbegin(h->eventdb)) {
	msg_set_reason("Internal error: failed to start database transaction");
	return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qe_islocked);
    r = qstep(h->qe_islocked);
    if(r == SQLITE_ROW) {
	const char *curowner = (const char *)sqlite3_column_text(h->qe_islocked, 0);
	msg_set_reason("This node is already locked (by node %s). Please try again later.", curowner);
	sqlite3_reset(h->qe_islocked);
	ret = FAIL_LOCKED;
	goto job_lock_err;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to verify job lock");
	goto job_lock_err;
    }

    sqlite3_reset(h->qe_hasjobs);
    r = qstep(h->qe_hasjobs);
    if(r == SQLITE_ROW) {
	msg_set_reason("There are active jobs on this node and it currently cannot be locked. Please try again later.");
	sqlite3_reset(h->qe_hasjobs);
	ret = FAIL_LOCKED;
	goto job_lock_err;
    }
    if(r != SQLITE_DONE) {
	msg_set_reason("Internal error: failed to verify active job presence");
	goto job_lock_err;
    }

    sqlite3_reset(h->qe_lock);
    if(qbind_text(h->qe_lock, ":node", owner) ||
       qstep_noret(h->qe_lock) ||
       qcommit(h->eventdb)) {
	msg_set_reason("Internal error: failed to activate cluster locking");
	goto job_lock_err;
    }

    return OK;

 job_lock_err:
    qrollback(h->eventdb);
    return ret;
}

rc_ty sx_hashfs_job_unlock(sx_hashfs_t *h, const char *owner) {
    rc_ty ret = FAIL_EINTERNAL;
    const char *curowner;
    sx_uuid_t node;
    int r;

    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(owner && uuid_from_string(&node, owner)) {
	msg_set_reason("Invalid lock owner");
	return EINVAL;
    }

    if(qbegin(h->eventdb)) {
	msg_set_reason("Internal error: failed to start database transaction");
	return FAIL_EINTERNAL;
    }

    sqlite3_reset(h->qe_islocked);
    r = qstep(h->qe_islocked);
    if(r == SQLITE_DONE) {
	ret = OK;
	goto job_unlock_err;
    }

    if(r != SQLITE_ROW) {
	msg_set_reason("Internal error: failed to verify job lock");
	goto job_unlock_err;
    }

    if(owner) {
	curowner = (const char *)sqlite3_column_text(h->qe_islocked, 0);
	r = strcmp(owner, curowner);
	if(r) {
	    msg_set_reason("This node is locked by %s and cannot be unlocked by %s", curowner, owner);
	    sqlite3_reset(h->qe_islocked);
	    goto job_unlock_err;
	}
    }
    sqlite3_reset(h->qe_islocked);

    sqlite3_reset(h->qe_unlock);
    if(qstep_noret(h->qe_unlock) ||
       qcommit(h->eventdb)) {
	msg_set_reason("Internal error: failed to deactivate cluster locking");
	goto job_unlock_err;
    }

    return OK;

 job_unlock_err:
    qrollback(h->eventdb);
    return ret;
}

static void ignore(int v)
{
    if (v < 0)
        PWARN("ignoring write failure");
}

void sx_hashfs_job_trigger(sx_hashfs_t *h) {
    if(h && h->job_trigger >= 0) {
        ignore(write(h->job_trigger, ".", 1));
    }
}

void sx_hashfs_xfer_trigger(sx_hashfs_t *h) {
    if(h && h->xfer_trigger >= 0) {
        ignore(write(h->xfer_trigger, ".", 1));
    }
}

void sx_hashfs_gc_trigger(sx_hashfs_t *h) {
    if(h && h->gc_trigger >= 0) {
        INFO("triggered GC");
        ignore(write(h->gc_trigger, ".", 1));
    }
}

rc_ty sx_hashfs_xfer_tonodes(sx_hashfs_t *h, sx_hash_t *block, unsigned int size, const sx_nodelist_t *targets) {
    const sx_node_t *self = sx_hashfs_self(h);
    unsigned int i, nnodes;
    rc_ty ret;

    if(!targets) {
	NULLARG();
	return EFAULT;
    }

    if(!self) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }
    if (sx_hashfs_block_get(h, size, block, NULL) != OK) {
        char hash[sizeof(sx_hash_t)*2+1];
        bin2hex(block->b, sizeof(block->b), hash, sizeof(hash));
        DEBUG("Asked to push a hash we don't have: #%s#", hash);
    }

    nnodes = sx_nodelist_count(targets);
    sqlite3_reset(h->qx_add);

    if(qbind_blob(h->qx_add, ":b", block, sizeof(*block))) {
	ret = FAIL_EINTERNAL;
	goto xfer_err;
    }

    for(i=0; i<nnodes; i++) {
	const sx_node_t *target = sx_nodelist_get(targets, i);
	const sx_uuid_t *target_uuid;
	int r;

	if(!sx_node_cmp(target, self))
	    continue;

	target_uuid = sx_node_uuid(target);
	if(qbind_int(h->qx_add, ":s", size) ||
	   qbind_blob(h->qx_add, ":n", target_uuid->binary, sizeof(target_uuid->binary))) {
	    break;
	}
	r = qstep(h->qx_add);
	if(r != SQLITE_DONE && r != SQLITE_CONSTRAINT)
	    break;

	sqlite3_reset(h->qx_add);
    }

    ret = (i == nnodes) ? OK : FAIL_EINTERNAL;
    DEBUG("xfer_to_nodes job added: %s", ret == OK ? "OK" : "Error");

    /* Trigger block manager to perform pushes */
    sx_hashfs_xfer_trigger(h);
 xfer_err:
    sqlite3_reset(h->qx_add);

    if(ret != OK)
	msg_set_reason("Internal error: failed to add block transfer request to database");

    return ret;
}

rc_ty sx_hashfs_xfer_tonode(sx_hashfs_t *h, sx_hash_t *block, unsigned int size, const sx_node_t *target) {
    sx_nodelist_t *targets = sx_nodelist_new();
    rc_ty ret;

    if(!targets)
	return ENOMEM;

    ret = sx_nodelist_add(targets, sx_node_dup(target));

    if(ret == OK)
	ret = sx_hashfs_xfer_tonodes(h, block, size, targets);

    sx_nodelist_delete(targets);

    return ret;
}

static rc_ty hash_of_blob_result(sx_hash_t *hash, sqlite3_stmt *stmt, int col)
{
    int len = sqlite3_column_bytes(stmt, col);
    if (len != sizeof(hash->b)) {
        WARN("Bad blob result length: %d", len);
        return FAIL_EINTERNAL;
    }
    memcpy(hash->b, sqlite3_column_blob(stmt, col), sizeof(hash->b));
    return OK;
}

static rc_ty foreach_hdb_blob(sx_hashfs_t *h, int *terminate,
                              sqlite3_stmt *loop[][HASHDBS], const char *loopvar, int col, uint64_t *count)
{
    unsigned i,j;
    int64_t k;
    if (!h || !terminate || !loop || !count) {
	NULLARG();
        return EFAULT;
    }
    *count = 0;
    for (j=0;j<SIZES && !*terminate;j++) {
        for (i=0;i<HASHDBS && !*terminate;i++) {
            int ret;
            sqlite3_stmt *q = loop[j][i];
            DEBUG("Running %s", sqlite3_sql(q));
            sqlite3_stmt *q_gc1 = h->qb_gc_revision_blocks[j][i];
            sqlite3_stmt *q_gc2 = h->qb_gc_revision[j][i];
            sqlite3_stmt *q_gc3 = h->qb_gc_reserve[j][i];
            sqlite3_reset(q);
            if (loopvar && qbind_blob(q, loopvar, "", 0))
                return FAIL_EINTERNAL;
            int has_last;
            do {
                has_last = 0;
                if (qbegin(h->datadb[j][i]))
                    return FAIL_EINTERNAL;
                ret = SQLITE_ROW;
                sqlite3_reset(q);
                sx_hash_t last;
                for (k=0;k<gc_max_batch && ret == SQLITE_ROW && !*terminate; k++) {
                    ret = qstep(q);
                    if (ret == SQLITE_ROW) {
                        sx_hash_t var;
                        sqlite3_reset(q_gc1);
                        sqlite3_reset(q_gc2);
                        if (!hash_of_blob_result(&last, q, 0) &&
                            !hash_of_blob_result(&var, q, col) &&
                            !qbind_blob(q_gc1, ":revision_id", var.b, sizeof(var.b)) &&
                            !qbind_blob(q_gc2, ":revision_id", var.b, sizeof(var.b)) &&
                            !qbind_blob(q_gc3, ":revision_id", var.b, sizeof(var.b)) &&
                            !qstep_noret(q_gc1)) {
                            has_last = 1;
                            k += sqlite3_changes(h->datadb[j][i]->handle);
                            if (!qstep_noret(q_gc2)) {
                                k += sqlite3_changes(h->datadb[j][i]->handle);
                                if (!qstep_noret(q_gc3)) {
                                    k += sqlite3_changes(h->datadb[j][i]->handle);
                                    (*count)++;
                                }
                            } else {
                                ret = -1;
                            }
                        } else {
                            ret = -1;
                        }
                        DEBUGHASH("Got revision_id", &var);
                        DEBUGHASH("Got loop id", &last);
                    }
                }
                sqlite3_reset(q);
                if ((ret == SQLITE_ROW || ret == SQLITE_DONE) && qcommit(h->datadb[j][i]))
                    return FAIL_EINTERNAL;
                if(loopvar && has_last && qbind_blob(q, loopvar, last.b, sizeof(last.b)))
                    ret = -1;
            } while (!*terminate && (ret == SQLITE_ROW || (ret == SQLITE_DONE && has_last)));
            sqlite3_reset(q);
            sqlite3_reset(q_gc1);
            sqlite3_reset(q_gc2);
            if (ret != SQLITE_DONE) {
                qrollback(h->datadb[j][i]);
                return FAIL_EINTERNAL;
            }
        }
    }
    return OK;
}

static rc_ty bindall(sqlite3_stmt *stmt[][HASHDBS], const char *var, int64_t val)
{
    unsigned j, i;
    if (!stmt) {
        NULLARG();
        return EFAULT;
    }
    for (j=0;j<SIZES;j++) {
        for (i=0;i<HASHDBS;i++) {
            sqlite3_reset(stmt[j][i]);
            if (qbind_int64(stmt[j][i], var, val))
                return FAIL_EINTERNAL;
        }
    }
    return OK;
}

rc_ty sx_hashfs_gc_periodic(sx_hashfs_t *h, int *terminate, int grace_period)
{
    /* tokens expire when there was no upload activity within GC_GRACE_PERIOD
     * (i.e. ~2 days) */
    uint64_t real_now = time(NULL), now;
    uint64_t gc_noactivity = 0, gc_ttl = 0, expires = real_now - grace_period;
    rc_ty ret = OK;
    sqlite3_reset(h->qt_gc_revisions);
    if (grace_period < 0) {
        now = 1ll << 62;
        DEBUG("now set to: %lld", (long long)now);
    } else
        now = real_now;
    if (qbind_int64(h->qt_gc_revisions, ":now", now) ||
        qstep_noret(h->qt_gc_revisions))
        return FAIL_EINTERNAL;
    INFO("Deleted %d tokens", sqlite3_changes(h->tempdb->handle));
    DEBUG("find_expired, expires: %lld", (long long)expires);
    if (bindall(h->qb_find_expired_reservation, ":expires", expires) ||
        bindall(h->qb_find_expired_reservation2, ":now", now) ||
        foreach_hdb_blob(h, terminate,
                         h->qb_find_expired_reservation, ":lastreserve_id",
                         1, &gc_noactivity) ||
        foreach_hdb_blob(h, terminate,
                         h->qb_find_expired_reservation2, NULL, 0, &gc_ttl))
        ret = FAIL_EINTERNAL;
    INFO("GCed reservations: no activity %lld, ttl expired %lld", (long long)gc_noactivity, (long long)gc_ttl);
    if (gc_eventdb(h))
        ret = FAIL_EINTERNAL;
    return ret;
}

rc_ty sx_hashfs_gc_run(sx_hashfs_t *h, int *terminate)
{
    unsigned i, j, k;
    uint64_t gc_unused_tokens = 0, gc_blocks = 0;
    int ret = 0;
    ret = 0;

    int age = sxi_hdist_version(h->hd);
    if (bindall(h->qb_find_unused_revision, ":age", age) ||
        foreach_hdb_blob(h, terminate,
                         h->qb_find_unused_revision, ":last_revision_id",
                         0, &gc_unused_tokens))
        ret = -1;
    for (j=0;j<SIZES && !ret && !*terminate ;j++) {
        for (i=0;i<HASHDBS && !ret && !*terminate;i++) {
            int64_t last = 0;
            sqlite3_stmt *q = h->qb_find_unused_block[j][i];
            sqlite3_stmt *q_gc = h->qb_gc1[j][i];
            sqlite3_stmt *q_setfree = h->qb_setfree[j][i];
            do {
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                sqlite3_reset(q_setfree);
                if (qbind_int64(q, ":last", last))
                    break;
                if (qbegin(h->datadb[j][i])) {
                    ret = -1;
                    break;
                }
                for (k=0;k<gc_max_batch && (ret = qstep(q)) == SQLITE_ROW && !*terminate; k++) {
                    int is_null = sqlite3_column_type(q, 1) == SQLITE_NULL;
                    last = sqlite3_column_int64(q, 0);
                    const sx_hash_t *hash = sqlite3_column_blob(q, 2);
                    if (hash && sqlite3_column_bytes(q, 2) == sizeof(*hash)) {
                        if (sx_hashfs_blkrb_can_gc(h, hash, bsz[j]) != OK) {
                            DEBUGHASH("Hash is locked by rebalance", hash);
                            continue;
                        }
                    }
                    if (hash)
                        DEBUGHASH("freeing block with hash", hash);
                    if (!is_null) {
                        int64_t blockno = sqlite3_column_int64(q, 1);
                        DEBUG("freeing blockno %ld, @%d/%d/%ld", blockno, j, i, blockno * bsz[j]);
                        if (qbind_int64(q_setfree, ":blockno", blockno) ||
                            qstep_noret(q_setfree)) {
                            ret = -1;
                            break;
                        }
                    }
                    if (qbind_int64(q_gc, ":blockid", last) ||
                        qstep_noret(q_gc)) {
                        ret = -1;
                        break;
                    }
                    gc_blocks++;
                }
                sqlite3_reset(q);
                sqlite3_reset(q_gc);
                sqlite3_reset(q_setfree);
                if (ret == SQLITE_DONE)
                    ret = 0;
                if (!ret || ret == SQLITE_ROW) {
                    if (qcommit(h->datadb[j][i])) {
                        ret = -1;
                        break;
                    }
                } else
                    qrollback(h->datadb[j][i]);
            } while (ret == SQLITE_ROW && !*terminate);
        }
    }
    INFO("GCed %lld hashes and %lld unused tokens", (long long)gc_blocks, (long long)gc_unused_tokens);
    return ret ? FAIL_EINTERNAL : OK;
}

static rc_ty print_datadb_count(sx_hashfs_t *h, const char *table, int *terminate)
{
    rc_ty ret = OK;
    uint64_t count = 0;
    unsigned i,j;
    char query[128];
    sqlite3_stmt *q = NULL;
    snprintf(query, sizeof(query), "SELECT COUNT(*) FROM %s", table);
    for (j=0;j<SIZES && !*terminate;j++) {
        for (i=0;i<HASHDBS && !*terminate;i++) {
            qnullify(q);
            if (qprep(h->datadb[j][i], &q, query) || qstep_ret(q)) {
                WARN("print_count failed");
                ret = FAIL_EINTERNAL;
                break;
            }
            count += sqlite3_column_int64(q, 0);
            qnullify(q);
        }
    }
    qnullify(q);
    if (!terminate)
        return ret;
    INFO("Table %s has %lld entries", table, (long long)count);
    return ret;
}

rc_ty sx_hashfs_gc_info(sx_hashfs_t *h, int *terminate)
{
    rc_ty ret = OK;
    struct timeval tv0, tv1;
    gettimeofday(&tv0, NULL);
    if (print_datadb_count(h, "blocks", terminate) ||
        print_datadb_count(h, "revision_ops", terminate) ||
        print_datadb_count(h, "reservations", terminate) ||
        print_datadb_count(h, "revision_blocks", terminate) ||
        print_datadb_count(h, "avail", terminate))
        ret = FAIL_EINTERNAL;
    gettimeofday(&tv1, NULL);
    INFO("GC info completed in %.1fs", timediff(&tv0, &tv1));
    return ret;
}

rc_ty sx_hashfs_gc_expire_all_reservations(sx_hashfs_t *h)
{
    if(h && h->gc_trigger >= 0 && h->gc_expire_trigger >= 0) {
        INFO("triggered force expire");
        ignore(write(h->gc_expire_trigger, ".", 1));
        ignore(write(h->gc_trigger, ".", 1));
    }
    return OK;
}

int64_t sx_hashfs_hdist_getversion(sx_hashfs_t *h) {
    return h ? h->hd_rev : 0;
}

rc_ty sx_hashfs_hdist_change_req(sx_hashfs_t *h, const sx_nodelist_t *newdist, job_t *job_id) {
    sxi_hdist_t *newmod;
    unsigned int nnodes, minnodes, i, cfg_len;
    int64_t newclustersize = 0, minclustersize;
    sx_nodelist_t *targets;
    job_t finish_job;
    const void *cfg;
    rc_ty r;

    DEBUG("IN %s", __func__);
    if(!h || !newdist || !job_id) {
	NULLARG();
	return EFAULT;
    }

    if(!h->have_hd) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is still being rebalanced");
	return EINVAL;
    }

    if(sx_nodelist_count(h->faulty_nodes)) {
	msg_set_reason("The cluster contains faulty nodes which are still being replaced");
	return EINVAL;
    }

    if(sx_nodelist_count(h->ignored_nodes)) {
	msg_set_reason("The cluster is degraded, please replace all faulty nodes and try again");
	return EINVAL;
    }

    r = get_min_reqs(h, &minnodes, &minclustersize);
    if(r) {
	msg_set_reason("Failed to compute cluster requirements");
	return r;
    }

    nnodes = sx_nodelist_count(newdist);
    if(nnodes < minnodes) {
	msg_set_reason("Invalid distribution: this cluster requires at least %u nodes to operate.", minnodes);
	return EINVAL;
    }

    if((r = sxi_hdist_get_cfg(h->hd, &cfg, &cfg_len)) != OK) {
	msg_set_reason("Failed to duplicate current distribution (get)");
	return r;
    }

    if(!(newmod = sxi_hdist_from_cfg(cfg, cfg_len))) {
	msg_set_reason("Failed to duplicate current distribution (from_cfg)");
	return EINVAL;
    }

    if((r = sxi_hdist_newbuild(newmod))) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to update current distribution");
	return r;
    }

    for(i=0; i<nnodes; i++) {
	const sx_node_t *n = sx_nodelist_get(newdist, i);
	unsigned int j;
	if(sx_node_capacity(n) < SXLIMIT_MIN_NODE_SIZE) {
	    sxi_hdist_free(newmod);
	    msg_set_reason("Invalid capacity: Node %s cannot be smaller than %u bytes", sx_node_uuid_str(n), SXLIMIT_MIN_NODE_SIZE);
	    return EINVAL;
	}
	for(j=i+1; j<nnodes; j++) {
	    const sx_node_t *other = sx_nodelist_get(newdist, j);
	    if(!sx_node_cmp(n, other)) {
		sxi_hdist_free(newmod);
		msg_set_reason("Node %s cannot appear more than once", sx_node_uuid_str(n));
		return EINVAL;
	    }
	    if(!sx_node_cmp_addrs(n, other)) {
		sxi_hdist_free(newmod);
		msg_set_reason("Node %s and %s share the same address", sx_node_uuid_str(n), sx_node_uuid_str(other));
		return EINVAL;
	    }
	}
	newclustersize += sx_node_capacity(n);
	r = sxi_hdist_addnode(newmod, sx_node_uuid(n), sx_node_addr(n), sx_node_internal_addr(n), sx_node_capacity(n), NULL);
	if(r) {
	    sxi_hdist_free(newmod);
	    msg_set_reason("Failed to update current distribution");
	    return FAIL_EINTERNAL;
	}
    }

    if(newclustersize < minclustersize) {
	sxi_hdist_free(newmod);
	msg_set_reason("Invalid distribution: this cluster requires a total capacity of at least %lld bytes to operate.", (long long)minclustersize);
	return EINVAL;
    }

    if((r = sxi_hdist_build(newmod)) != OK) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to build updated distribution");
	return r;
    }

    if((r = sxi_hdist_get_cfg(newmod, &cfg, &cfg_len)) != OK) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to retrieve updated distribution");
	return r;
    }

    targets = sx_nodelist_new();
    if(!targets) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to setup job targets");
	return ENOMEM;
    }

    if((r = sx_nodelist_addlist(targets, sxi_hdist_nodelist(newmod, 1))) ||
       (r = sx_nodelist_addlist(targets, sxi_hdist_nodelist(newmod, 0)))) {
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to setup job targets");
	return r;
    }

    r = sx_hashfs_job_new_begin(h);
    if(r) {
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	return r;
    }

    /* FIXMERB: review TTL and ttl_extend */
    r = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, 0, job_id, JOBTYPE_JLOCK, sx_nodelist_count(targets) * 20, "JLOCK", NULL, 0, targets);
    if(r) {
	INFO("job_new (jlock) returned: %s", rc2str(r));
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, *job_id, 0, job_id, JOBTYPE_DISTRIBUTION, sx_nodelist_count(targets) * 120, "DISTRIBUTION", cfg, cfg_len, targets);
    if(r) {
	INFO("job_new (distribution) returned: %s", rc2str(r));
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, *job_id, 0, job_id, JOBTYPE_STARTREBALANCE, sx_nodelist_count(targets) * 120, "DISTRIBUTION", NULL, 0, targets);
    if(r) {
	INFO("job_new (startrebalance) returned: %s", rc2str(r));
	sx_nodelist_delete(targets);
	sxi_hdist_free(newmod);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, *job_id, 0, &finish_job, JOBTYPE_FINISHREBALANCE, JOB_NO_EXPIRY, "DISTRIBUTION", NULL, 0, targets);
    sx_nodelist_delete(targets);
    sxi_hdist_free(newmod);
    if(r) {
	INFO("job_new (finishrebalance) returned: %s", rc2str(r));
	return r;
    }

    r = sx_hashfs_job_new_end(h);
    if(r)
	INFO("Failed to commit jobadd");
    else
	sx_hashfs_job_trigger(h);

    return r;
}

rc_ty sx_hashfs_hdist_replace_req(sx_hashfs_t *h, const sx_nodelist_t *replacements, job_t *job_id) {
    unsigned int nnodes, cnodes, i, cfg_len;
    const sx_nodelist_t *curdist, *targets;
    sxi_hdist_t *newmod;
    sx_blob_t *jdata;
    const void *cfg;
    rc_ty r;

    DEBUG("IN %s", __func__);
    if(!h || !replacements || !job_id) {
	NULLARG();
	return EFAULT;
    }

    if(!h->have_hd) {
	WARN("Called before initialization");
	return FAIL_EINIT;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is being rebalanced");
	return EINVAL;
    }

    if(sx_nodelist_count(h->faulty_nodes)) {
	msg_set_reason("The cluster contains faulty nodes which are still being replaced");
	return EINVAL;
    }

    for(i = 0; i<sx_nodelist_count(h->ignored_nodes); i++) {
	const sx_node_t *ignode = sx_nodelist_get(h->ignored_nodes, i);
	const sx_uuid_t *ignuuid = sx_node_uuid(ignode);
	if(sx_nodelist_lookup(replacements, ignuuid))
	    continue;
	msg_set_reason("Faulty node %s must be replaced too", ignuuid->string);
	return EINVAL;
    }

    curdist = sx_hashfs_all_nodes(h, NL_PREV);
    nnodes = sx_nodelist_count(replacements);
    if(!nnodes) {
	msg_set_reason("No node replacement requested");
	return EINVAL;
    }
    cnodes = sx_nodelist_count(curdist);
    if(nnodes >= cnodes) {
	msg_set_reason("Too many replaced nodes");
	return EINVAL;
    }

    for(i=0; i<nnodes; i++) {
	const sx_node_t *newnode = sx_nodelist_get(replacements, i);
	const sx_uuid_t *newuuid = sx_node_uuid(newnode);
	const sx_node_t *oldnode = sx_nodelist_lookup(curdist, newuuid);

	if(!oldnode) {
	    msg_set_reason("Node %s is not a current cluster member", newuuid->string);
	    return EINVAL;
	}
	if(sx_node_capacity(newnode) != sx_node_capacity(oldnode)) {
	    msg_set_reason("Node %s can only be replaced with a new node of the same size (%lld bytes)", newuuid->string, (long long)sx_node_capacity(oldnode));
	    return EINVAL;
	}
    }

    if((r = sxi_hdist_get_cfg(h->hd, &cfg, &cfg_len)) != OK) {
	msg_set_reason("Failed to duplicate current distribution (get)");
	return r;
    }

    if(!(newmod = sxi_hdist_from_cfg(cfg, cfg_len))) {
	msg_set_reason("Failed to duplicate current distribution (from_cfg)");
	return EINVAL;
    }

    if((r = sxi_hdist_newbuild(newmod))) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to update current distribution");
	return r;
    }

    for(i=0; i<cnodes; i++) {
	const sx_node_t *oldnode = sx_nodelist_get(curdist, i);
	const sx_node_t *newnode = sx_nodelist_lookup(replacements, sx_node_uuid(oldnode));
	if(newnode) {
	    unsigned int j;
	    for(j=i+1; j<cnodes; j++) {
		const sx_node_t *other = sx_nodelist_get(curdist, j);
		if(!sx_node_cmp_addrs(newnode, other)) {
		    sxi_hdist_free(newmod);
		    msg_set_reason("Node %s and %s share the same address", sx_node_uuid_str(newnode), sx_node_uuid_str(other));
		    return EINVAL;
		}
	    }
	    oldnode = newnode;
	}
	r = sxi_hdist_addnode(newmod, sx_node_uuid(oldnode), sx_node_addr(oldnode), sx_node_internal_addr(oldnode), sx_node_capacity(oldnode), NULL);
	if(r) {
	    sxi_hdist_free(newmod);
	    msg_set_reason("Failed to update current distribution");
	    return FAIL_EINTERNAL;
	}
    }

    if((r = sxi_hdist_build(newmod)) != OK) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to build updated distribution");
	return r;
    }

    if(sxi_hdist_rebalanced(newmod)) {
	msg_set_reason("Failed to flat the current distribution");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    if((r = sxi_hdist_get_cfg(newmod, &cfg, &cfg_len)) != OK) {
	sxi_hdist_free(newmod);
	msg_set_reason("Failed to retrieve updated distribution");
	return r;
    }

    jdata = sx_nodelist_to_blob(replacements);
    if(!jdata || sx_blob_add_blob(jdata, cfg, cfg_len)) {
	msg_set_reason("Failed to define job data");
	sxi_hdist_free(newmod);
	sx_blob_free(jdata);
	return ENOMEM;
    }
    sx_blob_to_data(jdata, &cfg, &cfg_len);

    targets = sxi_hdist_nodelist(newmod, 0);
    r = sx_hashfs_job_new_begin(h);
    if(r) {
	msg_set_reason("Failed to setup job targets");
	sxi_hdist_free(newmod);
	sx_blob_free(jdata);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, 0, job_id, JOBTYPE_JLOCK, sx_nodelist_count(targets) * 20, "JLOCK", NULL, 0, targets);
    if(r) {
	INFO("job_new (jlock) returned: %s", rc2str(r));
	sxi_hdist_free(newmod);
	sx_blob_free(jdata);
	return r;
    }

    r = sx_hashfs_job_new_notrigger(h, *job_id, 0, job_id, JOBTYPE_REPLACE, sx_nodelist_count(targets) * 120, "DISTRIBUTION", cfg, cfg_len, targets);
    sxi_hdist_free(newmod);
    sx_blob_free(jdata);
    if(r) {
	INFO("job_new (replace) returned: %s", rc2str(r));
	return r;
    }

    r = sx_hashfs_job_new_end(h);
    if(r)
	INFO("Failed to commit jobadd");
    else
	sx_hashfs_job_trigger(h);

    return r;
}

rc_ty sx_hashfs_hdist_change_add(sx_hashfs_t *h, const void *cfg, unsigned int cfg_len) {
    int64_t newclustersize = 0, minclustersize;
    unsigned int nnodes, minnodes, i;
    sxi_hdist_t *newmod;
    const sx_nodelist_t *nodes;
    sqlite3_stmt *q = NULL;
    rc_ty ret;

    DEBUG("IN %s", __func__);
    if(!h || !cfg) {
	NULLARG();
	return EINVAL;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is being rebalanced");
	return EINVAL;
    }

    if(sx_nodelist_count(h->faulty_nodes)) {
	msg_set_reason("The cluster contains faulty nodes which are still being replaced");
	return EINVAL;
    }

    newmod = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!newmod) {
	msg_set_reason("Failed to load the new distribution");
	return EINVAL;
    }

    if(sxi_hdist_buildcnt(newmod) != 2 ||
       (h->have_hd && (!sxi_hdist_same_origin(newmod, h->hd) || sxi_hdist_version(newmod) != sxi_hdist_version(h->hd) + 1))) {
	sxi_hdist_free(newmod);
	msg_set_reason("The new model is not a direct descendent of the current model");
	return EINVAL;
    }

    if(qbegin(h->db)) {
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    ret = get_min_reqs(h, &minnodes, &minclustersize);
    if(ret) {
	msg_set_reason("Failed to compute cluster requirements");
	goto change_add_fail;
    }

    nodes = sxi_hdist_nodelist(newmod, 0);
    if(!nodes || !(nnodes = sx_nodelist_count(nodes))) {
	msg_set_reason("Failed to retrieve the list of the updated nodes");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(nnodes < minnodes) {
	msg_set_reason("Invalid distribution: this cluster requires at least %u nodes to operate.", minnodes);
	ret = EINVAL;
	goto change_add_fail;
    }

    for(i = 0; i<nnodes; i++) {
	const sx_node_t *n = sx_nodelist_get(nodes, i);
	unsigned int j;
	if(sx_node_capacity(n) < SXLIMIT_MIN_NODE_SIZE) {
	    msg_set_reason("Invalid capacity: Node %s cannot be smaller than %u bytes", sx_node_uuid_str(n), SXLIMIT_MIN_NODE_SIZE);
	    ret = EINVAL;
	    goto change_add_fail;
	}
	for(j=i+1; j<nnodes; j++) {
	    const sx_node_t *other = sx_nodelist_get(nodes, j);
	    if(!sx_node_cmp(n, other)) {
		msg_set_reason("Node %s cannot appear more than once", sx_node_uuid_str(n));
		ret = EINVAL;
		goto change_add_fail;
	    }
	    if(!sx_node_cmp_addrs(n, other)) {
		msg_set_reason("Node %s and %s share the same address", sx_node_uuid_str(n), sx_node_uuid_str(other));
		ret = EINVAL;
		goto change_add_fail;
	    }
	}
	newclustersize += sx_node_capacity(n);
    }

    if(newclustersize < minclustersize) {
	msg_set_reason("Invalid distribution: this cluster requires a total capacity of at least %lld bytes to operate.", (long long)minclustersize);
	ret = EINVAL;
	goto change_add_fail;
    }

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)")) {
	msg_set_reason("Failed to save the updated distribution model");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(h->have_hd) {
	const void *cur_cfg;
	unsigned int cur_cfg_len;

	ret = sxi_hdist_get_cfg(h->hd, &cur_cfg, &cur_cfg_len);
	if(ret) {
	    msg_set_reason("Failed to retrieve the current distribution model");
	    goto change_add_fail;
	}
	if(qbind_text(q, ":k", "current_dist") ||
	   qbind_blob(q, ":v", cur_cfg, cur_cfg_len) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save current distribution model");
	    ret = FAIL_EINTERNAL;
	    goto change_add_fail;
	}

	sqlite3_reset(q);
	if(qbind_text(q, ":k", "current_dist_rev") ||
	   qbind_int64(q, ":v", sxi_hdist_version(h->hd)) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save current distribution model");
	    ret = FAIL_EINTERNAL;
	    goto change_add_fail;
	}
	sqlite3_reset(q);
    }

    if(qbind_text(q, ":k", "dist") ||
       qbind_blob(q, ":v", cfg, cfg_len) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save target distribution model");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    sqlite3_reset(q);
    if(qbind_text(q, ":k", "dist_rev") ||
       qbind_int64(q, ":v", sxi_hdist_version(newmod)) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save target distribution model");
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(sx_hashfs_set_progress_info(h, INPRG_REBALANCE_RUNNING, "Updating distribution model")) {
	ret = FAIL_EINTERNAL;
	goto change_add_fail;
    }

    if(qcommit(h->db)) {
	msg_set_reason("Failed to save distribution model");
	ret = FAIL_EINTERNAL;
    } else {
	ret = OK;
	DEBUG("Distribution change added from %lld to %lld", (long long)h->hd_rev, (long long)sxi_hdist_version(newmod));
    }
 change_add_fail:
    qnullify(q);
    if(ret != OK)
	qrollback(h->db);
    sxi_hdist_free(newmod);

    return ret;
}

rc_ty sx_hashfs_hdist_replace_add(sx_hashfs_t *h, const void *cfg, unsigned int cfg_len, const sx_nodelist_t *badnodes) {
    unsigned int nnodes, badnnodes, i;
    sxi_hdist_t *newmod;
    const sx_nodelist_t *oldnodes, *newnodes;
    sx_nodelist_t *faulty = NULL;
    sqlite3_stmt *q = NULL;
    rc_ty ret;

    DEBUG("IN %s", __func__);
    if(!h || !cfg || !badnodes) {
	NULLARG();
	return EINVAL;
    }

    badnnodes = sx_nodelist_count(badnodes);
    if(!badnnodes) {
	msg_set_reason("No node to be replaced was provided");
	return EINVAL;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is being rebalanced");
	return EINVAL;
    }

    if(sx_nodelist_count(h->faulty_nodes)) {
	msg_set_reason("The cluster contains faulty nodes which are still being replaced");
	return EINVAL;
    }

    for(i = 0; i<sx_nodelist_count(h->ignored_nodes); i++) {
	const sx_node_t *ignode = sx_nodelist_get(h->ignored_nodes, i);
	const sx_uuid_t *ignuuid = sx_node_uuid(ignode);
	if(sx_nodelist_lookup(badnodes, ignuuid))
	    continue;
	msg_set_reason("Node %s is currently marked as fault and must be replaced", ignuuid->string);
	return EINVAL;
    }

    newmod = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!newmod) {
	msg_set_reason("Failed to load the new distribution");
	return EINVAL;
    }

    if(sxi_hdist_buildcnt(newmod) != 1) {
	sxi_hdist_free(newmod);
	msg_set_reason("Illegal distribution");
	return EINVAL;
    }

    if(h->have_hd && (!sxi_hdist_same_origin(newmod, h->hd) || sxi_hdist_version(newmod) != sxi_hdist_version(h->hd) + 2)) {
	sxi_hdist_free(newmod);
	msg_set_reason("The new model is not a direct descendent of the current model");
	return EINVAL;
    }

    if(qbegin(h->db)) {
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    newnodes = sxi_hdist_nodelist(newmod, 0);
    if(!newnodes || !(nnodes = sx_nodelist_count(newnodes))) {
	msg_set_reason("Failed to retrieve the list of the updated nodes");
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
    }
    if(h->have_hd) {
	oldnodes = sx_hashfs_all_nodes(h, NL_NEXT);
	if(nnodes != sx_nodelist_count(oldnodes)) {
	    msg_set_reason("Distribution has changed: different count.");
	    ret = EINVAL;
	    goto replace_add_fail;
	}
    } else
	oldnodes = NULL;

    for(i = 0; i<nnodes; i++) {
	const sx_node_t *nn = sx_nodelist_get(newnodes, i);
	const sx_node_t *on;
	unsigned int j;

	if(oldnodes) {
	    on = sx_nodelist_lookup(oldnodes, sx_node_uuid(nn));
	    if(!on || sx_node_capacity(nn) != sx_node_capacity(on)) {
		msg_set_reason("Distribution has changed: different nodes or capacity.");
		ret = EINVAL;
		goto replace_add_fail;
	    }
	}
	for(j=i+1; j<nnodes; j++) {
	    const sx_node_t *other = sx_nodelist_get(newnodes, j);
	    if(!sx_node_cmp(nn, other)) {
		msg_set_reason("Node %s cannot appear more than once", sx_node_uuid_str(nn));
		ret = EINVAL;
		goto replace_add_fail;
	    }
	    if(!sx_node_cmp_addrs(nn, other)) {
		msg_set_reason("Node %s and %s share the same address", sx_node_uuid_str(nn), sx_node_uuid_str(other));
		ret = EINVAL;
		goto replace_add_fail;
	    }
	}
    }

    if(qprep(h->db, &q, "DELETE FROM faultynodes WHERE dist = :distrev") ||
       qbind_int64(q, ":distrev", sxi_hdist_version(newmod)) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to clean up the list of faulty nodes");
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
    }
    qnullify(q);

    faulty = sx_nodelist_new();
    if(!faulty) {
	msg_set_reason("Out of memory creating faulty node list");
	ret = ENOMEM;
	goto replace_add_fail;
    }
    if(qprep(h->db, &q, "INSERT INTO faultynodes (dist, node) VALUES (:distrev, :nodeid)") ||
       qbind_int64(q, ":distrev", sxi_hdist_version(newmod))) {
	msg_set_reason("Failed to save the updated distribution model");
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
    }
    for(i = 0; i<badnnodes; i++) {
	const sx_node_t *bn = sx_nodelist_get(badnodes, i);
	const sx_node_t *nn = sx_nodelist_lookup(newnodes, sx_node_uuid(bn));
	const sx_uuid_t *nnuuid;
	if(!nn) {
	    msg_set_reason("Faulty node %s is not found in the distribution", sx_node_uuid_str(nn));
	    ret = EINVAL;
	    goto replace_add_fail;
	}
	nnuuid = sx_node_uuid(nn);
	if(sx_nodelist_lookup(faulty, nnuuid)) {
	    msg_set_reason("Duplicated faulty node %s is not found in the distribution", sx_node_uuid_str(nn));
	    ret = EINVAL;
	    goto replace_add_fail;
	}
	if(sx_nodelist_add(faulty, sx_node_dup(nn))) {
	    msg_set_reason("Out of memory creating faulty node list");
	    ret = ENOMEM;
	    goto replace_add_fail;
	}
	if(qbind_blob(q, ":nodeid", nnuuid->binary, sizeof(nnuuid->binary)) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to clean up the list of faulty nodes");
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
	}
    }
    qnullify(q);

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)")) {
	msg_set_reason("Failed to save the updated distribution model");
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
    }

    if(h->have_hd) {
	const void *cur_cfg;
	unsigned int cur_cfg_len;

	ret = sxi_hdist_get_cfg(h->hd, &cur_cfg, &cur_cfg_len);
	if(ret) {
	    msg_set_reason("Failed to retrieve the current distribution model");
	    goto replace_add_fail;
	}
	if(qbind_text(q, ":k", "current_dist") ||
	   qbind_blob(q, ":v", cur_cfg, cur_cfg_len) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save current distribution model");
	    ret = FAIL_EINTERNAL;
	    goto replace_add_fail;
	}

	sqlite3_reset(q);
	if(qbind_text(q, ":k", "current_dist_rev") ||
	   qbind_int64(q, ":v", sxi_hdist_version(h->hd)) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to save current distribution model");
	    ret = FAIL_EINTERNAL;
	    goto replace_add_fail;
	}
	sqlite3_reset(q);
    }

    if(qbind_text(q, ":k", "dist") ||
       qbind_blob(q, ":v", cfg, cfg_len) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save target distribution model");
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
    }

    sqlite3_reset(q);
    if(qbind_text(q, ":k", "dist_rev") ||
       qbind_int64(q, ":v", sxi_hdist_version(newmod)) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save target distribution model");
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
    }
    qnullify(q);

    if(sx_hashfs_set_progress_info(h, INPRG_REPLACE_RUNNING, "Updating distribution model with faulty nodes")) {
	ret = FAIL_EINTERNAL;
	goto replace_add_fail;
    }

    if(qcommit(h->db)) {
	msg_set_reason("Failed to save distribution model");
	ret = FAIL_EINTERNAL;
    } else {
	ret = OK;
	DEBUG("Distribution change added from %lld to %lld", (long long)h->hd_rev, (long long)sxi_hdist_version(newmod));
    }
 replace_add_fail:
    qnullify(q);
    if(ret != OK)
	qrollback(h->db);
    sx_nodelist_delete(faulty);
    sxi_hdist_free(newmod);

    return ret;
}

rc_ty sx_hashfs_hdist_change_revoke(sx_hashfs_t *h) {
    sqlite3_stmt *q = NULL;
    rc_ty ret = FAIL_EINTERNAL;

    if(qbegin(h->db))
	return FAIL_EINTERNAL;

    if(h->have_hd) {
	/* Revert to previous distribution (if any exists) */
	int rcount;
	if(qprep(h->db, &q, "SELECT COUNT(*) FROM hashfs WHERE key IN ('current_dist_rev', 'current_dist')") || qstep_ret(q))
	    goto change_revoke_fail;
	rcount = sqlite3_column_int(q, 0);
	qnullify(q);
	if(rcount == 2) {
	    if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('dist_rev', 'dist')") || qstep_noret(q))
		goto change_revoke_fail;
	    qnullify(q);

	    if(qprep(h->db, &q, "UPDATE hashfs SET key = 'dist' WHERE key = 'current_dist'") || qstep_noret(q))
		goto change_revoke_fail;
	    qnullify(q);

	    if(qprep(h->db, &q, "UPDATE hashfs SET key = 'dist_rev' WHERE key = 'current_dist_rev'") || qstep_noret(q))
		goto change_revoke_fail;
	    qnullify(q);
	} else if(rcount != 0) {
	    WARN("Found severe inconsistentencies in the distribution, attempting to recover");
	    /* Should really never land in here */
	    if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key = 'dist_rev' AND EXISTS (SELECT 1 FROM hashfs WHERE key = 'current_dist_rev')") || qstep_noret(q))
		goto change_revoke_fail;
	    qnullify(q);
	    if(qprep(h->db, &q, "UPDATE hashfs SET key = 'dist_rev' WHERE key = 'current_dist_rev'") || qstep_noret(q))
		goto change_revoke_fail;
	    qnullify(q);

	    if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key = 'dist' AND EXISTS (SELECT 1 FROM hashfs WHERE key = 'current_dist')") || qstep_noret(q))
		goto change_revoke_fail;
	    qnullify(q);
	    if(qprep(h->db, &q, "UPDATE hashfs SET key = 'dist' WHERE key = 'current_dist'") || qstep_noret(q))
		goto change_revoke_fail;
	    qnullify(q);
	}
    } else {
	/* Revirgin the node */
	if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('current_dist_rev', 'current_dist', 'dist_rev', 'dist', 'cluster_name')") || qstep_noret(q))
	    goto change_revoke_fail;
	qnullify(q);

	if(qprep(h->db, &q, "INSERT INTO hashfs (key, value) VALUES (:k , :v)"))
	    goto change_revoke_fail;

	if(qbind_text(q, ":k", "current_dist_rev") || qbind_int64(q, ":v", 0) || qstep_noret(q))
	    goto change_revoke_fail;
	sqlite3_reset(q);
	if(qbind_text(q, ":k", "current_dist") || qbind_blob(q, ":v", "", 0) || qstep_noret(q))
	    goto change_revoke_fail;
	qnullify(q);
    }

    if(sx_hashfs_set_progress_info(h, INPRG_IDLE, NULL))
	goto change_revoke_fail;

    ret = sx_hashfs_job_unlock(h, NULL);
    if(ret)
	goto change_revoke_fail;

    if(qcommit(h->db))
	goto change_revoke_fail;

    ret = OK;

 change_revoke_fail:
    qnullify(q);

    if(ret != OK) {
	msg_set_reason("Failed to rewoke the updated distribution model");
	qrollback(h->db);
    }

    return ret;
}

rc_ty sx_hashfs_hdist_set_rebalanced(sx_hashfs_t *h) {
    sxi_hdist_t *rebalanced;
    const void *cfg;
    unsigned int cfg_len;
    sqlite3_stmt *q = NULL;
    rc_ty ret;

    if(!h) {
	NULLARG();
	return EINVAL;
    }

    if(!h->have_hd) {
	msg_set_reason("The cluster is already rebalanced");
	return EINVAL;
    }

    if(!h->is_rebalancing) {
	msg_set_reason("The cluster is already rebalanced");
	return EINVAL;
    }

    ret = sxi_hdist_get_cfg(h->hd, &cfg, &cfg_len);
    if(ret) {
	msg_set_reason("Failed to retrieve the current distribution model");
	return ret;
    }

    rebalanced = sxi_hdist_from_cfg(cfg, cfg_len);
    if(!rebalanced) {
	msg_set_reason("Failed to clone the current distribution model");
	return FAIL_EINTERNAL;
    }

    if(sxi_hdist_rebalanced(rebalanced)) {
	msg_set_reason("Failed to flat the current distribution");
	sxi_hdist_free(rebalanced);
	return FAIL_EINTERNAL;
    }

    ret = sxi_hdist_get_cfg(rebalanced, &cfg, &cfg_len);
    if(ret) {
	msg_set_reason("Failed to retrieve the rebalanced distribution model");
	sxi_hdist_free(rebalanced);
	return ret;
    }

    if(qbegin(h->db)) {
	sxi_hdist_free(rebalanced);
	return FAIL_EINTERNAL;
    }

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)")) {
	msg_set_reason("Failed to save the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    if(qbind_text(q, ":k", "dist") ||
       qbind_blob(q, ":v", cfg, cfg_len) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    sqlite3_reset(q);
    if(qbind_text(q, ":k", "dist_rev") ||
       qbind_int64(q, ":v", sxi_hdist_version(rebalanced)) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to save the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    qnullify(q);
    if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('current_dist', 'current_dist_rev')") ||
       qstep_noret(q)) {
	msg_set_reason("Failed to enable the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    if(qcommit(h->db)) {
	msg_set_reason("Failed to commit the rebalanced distribution model");
	ret = FAIL_EINTERNAL;
	goto rebalanced_fail;
    }

    ret = OK;
    INFO("Distribution rebalanced (version changed from %lld to %lld)", (long long)h->hd_rev, (long long)sxi_hdist_version(rebalanced));

 rebalanced_fail:
    qnullify(q);
    if(ret != OK)
	qrollback(h->db);
    sxi_hdist_free(rebalanced);

    return ret;
}

static rc_ty create_repair_job(sx_hashfs_t *h) {
    sx_nodelist_t *singlenode = NULL;
    sx_uuid_t fakeuuid;
    job_t job_id;
    rc_ty ret;

    DEBUG("IN %s", __func__);
    singlenode = sx_nodelist_new();
    if(!singlenode) {
	WARN("Cannot allocate single node nodelist");
	return ENOMEM;
    }
    uuid_from_string(&fakeuuid, "00000000-0000-0000-0000-000000000000");
    ret = sx_nodelist_add(singlenode, sx_node_new(&fakeuuid, "0.0.0.0", "0.0.0.0", 1));
    if(ret) {
	WARN("Cannot add self to nodelist");
	goto create_repair_err;
    }

    ret = sx_hashfs_job_new_begin(h);
    if(ret)
	goto create_repair_err;

    ret = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, 0, &job_id, JOBTYPE_REPLACE_BLOCKS, JOB_NO_EXPIRY, "BLOCKRP", NULL, 0, singlenode);
    if(ret)
	goto create_repair_err;

    ret = sx_hashfs_job_new_notrigger(h, job_id, 0, &job_id, JOBTYPE_REPLACE_FILES, JOB_NO_EXPIRY, "FILERP", NULL, 0, singlenode);
    if(ret)
	goto create_repair_err;

    ret = sx_hashfs_job_new_end(h);
    if(ret)
	goto create_repair_err;

    ret = OK;

 create_repair_err:
    sx_nodelist_delete(singlenode);
    DEBUG("OUT %s with %d", __func__, ret);
    return ret;
}

rc_ty sx_hashfs_hdist_change_commit(sx_hashfs_t *h) {
    sqlite3_stmt *q = NULL;
    rc_ty s = OK;

    DEBUG("IN %s", __func__);
    if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('current_dist', 'current_dist_rev')") ||
       qstep_noret(q)) {
	msg_set_reason("Failed to enable new distribution model");
	s = FAIL_EINTERNAL;
    } else if((s = sx_hashfs_job_unlock(h, NULL)) != OK)
	WARN("Failed to unlock jobs after enabling new model");
    else if((s = create_repair_job(h)) != OK)
	WARN("Failed to create repair job");
    else
	DEBUG("Distribution change committed");

    qnullify(q);
    return s;
}

rc_ty sx_hashfs_hdist_rebalance(sx_hashfs_t *h) {
    job_t job_id;
    sx_nodelist_t *singlenode = NULL;
    const sx_node_t *self = sx_hashfs_self(h);
    rc_ty ret;

    DEBUG("IN %s", __func__);

    sqlite3_reset(h->qx_wipehold);
    if(qstep_noret(h->qx_wipehold)) {
	WARN("Failed to wipe hold list");
	return FAIL_EINTERNAL;
    }

    singlenode = sx_nodelist_new();
    if(!singlenode) {
	WARN("Cannot allocate single node nodelist");
	return ENOMEM;
    }

    ret = sx_nodelist_add(singlenode, sx_node_dup(self));
    if(ret) {
	WARN("Cannot add self to nodelist");
	goto hdistreb_fail;
    }

    ret = sx_hashfs_job_new_begin(h);
    if(ret)
	goto hdistreb_fail;

    ret = sx_hashfs_job_new_notrigger(h, JOB_NOPARENT, 0, &job_id, JOBTYPE_REBALANCE_BLOCKS, JOB_NO_EXPIRY, "BLOCKRB", NULL, 0, singlenode);
    if(ret)
	goto hdistreb_fail;

    ret = sx_hashfs_job_new_notrigger(h, job_id, 0, &job_id, JOBTYPE_REBALANCE_FILES, JOB_NO_EXPIRY, "FILERB", NULL, 0, singlenode);
    if(ret)
	goto hdistreb_fail;

    ret = sx_hashfs_job_new_end(h);
    if(ret)
	goto hdistreb_fail;

    ret = OK;

 hdistreb_fail:
    sx_nodelist_delete(singlenode);
    return ret;
}

rc_ty sx_hashfs_challenge_gen(sx_hashfs_t *h, sx_hash_challenge_t *c, int random_challenge) {
    unsigned char md[SXI_SHA1_BIN_LEN];
    unsigned int mdlen;
    sxi_hmac_sha1_ctx *hmac_ctx;
    rc_ty ret;

    if(random_challenge) {
	if(sxi_rand_pseudo_bytes(c->challenge, sizeof(c->challenge)) == -1) {
	    WARN("Cannot generate random bytes");
	    msg_set_reason("Failed to generate random nounce");
	    return FAIL_EINTERNAL;
	}
    }

    hmac_ctx = sxi_hmac_sha1_init();
    if (!hmac_ctx)
        return 1;
    if(!sxi_hmac_sha1_init_ex(hmac_ctx, &h->tokenkey, sizeof(h->tokenkey)) ||
       !sxi_hmac_sha1_update(hmac_ctx, c->challenge, sizeof(c->challenge)) ||
       !sxi_hmac_sha1_update(hmac_ctx, h->cluster_uuid.binary, sizeof(h->cluster_uuid.binary)) ||
       !sxi_hmac_sha1_final(hmac_ctx, md, &mdlen) ||
       mdlen != sizeof(c->response)) {
	msg_set_reason("Failed to compute nounce hmac");
	CRIT("Cannot genearate nounce hmac");
	ret = FAIL_EINTERNAL;
    } else {
	memcpy(c->response, md, sizeof(c->response));
	ret = OK;
    }
    sxi_hmac_sha1_cleanup(&hmac_ctx);

    return ret;
}

/* MODHDIST: this has got a lot in common with sx_storage_activate
 * except it's the entry for the cluster instead od sxadm */
rc_ty sx_hashfs_setnodedata(sx_hashfs_t *h, const char *name, const sx_uuid_t *node_uuid, uint16_t port, int use_ssl, const char *ssl_ca_crt) {
    rc_ty ret = FAIL_EINTERNAL;
    char *ssl_ca_file = NULL;
    sqlite3_stmt *q = NULL;
    int rollback = 0;

    if(!h || !name || !node_uuid || !*name) {
	NULLARG();
	return EFAULT;
    }
    if(!sx_storage_is_bare(h)) {
	msg_set_reason("Storage was already activated");
	return EINVAL;
    }

    if(use_ssl && ssl_ca_crt) {
	unsigned int cafilen = strlen(h->dir) + sizeof("/sxcert.012345678.pem");
	unsigned int cadatalen = strlen(ssl_ca_crt);
	if(cadatalen) {
	    ssl_ca_file = wrap_malloc(cafilen);
	    if(!ssl_ca_file) {
		OOM();
		return ENOMEM;
	    }
	    snprintf(ssl_ca_file, cafilen, "%s/sxcert.pem", h->dir);
	    while(1) {
		int fd = open(ssl_ca_file, O_CREAT|O_EXCL|O_WRONLY, 0640);
		if(fd < 0) {
		    if(errno == EEXIST) {
			snprintf(ssl_ca_file, cafilen, "%s/sxcert.%08x.pem", h->dir, sxi_rand());
			continue;
		    }
		    msg_set_reason("Cannot create CA certificate file");
		    goto setnodedata_fail;
		}
		while(cadatalen) {
		    ssize_t done = write(fd, ssl_ca_crt, cadatalen);
		    if(done < 0) {
			close(fd);
			msg_set_reason("Cannot write CA certificate file");
			goto setnodedata_fail;
		    }
		    cadatalen -= done;
		    ssl_ca_crt += done;
		}
		if(close(fd)) {
		    msg_set_reason("Cannot close CA certificate file");
		    goto setnodedata_fail;
		}
		break;
	    }
	}
    }

    if(qbegin(h->db))
	goto setnodedata_fail;
    rollback = 1;

    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)"))
	goto setnodedata_fail;

    if(qbind_text(q, ":k", "ssl_ca_file") || qbind_text(q, ":v", ssl_ca_file ? ssl_ca_file : "") || qstep_noret(q))
	goto setnodedata_fail;
    if(qbind_text(q, ":k", "cluster_name") || qbind_text(q, ":v", name) || qstep_noret(q))
	goto setnodedata_fail;
    if(qbind_text(q, ":k", "http_port") || qbind_int(q, ":v", port) || qstep_noret(q))
	goto setnodedata_fail;
    if(qbind_text(q, ":k", "node") || qbind_blob(q, ":v", node_uuid->binary, sizeof(node_uuid->binary)) || qstep_noret(q))
	goto setnodedata_fail;
    qnullify(q);

    if(qprep(h->db, &q, "DELETE FROM users WHERE uid <> 0") || qstep_noret(q))
	goto setnodedata_fail;

    if(qcommit(h->db))
	goto setnodedata_fail;

    ret = OK;
    rollback = 0;
 setnodedata_fail:
    if(rollback)
	qrollback(h->db);

    sqlite3_finalize(q);
    free(ssl_ca_file);
    return ret;
}


static int64_t dbfilesize(sxi_db_t *db) {
    const char *dbfile;
    struct stat st;
    if(!db || !db->handle)
	return 0;

    dbfile = sqlite3_db_filename(db->handle, "main");
    if(!dbfile)
	return 0;

    if(stat(dbfile, &st))
	return 0;

    return st.st_size;
}

void sx_storage_usage(sx_hashfs_t *h, int64_t *allocated, int64_t *committed) {
    unsigned int i, j;
    int64_t al, ci;

    /* The allocated size is the amount of space taken on disk.
     * - for the DB files this it the size of the .db files
     * - for the DATA files this is the size of the .bin files
     *
     * The committed size is the amount of space actually used.
     * - for the DB files it's the same as for allocated (*)
     * - for the DATA files this is the number of blocks * the block size
     * (*) we could use PRAGMA (page_size * page_count) here but, considering that
     * the outcome could be puzzling to the casual user and that, in the end, the
     * difference would be pretty tiny compared to the overall space usage, it's
     * better to just account for the file size here too
     */

    al = dbfilesize(h->db);
    al += dbfilesize(h->tempdb);
    al += dbfilesize(h->tempdb);
    al += dbfilesize(h->eventdb);
    al += dbfilesize(h->xferdb);

    for(i=0; i<METADBS; i++)
	al += dbfilesize(h->metadb[i]);

    ci = al;

    for(j=0; j<SIZES; j++) {
	for(i=0; i<HASHDBS; i++) {
	    int64_t rows = get_count(h->datadb[j][i], "blocks");
	    int64_t dbsize = dbfilesize(h->datadb[j][i]);
	    struct stat st;

	    al += dbsize;
	    ci += dbsize + rows * bsz[j];
	    if(!fstat(h->datafd[j][i], &st))
		al += st.st_size;
	}
    }
    if(allocated)
	*allocated = al;
    if(committed)
	*committed = ci;
}

const sx_uuid_t *sx_hashfs_distinfo(sx_hashfs_t *h, unsigned int *version, uint64_t *checksum) {
    const sx_uuid_t *ret;
    if(!h || !h->have_hd)
	return NULL;

    ret = sxi_hdist_uuid(h->hd);
    if(!ret)
	return NULL;

    if(checksum)
	*checksum = sxi_hdist_checksum(h->hd);
    if(version)
	*version = sxi_hdist_version(h->hd);

    return ret;
}

/* starts iterating from the beginning */
static rc_ty sx_hashfs_blocks_restart(sx_hashfs_t *h, unsigned rebalance_version)
{
    DEBUG("iteration reset with rebalance_version: %d", rebalance_version);
    unsigned i,j;
    for(j=0;j<SIZES;j++) {
        for(i=0;i<HASHDBS;i++) {
            sqlite3_reset(h->rit.q[j][i]);
            sqlite3_reset(h->qb_get_meta[j][i]);
            sqlite3_reset(h->qb_deleteold[j][i]);
            sqlite3_clear_bindings(h->rit.q[j][i]);
            sqlite3_clear_bindings(h->qb_get_meta[j][i]);
            sqlite3_clear_bindings(h->qb_deleteold[j][i]);
        }
    }
    for(j=0;j<SIZES;j++) {
        for(i=0;i<HASHDBS;i++) {
            if (qbind_blob(h->rit.q[j][i], ":prevhash", "", 0))
                return FAIL_EINTERNAL;
            if (qbind_int(h->qb_get_meta[j][i], ":current_age", rebalance_version))
                return FAIL_EINTERNAL;
            if (qbind_int(h->qb_deleteold[j][i], ":current_age", rebalance_version))
                return FAIL_EINTERNAL;
        }
    }
    h->rit.sizeidx = h->rit.ndbidx = 0;
    h->rit.retry_mode = 0;
    return OK;
}

void sx_hashfs_blockmeta_free(block_meta_t **blockmetaptr)
{
    if (!blockmetaptr)
        return;
    block_meta_t* blockmeta = *blockmetaptr;
    free(blockmeta->entries);
    blockmeta->entries = NULL;
    blockmeta->count = 0;
    free(blockmeta);
    *blockmetaptr = NULL;
}

static rc_ty fill_block_meta(sx_hashfs_t *h, sqlite3_stmt *qmeta, block_meta_t *blockmeta)
{
    int ret;
    if (qbind_blob(qmeta, ":hash", blockmeta->hash.b, sizeof(blockmeta->hash.b)))
        return FAIL_EINTERNAL;
    blockmeta->count = 0;
    sqlite3_reset(qmeta);
    while ((ret = qstep(qmeta)) == SQLITE_ROW) {
        int op = sqlite3_column_int(qmeta, 1);
        if (!op)
            continue;
        blockmeta->entries = wrap_realloc_or_free(blockmeta->entries, ++blockmeta->count * sizeof(*blockmeta->entries));
        if (!blockmeta->entries)
            return ENOMEM;
        block_meta_entry_t *e = &blockmeta->entries[blockmeta->count - 1];
        int len = sqlite3_column_bytes(qmeta, 2);
        if (len != sizeof(e->revision_id.b)) {
            WARN("bad revision size: %d", len);
            return FAIL_EINTERNAL;
        }
        memcpy(&e->revision_id.b, sqlite3_column_blob(qmeta, 2), len);
        e->replica = sqlite3_column_int(qmeta, 0);
        DEBUG("set replica to %d", e->replica);
        e->op = op;
    }
    sqlite3_reset(qmeta);
    if (ret != SQLITE_DONE)
        return FAIL_EINTERNAL;
    return OK;
}

static rc_ty sx_hashfs_blockmeta_get(sx_hashfs_t *h, rc_ty ret, sqlite3_stmt *q, sqlite3_stmt *qmeta, unsigned int blocksize, block_meta_t *blockmeta)
{
    if (ret == SQLITE_DONE) {
        /* reset iterator for current db, so next time it starts
         * from the beginning */
        sqlite3_reset(q);
        if (qbind_blob(q, ":prevhash", "", 0)) {
            sqlite3_reset(q);
            sqlite3_reset(qmeta);
            return FAIL_EINTERNAL;
        }
    } else if (ret == SQLITE_ROW) {
        if (sqlite3_column_bytes(q, 0) != sizeof(blockmeta->hash)) {
            WARN("bad blob size for query: %s", sqlite3_sql(q));
            sqlite3_reset(q);
            sqlite3_reset(qmeta);
            return FAIL_EINTERNAL;
        }
        memcpy(blockmeta, sqlite3_column_blob(q, 0), sizeof(blockmeta->hash));
        blockmeta->blocksize = blocksize;
        sqlite3_reset(q);
        if (qbind_blob(q, ":prevhash", blockmeta->hash.b, sizeof(blockmeta->hash.b))) {
            sqlite3_reset(q);
            sqlite3_reset(qmeta);
            return FAIL_EINTERNAL;
        }
        if (fill_block_meta(h, qmeta, blockmeta)) {
            sqlite3_reset(q);
            sqlite3_reset(qmeta);
            WARN("failed to fill block meta");
            return FAIL_EINTERNAL;
        }
        if (blockmeta->count) {
            DEBUG("hs:%d,ndb:%d,blockmeta count=%u", h->rit.sizeidx, h->rit.ndbidx, blockmeta->count);
            DEBUGHASH("_next: ", &blockmeta->hash);
            return OK;
        }
        DEBUGHASH("blockmeta.count=0", &blockmeta->hash);
        /* blockmeta.count = 0 means this hash has no OLD counters
         * so skip it
         * */
        if (sx_hashfs_br_delete(h, blockmeta))
            return FAIL_EINTERNAL;
    } else {
        WARN("iteration failed");
        sqlite3_reset(q);
        sqlite3_reset(qmeta);
        return FAIL_EINTERNAL;
    }
    return ret;
}

static rc_ty sx_hashfs_blocks_retry_next(sx_hashfs_t *h, block_meta_t *blockmeta)
{
    sqlite3_stmt *q = h->rit.q_sel;
    sqlite3_reset(q);
    rc_ty ret;
    do {
        ret = qstep(q);
        int hs;

        if (ret == SQLITE_DONE) {
            sx_hashfs_blockmeta_get(h, ret, q, NULL, 0, NULL);
            return ITER_NO_MORE;
        }
        if (ret == SQLITE_ROW) {
            int bs = sqlite3_column_int(q, 1);
            for(hs = 0; hs < SIZES; hs++)
                if(bsz[hs] == bs)
                    break;
            if(hs == SIZES) {
                WARN("bad blocksize: %d", bs);
                return FAIL_BADBLOCKSIZE;
            }
            const sx_hash_t* hash = sqlite3_column_blob(q, 0);
            if (!hash)
                return EFAULT;
            DEBUGHASH("retry_next", hash);
            unsigned int ndb = gethashdb(hash);
            ret = sx_hashfs_blockmeta_get(h, ret, q, h->qb_get_meta[hs][ndb], bs, blockmeta);
            if (ret != SQLITE_ROW)
                return ret;
        }
    } while (ret == SQLITE_ROW);
    sqlite3_reset(q);
    return ret;
}

/* iterate over all hashes once */
static rc_ty sx_hashfs_blocks_next(sx_hashfs_t *h, block_meta_t *blockmeta)
{
    int ret;
    memset(blockmeta, 0, sizeof(*blockmeta));
    for (;h->rit.sizeidx < SIZES; h->rit.sizeidx++) {
        for (;h->rit.ndbidx < HASHDBS; h->rit.ndbidx++) {
            sqlite3_stmt *q = h->rit.q[h->rit.sizeidx][h->rit.ndbidx];
            sqlite3_stmt *qmeta = h->qb_get_meta[h->rit.sizeidx][h->rit.ndbidx];
            sqlite3_reset(q);
            sqlite3_reset(qmeta);
            do {
                ret = qstep(q);
                DEBUG("iterating on size=%d,ndb=%d => %s", h->rit.sizeidx, h->rit.ndbidx,
                      ret == SQLITE_DONE ? "DONE" :
                      ret == SQLITE_ROW ? "ROW" :
                      "ERROR");
                ret = sx_hashfs_blockmeta_get(h, ret, q, qmeta, bsz[h->rit.sizeidx], blockmeta);
                sqlite3_reset(q);
                if (ret != SQLITE_ROW && ret != SQLITE_DONE)
                    return ret;
            } while (ret == SQLITE_ROW);
        }
        h->rit.ndbidx = 0;
    }
    DEBUG("iteration cycle finished");
    return ITER_NO_MORE; /* end of all hashes */
}

rc_ty sx_hashfs_br_init(sx_hashfs_t *h)
{
    rc_ty ret = OK;
    unsigned rebalance_version = sxi_hdist_version(h->hd);
    DEBUG("block rebalance initialized");
    h->rit.rebalance_ver = rebalance_version;
    h->rit.retry_mode = 0;
    sqlite3_reset(h->rit.q_reset);
    if (qstep_noret(h->rit.q_reset))
        ret = FAIL_EINTERNAL;
    sqlite3_reset(h->rit.q_reset);
    return ret;
}

rc_ty sx_hashfs_br_begin(sx_hashfs_t *h)
{
    unsigned rebalance_version = sxi_hdist_version(h->hd);
    if (rebalance_version != h->rit.rebalance_ver) {
        rc_ty rc;
        sx_hashfs_br_init(h);
        rc = sx_hashfs_blocks_restart(h, h->rit.rebalance_ver);
        if (rc)
            return rc;
    }
    /* iteration finished, check whether we processed all hashes */
    if (h->rit.retry_mode) {
        int64_t n;
        sqlite3_reset(h->rit.q_count);
        if (qstep_ret(h->rit.q_count))
            return FAIL_EINTERNAL;
        n = sqlite3_column_int64(h->rit.q_count, 0);
        sqlite3_reset(h->rit.q_count);
        DEBUG("retry: %lld hashes", (long long)n);
        if (!n) {
            DEBUG("outer iteration done");
            return ITER_NO_MORE;
        }
    }
    return OK;
}

rc_ty sx_hashfs_br_next(sx_hashfs_t *h, block_meta_t **blockmetaptr)
{
    rc_ty ret;
    if (!blockmetaptr) {
        NULLARG();
        return EFAULT;
    }
    block_meta_t *blockmeta = *blockmetaptr = wrap_calloc(1, sizeof(*blockmeta));
    if (!blockmeta)
        return ENOMEM;
    if (h->rit.retry_mode)
        ret = sx_hashfs_blocks_retry_next(h, blockmeta);
    else
        ret = sx_hashfs_blocks_next(h, blockmeta);
    if (ret == OK) {
        DEBUGHASH("br_next", &blockmeta->hash);
        return OK;
    }
    sx_hashfs_blockmeta_free(blockmetaptr);
    if (ret == ITER_NO_MORE) {
        h->rit.retry_mode = 1;
        DEBUG("retry_mode enabled");
    }
    return ret;
}

rc_ty sx_hashfs_br_use(sx_hashfs_t *h, const block_meta_t *blockmeta)
{
    rc_ty ret = OK;
    if (!h || !blockmeta) {
        NULLARG();
        return EFAULT;
    }
    sqlite3_reset(h->rit.q_add);
    if(qbind_blob(h->rit.q_add, ":hash", &blockmeta->hash, sizeof(blockmeta->hash)) ||
       qbind_int(h->rit.q_add, ":blocksize", blockmeta->blocksize) ||
       qstep_noret(h->rit.q_add))
        ret = FAIL_EINTERNAL;
    sqlite3_reset(h->rit.q_add);
    DEBUGHASH("br_use", &blockmeta->hash);
    return ret;
}

rc_ty sx_hashfs_br_done(sx_hashfs_t *h, const block_meta_t *blockmeta)
{
    rc_ty ret = OK;
    if (!h || !blockmeta) {
        NULLARG();
        return EFAULT;
    }
    sqlite3_reset(h->rit.q_remove);
    if(qbind_blob(h->rit.q_remove, ":hash", &blockmeta->hash, sizeof(blockmeta->hash)) ||
       qstep_noret(h->rit.q_remove))
        ret = FAIL_EINTERNAL;
    sqlite3_reset(h->rit.q_add);
    DEBUGHASH("br_done", &blockmeta->hash);
    return ret;
}

rc_ty sx_hashfs_br_delete(sx_hashfs_t *h, const block_meta_t *blockmeta)
{
    unsigned int ndb, hs;
    rc_ty ret;

    if (!h || !blockmeta) {
        NULLARG();
        return EFAULT;
    }
    if ((ret = sx_hashfs_br_done(h, blockmeta)))
        return ret;
    ndb = gethashdb(&blockmeta->hash);
    for(hs = 0; hs < SIZES; hs++)
	if(bsz[hs] == blockmeta->blocksize)
	    break;
    if(hs == SIZES) {
	WARN("bad blocksize: %d", blockmeta->blocksize);
	return FAIL_BADBLOCKSIZE;
    }
    sqlite3_stmt *q = h->qb_deleteold[hs][ndb];
    sqlite3_reset(q);
    if (qbind_blob(q, ":hash", blockmeta->hash.b, sizeof(blockmeta->hash.b)))
        return FAIL_EINTERNAL; 
    /* :current_age bound in blocks_restart */
    ret = qstep_noret(q);
    sqlite3_reset(q);
    if (ret == OK) {
        DEBUGHASH("_delete on", &blockmeta->hash);
    }
    return ret;
}

rc_ty sx_hashfs_blkrb_hold(sx_hashfs_t *h, const sx_hash_t *block, unsigned int blocksize, const sx_node_t *node) {
    const sx_uuid_t *uuid;

    if(!h || !block || !node) {
        NULLARG();
        return EFAULT;
    }

    if(sx_hashfs_check_blocksize(blocksize) != OK) {
        WARN("bad blocksize: %d", blocksize);
	return FAIL_BADBLOCKSIZE;
    }

    uuid = sx_node_uuid(node);
    if(!uuid) {
	msg_set_reason("Invalid node target for block to put on hold");
        WARN("invalid target node");
	return EINVAL;
    }

    sqlite3_reset(h->qx_hold);
    if(qbind_blob(h->qx_hold, ":b", block, sizeof(*block)) ||
       qbind_int(h->qx_hold, ":s", blocksize) ||
       qbind_blob(h->qx_hold, ":n", uuid->binary, sizeof(uuid->binary)) ||
       qstep_noret(h->qx_hold))
	return FAIL_EINTERNAL;

    return OK;
}

rc_ty sx_hashfs_blkrb_can_gc(sx_hashfs_t *h, const sx_hash_t *block, unsigned int blocksize) {
    rc_ty ret;
    int r;

    if(!h || !block) {
        NULLARG();
        return EFAULT;
    }

    if(sx_hashfs_check_blocksize(blocksize) != OK)
	return FAIL_BADBLOCKSIZE;

    sqlite3_reset(h->qx_isheld);
    if(qbind_blob(h->qx_isheld, ":b", block, sizeof(*block)) ||
       qbind_int(h->qx_isheld, ":s", blocksize))
	return FAIL_EINTERNAL;

    r = qstep(h->qx_isheld);
    if(r == SQLITE_ROW) {
	sqlite3_reset(h->qx_isheld);
	ret = FAIL_LOCKED;
    } else if(r == SQLITE_DONE)
	ret = OK;
    else
	ret = FAIL_EINTERNAL;

    return ret;
}

rc_ty sx_hashfs_blkrb_release(sx_hashfs_t *h, uint64_t pushq_id) {
    if(!h) {
        NULLARG();
        return EFAULT;
    }

    sqlite3_reset(h->qx_isheld);
    if(qbind_int64(h->qx_release, ":pushid", pushq_id) ||
       qstep_noret(h->qx_release))
	return FAIL_EINTERNAL;

    return OK;
}

rc_ty sx_hashfs_blkrb_is_complete(sx_hashfs_t *h) {
    int s;

    sqlite3_reset(h->qx_hasheld);
    s = qstep(h->qx_hasheld);
    sqlite3_reset(h->qx_hasheld);

    if(s == SQLITE_ROW)
	return EEXIST;
    else if(s == SQLITE_DONE)
	return OK;
    else
	return FAIL_EINTERNAL;
}

rc_ty sx_hashfs_relocs_populate(sx_hashfs_t *h) {
    const sx_hashfs_volume_t *vol;
    sx_uuid_t selfuuid;
    unsigned int i;
    rc_ty r;

    for(i=0; i<METADBS; i++) {
	sqlite3_reset(h->qm_wiperelocs[i]);
	if(qstep_noret(h->qm_wiperelocs[i])) {
	    WARN("Failed to wipe relocation queue on db %u", i);
	    return FAIL_EINTERNAL;
	}
    }

    if(sx_hashfs_self_uuid(h, &selfuuid))
	return FAIL_EINTERNAL;
	
    r = sx_hashfs_volume_first(h, &vol, 0);
    while(r == OK) {
	sx_nodelist_t *prevnodes, *nextnodes;
	const sx_node_t *prev, *next;
	sx_hash_t hash;

	if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), vol->name, strlen(vol->name), &hash)) {
	    WARN("hashing volume name failed");
	    return FAIL_EINTERNAL;
	}

	prevnodes = sx_hashfs_all_hashnodes(h, NL_PREV, &hash, vol->max_replica);
	if(!prevnodes) {
	    WARN("cannot determine the previous owner of block: %s", msg_get_reason());
	    return FAIL_EINTERNAL;
	}

	nextnodes = sx_hashfs_all_hashnodes(h, NL_NEXT, &hash, vol->max_replica);
	if(!nextnodes) {
	    WARN("cannot determine the next owner of block");
	    sx_nodelist_delete(prevnodes);
	    return FAIL_EINTERNAL;
	}

	prev = sx_nodelist_lookup_index(prevnodes, &selfuuid, &i);
	if(prev) {
	    /* I was the i-th owner of this volume */
	    next = sx_nodelist_get(nextnodes, i);
	    if(next && !sx_nodelist_lookup(prevnodes, sx_node_uuid(next))) {
		const sx_uuid_t *uuid = sx_node_uuid(next);

		INFO("Setting files in volume %s to be relocated from here to %s(%s) for replica %u", vol->name, sx_node_uuid_str(next), sx_node_addr(next), i);
		/* The upcoming i-th owner of this volume wans't already an owner:
		 * all volume files are setup for relocation */
		for(i=0; i<METADBS; i++) {
		    sqlite3_reset(h->qm_addrelocs[i]);
		    if(qbind_blob(h->qm_addrelocs[i], ":node", uuid->binary, sizeof(uuid->binary)) ||
		       qbind_int64(h->qm_addrelocs[i], ":volid", vol->id) ||
		       qstep_noret(h->qm_addrelocs[i])) {
			WARN("Failed to add relocation queue on db %u for volume %llu", i, (long long)vol->id);
			sx_nodelist_delete(prevnodes);
			sx_nodelist_delete(nextnodes);
			return FAIL_EINTERNAL;
		    }
		}
	    }
	}

	sx_nodelist_delete(prevnodes);
	sx_nodelist_delete(nextnodes);
	r = sx_hashfs_volume_next(h);
    }

    return r == ITER_NO_MORE ? OK : r;
}


void sx_hashfs_relocs_begin(sx_hashfs_t *h) {
    h->relocdb_start = h->relocdb_cur = sxi_rand() % METADBS;
    h->relocid = 0;
}

static rc_ty relocs_delete(sx_hashfs_t *h, unsigned int relocdb, int64_t relocid) {
    sqlite3_reset(h->qm_delreloc[relocdb]);
    if(qbind_int64(h->qm_delreloc[relocdb], ":fileid", relocid) ||
       qstep_noret(h->qm_delreloc[relocdb]))
	return FAIL_EINTERNAL;
    return OK;
}


void sx_hashfs_reloc_free(const sx_reloc_t *reloc) {
    sx_reloc_t *dr = (sx_reloc_t *)reloc;
    if(!dr)
	return;
    free(dr->blocks);
    sxc_meta_free(dr->metadata);
    free(dr);
}


rc_ty sx_hashfs_relocs_next(sx_hashfs_t *h, const sx_reloc_t **reloc) {
    if(!h || !reloc) {
	NULLARG();
	return EFAULT;
    }

    *reloc = NULL;
    while(1) {
	unsigned int ndb = h->relocdb_cur;
	sqlite3_stmt *q = h->qm_getreloc[ndb];
	const sx_hashfs_volume_t *volume;
    	const char *name, *rev;
	const void *content;
	unsigned int content_len, i;
	sx_uuid_t targetid;
	sx_reloc_t *rlc;
	int64_t volid;
	rc_ty ret;
	int r;

	sqlite3_reset(q);
	if(qbind_int64(q, ":prev", h->relocid))
	    return FAIL_EINTERNAL;

	r = qstep(q);
	if(r == SQLITE_DONE) {
	    h->relocid = 0;
	    h->relocdb_cur = (ndb + 1) % METADBS;
	    if(h->relocdb_cur == h->relocdb_start)
		return ITER_NO_MORE;
	    continue;
	}

	if(r != SQLITE_ROW)
	    return FAIL_EINTERNAL;

	h->relocid = sqlite3_column_int64(q, 0);
	if(sqlite3_column_type(q, 2) == SQLITE_NULL) {
	    /* This file no longer exists */
	    sqlite3_reset(q);
	    ret = relocs_delete(h, ndb, h->relocid);
	    if(ret != OK)
		return ret;
	    continue;
	}

	volid = sqlite3_column_int64(q, 2);
	name = (const char *)sqlite3_column_text(q, 3);
	rev = (const char *)sqlite3_column_text(q, 5);
	content_len = sqlite3_column_bytes(q, 6);
	content = sqlite3_column_blob(q, 6);
	if(!name ||
	   sqlite3_column_bytes(q, 1) != sizeof(targetid.binary) ||
	   !rev ||
	   (!content && content_len) ||
	   content_len % sizeof(sx_hash_t)) {
	    WARN("Bad file %lld in %u", (long long)h->relocid, ndb);
	    sqlite3_reset(q);
	    return FAIL_EINTERNAL;
	}

	rlc = wrap_calloc(1, sizeof(*rlc));
	if(!rlc) {
	    sqlite3_reset(q);
	    return ENOMEM;
	}
	if(content_len) {
	    rlc->blocks = wrap_malloc(content_len);
	    if(!rlc->blocks) {
		sqlite3_reset(q);
		sx_hashfs_reloc_free(rlc);
		return ENOMEM;
	    }
	}
	rlc->metadata = sxc_meta_new(h->sx);
	if(!rlc->metadata) {
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return ENOMEM;
	}

	if(parse_revision(rev, &rlc->file.created_at)) {
	    WARN("Bad revision on file %lld in %u", (long long)h->relocid, ndb);
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return FAIL_EINTERNAL;
	}

	rlc->file.file_size = sqlite3_column_int64(q, 4);
	uuid_from_binary(&targetid, sqlite3_column_blob(q, 1));
	rlc->target = sx_nodelist_lookup(sx_hashfs_all_nodes(h, NL_NEXT), &targetid);
	if(!rlc->target) {
	    WARN("File id %lld in %u has invalid target %s", (long long)h->relocid, ndb, targetid.string);
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return FAIL_EINTERNAL;
	}

	sxi_strlcpy(rlc->file.name, name, sizeof(rlc->file.name));
	sxi_strlcpy(rlc->file.revision, rev, sizeof(rlc->file.revision));
	rlc->file.nblocks = size_to_blocks(rlc->file.file_size, NULL, &rlc->file.block_size);
	if(content_len)
	    memcpy(rlc->blocks, content, content_len);

	ret = sx_hashfs_volume_by_id(h, volid, &volume);
	if(ret) {
	    sqlite3_reset(q);
	    sx_hashfs_reloc_free(rlc);
	    return ret;
	}
	memcpy(&rlc->volume, volume, sizeof(*volume));

	ret = fill_filemeta(h, ndb, h->relocid);
	sqlite3_reset(q);
	if(ret != OK) {
	    WARN("Failed to load metadata for file %lld in %u", (long long)h->relocid, ndb);
	    sx_hashfs_reloc_free(rlc);
	    return ret;
	}

	for(i=0; i<h->nmeta; i++) {
	    if(sxc_meta_setval(rlc->metadata, h->meta[i].key, h->meta[i].value, h->meta[i].value_len)) {
		sx_hashfs_reloc_free(rlc);
		return ENOMEM;
	    }
	}

	rlc->reloc_id = h->relocid;
	rlc->reloc_db = ndb;

	*reloc = rlc;
	return OK;
    }
}

rc_ty sx_hashfs_relocs_delete(sx_hashfs_t *h, const sx_reloc_t *reloc) {
    if(!h || !reloc) {
	NULLARG();
	return EFAULT;
    }

    return relocs_delete(h, reloc->reloc_db, reloc->reloc_id);
}

rc_ty sx_hashfs_reset_volume_cursize(sx_hashfs_t *h, int64_t volume_id, int64_t size) {
    rc_ty ret = FAIL_EINTERNAL;

    sqlite3_reset(h->q_setvolcursize);
    if(qbind_int64(h->q_setvolcursize, ":size", size) ||
       qbind_int64(h->q_setvolcursize, ":volume", volume_id) ||
       qbind_int64(h->q_setvolcursize, ":now", time(NULL)) ||
       qstep_noret(h->q_setvolcursize)) {
        WARN("Failed to reset volume size for volume %lld", (long long)volume_id);
        msg_set_reason("Failed to reset volume size");
        goto sx_hashfs_reset_volume_cursize_err;
    }

    ret = OK;
    sx_hashfs_reset_volume_cursize_err:
    sqlite3_reset(h->q_setvolcursize);
    return ret;
}

rc_ty sx_hashfs_update_volume_cursize(sx_hashfs_t *h, int64_t volume_id, int64_t size) {
    rc_ty ret = FAIL_EINTERNAL;

    if(!volume_id) {
        CRIT("Invalid volume_id argument");
        return EINVAL;
    }

    sqlite3_reset(h->q_updatevolcursize);
    if(qbind_int64(h->q_updatevolcursize, ":size", size) ||
       qbind_int64(h->q_updatevolcursize, ":volume", volume_id) ||
       qbind_int64(h->q_updatevolcursize, ":now", time(NULL)) ||
       qstep_noret(h->q_updatevolcursize)) {
        WARN("Failed to update volume size for volume %lld", (long long)volume_id);
        msg_set_reason("Failed to update volume size");
        goto sx_hashfs_update_volume_cursize_err;
    }

    ret = OK;
    sx_hashfs_update_volume_cursize_err:
    sqlite3_reset(h->q_updatevolcursize);
    return ret;
}

rc_ty sx_hashfs_rb_cleanup(sx_hashfs_t *h) {
    const sx_hashfs_volume_t *vol;
    sx_uuid_t selfuuid;
    unsigned int i;
    rc_ty r;
    const sx_nodelist_t *nodes;
    unsigned int nnodes;

    sqlite3_reset(h->qx_wipehold);
    if(qstep_noret(h->qx_wipehold)) {
	WARN("Failed to wipe hold list");
	return FAIL_EINTERNAL;
    }

    for(i=0; i<METADBS; i++) {
	sqlite3_reset(h->qm_wiperelocs[i]);
	if(qstep_noret(h->qm_wiperelocs[i])) {
	    WARN("Failed to wipe relocation queue on db %u", i);
	    return FAIL_EINTERNAL;
	}
    }

    /* Get list of all nodes */
    nodes = sx_hashfs_all_nodes(h, NL_PREVNEXT);
    if(!nodes) {
        WARN("Failed to get NL_PREVNEXT nodes list");
        return FAIL_EINTERNAL;
    }
    nnodes = sx_nodelist_count(nodes);

    /* Iterate over nodes and reset their last push time */
    for(i = 0; i < nnodes; i++) {
        const sx_node_t *n = sx_nodelist_get(nodes, i);
        if(!n) {
            WARN("Failed to get node from nodes list");
            return FAIL_EINTERNAL;
        }

        sqlite3_reset(h->q_setnodepushtime);
        if(qbind_blob(h->q_setnodepushtime, ":node", sx_node_uuid(n)->binary, UUID_BINARY_SIZE)
           || qbind_int64(h->q_setnodepushtime, ":now", 0) || qstep_noret(h->q_setnodepushtime)) {
            WARN("Failed to reset node %s push time", sx_node_addr(n));
            return FAIL_EINTERNAL;
        }
    }

    if(sx_hashfs_self_uuid(h, &selfuuid))
	return FAIL_EINTERNAL;

    r = sx_hashfs_volume_first(h, &vol, 0);
    while(r == OK) {
	sx_nodelist_t *volnodes;
	sx_hash_t hash;

	if(hash_buf(h->cluster_uuid.string, strlen(h->cluster_uuid.string), vol->name, strlen(vol->name), &hash)) {
	    WARN("hashing volume name failed");
	    return FAIL_EINTERNAL;
	}

	volnodes = sx_hashfs_all_hashnodes(h, NL_NEXT, &hash, vol->max_replica);
	if(!volnodes)
	    return FAIL_EINTERNAL;

	do {
	    INFO("Nodes for volume %s:", vol->name);
	    for(i=0; i<sx_nodelist_count(volnodes); i++) {
		const sx_node_t *n = sx_nodelist_get(volnodes, i);
		INFO(" - %s(%s)", sx_node_uuid_str(n), sx_node_addr(n));
	    }
	} while(0);

	if(!sx_nodelist_lookup(volnodes, &selfuuid)) {
	    INFO("Removing all files from %s which no longer belong in here", vol->name);
	    for(i=0; i<METADBS; i++) {
		sqlite3_reset(h->qm_delbyvol[i]);
		if(qbind_int64(h->qm_delbyvol[i], ":volid", vol->id) ||
		   qstep_noret(h->qm_delbyvol[i])) {
                    WARN("Failed to delete relocated files on %u for volume %llu", i, (long long)vol->id);
                    sx_nodelist_delete(volnodes);
                    return FAIL_EINTERNAL;
                }
	    }
            if(sx_hashfs_reset_volume_cursize(h, vol->id, 0)) {
                sx_nodelist_delete(volnodes);
                return FAIL_EINTERNAL;
            }
	}
	sx_nodelist_delete(volnodes);
	r = sx_hashfs_volume_next(h);
    }

    if(r == ITER_NO_MORE)
	r = OK;

    return r;
}

sx_inprogress_t sx_hashfs_get_progress_info(sx_hashfs_t *h, const char **description) {
    sx_inprogress_t ret = INPRG_ERROR;
    int r;

    if(!h) {
	NULLARG();
	return INPRG_ERROR;
    }

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "rebalance_message"))
	goto getrblinfo_fail;
    r = qstep(h->q_getval);
    if(r != SQLITE_ROW) {
	if(r == SQLITE_DONE)
	    ret = INPRG_IDLE;
	goto getrblinfo_fail;
    }

    if(description) {
	const char *desc = (const char *)sqlite3_column_text(h->q_getval, 0);
	if(!desc)
	    desc = "Status description not available";
	sxi_strlcpy(h->job_message, desc, sizeof(h->job_message));
	*description = h->job_message;
    }

    /* Small race in here but it's not worth a transaction
     * "rebalance_complete" has got the last saying */
    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "rebalance_complete"))
	goto getrblinfo_fail;
    r = qstep(h->q_getval);
    if(r == SQLITE_ROW) {
	ret = sqlite3_column_int(h->q_getval, 0);
    } else {
	if(description)
	    *description = NULL;
	if(r == SQLITE_DONE)
	    ret = INPRG_IDLE;
	goto getrblinfo_fail;
    }

 getrblinfo_fail:
    sqlite3_reset(h->q_getval);
    if(ret == INPRG_ERROR)
	msg_set_reason("Failed to retrieve rebalance state info from the database");

    return ret;
}

rc_ty sx_hashfs_set_progress_info(sx_hashfs_t *h, sx_inprogress_t state, const char *description) {
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;

    if(!h) {
	NULLARG();
	return EFAULT;
    }

    if(state < INPRG_IDLE || state >= INPRG_LAST)
	return EINVAL;

    if(state == INPRG_IDLE) {
	if(qprep(h->db, &q, "DELETE FROM hashfs WHERE key IN ('rebalance_message', 'rebalance_complete')") ||
	   qstep_noret(q))
	    msg_set_reason("Failed to set rebalance state info to inactive");
	else
	    ret = OK;
	goto setrblinfo_fail;
    }

    if(!description)
	description = "Status description not available";

    qnullify(q);
    if(qprep(h->db, &q, "INSERT OR REPLACE INTO hashfs (key, value) VALUES (:k , :v)"))
	goto setrblinfo_fail;
    if(qbind_text(q, ":k", "rebalance_message") || qbind_text(q, ":v", description) || qstep_noret(q))
	goto setrblinfo_fail;
    if(qbind_text(q, ":k", "rebalance_complete") || qbind_int(q, ":v", state) || qstep_noret(q))
	goto setrblinfo_fail;
    INFO("Status: %s", description);
    ret = OK;

 setrblinfo_fail:
    sqlite3_finalize(q);

    if(ret == FAIL_EINTERNAL)
	msg_set_reason("Failed to update rebalance state info");

    return ret;
}

static rc_ty compute_volume_sizes(sx_hashfs_t *h) {
    rc_ty ret = FAIL_EINTERNAL, s;
    const sx_hashfs_volume_t *vol = NULL;
    sqlite3_stmt *q = NULL;
    unsigned int i;
    int locks[METADBS+1];

    memset(locks, 0, sizeof(int) * (METADBS+1));

    /* Iterate over all volumes */
    for(s = sx_hashfs_volume_first(h, &vol, 0); s == OK; s = sx_hashfs_volume_next(h)) {
        int64_t size = 0;

        /* Update volume size only if I've' just become a volnode for given volume */
        if(!is_new_volnode(h, vol))
            continue;

        if(qbegin(h->db))
            goto compute_volume_sizes_err;
        locks[METADBS] = 1;

        for(i=0; i<METADBS; i++) {
            if(qbegin(h->metadb[i]))
                goto compute_volume_sizes_err;
            locks[i] = 1;
        }

        /* Iterate over all meta databases */
        for(i = 0; i < METADBS; i++) {
            q = h->qm_sumfilesizes[i];
            int r;

            sqlite3_reset(q);
            if(qbind_int64(q, ":volid", vol->id)) {
                WARN("Failed to bind volume ID to a query");
                goto compute_volume_sizes_err;
            }

            /* Get volume size from one of meta databases */
            while((r = qstep(q)) == SQLITE_ROW)
                size += sqlite3_column_int64(q, 0);

            if(r != SQLITE_DONE) {
                WARN("Failed to query sum of file sizes for meta db: %d", i);
                goto compute_volume_sizes_err;
            }
        }

        /* Set proper volume size */
        if(sx_hashfs_reset_volume_cursize(h, vol->id, size)) {
            WARN("Failed to set volume %s size to %lld", vol->name, (long long)size);
            goto compute_volume_sizes_err;
        }

        if(qcommit(h->db))
            goto compute_volume_sizes_err;
        else
            locks[METADBS] = 0;

        /* Unlock meta dbs */
        for(i = 0; i < METADBS; i++) {
            qrollback(h->metadb[i]);
            locks[i] = 0;
        }
    }

    /* Check for iteration correctness */
    if(s != ITER_NO_MORE) {
        WARN("Failed to iterate volumes");
        goto compute_volume_sizes_err;
    }

    ret = OK;
    compute_volume_sizes_err:

    if(ret != OK && locks[METADBS])
        qrollback(h->db);

    for(i = 0; i < METADBS; i++)
        if(locks[i])
            qrollback(h->metadb[i]);

    sqlite3_reset(q);
    return ret;
}

rc_ty sx_hashfs_hdist_endrebalance(sx_hashfs_t *h) {
    job_t job_id;
    sx_nodelist_t *singlenode = NULL;
    const sx_node_t *self = sx_hashfs_self(h);
    rc_ty ret;

    DEBUG("IN %s", __func__);

    singlenode = sx_nodelist_new();
    if(!singlenode) {
	WARN("Cannot allocate single node nodelist");
	return ENOMEM;
    }

    ret = sx_nodelist_add(singlenode, sx_node_dup(self));
    if(ret)
	WARN("Cannot add self to nodelist");
    else
	ret = sx_hashfs_job_new(h, 0, &job_id, JOBTYPE_REBALANCE_CLEANUP, JOB_NO_EXPIRY, "CLEANRB", NULL, 0, singlenode);

    sx_nodelist_delete(singlenode);

    /* Compute volume sizes */
    if(ret == OK && compute_volume_sizes(h) != OK)
        WARN("Failed to compute volume sizes");

    return ret;
}

/* Change volume ownership. newid -> new user ID */
static rc_ty volume_chown(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, int64_t newid) {
    rc_ty ret = FAIL_EINTERNAL;

    if(!vol || newid < 0) {
        WARN("Failed to change volume ownership: incorrect argument");
        msg_set_reason("Incorrect argument");
        return EINVAL;
    }

    /* Change volume owner */
    sqlite3_reset(h->q_chownvolbyid);
    if(qbind_int64(h->q_chownvolbyid, ":owner", newid) ||
       qbind_int64(h->q_chownvolbyid, ":volid", vol->id) ||
       qstep_noret(h->q_chownvolbyid)) {
        msg_set_reason("Could not change volume ownership");
        goto volume_chown_err;
    }

    sqlite3_reset(h->q_grant);
    if(qbind_int64(h->q_grant, ":volid", vol->id) || qbind_int64(h->q_grant, ":uid", newid) ||
       qbind_int(h->q_grant, ":priv", PRIV_READ | PRIV_WRITE) || qstep_noret(h->q_grant)) {
        msg_set_reason("Could not add read-write privs for new volume owner");
        goto volume_chown_err;
    }

    /* Remove old volume owner privs */
    sqlite3_reset(h->q_dropvolprivs);
    if(qbind_int64(h->q_dropvolprivs, ":volid", vol->id) || qbind_int64(h->q_dropvolprivs, ":uid", vol->owner) || qstep_noret(h->q_dropvolprivs)) {
        msg_set_reason("Could not drop privs for old volume owner");
        goto volume_chown_err;
    }

    ret = OK;
volume_chown_err:
    sqlite3_reset(h->q_chownvolbyid);
    sqlite3_reset(h->q_grant);
    sqlite3_reset(h->q_dropvolprivs);
    return ret;
}

static rc_ty volume_resize(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, int64_t newsize) {
    rc_ty ret = FAIL_EINTERNAL;

    if(!vol) {
        NULLARG();
        return EINVAL;
    }

    sqlite3_reset(h->q_resizevol);
    if(qbind_int64(h->q_resizevol, ":size", newsize)
       || qbind_int64(h->q_resizevol, ":volid", vol->id)
       || qstep_noret(h->q_resizevol)) {
        msg_set_reason("Could not update volume size");
        goto volume_resize_err;
    }

    INFO("Volume %s size changed to %lld", vol->name, (long long)newsize);
    ret = OK;
volume_resize_err:
    sqlite3_reset(h->q_resizevol);
    return ret;
}

static rc_ty volume_change_revs(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, unsigned int max_revs) {
    rc_ty ret = FAIL_EINTERNAL;

    if(!vol) {
        NULLARG();
        return EINVAL;
    }

    sqlite3_reset(h->q_changerevs);
    if(qbind_int(h->q_changerevs, ":revs", max_revs)
       || qbind_int64(h->q_changerevs, ":volid", vol->id)
       || qstep_noret(h->q_changerevs)) {
        msg_set_reason("Could not update volume revisions limit");
        goto volume_change_revs_err;
    }

    INFO("Volume %s revisions limit changed to %d", vol->name, max_revs);
    ret = OK;
volume_change_revs_err:
    sqlite3_reset(h->q_changerevs);
    return ret;
}

rc_ty sx_hashfs_volume_mod(sx_hashfs_t *h, const char *volume, const char *newowner, int64_t newsize, int max_revs) {
    rc_ty ret = FAIL_EINTERNAL, s;
    const sx_hashfs_volume_t *vol = NULL;
    sx_uid_t newid;

    if(!volume || !newowner) {
        NULLARG();
        return ret;
    }

    if(!*newowner && newsize == -1 && max_revs == -1)
        return OK; /* Nothing to do */

    if(qbegin(h->db)) {
        msg_set_reason("Database is locked");
        return ret;
    }

    if(sx_hashfs_volume_by_name(h, volume, &vol) != OK) {
        msg_set_reason("Volume does not exist or is enabled");
        return ENOENT;
    }

    if(*newowner) {
        if(sx_hashfs_get_uid(h, newowner, &newid) != OK) {
            msg_set_reason("New volume owner does not exist or is disabled");
            ret = ENOENT;
            goto sx_hashfs_volume_mod_err;
        }

        if(newid != vol->owner && (s = volume_chown(h, vol, newid)) != OK) {
            ret = s;
            msg_set_reason("Failed to change volume owner: %s", msg_get_reason());
            goto sx_hashfs_volume_mod_err;
        }
        INFO("Volume %s owner changed to %s", vol->name, newowner);
    }

    if(newsize != -1 && newsize != vol->size) {
        /* Check new volume size correctness */
        if((s = sx_hashfs_check_volume_size(h, newsize, vol->max_replica))) {
            WARN("Invalid volume size given");
            ret = s;
            goto sx_hashfs_volume_mod_err;
        }

        /* Perform resize operation */
        if((s = volume_resize(h, vol, newsize)) != OK) {
            msg_set_reason("Failed to resize volume: %s", msg_get_reason());
            ret = s;
            goto sx_hashfs_volume_mod_err;
        }
    }

    if(max_revs != -1 && (unsigned int)max_revs <= SXLIMIT_MAX_REVISIONS && (unsigned int)max_revs >= SXLIMIT_MIN_REVISIONS) {
        /* Revisions correctness has been checked, modify volume now */
        if((s = volume_change_revs(h, vol, max_revs)) != OK) {
            msg_set_reason("Failed to change volume revisions limit: %s", msg_get_reason());
            ret = s;
            goto sx_hashfs_volume_mod_err;
        }
    }

    if(qcommit(h->db)) {
        WARN("Failed to commit changes");
        goto sx_hashfs_volume_mod_err;
    }

    ret = OK;
sx_hashfs_volume_mod_err:
    if(ret != OK)
        qrollback(h->db);

    return ret;
}

static const sx_node_t* sx_hashfs_first_nonfailed(sx_hashfs_t *h, sx_nodelist_t *nodelist)
{
    unsigned i;
    for (i=0;i<sx_nodelist_count(nodelist);i++) {
        const sx_node_t *node = sx_nodelist_get(nodelist, i);
        if (sx_hashfs_is_node_faulty(h, sx_node_uuid(node)))
            continue;
        return node;
    }
    return NULL;
}

static rc_ty sx_hashfs_should_repair(sx_hashfs_t *h, const block_meta_t *blockmeta, const sx_uuid_t *target)
{
    const sx_node_t *self = sx_hashfs_self(h);
    unsigned max_replica = 0, i;
    sx_nodelist_t *hashnodes;
    rc_ty ret = ITER_NO_MORE;
    if (!h || !blockmeta || !self || !target) {
        NULLARG();
        return EFAULT;
    }
    for (i=0;i<blockmeta->count;i++) {
        const block_meta_entry_t *entry = &blockmeta->entries[i];
        if (entry->op > 0 && entry->replica > max_replica)
            max_replica = entry->replica;
    }
    if (!max_replica)
        return ITER_NO_MORE;
    DEBUG("max_replica: %d", max_replica);
    hashnodes = sx_hashfs_all_hashnodes(h, NL_PREV, &blockmeta->hash, max_replica);
    if (!hashnodes) {
        WARN("cannot determine nodes for hash");
        return FAIL_EINTERNAL;
    }
    if (!sx_node_cmp(sx_hashfs_first_nonfailed(h, hashnodes), self)) {
        /* this node would be responsible for pushing */
        if (sx_nodelist_lookup(hashnodes, target)) {
            /* target used to be a replica for this hash */
            DEBUGHASH("repairing hash", &blockmeta->hash);
            ret = OK;
        }
    }
    sx_nodelist_delete(hashnodes);
    return ret;
}

static rc_ty sx_hashfs_file_find_step(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *maxrev, sx_hashfs_file_t *file, sx_find_cb_t cb, void *ctx)
{
    unsigned int fdb;
    int ret;
    rc_ty rc = ITER_NO_MORE;
    sqlite3_stmt *q;
    if (!h || !volume || !maxrev || !file) {
        NULLARG();
        return EFAULT;
    }
    fdb = file->name[0] ? getmetadb(file->name) : 0;
    if (file->revision[0]) {
        q = h->qm_list_rev_dec[fdb];
        sqlite3_reset(q);
        if (qbind_int(q, ":volid", volume->id) ||
            qbind_text(q, ":name", file->name) ||
            qbind_text(q, ":maxrev", file->revision))
            return FAIL_EINTERNAL;
        ret = qstep(q);
        if (ret == SQLITE_ROW) {
            file->file_size = sqlite3_column_int64(q, 0);
            sxi_strlcpy(file->revision, (const char*)sqlite3_column_text(q, 1), sizeof(file->revision));
            DEBUG("found: name=%s, revision=%s", file->name, file->revision);
            if (cb && !cb(volume, file, sqlite3_column_blob(q, 2), sqlite3_column_bytes(q, 2) / SXI_SHA1_BIN_LEN, ctx))
                rc = FAIL_ETOOMANY;
            else
                rc = OK;
        } else if (ret == SQLITE_DONE) {
            DEBUG("no more revisions for %s", file->name);
            file->revision[0] = '\0';
        } else {
            rc = FAIL_EINTERNAL;
        }
        sqlite3_reset(q);
        if (rc != ITER_NO_MORE)
            return rc;
    }

    do {
        q = h->qm_list_file[fdb];
        DEBUG("previous:%s, maxrev:%s", file->name, maxrev);
        if (qbind_int(q, ":volid", volume->id) ||
            qbind_text(q, ":previous", file->name) ||
            qbind_text(q, ":maxrev", maxrev))
            return FAIL_EINTERNAL;
        sqlite3_reset(q);
        ret = qstep(q);
        if (ret == SQLITE_ROW) {
            memset(file, 0, sizeof(*file));
            file->file_size = sqlite3_column_int64(q, 0);
            sxi_strlcpy(file->revision, (const char*)sqlite3_column_text(q, 1), sizeof(file->revision));
            sxi_strlcpy(file->name, (const char*)sqlite3_column_text(q, 3), sizeof(file->name));
            DEBUG("found new: name=%s, revision=%s", file->name, file->revision);
            if (cb && !cb(volume, file, sqlite3_column_blob(q, 2), sqlite3_column_bytes(q, 2) / SXI_SHA1_BIN_LEN, ctx))
                rc = FAIL_ETOOMANY;
            else
                rc = OK;
        } else if (ret == SQLITE_DONE) {
            DEBUG("no more files in fdb %d", fdb);
            file->name[0] = '\0';
            file->revision[0] = '\0';
            fdb++;
        } else {
            rc = FAIL_EINTERNAL;
        }
        sqlite3_reset(q);
        if (rc != OK && rc != ITER_NO_MORE)
            return rc;
        if (fdb >= METADBS)
            return ITER_NO_MORE;
    } while (ret == SQLITE_DONE);
    return rc;
}

rc_ty sx_hashfs_file_find(sx_hashfs_t *h, const sx_hashfs_volume_t *volume, const char *lastpath, const char *lastrev, const char *maxrev, sx_find_cb_t cb, void *ctx)
{
    rc_ty rc;
    sx_hashfs_file_t file;
    if(!h || !volume || !maxrev) {
        NULLARG();
        return EFAULT;
    }

    if(check_revision(maxrev)) {
	msg_set_reason("Invalid starting revision");
	return EINVAL;
    }

    if(lastpath && check_file_name(lastpath)<0) {
	msg_set_reason("Invalid starting file name");
	return EINVAL;
    }

    if(lastrev && check_revision(lastrev)) {
	msg_set_reason("Invalid starting revision");
	return EINVAL;
    }

    memset(&file, 0, sizeof(file));
    if (lastpath)
        sxi_strlcpy(file.name, lastpath, sizeof(file.name));
    if (lastrev)
        sxi_strlcpy(file.revision, lastrev, sizeof(file.revision));
    while ((rc = sx_hashfs_file_find_step(h, volume, maxrev, &file, cb, ctx)) == OK) {
        DEBUG("name: %s, revision: %s", file.name, file.revision);
    }
    return rc;
}

rc_ty sx_hashfs_br_find(sx_hashfs_t *h, const sx_block_meta_index_t *previous, unsigned rebalance_ver, const sx_uuid_t *target, block_meta_t **blockmetaptr)
{
    int ret;
    rc_ty rc;
    if (!h || !blockmetaptr) {
        NULLARG();
        return EFAULT;
    }
    *blockmetaptr = NULL;
    const sx_hash_t *hash = previous ? (const sx_hash_t*)&previous->b[1] : NULL;
    unsigned int ndb = hash ? gethashdb(hash) : 0;
    unsigned int sizeidx = previous ? previous->b[0] : 0;
    if (sizeidx >= SIZES) {
        WARN("bad size: %d", sizeidx);
        return EINVAL;
    }
    block_meta_t *blockmeta = *blockmetaptr = wrap_calloc(1, sizeof(*blockmeta));
    if (!blockmeta)
        return ENOMEM;
    DEBUG("rebalance_ver: %d", rebalance_ver);
    do {
        DEBUG("ndb: %d, sizeidx: %d", ndb, sizeidx);
        sqlite3_stmt *q = h->rit.q[sizeidx][ndb];
        sqlite3_stmt *qmeta = h->qb_get_meta[sizeidx][ndb];
        sqlite3_reset(q);
        sqlite3_reset(qmeta);
        sqlite3_clear_bindings(q);
        sqlite3_clear_bindings(qmeta);
        if (qbind_blob(q, ":prevhash", hash ? hash : (const void*)"", hash ? sizeof(*hash) : 0) ||
            qbind_int(qmeta, ":current_age", rebalance_ver)) {
            rc = FAIL_EINTERNAL;
            break;
        }
        do {
            ret = qstep(q);
            ret = sx_hashfs_blockmeta_get(h, ret, q, qmeta, bsz[sizeidx], blockmeta);
            sqlite3_reset(q);
            if (ret == OK) {
                DEBUG("row");
                rc = sx_hashfs_should_repair(h, blockmeta, target);
                if (rc == OK) {
                    /* this node is responsible for sending this data,
                     * and the target is supposed to have it */
                    DEBUGHASH("should_repair: true", &blockmeta->hash);
                    blockmeta->cursor.b[0] = sizeidx;
                    memcpy(&blockmeta->cursor.b[1], &blockmeta->hash, sizeof(blockmeta->hash));
                    return OK;
                }
                if (rc != ITER_NO_MORE)
                    break;
            } else if (ret == SQLITE_DONE) {
                rc = ITER_NO_MORE;
            } else if (ret != SQLITE_ROW) {
                rc = FAIL_EINTERNAL;
            }
        } while (ret == OK || ret == SQLITE_ROW);
        if (rc == ITER_NO_MORE) {
            if (++ndb >= HASHDBS) {
                ndb = 0;
                if (++sizeidx >= SIZES) {
                    sx_hashfs_blockmeta_free(blockmetaptr);
                    DEBUG("iteration done");
                    return ITER_NO_MORE;
                }
            }
            rc = OK;
            hash = NULL;/* reset iteration */
        }
    } while(rc == OK);
    WARN("iteration failed: %s", rc2str(rc));
    sx_hashfs_blockmeta_free(blockmetaptr);
    return rc;
}

rc_ty sx_hashfs_replace_getstartblock(sx_hashfs_t *h, unsigned int *version, const sx_node_t **node, int *have_blkidx, uint8_t *blkidx) {
    sqlite3_stmt *q = NULL;
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h || !version || !node || !have_blkidx || !blkidx) {
        NULLARG();
        return EFAULT;
    }

    if(qprep(h->db, &q, "SELECT node, last_block FROM replaceblocks LIMIT (SELECT ABS(COALESCE(RANDOM() % COUNT(*), 0)) FROM replaceblocks), 1"))
	goto getnode_fail;

    r = qstep(q);
    if(r == SQLITE_ROW) {
	const void *nodeid = sqlite3_column_blob(q, 0);
	const void *last = sqlite3_column_blob(q, 1);
	sx_uuid_t nuuid;

	if(!nodeid || sqlite3_column_bytes(q, 0) != UUID_BINARY_SIZE)
	    goto getnode_fail;
	if(last) {
	    if(sqlite3_column_bytes(q, 1) != 21)
		goto getnode_fail;
	    memcpy(blkidx, last, 21);
	    *have_blkidx = 1;
	} else
	    *have_blkidx = 0;
	uuid_from_binary(&nuuid, nodeid);
	*version = sxi_hdist_version(h->hd);
	*node = sx_nodelist_lookup(sx_hashfs_all_nodes(h, NL_NEXT), &nuuid);
	if(*node)
	    ret = OK;
    } else if (r == SQLITE_DONE)
	ret = ITER_NO_MORE;

 getnode_fail:
    sqlite3_finalize(q);
    return ret;
}

rc_ty sx_hashfs_replace_setlastblock(sx_hashfs_t *h, const sx_uuid_t *node, const uint8_t *blkidx) {
    sqlite3_stmt *q = NULL;
    int ret = FAIL_EINTERNAL;

    if(!h || !node) {
        NULLARG();
        return EFAULT;
    }

    if(blkidx) {
	if(qprep(h->db, &q, "UPDATE replaceblocks SET last_block = :block WHERE node = :node") ||
	   qbind_blob(q, ":block", blkidx, 21))
	    goto setnode_fail;
    } else {
	if(qprep(h->db, &q, "DELETE FROM replaceblocks WHERE node = :node"))
	    goto setnode_fail;
    }

    if(!qbind_blob(q, ":node", node->binary, sizeof(node->binary)) &&
       !qstep_noret(q))
	ret = OK;

 setnode_fail:
    sqlite3_finalize(q);
    return ret;
}

rc_ty sx_hashfs_replace_getstartfile(sx_hashfs_t *h, char *maxrev, char *startvol, char *startfile, char *startrev) {
    sqlite3_stmt *q = NULL;
    rc_ty ret = FAIL_EINTERNAL;
    int r;

    if(!h || !maxrev || !startvol || !startfile || !startrev) {
        NULLARG();
        return EFAULT;
    }

    if(qprep(h->db, &q, "SELECT vol, file, rev, maxrev FROM replacefiles LIMIT (SELECT ABS(COALESCE(RANDOM() % COUNT(*), 0)) FROM replacefiles), 1"))
	goto getfile_fail;

    r = qstep(q);
    if(r == SQLITE_ROW) {
	sxi_strlcpy(startvol, (const char *)sqlite3_column_text(q, 0), SXLIMIT_MAX_VOLNAME_LEN + 1);
	sxi_strlcpy(startfile, (const char *)sqlite3_column_text(q, 1), SXLIMIT_MAX_FILENAME_LEN + 1);
	sxi_strlcpy(startrev, (const char *)sqlite3_column_text(q, 2), REV_LEN + 1);
	sxi_strlcpy(maxrev, (const char *)sqlite3_column_text(q, 3), REV_LEN + 1);
	ret = OK;
    } else if (r == SQLITE_DONE)
	ret = ITER_NO_MORE;

 getfile_fail:
    sqlite3_finalize(q);
    return ret;
}

rc_ty sx_hashfs_replace_setlastfile(sx_hashfs_t *h, char *lastvol, char *lastfile, char *lastrev) {
    sqlite3_stmt *q = NULL;
    rc_ty ret = FAIL_EINTERNAL;

    if(!h || !lastvol) {
        NULLARG();
        return EFAULT;
    }
    if(!lastfile) {
	if(qprep(h->db, &q, "DELETE FROM replacefiles WHERE vol = :volume"))
	    goto setfile_fail;
    } else {
	if(!lastrev) {
	    NULLARG();
	    return EFAULT;
	}
	if(qprep(h->db, &q, "UPDATE replacefiles SET file = :file, rev = :rev WHERE vol = :volume") ||
	   qbind_text(q, ":file", lastfile) ||
	   qbind_text(q, ":rev", lastrev))
	    goto setfile_fail;
    }
    if(!qbind_text(q, ":volume", lastvol) &&
       !qstep_noret(q))
	ret = OK;

 setfile_fail:
    sqlite3_finalize(q);
    return ret;
}

rc_ty sx_hashfs_init_replacement(sx_hashfs_t *h) {
    const sx_hashfs_volume_t *vol;
    const sx_nodelist_t *nodes;
    rc_ty ret = FAIL_EINTERNAL;
    sqlite3_stmt *q = NULL;
    sx_uuid_t myid;
    unsigned int nnode, nnodes;

    if(!h) {
        NULLARG();
        return EFAULT;
    }

    if(qprep(h->db, &q, "DELETE FROM replaceblocks") || qstep_noret(q))
	goto init_replacement_fail;
    qnullify(q);
    if(qprep(h->db, &q, "DELETE FROM replacefiles") || qstep_noret(q))
	goto init_replacement_fail;
    qnullify(q);

    nnodes = sx_nodelist_count(h->faulty_nodes);
    if(nnodes) {
	if(qprep(h->db, &q, "UPDATE volumes SET volume = '.BAD' || volume, enabled = 0, changed = 0 WHERE volume NOT LIKE '.BAD%' AND replica <= :replica") ||
	   qbind_int(q, ":replica", nnodes) ||
	   qstep_noret(q))
	    goto init_replacement_fail;
	qnullify(q);
    }

    if(sx_hashfs_self_uuid(h, &myid))
	goto init_replacement_fail;
    if(!sx_hashfs_is_node_faulty(h, &myid)) {
	ret = OK; /* I am a good node */
	goto init_replacement_fail;
    }

    /* Blocks */
    nodes = sx_hashfs_all_nodes(h, NL_NEXT);
    nnodes = sx_nodelist_count(nodes);
    if(qprep(h->db, &q, "INSERT INTO replaceblocks (node) VALUES(:uuid)"))
	goto init_replacement_fail;
    for(nnode = 0; nnode < nnodes; nnode++) {
	const sx_uuid_t *nodeid = sx_node_uuid(sx_nodelist_get(nodes, nnode));
	if(sx_hashfs_is_node_faulty(h, nodeid))
	    continue;
	if(qbind_blob(q, ":uuid", nodeid->binary, sizeof(nodeid->binary)) ||
	   qstep_noret(q))
	    goto init_replacement_fail;
    }
    qnullify(q);

    /* Files */
    if(qprep(h->db, &q, "INSERT INTO replacefiles (vol, maxrev) VALUES(:volume, strftime('%Y-%m-%d %H:%M:%f', 'now', '30 minutes') || ':ffffffffffffffffffffffffffffffff')"))
       goto init_replacement_fail;

    for(ret = sx_hashfs_volume_first(h, &vol, 0); ret == OK; ret = sx_hashfs_volume_next(h)) {
	if(!sx_hashfs_is_or_was_my_volume(h, vol))
	    continue;
	if(qbind_text(q, ":volume", vol->name) ||
	   qstep_noret(q)) {
	    ret = FAIL_EINTERNAL;
	    goto init_replacement_fail;
	}
    }
    if(ret != ITER_NO_MORE)
	goto init_replacement_fail;
    ret = OK;

 init_replacement_fail:
    qnullify(q);
    return ret;
}

static rc_ty bump_hdist_version_only(sx_hashfs_t *h, int inactive_dist, int64_t *new_rev) {
    unsigned int cur_cfg_len, cfg_len, nnode, nnodes;
    const sx_nodelist_t *nodes;
    sxi_hdist_t *newmod = NULL;
    const void *cur_cfg, *cfg;

    if(!h) {
        NULLARG();
        return EFAULT;
    }

    if(!h->have_hd) {
	msg_set_reason("This node is inactive");
	return EINVAL;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is being rebalanced");
	return EINVAL;
    }

    if(sxi_hdist_get_cfg(h->hd, &cur_cfg, &cur_cfg_len)) {
	msg_set_reason("Failed to retrive current distribution (get)");
	return FAIL_EINTERNAL;
    }
    if(!(newmod = sxi_hdist_from_cfg(cur_cfg, cur_cfg_len))) {
	msg_set_reason("Failed to duplicate current distribution (from_cfg)");
	return ENOMEM;
    }
    if(sxi_hdist_newbuild(newmod)) {
	msg_set_reason("Failed to update node distribution");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    nodes = sx_hashfs_all_nodes(h, NL_NEXT);
    nnodes = sx_nodelist_count(nodes);
    for(nnode=0; nnode<nnodes; nnode++) {
	const sx_node_t *n = sx_nodelist_get(nodes, nnode);
	if(sxi_hdist_addnode(newmod, sx_node_uuid(n), sx_node_addr(n), sx_node_internal_addr(n), sx_node_capacity(n), NULL)) {
	    msg_set_reason("Failed to update node distribution");
	    sxi_hdist_free(newmod);
	    return ENOMEM;
	}
    }
    if(sxi_hdist_build(newmod)) {
	msg_set_reason("Failed to build updated distribution");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }
    if(sxi_hdist_rebalanced(newmod)) {
	msg_set_reason("Failed to flat the updated distribution");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }
    if(sxi_hdist_get_cfg(newmod, &cfg, &cfg_len)) {
	msg_set_reason("Failed to retrieve the updated distribution model");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    if(inactive_dist) {
	if(qbind_text(h->q_setval, ":k", "current_dist") ||
	   qbind_blob(h->q_setval, ":v", cur_cfg, cur_cfg_len) ||
	   qstep_noret(h->q_setval)) {
	    msg_set_reason("Failed to save updated distribution model");
	    sxi_hdist_free(newmod);
	    return FAIL_EINTERNAL;
	}
	if(qbind_text(h->q_setval, ":k", "current_dist_rev") ||
	   qbind_int64(h->q_setval, ":v", sxi_hdist_version(h->hd)) ||
	   qstep_noret(h->q_setval)) {
	    msg_set_reason("Failed to save updated distribution model");
	    sxi_hdist_free(newmod);
	    return FAIL_EINTERNAL;
	}
    }

    sqlite3_reset(h->q_setval);
    if(qbind_text(h->q_setval, ":k", "dist") ||
       qbind_blob(h->q_setval, ":v", cfg, cfg_len) ||
       qstep_noret(h->q_setval)) {
	msg_set_reason("Failed to save updated distribution model");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }
    if(qbind_text(h->q_setval, ":k", "dist_rev") ||
       qbind_int64(h->q_setval, ":v", sxi_hdist_version(newmod)) ||
       qstep_noret(h->q_setval)) {
	msg_set_reason("Failed to save updated distribution model");
	sxi_hdist_free(newmod);
	return FAIL_EINTERNAL;
    }

    if(new_rev)
	*new_rev = sxi_hdist_version(newmod);
    sxi_hdist_free(newmod);
    return OK;
}

rc_ty sx_hashfs_set_unfaulty(sx_hashfs_t *h, const sx_uuid_t *nodeid, int64_t dist_rev) {
    const sx_nodelist_t *nodes;
    sqlite3_stmt *q = NULL;
    int r;
    rc_ty ret = FAIL_EINTERNAL;

    if(!h || !nodeid) {
        NULLARG();
        return EFAULT;
    }

    if(dist_rev != h->hd_rev)
	return ENOENT;

    nodes = sx_hashfs_all_nodes(h, NL_NEXT);
    if(!sx_nodelist_lookup(nodes, nodeid) ||
       !sx_hashfs_is_node_faulty(h, nodeid))
	return EINVAL;

    if(qbegin(h->db)) {
        msg_set_reason("Database is locked");
        return FAIL_EINTERNAL;
    }
    if(qprep(h->db, &q, "UPDATE faultynodes SET restored = 1 WHERE dist = :dist AND node = :nodeid") ||
       qbind_int64(q, ":dist", dist_rev) ||
       qbind_blob(q, ":nodeid", nodeid->binary, sizeof(nodeid->binary)) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to update faulty nodes list");
	goto unfaulty_err;
    }
    if(!sqlite3_changes(h->db->handle)) {
	ret = OK;
	goto unfaulty_err;
    }

    qnullify(q);
    if(qprep(h->db, &q, "SELECT 1 FROM faultynodes WHERE dist = :dist AND restored = 0") ||
       qbind_int64(q, ":dist", dist_rev)) {
	msg_set_reason("Failed to check faulty nodes list");
	goto unfaulty_err;
    }
    r = qstep(q);
    if(r == SQLITE_ROW) {
	ret = OK;
	goto unfaulty_err;
    }
    qnullify(q);

    /* All faulty nodes are repaired: drop them, bump the dist */
    if(qprep(h->db, &q, "DELETE FROM faultynodes WHERE dist = :dist") ||
       qbind_int64(q, ":dist", dist_rev) ||
       qstep_noret(q)) {
	msg_set_reason("Failed to wipe faulty nodes list clean");
	goto unfaulty_err;
    }
    qnullify(q);

    ret = bump_hdist_version_only(h, 0, NULL);
    if(ret != OK)
	goto unfaulty_err;

    if(sx_hashfs_set_progress_info(h, INPRG_IDLE, NULL))
	goto unfaulty_err;

    ret = OK;

 unfaulty_err:
    if(ret == OK && qcommit(h->db))
	ret = FAIL_EINTERNAL;

    sqlite3_finalize(q);
    if(ret != OK)
	qrollback(h->db);

    return ret;
}

rc_ty sx_hashfs_setignored(sx_hashfs_t *h, const sx_nodelist_t *ignodes) {
    const sx_nodelist_t *allnodes;
    sx_nodelist_t *normnodes;
    unsigned int i, nnodes;
    sqlite3_stmt *q = NULL;
    int64_t newrev;
    rc_ty ret = FAIL_EINTERNAL;

    DEBUG("IN %s", __FUNCTION__);
    if(!h || !ignodes) {
	NULLARG();
	return EINVAL;
    }

    nnodes = sx_nodelist_count(ignodes);
    if(!nnodes) {
	msg_set_reason("No node was provided");
	return EINVAL;
    }

    if(!h->have_hd) {
	msg_set_reason("This node is inactive");
	return EINVAL;
    }

    if(h->is_rebalancing) {
	msg_set_reason("The cluster is being rebalanced");
	return EINVAL;
    }

    if(sx_nodelist_count(h->faulty_nodes)) {
	msg_set_reason("The cluster contains faulty nodes which are still being replaced");
	return EINVAL;
    }

    allnodes = sx_hashfs_all_nodes(h, NL_NEXT);
    normnodes = sx_nodelist_new();
    if(!normnodes) {
	msg_set_reason("Out of memory while duplicating list of nodes");
	return ENOMEM;
    }
    for(i=0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(ignodes, i);
	const sx_uuid_t *nodeid = sx_node_uuid(node);

	if(sx_nodelist_lookup(normnodes, nodeid))
	    continue;

	if(!sx_nodelist_lookup(allnodes, nodeid)) {
	    sx_nodelist_delete(normnodes);
	    msg_set_reason("Node %s is not an active cluster member", nodeid->string);
	    return EINVAL;
	}

	if(sx_nodelist_add(normnodes, sx_node_dup(node))) {
	    sx_nodelist_delete(normnodes);
	    msg_set_reason("Out of memory while duplicating list of nodes");
	    return ENOMEM;
	}
    }

    if(sx_nodelist_addlist(normnodes, h->ignored_nodes)) {
	sx_nodelist_delete(normnodes);
	msg_set_reason("Out of memory while duplicating list of nodes");
	return ENOMEM;
    }
    nnodes = sx_nodelist_count(normnodes);
    if(nnodes >= sx_nodelist_count(allnodes)) {
	sx_nodelist_delete(normnodes);
	msg_set_reason("Cannot tag all nodes");
	return EINVAL;
    }

    if(qbegin(h->db)) {
	sx_nodelist_delete(normnodes);
	msg_set_reason("Internal error: failed to start trasaction");
	return FAIL_EINTERNAL;
    }

    ret = bump_hdist_version_only(h, 1, &newrev);
    if(ret != OK)
	goto setignored_fail;

    if(qprep(h->db, &q, "INSERT OR IGNORE INTO ignorednodes (dist, node) VALUES (:dist, :nodeid)") ||
       qbind_int64(q, ":dist", newrev)) {
	msg_set_reason("Failed to update the node database");
	ret = FAIL_EINTERNAL;
	goto setignored_fail;
    }
    for(i = 0; i<nnodes; i++) {
	const sx_node_t *node = sx_nodelist_get(normnodes, i);
	const sx_uuid_t *nodeid = sx_node_uuid(node);

	if(qbind_blob(q, ":nodeid", nodeid->binary, sizeof(nodeid->binary)) ||
	   qstep_noret(q)) {
	    msg_set_reason("Failed to update the node database");
	    ret = FAIL_EINTERNAL;
	    goto setignored_fail;
	}
    }
    qnullify(q);

    if(qcommit(h->db)) {
	msg_set_reason("Failed to save distribution model");
	ret = FAIL_EINTERNAL;
    }

 setignored_fail:
    qnullify(q);
    if(ret != OK)
	qrollback(h->db);
    sx_nodelist_delete(normnodes);

    return ret;
}

rc_ty sx_hashfs_node_status(sx_hashfs_t *h, sxi_node_status_t *status) {
    const sx_node_t *n;
    time_t t = time(NULL);
    struct tm *tm;

    if(!status) {
        NULLARG();
        return EINVAL;
    }
    memset(status, 0, sizeof(*status));

    /* System information */
    if(sxi_report_os(h->sx, status->os_name, sizeof(status->os_name), status->os_arch, sizeof(status->os_arch),
        status->os_release, sizeof(status->os_release), status->os_version, sizeof(status->os_version))) {
        WARN("Failed to get OS information: %s", sxc_geterrmsg(h->sx));
        return FAIL_EINTERNAL;
    }

    /* Processor information */
    if(sxi_report_cpu(h->sx, &status->cores, status->endianness, sizeof(status->endianness))) {
        WARN("Failed to get CPU information: %s", sxc_geterrmsg(h->sx));
        return FAIL_EINTERNAL;
    }

    /* Filesystem information */
    if(sxi_report_fs(h->sx, h->dir, &status->block_size, &status->total_blocks, &status->avail_blocks)) {
        WARN("Failed to get hashFS node directory filesystem information: %s", sxc_geterrmsg(h->sx));
        return FAIL_EINTERNAL;
    }

    /* Get available memory */
    if(sxi_report_mem(h->sx, &status->mem_total)) {
        WARN("Failed to get memory information: %s", sxc_geterrmsg(h->sx));
        return FAIL_EINTERNAL;
    }

    tm = gmtime(&t);
    if (tm && strftime(status->utctime, sizeof(status->utctime), "%Y-%m-%d %H:%M:%S UTC", tm) <= 0) {
        WARN("Failed to get UTC time");
        return FAIL_EINTERNAL;
    }
    tm = localtime(&t);
    if (tm && strftime(status->localtime, sizeof(status->localtime), "%Y-%m-%d %H:%M:%S %Z", tm) <= 0) {
        WARN("Failed to get local time");
        return FAIL_EINTERNAL;
    }

    /* Storage information */
    snprintf(status->storage_dir, sizeof(status->storage_dir), "%s", h->dir);
    sx_storage_usage(h, &status->storage_allocated, &status->storage_commited);
    snprintf(status->hashfs_version, sizeof(status->hashfs_version), "%s", sx_hashfs_version(h));
    n = sx_hashfs_self(h);
    status->is_bare = n ? 0 : 1; /* Node is bare when n == NULL */
    if(n) {
        snprintf(status->internal_addr, sizeof(status->internal_addr), "%s", sx_node_internal_addr(n));
        snprintf(status->addr, sizeof(status->addr), "%s", sx_node_addr(n));
        snprintf(status->uuid, sizeof(status->uuid), "%s", sx_node_uuid_str(n));
    }

    snprintf(status->libsx_version, sizeof(status->libsx_version), "%s", sxc_get_version());
    snprintf(status->hashfs_version, sizeof(status->hashfs_version), "%s", HASHFS_VERSION);

    const char *local_heal = sx_hashfs_heal_status_local(h);
    const char *remote_heal = sx_hashfs_heal_status_remote(h);
    if (local_heal)
        snprintf(status->heal_status, sizeof(status->heal_status), "local: %s", local_heal);
    else if (remote_heal)
        snprintf(status->heal_status, sizeof(status->heal_status), "remote: %s", remote_heal);
    else
        snprintf(status->heal_status, sizeof(status->heal_status), "DONE");
    status->heal_status[sizeof(status->heal_status)-1] = '\0';
    return OK;
}

rc_ty sx_hashfs_distlock_get(sx_hashfs_t *h, char *lockid, unsigned int lockid_len) {
    int r;
    rc_ty ret = FAIL_EINTERNAL;

    sqlite3_reset(h->q_getval);

    if(qbind_text(h->q_getval, ":k", "distlock")) {
        WARN("Failed to prepare getval query");
        goto sx_hashfs_distlock_get_err;
    }

    r = qstep(h->q_getval);
    if(r == SQLITE_DONE) {
        if(lockid && lockid_len)
            lockid[0] = '\0';
        ret = ENOENT;
    } else if(r == SQLITE_ROW) {
        if(lockid && lockid_len) {
            const char *lockid_str = (const char *)sqlite3_column_text(h->q_getval, 0);

            if(!lockid_str) {
                WARN("Failed to get distribution lock");
                goto sx_hashfs_distlock_get_err;
            }
            sxi_strlcpy(lockid, lockid_str, lockid_len);
        }
        ret = OK;
    } else {
        WARN("Failed to check distribution lock");
        goto sx_hashfs_distlock_get_err;
    }

sx_hashfs_distlock_get_err:
    sqlite3_reset(h->q_getval);
    return ret;
}

rc_ty sx_hashfs_distlock_acquire(sx_hashfs_t *h, const char *lockid) {
    rc_ty ret = FAIL_EINTERNAL, s;
    char existing_lock[AUTH_UID_LEN*2+32];

    if(!lockid) {
        WARN("NULL lockid argument");
        return EINVAL;
    }

    if(qbegin(h->db)) {
        WARN("Failed to lock database");
        return FAIL_LOCKED;
    }

    s = sx_hashfs_distlock_get(h, existing_lock, sizeof(existing_lock));
    if(s == OK) {
        ret = EEXIST;
        DEBUG("Lock operation requested but there is currently a lock stored: %s", existing_lock);
        goto sx_hashfs_distlock_acquire_err;
    } else if(s != ENOENT) {
        WARN("Failed to check distlock existence");
        ret = s;
        goto sx_hashfs_distlock_acquire_err;
    }

    sqlite3_reset(h->q_setval);
    if(qbind_text(h->q_setval, ":k", "distlock") ||
       qbind_text(h->q_setval, ":v", lockid) ||
       qstep_noret(h->q_setval)) {
        WARN("Failed to acquire lock");
        goto sx_hashfs_distlock_acquire_err;
    }

    if(qcommit(h->db)) {
        WARN("Failed to commit distlock acquisition");
        goto sx_hashfs_distlock_acquire_err;
    }

    ret = OK;
sx_hashfs_distlock_acquire_err:
    if(ret != OK)
        qrollback(h->db);
    sqlite3_reset(h->q_setval);
    return ret;
}

/* Release distribution lock */
rc_ty sx_hashfs_distlock_release(sx_hashfs_t *h) {
    sqlite3_reset(h->q_delval);
    if(qbind_text(h->q_delval, ":k", "distlock") || qstep_noret(h->q_delval)) {
        WARN("Failed to unlock distribution");
        return FAIL_EINTERNAL;
    }

    return OK;
}

rc_ty sx_hashfs_cluster_set_name(sx_hashfs_t *h, const char *name) {
    if(!h || !name) {
        msg_set_reason("Invalid argument");
        return EINVAL;
    }

    sqlite3_reset(h->q_setval);
    if(qbind_text(h->q_setval, ":k", "cluster_name") || qbind_text(h->q_setval, ":v", name) || qstep_noret(h->q_setval)) {
        msg_set_reason("Failed to set cluster name");
        return FAIL_EINTERNAL;
    }

    free(h->cluster_name);
    h->cluster_name = wrap_strdup(name);
    if (!h->cluster_name) {
        msg_set_reason("Failed to get cluster name: Out of memory");
        return FAIL_EINTERNAL;
    }

    return OK;
}

rc_ty sx_hashfs_cluster_get_name(sx_hashfs_t *h, const char **name) {
    const char *n;

    if(!h || !name) {
        msg_set_reason("Invalid argument");
        return EINVAL;
    }

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "cluster_name") || qstep_ret(h->q_getval)) {
        msg_set_reason("Failed to get cluster name");
        return FAIL_EINTERNAL;
    }

    n = (const char*)sqlite3_column_text(h->q_getval, 0);
    if(!n) {
        msg_set_reason("Failed go get cluster name");
        return FAIL_EINTERNAL;
    }

    free(h->cluster_name);
    h->cluster_name = wrap_strdup((const char*)sqlite3_column_text(h->q_getval, 0));
    if (!h->cluster_name) {
        msg_set_reason("Failed to get cluster name: Out of memory");
        return FAIL_EINTERNAL;
    }

    *name = h->cluster_name;
    return OK;
}

rc_ty sx_hashfs_cluster_set_mode(sx_hashfs_t *h, int readonly) {
    sqlite3_reset(h->q_setval);
    if(readonly != 0 && readonly != 1) {
        msg_set_reason("Invalid argument");
        return EINVAL;
    }

    if(qbind_text(h->q_setval, ":k", "mode") || qbind_text(h->q_setval, ":v", readonly ? "ro" : "rw") || qstep_noret(h->q_setval)) {
        WARN("Failed to set cluster operating mode");
        return FAIL_EINTERNAL;
    }

    INFO("Cluster has been switched to '%s' mode", readonly ? "read-only" : "read-write");
    return OK;
}

rc_ty sx_hashfs_cluster_get_mode(sx_hashfs_t *h, int *mode) {
    const char *mode_str;
    int r;
    rc_ty ret = FAIL_EINTERNAL;

    if(!h || !mode) {
        NULLARG();
        return EINVAL;
    }

    sqlite3_reset(h->q_getval);
    if(qbind_text(h->q_getval, ":k", "mode")) {
        WARN("Failed to get cluster operating mode");
        return FAIL_EINTERNAL;
    }

    r = qstep(h->q_getval);
    if(r == SQLITE_DONE) {
        *mode = 0; /* Default fallback, when not set cluster is in read-write mode */
        ret = OK;
        goto sx_hashfs_cluster_get_mode_err;
    } else if(r != SQLITE_ROW) {
        WARN("Failed to get cluster operating mode");
        goto sx_hashfs_cluster_get_mode_err;
    }

    mode_str = (const char *)sqlite3_column_text(h->q_getval, 0);
    if(!mode_str) {
        WARN("Failed to get cluster operating mode");
        goto sx_hashfs_cluster_get_mode_err;
    }

    if(!strncmp(mode_str, "ro", 2))
        *mode = 1;
    else if(!strncmp(mode_str, "rw", 2))
        *mode = 0;
    else {
        WARN("Failed to get cluster operating mode: invalid mode");
        goto sx_hashfs_cluster_get_mode_err;
    }

    ret = OK;
sx_hashfs_cluster_get_mode_err:
    if(ret == OK)
        h->readonly = *mode;
    else
        h->readonly = 0;
    sqlite3_reset(h->q_getval);
    return ret;
}

int sx_hashfs_is_readonly(sx_hashfs_t *h) {
    return h ? h->readonly : 0;
}

rc_ty sx_hashfs_list_revision_blocks(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, const sx_uuid_t *target, sx_hash_t *min_revision_id, unsigned age_limit, unsigned i, lrb_cb_t cb, lrb_count_t cb_count) {
    rc_ty rc = FAIL_EINTERNAL;
    if (!vol || !target || !cb) {
        NULLARG();
        return rc;
    }
    if (i >= METADBS) {
        msg_set_reason("Invalid metadb");
        return rc;
    }
    sqlite3_reset(h->qm_needs_upgrade[i]);
    int r = qstep(h->qm_needs_upgrade[i]);
    sqlite3_reset(h->qm_needs_upgrade[i]);
    if (r != SQLITE_DONE) {
        msg_set_reason("Upgrade not yet completed");
        return EAGAIN;
    }
    do {
        sx_hash_t id;
        sqlite3_stmt *qcount = h->qm_count_rb[i], *q = h->qm_get_rb[i];
        sqlite3_reset(qcount);
        sqlite3_reset(q);
        int ret, k=0;
        /* FIXME: bump hashfs version elsewhere, or have local age separate from
         * hdist version */
        age_limit++;
        if (qbind_int64(q, ":volume_id", vol->id) || qbind_int64(qcount, ":volume_id", vol->id) ||
            qbind_int(q, ":age_limit", age_limit) || qbind_int(qcount, ":age_limit", age_limit))
            break;
        if(min_revision_id) {
            if (qbind_blob(q, ":min_revision_id", min_revision_id->b, sizeof(min_revision_id->b)) ||
                qbind_blob(qcount, ":min_revision_id", min_revision_id->b, sizeof(min_revision_id->b)))
                break;
        } else {
            if (qbind_blob(q, ":min_revision_id", "", 0) ||
                qbind_blob(qcount, ":min_revision_id", "", 0))
                break;
        }
        DEBUG("before query, volume name: %s, metadb: %d, volume id: %lld, age_limit: %d", vol->name, i, (long long)vol->id, age_limit);
        if (qstep_ret(qcount))
            break;
        if (cb_count(sqlite3_column_int64(qcount, 0))) {
            msg_set_reason("count callback failed");
            ret = -1;
            break;
        }
        sqlite3_reset(qcount);
        while ((ret = qstep(q)) == SQLITE_ROW && (k++ < gc_max_batch)) {
            sx_hash_t revision_id;
            int64_t size = sqlite3_column_int64(q, 0);
            if (hash_of_blob_result(&revision_id, q, 1)) {
                msg_set_reason("corrupt hash blob");
                ret = -1;
                break;
            }
            const sx_hash_t *contents = sqlite3_column_blob(q, 2);
            unsigned int block_size;
            int64_t nblocks = size_to_blocks(size, NULL, &block_size);
            DEBUG("volume: %s, metadb: %d, file: %s", vol->name, i, sqlite3_column_text(q, 3));
            DEBUGHASH("row revision", &revision_id);
            if (nblocks * sizeof(*contents) != sqlite3_column_bytes(q, 2)) {
                msg_set_reason("corrupt file blob: %ld * %lu != %d", nblocks, sizeof(*contents), sqlite3_column_bytes(q, 2));
                ret = -1;
                break;
            }
            if (cb(vol, target, &revision_id, contents, nblocks, block_size)) {
                msg_set_reason("block revision list callback failed");
                ret = -1;
                break;
            }
            if (!min_revision_id)
                min_revision_id = &id;
            memcpy(min_revision_id->b, revision_id.b, sizeof(revision_id.b));
        }
        sqlite3_reset(q);
        if (ret != SQLITE_ROW && ret != SQLITE_DONE)
            break;
        rc = OK;
        if (min_revision_id) {
            DEBUG("after query, volume name: %s, metadb: %d, volume id: %lld, age_limit: %d, sent revisions %d", vol->name, i, (long long)vol->id, age_limit, k);
            DEBUGHASH("last revision in this iteration", min_revision_id);
        }
    } while(0);
    DEBUG("returning: %s", rc2str(rc));
    return rc;
}

rc_ty sx_hashfs_heal_update(sx_hashfs_t *h, const sx_hashfs_volume_t *vol, const sx_hash_t *min_revision_id, unsigned metadb)
{
    if (!h || !vol) {
        NULLARG();
        return EFAULT;
    }
    if (metadb >= METADBS) {
        WARN("Invalid metadb: %u", metadb);
        return EINVAL;
    }
    if (min_revision_id) {
        DEBUGHASH("Updating min_revision_id to", min_revision_id);
        sqlite3_stmt *qupd = h->qm_upd_heal_volume[metadb];
        sqlite3_reset(qupd);
        if (qbind_blob(qupd,":min_revision_id",min_revision_id->b,sizeof(min_revision_id->b)) ||
            qbind_text(qupd,":name",vol->name) ||
            qstep_noret(qupd))
            return FAIL_EINTERNAL;
    } else {
        DEBUG("Finished volume heal for %s", vol->name);
        sqlite3_stmt *qdel = h->qm_del_heal_volume[metadb];
        sqlite3_reset(qdel);
        if (qbind_text(qdel,":name",vol->name) ||
            qstep_noret(qdel))
            return FAIL_EINTERNAL;
    }
    return OK;
}

rc_ty sx_hashfs_remote_heal(sx_hashfs_t *h, heal_cb_t cb)
{
    rc_ty rc = FAIL_EINTERNAL;
    unsigned i;
    unsigned has_heal = 0;
    DEBUG("IN");
    for (i=0;i<METADBS;i++) {
        char prev[SXLIMIT_MAX_VOLNAME_LEN+1];
        int ret;
        prev[0] = 0;
        sqlite3_stmt *qsel = h->qm_sel_heal_volume[i];
        sqlite3_stmt *qupd = h->qm_upd_heal_volume[i];
        sqlite3_reset(qsel);
        sqlite3_reset(qupd);
        if(qbind_text(qsel, ":prev", prev))
                break;
        do {
            ret = qstep(qsel);
            if (ret == SQLITE_ROW) {
                const sx_hashfs_volume_t *volume;
                const unsigned char *name = sqlite3_column_text(qsel, 0);
                int max_age = sqlite3_column_int(qsel, 1);
                sx_hash_t min_revision_id;
                const sx_hash_t *min_revision_in = NULL;
                strncpy(prev, (const char*)name, sizeof(prev));
                prev[sizeof(prev)-1] = '\0';
                DEBUG("heal: volume=%s, max_Age=%d, metadb: %d", name, max_age, i);
                if(sx_hashfs_volume_by_name(h, (const char*)name, &volume))
                    break;
                if (sqlite3_column_bytes(qsel, 2)) {
                    if (hash_of_blob_result(&min_revision_id, qsel, 2))
                        break;
                    min_revision_in = &min_revision_id;
                }
                /* TODO: transaction? */
                has_heal = 1;
                if (cb(h, volume, min_revision_in, max_age, i))
                    break;
            }
        } while(ret == SQLITE_ROW);
        sqlite3_reset(qsel);
        sqlite3_reset(qupd);
        if (ret != SQLITE_DONE)
            break;
    }
    if (i == METADBS)
        return has_heal ? OK : ITER_NO_MORE;
    return rc;
}

int sx_hashfs_has_upgrade_job(sx_hashfs_t *h)
{
    int ret;
    sqlite3_stmt *q = h->qe_count_upgradejobs;
    sqlite3_reset(q);
    if (qstep_ret(q))
        return -1;
    ret = sqlite3_column_int(q, 0);
    sqlite3_reset(q);
    return ret;
}

const char *sx_hashfs_heal_status_local(sx_hashfs_t *h)
{
    switch (sx_hashfs_has_upgrade_job(h))
        {
        case 0:
            return NULL;
        case 1:
            return "In progress";
        default:
            return "Error";
        }
}

const char *sx_hashfs_heal_status_remote(sx_hashfs_t *h)
{
    if (sx_hashfs_has_upgrade_job(h))
        return "Waiting on local heal";
    for (unsigned i=0;i<METADBS;i++) {
        sqlite3_stmt *qsel = h->qm_sel_heal_volume[i];
        sqlite3_reset(qsel);
        if(qbind_text(qsel, ":prev", ""))
            break;
        int ret = qstep(qsel);
        sqlite3_reset(qsel);
        if (ret == SQLITE_ROW)
            return "Pending";
        else if (ret != SQLITE_DONE)
            return "Error";
    }
    return NULL;
}
