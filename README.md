# fpm_proctitle

A PHP extension that automatically sets the process title of **php-fpm** workers while they serve a
request, so `ps`/`top`/`htop` show what each process is doing:

```
$ ps -o pid,args -C php-fpm
    PID COMMAND
   1201 php-fpm: master process (/etc/php/php-fpm.conf)
   1202 php-fpm: pool www (10.0.0.7 -> GET https://example.com:443/api/orders?id=42)
   1203 php-fpm: pool www
```

When the request ends, the title goes back to FPM's original one (`php-fpm: pool www`).

- PHP 7.0 – 8.5 (every minor version from 7.0 to 8.5 is tested in CI with GitHub Actions)
- Linux only
- Only active under the `fpm-fcgi` SAPI. With other SAPIs (CLI, etc.) the extension loads but does
  nothing, so it can be enabled in a shared `php.ini`.

## Installation

### With [PIE](https://php.github.io/pie/)

```sh
pie install mtorromeo/fpm-proctitle
```

PIE builds the extension for the current PHP installation (or the one given with `--with-php-config`),
installs it and enables it. Build tools are required (`phpize`, `make`, a C compiler, `libtool`). In a
Dockerfile based on the official images:

```dockerfile
FROM php:8.4-fpm
COPY --from=ghcr.io/php/pie:bin /pie /usr/bin/pie
RUN apt-get update && apt-get install -y --no-install-recommends libtool unzip \
 && pie install mtorromeo/fpm-proctitle
```

### Manual

```sh
phpize
./configure --enable-fpm-proctitle
make
make test
sudo make install
echo "extension=fpm_proctitle.so" | sudo tee /etc/php/conf.d/fpm_proctitle.ini
```

Loading the extension from `php.ini` or `conf.d` is preferred. Loading it with
`php_admin_value[extension]` in the pool also works, but then it is initialised in the worker. If the
pool uses `chroot` and `/proc` is not accessible, the extension still works, but the title is truncated
to the length of the original one (`php-fpm: pool www`). The `chroot` case is not covered by the tests.

## Configuration

| Directive | Default | Description |
|---|---|---|
| `fpm_proctitle.enabled` | `1` | Enables or disables changing the title |
| `fpm_proctitle.format` | see below | Title format used during a request. An empty string leaves the title unchanged |

Default format:

```ini
fpm_proctitle.format = "php-fpm: pool %p (%{REMOTE_ADDR} -> %{REQUEST_METHOD} %s://%{HTTP_HOST}:%{SERVER_PORT}%{REQUEST_URI})"
```

Both directives are `PHP_INI_ALL`, so they can be set per pool with `php_value[...]` or
`php_admin_value[...]`:

```ini
; /etc/php/php-fpm.d/api.conf
[api]
php_admin_value[fpm_proctitle.format] = "php-fpm: pool %p [%P] %{REQUEST_METHOD} %{HTTP_HOST}%{DOCUMENT_URI}"
```

### Placeholders

| Placeholder | Value |
|---|---|
| `%p` | pool name |
| `%s` | scheme: `REQUEST_SCHEME` if not empty, otherwise `https` if `HTTPS` is not empty or `SERVER_PORT` is `443`, otherwise `http` |
| `%{NAME}` | any FastCGI parameter passed by the web server (`REMOTE_ADDR`, `HTTP_HOST`, `REQUEST_URI`, `DOCUMENT_URI`, `SCRIPT_FILENAME`, `HTTP_USER_AGENT`, …). A missing parameter becomes `-` |
| `%P` | worker pid |
| `%%` | a literal `%` |

Unknown placeholders are copied verbatim. Control characters in values are replaced with `?`.

"Empty" has the same meaning as PHP's `empty()`: a missing parameter, `""` or `"0"`.

## Caveats

- **Privacy.** Process titles are visible to every local user (`ps`). `REQUEST_URI` includes the query
  string, which may contain tokens or personal data. If that is a concern, use `%{DOCUMENT_URI}` or
  `%{SCRIPT_NAME}` instead.
- The maximum title length is the size of the master's original argv+environ area (usually a few
  hundred bytes), capped at 4095 characters: Linux does not show anything longer in
  `/proc/<pid>/cmdline`. Longer titles are truncated.

## How it works

In the master process (`fpm_env_init_main()` in `sapi/fpm/fpm/fpm_env.c`), php-fpm moves `environ` to
the heap and uses the whole contiguous memory area holding `argv[]` and `environ[]` as its title
buffer. Workers inherit this layout through `fork()`. FPM's title functions are not exported by the
binary, so the extension writes directly into the same buffer:

1. **MINIT** (in the master): reads the `arg_start`/`env_end` bounds from `/proc/self/stat` and checks
   that `arg_start` matches `argv[0]`.
2. **First request of each worker**: saves the title set by FPM (`php-fpm: pool NAME`) and derives the
   pool name from it. It then checks that no environment variable still points inside the area; if
   one does, only the `argv` space is used. It never goes past the bytes FPM zeroed after its own
   title: if the master's argv/environ strings were not contiguous (for example because something
   replaced a variable with `setenv()` at startup), FPM's buffer is shorter than the area and the
   strings after it are left alone. A process forked by a worker during a request (`pcntl_fork()`)
   keeps the worker's state.
3. **RINIT**: builds the title by reading the FastCGI parameters through `sapi_module.getenv`, without
   going through `$_SERVER`, and writes it into the buffer.
4. **RSHUTDOWN**: restores the original title.

`phpinfo()` (the `fpm_proctitle` section, from an FPM request) shows which buffer is in use and the
maximum title length.

## Releases

PIE and Packagist take versions from git tags. For a new release, update `PHP_FPM_PROCTITLE_VERSION` in
`php_fpm_proctitle.h` and create the matching tag (e.g. `1.0.0`). The source archive excludes the files
marked `export-ignore` in `.gitattributes`.

## License

MIT, see [LICENSE](LICENSE).

## Tests

```sh
make test                              # phpt (CLI)
tests/integration/run.sh               # local php-fpm + modules/fpm_proctitle.so
tests/integration/run.sh docker php:7.4-fpm
tests/integration/run.sh matrix        # php:7.0, 7.1, 7.4, 8.0, 8.3, 8.5 (set IMAGES="..." to change)
```

The integration tests start php-fpm with temporary pools and send requests with a FastCGI client
written in PHP (`tests/integration/fcgi_client.php`). They check:

- the title during a request: default format, scheme detection, custom formats, missing parameters,
  control characters, very long URIs truncated exactly at the end of the area, and a second pool with
  `php_admin_value[fpm_proctitle.format]`;
- that the title is restored when the request ends;
- that the pool's environment variables stay intact;
- with a second master whose environ is not contiguous (`envgap.c`, loaded with `LD_PRELOAD`), that
  the title stops at the end of FPM's buffer and the strings after it are not overwritten.
