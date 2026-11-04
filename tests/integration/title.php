<?php
header('Content-Type: application/json');
$cmdline = file_get_contents('/proc/self/cmdline');
$nul = strpos($cmdline, "\0");
// capacity of the argv+environ area: env_end - arg_start - 1
$cap = null;
// gap.conf.in master: end of FPM's own title buffer (right before the
// original PT_GAP string) and whether the strings past it are intact
$gap = null;
$sentinel = null;
$stat = file_get_contents('/proc/self/stat');
$fields = explode(' ', trim(substr($stat, strrpos($stat, ')') + 2)));
if (count($fields) >= 49) { // fields[0] is field 3
    $cap = (int) $fields[48] - (int) $fields[45] - 1;
    $env = (string) @file_get_contents('/proc/self/environ');
    $pos = strpos($env, "\0PT_GAP=original\0");
    if ($pos !== false) {
        $gap = (int) $fields[47] - (int) $fields[45] + $pos;
    }
    $sentinel = strpos($env, "\0PT_SENTINEL=intact\0") !== false;
}
echo json_encode(array(
    'cap'   => $cap,
    'gap'   => $gap,
    'sentinel' => $sentinel,
    'title' => $nul === false ? $cmdline : substr($cmdline, 0, $nul),
    'foo'   => getenv('FOO'),
    'pid'   => getmypid(),
    'php'   => PHP_VERSION,
    'ext'   => extension_loaded('fpm_proctitle'),
));
