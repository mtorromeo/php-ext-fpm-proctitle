/*
 * LD_PRELOAD shim for the integration tests: replaces PT_GAP before main(),
 * like an extension calling setenv() in its MINIT would. Its original string
 * stays in the argv+environ area but environ[] no longer points to it, so FPM
 * sees a gap and its title buffer ends right before it. The strings after the
 * gap (PT_SENTINEL) must not be overwritten by fpm_proctitle.
 */
#include <stdlib.h>

__attribute__((constructor))
static void envgap_init(void)
{
	if (getenv("PT_GAP")) {
		setenv("PT_GAP", "replaced", 1);
	}
}
