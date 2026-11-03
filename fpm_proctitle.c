/*
 * fpm_proctitle - automatic process titles for php-fpm workers.
 *
 * While a php-fpm worker serves a request its process title (as shown by
 * ps/top) is replaced by a configurable string built from the FastCGI
 * parameters of the request; at the end of the request the original title
 * set by FPM ("php-fpm: pool www") is restored.
 *
 * How it works (Linux only):
 *
 * php-fpm itself, in the master process (fpm_env_init_main), relocates
 * environ[] to the heap and uses the whole contiguous argv[] + environ[]
 * memory area as the buffer for the process title, writing it at argv[0].
 * Workers inherit that layout through fork(). This extension writes into the
 * very same buffer, with the same bounds FPM uses, which it learns from
 * /proc/self/stat (arg_start .. env_end).
 *
 * The fpm_* symbols (fpm_env_setproctitle & co.) are not exported by the
 * php-fpm binary, so they cannot be reused.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "ext/standard/info.h"
#include "php_fpm_proctitle.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

#ifndef ZEND_TSRMLS_CACHE_UPDATE
# define ZEND_TSRMLS_CACHE_UPDATE()
#endif

/* Same prefix as php-fpm's SETPROCTITLE_PREFIX + "pool " */
#define FPM_POOL_PREFIX "php-fpm: pool "

/* When the title overflows the original argv area, Linux serves
 * /proc/<pid>/cmdline from a single page: longer titles are never visible. */
#define PT_MAX_TITLE 4095

#define DEFAULT_FORMAT \
	"php-fpm: pool %p (%{REMOTE_ADDR} -> %{REQUEST_METHOD} %s://%{HTTP_HOST}:%{SERVER_PORT}%{REQUEST_URI})"

ZEND_DECLARE_MODULE_GLOBALS(fpm_proctitle)

/* Bounds of the title buffer, read once at MINIT. */
typedef enum {
	PT_METHOD_NONE = 0,   /* not running under php-fpm, or unusable */
	PT_METHOD_PROC_STAT,  /* full argv+environ area, from /proc/self/stat */
	PT_METHOD_ARGV_ONLY,  /* environ not relocated: argv strings only */
	PT_METHOD_STRLEN      /* fallback: length of the current title */
} pt_method;

static int        pt_is_fpm = 0;
static char      *pt_buf = NULL;     /* == argv[0] */
static size_t     pt_cap_env = 0;    /* env_end - arg_start - 1 */
static size_t     pt_cap_args = 0;   /* arg_end - arg_start - 1 */

/* Per-worker state, initialised lazily on the first request of a process. */
static pid_t      pt_pid = 0;
static pt_method  pt_mode = PT_METHOD_NONE;
static size_t     pt_cap = 0;        /* max title length (excluding NUL) */
static size_t     pt_last_len = 0;   /* length of the title currently written */
static char      *pt_idle = NULL;    /* title set by FPM for the idle worker */
static size_t     pt_idle_len = 0;
static const char *pt_pool = "?";
static char      *pt_work = NULL;    /* scratch buffer, pt_cap + 1 bytes */
static int        pt_changed = 0;    /* title modified during this request */

PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN("fpm_proctitle.enabled", "1", PHP_INI_ALL, OnUpdateBool,
		enabled, zend_fpm_proctitle_globals, fpm_proctitle_globals)
	STD_PHP_INI_ENTRY("fpm_proctitle.format", DEFAULT_FORMAT, PHP_INI_ALL, OnUpdateString,
		format, zend_fpm_proctitle_globals, fpm_proctitle_globals)
PHP_INI_END()

static void php_fpm_proctitle_init_globals(zend_fpm_proctitle_globals *g)
{
	g->enabled = 1;
	g->format = NULL;
}

/* {{{ /proc/self/stat parsing */
static int pt_read_proc_stat(uintptr_t *arg_start, uintptr_t *arg_end,
                             uintptr_t *env_start, uintptr_t *env_end)
{
	char data[4096];
	FILE *fp;
	size_t n;
	char *p, *tok, *save = NULL;
	int field;
	unsigned long long v[4] = {0, 0, 0, 0};

	fp = fopen("/proc/self/stat", "r");
	if (!fp) {
		return FAILURE;
	}
	n = fread(data, 1, sizeof(data) - 1, fp);
	fclose(fp);
	if (n == 0) {
		return FAILURE;
	}
	data[n] = '\0';

	/* field 2 (comm) may contain spaces and parentheses: skip to the last ')' */
	p = strrchr(data, ')');
	if (!p) {
		return FAILURE;
	}
	p++;

	/* the token after ')' is field 3 */
	field = 3;
	for (tok = strtok_r(p, " \n", &save); tok; tok = strtok_r(NULL, " \n", &save), field++) {
		if (field >= 48 && field <= 51) {
			v[field - 48] = strtoull(tok, NULL, 10);
			if (field == 51) {
				break;
			}
		}
	}
	if (field != 51) {
		return FAILURE; /* kernel < 3.5 */
	}

	*arg_start = (uintptr_t) v[0];
	*arg_end   = (uintptr_t) v[1];
	*env_start = (uintptr_t) v[2];
	*env_end   = (uintptr_t) v[3];
	return SUCCESS;
}
/* }}} */

static void pt_detect_buffer(void)
{
	uintptr_t arg_start, arg_end, env_start, env_end;

	pt_buf = (char *) sapi_module.executable_location;
	pt_cap_env = pt_cap_args = 0;

	if (!pt_buf) {
		return;
	}

	if (pt_read_proc_stat(&arg_start, &arg_end, &env_start, &env_end) == SUCCESS
	    && arg_start != 0
	    && arg_start == (uintptr_t) pt_buf
	    && arg_end > arg_start
	    && env_end >= arg_end
	    && env_start >= arg_end) {
		pt_cap_args = arg_end - arg_start - 1;
		pt_cap_env = env_end - arg_start - 1;
	}
}

/* Is any environ[] string still inside the title buffer? */
static int pt_environ_in_buffer(void)
{
	char **e;
	uintptr_t lo = (uintptr_t) pt_buf, hi = lo + pt_cap_env;

	if (!environ) {
		return 0;
	}
	for (e = environ; *e; e++) {
		uintptr_t p = (uintptr_t) *e;
		if (p >= lo && p <= hi) {
			return 1;
		}
	}
	return 0;
}

/* First request served by this process (i.e. after fork). */
static void pt_init_worker(void)
{
	size_t len;

	pt_pid = getpid();
	pt_mode = PT_METHOD_NONE;
	pt_cap = 0;

	if (pt_idle) {
		free(pt_idle);
		pt_idle = NULL;
	}
	if (pt_work) {
		free(pt_work);
		pt_work = NULL;
	}
	pt_pool = "?";

	if (!pt_buf) {
		return;
	}

	len = strlen(pt_buf);

	if (pt_cap_env > 0 && !pt_environ_in_buffer()) {
		pt_mode = PT_METHOD_PROC_STAT;
		pt_cap = pt_cap_env;
	} else if (pt_cap_args > 0) {
		pt_mode = PT_METHOD_ARGV_ONLY;
		pt_cap = pt_cap_args;
	} else {
		pt_mode = PT_METHOD_STRLEN;
		pt_cap = len;
	}

	if (pt_cap > PT_MAX_TITLE && len <= PT_MAX_TITLE) {
		pt_cap = PT_MAX_TITLE;
	}

	if (len > pt_cap) {
		/* should never happen: do not touch anything */
		pt_mode = PT_METHOD_NONE;
		pt_cap = 0;
		return;
	}

	pt_idle = strdup(pt_buf);
	pt_work = malloc(pt_cap + 1);
	if (!pt_idle || !pt_work || pt_cap == 0) {
		free(pt_idle);
		free(pt_work);
		pt_idle = pt_work = NULL;
		pt_mode = PT_METHOD_NONE;
		pt_cap = 0;
		return;
	}
	pt_idle_len = len;
	pt_last_len = len;

	if (strncmp(pt_idle, FPM_POOL_PREFIX, sizeof(FPM_POOL_PREFIX) - 1) == 0
	    && pt_idle[sizeof(FPM_POOL_PREFIX) - 1] != '\0') {
		pt_pool = pt_idle + sizeof(FPM_POOL_PREFIX) - 1;
	}
}

static void pt_write(const char *title, size_t len)
{
	size_t clear;

	if (len > pt_cap) {
		len = pt_cap;
	}
	clear = pt_last_len > len ? pt_last_len : len;

	memcpy(pt_buf, title, len);
	if (clear > len) {
		memset(pt_buf + len, 0, clear - len);
	}
	pt_buf[len] = '\0';
	pt_buf[pt_cap] = '\0';
	pt_last_len = len;
}

/* {{{ title formatting */
typedef struct {
	char *s;
	size_t len;
	size_t cap;
} pt_str;

static void pt_append(pt_str *b, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n && b->len < b->cap; i++) {
		unsigned char c = (unsigned char) s[i];
		b->s[b->len++] = (c < 0x20 || c == 0x7f) ? '?' : (char) c;
	}
}

static void pt_append_cstr(pt_str *b, const char *s)
{
	pt_append(b, s, strlen(s));
}

static const char *pt_param(const char *name, size_t len)
{
	if (!sapi_module.getenv) {
		return NULL;
	}
	/* the prototype is non-const in PHP 7.x */
	return sapi_module.getenv((char *) name, len);
}

#define PT_PARAM(lit) pt_param(lit, sizeof(lit) - 1)

/* PHP's empty() for a string value */
static int pt_empty(const char *v)
{
	return v == NULL || v[0] == '\0' || (v[0] == '0' && v[1] == '\0');
}

static const char *pt_scheme(void)
{
	const char *v = PT_PARAM("REQUEST_SCHEME");

	if (!pt_empty(v)) {
		return v;
	}
	if (!pt_empty(PT_PARAM("HTTPS"))) {
		return "https";
	}
	v = PT_PARAM("SERVER_PORT");
	if (v && strcmp(v, "443") == 0) {
		return "https";
	}
	return "http";
}

static size_t pt_format(const char *fmt, char *out, size_t cap)
{
	pt_str b;
	const char *p = fmt;

	b.s = out;
	b.len = 0;
	b.cap = cap;

	while (*p && b.len < b.cap) {
		const char *pct = strchr(p, '%');

		if (!pct) {
			pt_append_cstr(&b, p);
			break;
		}
		pt_append(&b, p, pct - p);
		p = pct + 1;

		switch (*p) {
			case 'p':
				pt_append_cstr(&b, pt_pool);
				p++;
				break;
			case 's':
				pt_append_cstr(&b, pt_scheme());
				p++;
				break;
			case 'P': {
				char pid[32];
				int n = snprintf(pid, sizeof(pid), "%ld", (long) pt_pid);
				pt_append(&b, pid, n > 0 ? (size_t) n : 0);
				p++;
				break;
			}
			case '%':
				pt_append(&b, "%", 1);
				p++;
				break;
			case '{': {
				const char *end = strchr(p + 1, '}');
				if (end && end > p + 1) {
					char name[256];
					size_t nlen = end - (p + 1);
					const char *v = NULL;
					if (nlen < sizeof(name)) {
						memcpy(name, p + 1, nlen);
						name[nlen] = '\0';
						v = pt_param(name, nlen);
					}
					pt_append_cstr(&b, v ? v : "-");
					p = end + 1;
				} else {
					pt_append(&b, "%", 1);
				}
				break;
			}
			case '\0':
				pt_append(&b, "%", 1);
				break;
			default:
				/* unknown placeholder: copy verbatim */
				pt_append(&b, "%", 1);
				break;
		}
	}

	return b.len;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(fpm_proctitle)
{
	ZEND_INIT_MODULE_GLOBALS(fpm_proctitle, php_fpm_proctitle_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	pt_is_fpm = sapi_module.name && strcmp(sapi_module.name, "fpm-fcgi") == 0;
	if (pt_is_fpm) {
		pt_detect_buffer();
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(fpm_proctitle)
{
	UNREGISTER_INI_ENTRIES();

	free(pt_idle);
	free(pt_work);
	pt_idle = pt_work = NULL;
	pt_mode = PT_METHOD_NONE;
	pt_cap = 0;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(fpm_proctitle)
{
	const char *fmt;
	size_t len;

#if defined(ZTS) && defined(COMPILE_DL_FPM_PROCTITLE)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	pt_changed = 0;

	/* SG(server_context) is NULL for "php-fpm -i" and similar */
	if (!pt_is_fpm || !SG(server_context) || !FPM_PROCTITLE_G(enabled)) {
		return SUCCESS;
	}

	if (pt_pid != getpid()) {
		pt_init_worker();
	}
	if (pt_mode == PT_METHOD_NONE) {
		return SUCCESS;
	}

	fmt = FPM_PROCTITLE_G(format);
	if (!fmt || !*fmt) {
		return SUCCESS;
	}

	len = pt_format(fmt, pt_work, pt_cap);
	pt_write(pt_work, len);
	pt_changed = 1;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(fpm_proctitle)
{
	if (pt_changed && pt_mode != PT_METHOD_NONE && pt_pid == getpid()) {
		pt_write(pt_idle, pt_idle_len);
	}
	pt_changed = 0;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(fpm_proctitle)
{
	const char *method;
	char cap[32];

	switch (pt_mode) {
		case PT_METHOD_PROC_STAT: method = "argv+environ (/proc/self/stat)"; break;
		case PT_METHOD_ARGV_ONLY: method = "argv only (environ not relocated)"; break;
		case PT_METHOD_STRLEN:    method = "current title length (fallback)"; break;
		default:                  method = pt_is_fpm ? "not initialised" : "inactive (not php-fpm)"; break;
	}
	snprintf(cap, sizeof(cap), "%lu", (unsigned long) pt_cap);

	php_info_print_table_start();
	php_info_print_table_header(2, "fpm_proctitle support", "enabled");
	php_info_print_table_row(2, "Version", PHP_FPM_PROCTITLE_VERSION);
	php_info_print_table_row(2, "Buffer", method);
	php_info_print_table_row(2, "Max title length", cap);
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

zend_module_entry fpm_proctitle_module_entry = {
	STANDARD_MODULE_HEADER,
	"fpm_proctitle",
	NULL,
	PHP_MINIT(fpm_proctitle),
	PHP_MSHUTDOWN(fpm_proctitle),
	PHP_RINIT(fpm_proctitle),
	PHP_RSHUTDOWN(fpm_proctitle),
	PHP_MINFO(fpm_proctitle),
	PHP_FPM_PROCTITLE_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_FPM_PROCTITLE
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(fpm_proctitle)
#endif
