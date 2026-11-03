--TEST--
fpm_proctitle: extension loads and registers its ini directives
--SKIPIF--
<?php if (!extension_loaded('fpm_proctitle')) die('skip'); ?>
--FILE--
<?php
var_dump(extension_loaded('fpm_proctitle'));
var_dump(ini_get('fpm_proctitle.enabled'));
var_dump(ini_get('fpm_proctitle.format'));
var_dump(ini_set('fpm_proctitle.format', '%p %{REQUEST_URI}') !== false);
var_dump(ini_get('fpm_proctitle.format'));
?>
--EXPECT--
bool(true)
string(1) "1"
string(101) "php-fpm: pool %p (%{REMOTE_ADDR} -> %{REQUEST_METHOD} %s://%{HTTP_HOST}:%{SERVER_PORT}%{REQUEST_URI})"
bool(true)
string(17) "%p %{REQUEST_URI}"
