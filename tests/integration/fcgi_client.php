<?php
/**
 * Minimal FastCGI client (PHP 7.0+ compatible).
 *
 * Usage: php fcgi_client.php HOST:PORT SCRIPT_FILENAME [KEY=VALUE ...]
 * KEY=__UNSET__ removes a default parameter.
 * Prints the response body (headers stripped).
 */

function fcgi_record($type, $id, $content)
{
    $len = strlen($content);
    $pad = (8 - ($len % 8)) % 8;
    return pack('CCnnCx', 1, $type, $id, $len, $pad) . $content . str_repeat("\0", $pad);
}

function fcgi_len($n)
{
    return $n < 128 ? chr($n) : pack('N', $n | 0x80000000);
}

function fcgi_params(array $params)
{
    $out = '';
    foreach ($params as $k => $v) {
        $out .= fcgi_len(strlen($k)) . fcgi_len(strlen($v)) . $k . $v;
    }
    return $out;
}

function fcgi_request($addr, array $params)
{
    $sock = stream_socket_client('tcp://' . $addr, $errno, $errstr, 5);
    if (!$sock) {
        fwrite(STDERR, "connect failed: $errstr\n");
        exit(2);
    }
    stream_set_timeout($sock, 10);

    $id = 1;
    $req = fcgi_record(1, $id, pack('nCx5', 1, 0)); // BEGIN_REQUEST, RESPONDER
    foreach (str_split(fcgi_params($params), 65535) as $chunk) {
        $req .= fcgi_record(4, $id, $chunk);
    }
    $req .= fcgi_record(4, $id, '');
    $req .= fcgi_record(5, $id, ''); // empty STDIN
    fwrite($sock, $req);

    $stdout = '';
    while (true) {
        $hdr = fread($sock, 8);
        if ($hdr === false || strlen($hdr) < 8) {
            break;
        }
        $h = unpack('Cversion/Ctype/nid/nlen/Cpad/Creserved', $hdr);
        $content = '';
        $need = $h['len'] + $h['pad'];
        while (strlen($content) < $need) {
            $chunk = fread($sock, $need - strlen($content));
            if ($chunk === false || $chunk === '') {
                break 2;
            }
            $content .= $chunk;
        }
        $content = substr($content, 0, $h['len']);
        if ($h['type'] == 6) {
            $stdout .= $content;
        } elseif ($h['type'] == 7) {
            fwrite(STDERR, $content);
        } elseif ($h['type'] == 3) {
            break;
        }
    }
    fclose($sock);

    $pos = strpos($stdout, "\r\n\r\n");
    return $pos === false ? $stdout : substr($stdout, $pos + 4);
}

if ($argc < 3) {
    fwrite(STDERR, "usage: php fcgi_client.php HOST:PORT SCRIPT_FILENAME [KEY=VALUE ...]\n");
    exit(1);
}
$script = $argv[2];
$params = array(
    'GATEWAY_INTERFACE' => 'FastCGI/1.0',
    'REQUEST_METHOD'    => 'GET',
    'SCRIPT_FILENAME'   => $script,
    'SCRIPT_NAME'       => '/' . basename($script),
    'REQUEST_URI'       => '/' . basename($script),
    'DOCUMENT_URI'      => '/' . basename($script),
    'QUERY_STRING'      => '',
    'SERVER_SOFTWARE'   => 'fcgi_client',
    'REMOTE_ADDR'       => '127.0.0.1',
    'REMOTE_PORT'       => '50000',
    'SERVER_ADDR'       => '127.0.0.1',
    'SERVER_PORT'       => '80',
    'SERVER_NAME'       => 'localhost',
    'SERVER_PROTOCOL'   => 'HTTP/1.1',
    'HTTP_HOST'         => 'localhost',
);
for ($i = 3; $i < $argc; $i++) {
    $kv = explode('=', $argv[$i], 2);
    if (count($kv) == 2) {
        if ($kv[1] === '__UNSET__') {
            unset($params[$kv[0]]);
        } else {
            $params[$kv[0]] = $kv[1];
        }
    }
}
echo fcgi_request($argv[1], $params);
