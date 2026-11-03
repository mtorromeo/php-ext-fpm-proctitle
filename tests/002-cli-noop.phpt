--TEST--
fpm_proctitle: outside php-fpm the process title is left untouched
--SKIPIF--
<?php if (!extension_loaded('fpm_proctitle')) die('skip'); if (!is_readable('/proc/self/cmdline')) die('skip /proc not available'); ?>
--INI--
fpm_proctitle.format=CHANGED
--FILE--
<?php
$argv0 = explode("\0", file_get_contents('/proc/self/cmdline'))[0];
var_dump(strpos($argv0, 'CHANGED') === false);
var_dump($argv0 !== '');
?>
--EXPECT--
bool(true)
bool(true)
