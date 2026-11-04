/*
 * libFuzzer target: the /proc/self/stat parser, checked against a naive
 * reference tokenizer.
 */

#include "../../fpm_proctitle.c"

#include <stdlib.h>

#ifdef ZTS
# error "the fuzz harness needs a NTS build of PHP"
#endif

sapi_module_struct sapi_module;
sapi_globals_struct sapi_globals;

#define CHECK(cond) do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: oracle failed: %s\n", __FILE__, __LINE__, #cond); \
			abort(); \
		} \
	} while (0)

/* fields 48..51 of a NUL-terminated stat line; returns how many were found */
static int reference(const char *data, unsigned long long v[4])
{
	const char *p = strrchr(data, ')');
	char tok[4096];
	size_t n;
	int field = 2, found = 0;

	if (!p) {
		return 0;
	}
	for (p++; *p; ) {
		while (*p == ' ' || *p == '\n') {
			p++;
		}
		if (!*p) {
			break;
		}
		field++;
		n = strcspn(p, " \n");
		if (field >= 48 && field <= 51) {
			/* strtoull() skips leading \t\f\v\r: stay inside the token */
			memcpy(tok, p, n);
			tok[n] = '\0';
			v[field - 48] = strtoull(tok, NULL, 10);
			found++;
		}
		p += n;
	}
	return found;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* same size as the buffer read by pt_read_proc_stat */
	char buf[4096], copy[4096];
	uintptr_t a[4] = {0, 0, 0, 0};
	unsigned long long v[4] = {0, 0, 0, 0};
	int ret, found;

	if (size > sizeof(buf) - 1) {
		size = sizeof(buf) - 1;
	}
	memcpy(buf, data, size);
	buf[size] = '\0';
	memcpy(copy, buf, size + 1);

	ret = pt_parse_proc_stat(buf, &a[0], &a[1], &a[2], &a[3]);
	found = reference(copy, v);

	CHECK((ret == SUCCESS) == (found == 4));
	if (ret == SUCCESS) {
		CHECK(a[0] == (uintptr_t) v[0] && a[1] == (uintptr_t) v[1]);
		CHECK(a[2] == (uintptr_t) v[2] && a[3] == (uintptr_t) v[3]);
	}
	return 0;
}
