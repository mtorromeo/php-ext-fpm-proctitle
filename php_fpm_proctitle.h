#ifndef PHP_FPM_PROCTITLE_H
#define PHP_FPM_PROCTITLE_H

extern zend_module_entry fpm_proctitle_module_entry;
#define phpext_fpm_proctitle_ptr &fpm_proctitle_module_entry

#define PHP_FPM_PROCTITLE_VERSION "1.0.0"

#if PHP_VERSION_ID >= 80000
typedef bool fpm_proctitle_bool;
#else
typedef zend_bool fpm_proctitle_bool;
#endif

ZEND_BEGIN_MODULE_GLOBALS(fpm_proctitle)
	fpm_proctitle_bool enabled;
	char *format;
ZEND_END_MODULE_GLOBALS(fpm_proctitle)

#if defined(ZTS) && defined(COMPILE_DL_FPM_PROCTITLE)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#define FPM_PROCTITLE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(fpm_proctitle, v)

#endif
