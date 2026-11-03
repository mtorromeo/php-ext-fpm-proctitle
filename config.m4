PHP_ARG_ENABLE([fpm-proctitle],
  [whether to enable fpm_proctitle support],
  [AS_HELP_STRING([--enable-fpm-proctitle],
    [Enable automatic php-fpm worker process titles])],
  [no])

if test "$PHP_FPM_PROCTITLE" != "no"; then
  case $host_alias in
    *linux*) ;;
    *)
      case `uname -s 2>/dev/null` in
        Linux) ;;
        *) AC_MSG_ERROR([fpm_proctitle is supported on Linux only]) ;;
      esac
      ;;
  esac

  AC_DEFINE(HAVE_FPM_PROCTITLE, 1, [Whether fpm_proctitle is enabled])
  PHP_NEW_EXTENSION(fpm_proctitle, fpm_proctitle.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
fi
