/* $Id: pkgindb.c,v 1.12 2012/05/30 09:27:12 imilh Exp $ */

/*
 * Copyright (c) 2009-2015 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Emile "iMil" Heitor <imil@NetBSD.org> .
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sqlite3.h>
#include "pkgin.h"

#define H_BUF   6

static sqlite3	*pdb;
static char		*pdberr = NULL;
static int		pdbres = 0;
static FILE		*sql_log_fp;
static int              repo_counter = 0;

static const char *pragmaopts[] = {
	"locking_mode = EXCLUSIVE",
	"empty_result_callbacks = 1",
	"synchronous = OFF",
	"journal_mode = MEMORY",
	NULL
};

char *pkgin_dbdir;
char *pkgin_sqldb;
char *pkgin_cache;
char *pkgin_errlog;
char *pkgin_sqllog;

void
setup_pkgin_dbdir(void)
{
	char *p;

	if ((p = getenv("PKGIN_DBDIR")) != NULL)
		pkgin_dbdir = xasprintf("%s", p);
	else
		pkgin_dbdir = xasprintf("%s", PKGIN_DBDIR);

	pkgin_sqldb = xasprintf("%s/pkgin.db", pkgin_dbdir);
	pkgin_cache = xasprintf("%s/cache", pkgin_dbdir);
	pkgin_errlog = xasprintf("%s/pkg_install-err.log", pkgin_dbdir);
	pkgin_sqllog = xasprintf("%s/sql.log", pkgin_dbdir);

	if (access(pkgin_dbdir, F_OK) != 0) {
		if (mkdir(pkgin_dbdir, 0755) < 0)
			err(1, "Failed to create %s", pkgin_dbdir);
	}

	if (access(pkgin_cache, F_OK) != 0) {
		if (mkdir(pkgin_cache, 0755) < 0)
			err(1, "Failed to create %s", pkgin_cache);
	}
}

uint8_t
have_privs(int reqd)
{
	if ((reqd & PRIVS_PKGDB) &&
	    (access(pkgdb_get_dir(), F_OK) == 0) &&
	    (access(pkgdb_get_dir(), W_OK) < 0))
		return 0;

	if ((reqd & PRIVS_PKGINDB) &&
	    (access(pkgin_dbdir, F_OK) == 0) &&
	    (access(pkgin_dbdir, W_OK) < 0))
		return 0;

	return 1;
}

const char *
pdb_version(void)
{
	return "SQLite "SQLITE_VERSION;
}

static void
pdb_err(const char *errmsg)
{
	warn("%s: %s", errmsg, sqlite3_errmsg(pdb));
	sqlite3_close(pdb);
	exit(EXIT_FAILURE);
}

/*
 * unused: optional parameter given as 4th argument of sqlite3_exec
 * argc  : row number
 * argv  : row
 * col   : column
 *
 *    col[0]    col[1]          col[argc]
 *  ______________________________________
 * | argv[0] | argv[1] | ... | argv[argc] |
 *
 * WARNING: callback is called on every line
 */
static int
pkgindb_simple_callback(void *param, int argc, char **argv, char **colname)
{
	pdbres = argc;

	if (argv == NULL)
		return PDB_ERR;

	return PDB_OK;
}

/* sqlite callback, record a single value */
int
pdb_get_value(void *param, int argc, char **argv, char **colname)
{
	char *value = (char *)param;

	if (argv != NULL) {
		XSTRCPY(value, argv[0]);

		return PDB_OK;
	}

	return PDB_ERR;
}

int
pkgindb_dovaquery(const char *fmt, ...)
{
	char *buf;
	va_list ap;
	int rv;

	va_start(ap, fmt);
	if (vasprintf(&buf, fmt, ap) == -1)
		errx(EXIT_FAILURE, "Insufficient memory to construct query");
	va_end(ap);

	rv = pkgindb_doquery(buf, NULL, NULL);

	free(buf);

	return rv;
}

int
pkgindb_doquery(const char *query,
	int (*pkgindb_callback)(void *, int, char **, char **), void *param)
{
	if (sqlite3_exec(pdb, query, pkgindb_callback, param, &pdberr)
		!= SQLITE_OK) {
		if (sql_log_fp != NULL) {
			if (pdberr != NULL)
				fprintf(sql_log_fp, "SQL error: %s\n", pdberr);
			fprintf(sql_log_fp, "SQL query: %s\n", query);
		}
		sqlite3_free(pdberr);

		return PDB_ERR;
	}

	return PDB_OK;
}

void
pkgindb_close()
{
	sqlite3_close(pdb);

	if (sql_log_fp != NULL)
		fclose(sql_log_fp);
}

uint8_t
upgrade_database()
{
	if (pkgindb_doquery(COMPAT_CHECK,
			pkgindb_simple_callback, NULL) == PDB_ERR) {
#ifdef notyet
		/*
		 * COMPAT_CHECK query leads to an error for an
		 * incompatible database
		 */
		printf(MSG_DATABASE_NOT_COMPAT);
		if (!check_yesno(DEFAULT_YES))
			exit(EXIT_FAILURE);
#endif

		pkgindb_reset();

		return 1;
	}

	return 0;
}

void
pkgindb_init()
{
	int i;
	char buf[BUFSIZ];

	/*
	 * Do not exit if pkgin_sqllog is not writable.
	 * Permit users to do list-operations
	 */
	sql_log_fp = fopen(pkgin_sqllog, "w");

	if (sqlite3_open(pkgin_sqldb, &pdb) != SQLITE_OK)
		pdb_err("Can't open database");

	/* generic query in order to check tables existence */
	if (pkgindb_doquery("select * from sqlite_master;",
			pkgindb_simple_callback, NULL) != PDB_OK)
		pdb_err("Can't access database");

	/* apply PRAGMA properties */
	for (i = 0; pragmaopts[i] != NULL; i++) {
		snprintf(buf, BUFSIZ, "PRAGMA %s;", pragmaopts[i]);
		pkgindb_doquery(buf, NULL, NULL);
	}

	pkgindb_doquery(CREATE_DRYDB, NULL, NULL);
}

/**
 * \brief destroy the database and re-create it (upgrade)
 */
void
pkgindb_reset()
{
	pkgindb_close();

	if (unlink(pkgin_sqldb) < 0)
		err(EXIT_FAILURE, MSG_DELETE_DB_FAILED, pkgin_sqldb);

	pkgindb_init();
}

int
pkg_db_mtime()
{
	struct stat	st;
	time_t	   	db_mtime = 0;
	char		str_mtime[20], buf[BUFSIZ];

	/* no pkgdb file */
	if (stat(pkgdb_get_dir(), &st) < 0)
		return 0;

	str_mtime[0] = '\0';

	pkgindb_doquery("SELECT PKGDB_MTIME FROM PKGDB;",
		pdb_get_value, str_mtime);

	if (str_mtime[0] != '\0')
		db_mtime = (time_t)strtol(str_mtime, (char **)NULL, 10);

	/* mtime is up to date */
	if (db_mtime == st.st_mtime)
		return 0;

	snprintf(buf, BUFSIZ, UPDATE_PKGDB_MTIME, (long long)st.st_mtime);
	/* update mtime */
	pkgindb_doquery(buf, NULL, NULL);

	return 1;
}

void
repo_record(char **repos)
{
	int	i;
	char	query[BUFSIZ], value[20];

	for (i = 0; repos[i] != NULL; i++) {
		snprintf(query, BUFSIZ, EXISTS_REPO, repos[i]);
		pkgindb_doquery(query, pdb_get_value, &value[0]);
                repo_counter++;

		if (value[0] == '0') {
			/* repository does not exists */
			snprintf(query, BUFSIZ, INSERT_REPO, repos[i]);
			pkgindb_doquery(query, NULL, NULL);
		}
	}
}

time_t
pkg_sum_mtime(char *repo)
{
	time_t	db_mtime = 0;
	char	str_mtime[20], query[BUFSIZ];

	str_mtime[0] = '\0';

	snprintf(query, BUFSIZ,
		"SELECT REPO_MTIME FROM REPOS WHERE REPO_URL GLOB \'%s*\';",
		repo);
	pkgindb_doquery(query, pdb_get_value, str_mtime);

	if (str_mtime[0] != '\0')
		db_mtime = (time_t)strtol(str_mtime, (char **)NULL, 10);

	return db_mtime;
}

void
pkgindb_stats()
{
        int     i;
        int64_t local_size;
        int64_t remote_size;
        char    h_local_size[H_BUF];
        char    h_remote_size[H_BUF];
        char    query[BUFSIZ];

        struct {
                char value[BUFSIZ];
                const char *op;
                const char *term;
                const char *place;
        } stats[] = {
                { "", "COUNT", "PKG_ID", "LOCAL" },
                { "", "COUNT", "PKG_ID", "REMOTE" },
                { "", "SUM", "SIZE_PKG", "LOCAL" },
                { "", "SUM", "FILE_SIZE", "REMOTE" },
                { "", NULL, NULL, NULL },
        };

        for (i = 0; stats[i].op != NULL; i++) {
                snprintf(query, BUFSIZ, "SELECT %s(%s) FROM %s_PKG;",
                        stats[i].op, stats[i].term, stats[i].place);
                pkgindb_doquery(query, pdb_get_value, stats[i].value);
        }

        local_size = strtol(stats[2].value, NULL, 10);
        remote_size = strtol(stats[3].value, NULL, 10);

        (void)humanize_number(h_local_size, H_BUF, local_size, "",
                        HN_AUTOSCALE, HN_B | HN_NOSPACE | HN_DECIMAL);
        (void)humanize_number(h_remote_size, H_BUF, remote_size, "",
                        HN_AUTOSCALE, HN_B | HN_NOSPACE | HN_DECIMAL);

        printf(MSG_LOCAL_STAT_TITLE
                MSG_LOCAL_PACKAGES
                MSG_LOCAL_PKG_SIZE
                MSG_REMOTE_STAT_TITLE
                MSG_REMOTE_NB_REPOS
                MSG_REMOTE_PACKAGES
                MSG_REMOTE_PKG_SIZE,
                stats[0].value, h_local_size, repo_counter, stats[1].value, h_remote_size);
}
