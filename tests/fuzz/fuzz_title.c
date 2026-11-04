/*
 * libFuzzer target: a php-fpm worker serving two requests.
 *
 * The real RINIT/RSHUTDOWN of the extension run against a title buffer
 * allocated with the exact size of the argv+environ area, so ASan catches any
 * write past env_end; the oracles below catch writes past FPM's own buffer
 * (pt_cap) that would still land inside the area.
 *
 * Input:
 *   [0..1] cap_env (mod 8192)     [2..3] cap_args (mod cap_env + 1)
 *   [4..5] gap: first non-zero byte after the idle title (0 = none)
 *   [6]    flags (FL_*)           [7]    byte filling the area past the gap
 *   then NUL-separated: idle title, format, KEY=VALUE FastCGI params.
 */

#include "../../fpm_proctitle.c"

#include <stdlib.h>

#ifdef ZTS
# error "the fuzz harness needs a NTS build of PHP"
#endif

#define FL_ENVIRON_IN_BUFFER 0x01 /* environ not relocated by FPM */
#define FL_FORK              0x02 /* pcntl_fork() between the requests */
#define FL_NO_GETENV         0x04 /* sapi_module.getenv == NULL */
#define FL_DISABLED          0x08 /* fpm_proctitle.enabled = 0 */
#define FL_SECOND_FORMAT     0x10 /* 2nd request uses the 1st param as format */

#define MAX_PARAMS 64

sapi_module_struct sapi_module;
sapi_globals_struct sapi_globals;

extern int pt_stub_log_calls; /* stubs.c */

static const char *params[MAX_PARAMS];
static size_t nparams;

#if PHP_VERSION_ID >= 80000
static char *fuzz_getenv(const char *name, size_t len)
#else
static char *fuzz_getenv(char *name, size_t len)
#endif
{
	size_t i;

	for (i = 0; i < nparams; i++) {
		if (strncmp(params[i], name, len) == 0 && params[i][len] == '=') {
			return (char *) params[i] + len + 1;
		}
	}
	return NULL;
}

/* what MSHUTDOWN does, minus the INI unregistration */
static void reset_state(void)
{
	free(pt_idle);
	free(pt_work);
	pt_idle = pt_work = NULL;
	pt_pool = "?";
	pt_pid = 0;
	pt_mode = PT_METHOD_NONE;
	pt_cap = 0;
	pt_last_len = 0;
	pt_changed = 0;
	pt_logged = 0;
	pt_stub_log_calls = 0;
}

#define CHECK(cond) do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: oracle failed: %s\n", __FILE__, __LINE__, #cond); \
			abort(); \
		} \
	} while (0)

/* Nothing may change outside FPM's buffer: pt_buf[0 .. pt_cap]. */
static void check_untouched(const char *area, const char *snap, size_t size)
{
	size_t from = pt_mode == PT_METHOD_NONE ? 0 : pt_cap + 1;

	CHECK(pt_mode == PT_METHOD_NONE || pt_cap < size);
	CHECK(from >= size || memcmp(area + from, snap + from, size - from) == 0);
}

/* The title is followed by zeros up to the end of FPM's buffer. */
static void check_tail_zero(const char *area)
{
	size_t i;

	if (pt_mode == PT_METHOD_NONE || pt_mode == PT_METHOD_STRLEN) {
		return;
	}
	for (i = strlen(area); i <= pt_cap; i++) {
		CHECK(area[i] == '\0');
	}
}

static void request(char *area, const char *snap, size_t size, const char *fmt, const char *idle)
{
	size_t i, len;

	fpm_proctitle_globals.format = (char *) fmt;

	PHP_RINIT(fpm_proctitle)(0, 0);
	check_untouched(area, snap, size);
	check_tail_zero(area);

	if (pt_changed) {
		len = strlen(area);
		CHECK(len <= pt_cap);
		for (i = 0; i < len; i++) {
			unsigned char c = (unsigned char) area[i];
			CHECK(c >= 0x20 && c != 0x7f);
		}
		if (!strchr(fmt, '%')) {
			/* no placeholders: the sanitized format, truncated */
			size_t flen = strlen(fmt);
			CHECK(len == (flen < pt_cap ? flen : pt_cap));
			for (i = 0; i < len; i++) {
				unsigned char c = (unsigned char) fmt[i];
				CHECK(area[i] == ((c < 0x20 || c == 0x7f) ? '?' : (char) c));
			}
		}
	}

	PHP_RSHUTDOWN(fpm_proctitle)(0, 0);
	check_untouched(area, snap, size);
	check_tail_zero(area);
	if (pt_mode != PT_METHOD_NONE) {
		CHECK(strcmp(area, idle) == 0);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *input, *fields[2], *p, *end;
	char *area, *snap, *idle;
	size_t cap_env, cap_args, gap, area_size, idle_len, i;
	uint8_t flags, fill;
	char outside[] = "FOO=bar";
	char *fake_environ[2];
	char **saved_environ;

	if (size < 8) {
		return 0;
	}
	cap_env = (data[0] | data[1] << 8) % 8192;
	cap_args = (data[2] | data[3] << 8) % (cap_env + 1);
	gap = data[4] | data[5] << 8;
	flags = data[6];
	fill = data[7] ? data[7] : 'E';

	/* NUL-separated fields; missing ones are empty */
	input = malloc(size - 8 + 1);
	memcpy(input, data + 8, size - 8);
	input[size - 8] = '\0';
	end = input + size - 8;
	p = input;
	for (i = 0; i < 2; i++) {
		fields[i] = p;
		p += strlen(p);
		if (p < end) {
			p++;
		}
	}
	nparams = 0;
	while (p < end && nparams < MAX_PARAMS) {
		params[nparams++] = p;
		p += strlen(p) + 1;
	}

	/* without /proc bounds (cap_env == 0) the area is just the title */
	idle_len = strlen(fields[0]);
	area_size = cap_env ? cap_env + 1 : idle_len + 1;
	if (idle_len > area_size - 1) {
		idle_len = area_size - 1;
	}

	area = malloc(area_size);
	memset(area, 0, area_size);
	memcpy(area, fields[0], idle_len);
	if (gap) {
		gap %= area_size;
		if (gap <= idle_len) {
			gap = idle_len + 1;
		}
		/* the last byte terminates the last environ string */
		for (i = gap; i + 1 < area_size; i++) {
			area[i] = (char) fill;
		}
	}
	idle = malloc(idle_len + 1);
	memcpy(idle, area, idle_len + 1);
	snap = malloc(area_size);
	memcpy(snap, area, area_size);

	reset_state();
	pt_is_fpm = 1;
	pt_buf = area;
	pt_cap_env = cap_env;
	pt_cap_args = cap_args;
	SG(server_context) = (void *) 1;
	sapi_module.getenv = (flags & FL_NO_GETENV) ? NULL : fuzz_getenv;
	fpm_proctitle_globals.enabled = !(flags & FL_DISABLED);

	fake_environ[0] = (flags & FL_ENVIRON_IN_BUFFER) ? area + (gap ? gap : area_size - 1) : outside;
	fake_environ[1] = NULL;
	saved_environ = environ;
	environ = fake_environ;

	request(area, snap, area_size, fields[1], idle);
	if (flags & FL_FORK) {
		pt_pid = 0;
	}
	request(area, snap, area_size, (flags & FL_SECOND_FORMAT) && nparams ? params[0] : fields[1], idle);

	/* a failure is logged once per worker, forked children included */
	CHECK(pt_stub_log_calls <= 1);
	CHECK(pt_stub_log_calls == 0 || (pt_mode == PT_METHOD_NONE && fpm_proctitle_globals.enabled));

	environ = saved_environ;
	reset_state();
	pt_buf = NULL;
	free(area);
	free(snap);
	free(idle);
	free(input);
	return 0;
}
