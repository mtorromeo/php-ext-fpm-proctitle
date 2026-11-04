/*
 * Stand-ins for the PHP symbols referenced by fpm_proctitle.c, so the
 * harnesses link without libphp. Built without the PHP headers: only the
 * symbol names matter.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* PHP's headers map snprintf() to this one, used by the %P placeholder */
int ap_php_snprintf(char *buf, size_t len, const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(buf, len, format, ap);
	va_end(ap);
	return n;
}

/* MINIT/MSHUTDOWN/MINFO only, never called by the harnesses */
#define UNREACHABLE(name) void name(void) { abort(); }

UNREACHABLE(OnUpdateBool)
UNREACHABLE(OnUpdateString)
UNREACHABLE(display_ini_entries)
UNREACHABLE(zend_ini_boolean_displayer_cb)
UNREACHABLE(zend_register_ini_entries)       /* PHP < 8.2 */
UNREACHABLE(zend_unregister_ini_entries)
UNREACHABLE(zend_register_ini_entries_ex)    /* PHP >= 8.2 */
UNREACHABLE(zend_unregister_ini_entries_ex)
UNREACHABLE(php_info_print_table_start)
UNREACHABLE(php_info_print_table_header)
UNREACHABLE(php_info_print_table_row)
UNREACHABLE(php_info_print_table_end)
