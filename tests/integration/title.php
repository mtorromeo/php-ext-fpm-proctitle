<?php
header('Content-Type: application/json');
$cmdline = file_get_contents('/proc/self/cmdline');
$nul = strpos($cmdline, "\0");
// capacity of the argv+environ area: env_end - arg_start - 1
$cap = null;
$stat = file_get_contents('/proc/self/stat');
$fields = explode(' ', trim(substr($stat, strrpos($stat, ')') + 2)));
if (count($fields) >= 49) { // fields[0] is field 3
    $cap = (int) $fields[48] - (int) $fields[45] - 1;
}
echo json_encode(array(
    'cap'   => $cap,
    'title' => $nul === false ? $cmdline : substr($cmdline, 0, $nul),
    'foo'   => getenv('FOO'),
    'pid'   => getmypid(),
    'php'   => PHP_VERSION,
    'ext'   => extension_loaded('fpm_proctitle'),
));
