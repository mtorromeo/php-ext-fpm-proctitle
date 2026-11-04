<?php
/**
 * Integration checks against a running php-fpm with fpm_proctitle loaded.
 *
 * Usage: php check.php HOST:PORT SCRIPT_FILENAME HOST:PORT_API HOST:PORT_GAP [CMDLINE_READER...]
 *
 * SCRIPT_FILENAME is the path of title.php as seen by php-fpm.
 * HOST:PORT_GAP is a second master started with envgap.so (see gap.conf.in).
 * CMDLINE_READER is an optional command prefix used to read
 * /proc/<pid>/cmdline of the worker (e.g. "docker exec CONTAINER").
 */

$addr   = $argv[1];
$script = $argv[2];
$addr2  = $argv[3];
$addr3  = $argv[4];
$reader = array_slice($argv, 5);
$client = __DIR__ . '/fcgi_client.php';

$failures = 0;
$checks = 0;

function req(array $params = array(), $to = null)
{
    global $addr, $script, $client;
    if ($to === null) {
        $to = $addr;
    }
    $cmd = escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg($client) . ' '
        . escapeshellarg($to) . ' ' . escapeshellarg($script);
    foreach ($params as $k => $v) {
        $cmd .= ' ' . escapeshellarg("$k=$v");
    }
    $out = shell_exec($cmd);
    $data = json_decode((string) $out, true);
    if (!is_array($data)) {
        fwrite(STDERR, "invalid response: " . var_export($out, true) . "\n");
        return array('title' => null, 'foo' => null, 'pid' => 0);
    }
    return $data;
}

function cmdline($pid)
{
    global $reader;
    if ($reader) {
        $cmd = implode(' ', array_map('escapeshellarg', $reader)) . ' cat /proc/' . (int) $pid . '/cmdline';
        $raw = (string) shell_exec($cmd);
    } else {
        $raw = (string) @file_get_contents('/proc/' . (int) $pid . '/cmdline');
    }
    $nul = strpos($raw, "\0");
    return $nul === false ? $raw : substr($raw, 0, $nul);
}

function check($name, $expected, $actual)
{
    global $failures, $checks;
    $checks++;
    if ($expected === $actual) {
        echo "ok   $name\n";
    } else {
        $failures++;
        echo "FAIL $name\n     expected: " . var_export($expected, true) . "\n     actual:   " . var_export($actual, true) . "\n";
    }
}

$base = 'php-fpm: pool www (127.0.0.1 -> GET %s://localhost:%s/title.php)';

// 1. default format
$r = req();
echo "# PHP {$r['php']}\n";
check('extension loaded', true, $r['ext']);
check('default format', sprintf($base, 'http', '80'), $r['title']);
check('environ preserved', 'bar', $r['foo']);
usleep(100000);
check('idle title restored', 'php-fpm: pool www', cmdline($r['pid']));

// 2. scheme detection (same rules as the original sprintf snippet)
$r = req(array('HTTPS' => 'on'));
check('scheme HTTPS=on', sprintf($base, 'https', '80'), $r['title']);
$r = req(array('HTTPS' => '0'));
check('scheme HTTPS=0 is empty()', sprintf($base, 'http', '80'), $r['title']);
$r = req(array('SERVER_PORT' => '443'));
check('scheme SERVER_PORT=443', sprintf($base, 'https', '443'), $r['title']);
$r = req(array('REQUEST_SCHEME' => 'gopher', 'HTTPS' => 'on'));
check('scheme REQUEST_SCHEME wins', sprintf($base, 'gopher', '80'), $r['title']);

// 3. missing parameters and control characters
$r = req(array('HTTP_HOST' => '__UNSET__'));
check('missing param -> "-"', 'php-fpm: pool www (127.0.0.1 -> GET http://-:80/title.php)', $r['title']);
$r = req(array('HTTP_HOST' => "evil\nhost\x01\x7f"));
check('control chars replaced', 'php-fpm: pool www (127.0.0.1 -> GET http://evil?host??:80/title.php)', $r['title']);

// 4. very long URI: truncated, no corruption of environ
$long = '/title.php?' . str_repeat('x', 5000);
$r = req(array('REQUEST_URI' => $long));
$prefix = 'php-fpm: pool www (127.0.0.1 -> GET http://localhost:80/title.php?xxxx';
check('long uri: prefix kept', true, is_string($r['title']) && strpos($r['title'], $prefix) === 0);
check('long uri: truncated', true, is_string($r['title']) && strlen($r['title']) < strlen($long));
check('long uri: environ preserved', 'bar', $r['foo']);
$expected_len = min((int) $r['cap'], 4095);
echo "     (area: {$r['cap']} bytes, title: " . strlen((string) $r['title']) . " bytes)\n";
check('long uri: truncated exactly at the area end', $expected_len, strlen((string) $r['title']));
usleep(100000);
check('long uri: idle title restored', 'php-fpm: pool www', cmdline($r['pid']));
$r = req();
check('after long uri: default again', sprintf($base, 'http', '80'), $r['title']);

// 5. custom format through PHP_VALUE
$r = req(array('PHP_VALUE' => 'fpm_proctitle.format="[%P] %p %{DOCUMENT_URI} %% %x %{MISSING} %{ %"'));
check('custom format', '[' . $r['pid'] . '] www /title.php % %x - %{ %', $r['title']);
$r = req(array('PHP_VALUE' => 'fpm_proctitle.format=""'));
check('empty format: title untouched', 'php-fpm: pool www', $r['title']);
$r = req(array('PHP_VALUE' => 'fpm_proctitle.enabled=0'));
check('disabled', 'php-fpm: pool www', $r['title']);

// 6. per-pool format from php_admin_value, pool name other than "www"
$r = req(array(), $addr2);
check('pool api: php_admin_value format', 'api|/title.php', $r['title']);
check('pool api: env', 'baz', $r['foo']);
usleep(100000);
check('pool api: idle title restored', 'php-fpm: pool api', cmdline($r['pid']));

// 7. environ not contiguous in the master: stay inside FPM's own buffer
$r = req(array('REQUEST_URI' => $long), $addr3);
echo "     (area: {$r['cap']} bytes, FPM buffer: " . var_export($r['gap'], true) . " bytes, title: " . strlen((string) $r['title']) . " bytes)\n";
check('env gap: area larger than FPM buffer', true, $r['gap'] !== null && $r['gap'] < $r['cap']);
check('env gap: strings past the gap intact', true, $r['sentinel']);
check('env gap: truncated at the end of FPM buffer', $r['gap'], strlen((string) $r['title']));
usleep(100000);
check('env gap: idle title restored', 'php-fpm: pool gap', cmdline($r['pid']));

echo "\n$checks checks, $failures failures\n";
exit($failures ? 1 : 0);
