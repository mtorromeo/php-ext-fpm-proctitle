# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`fpm_proctitle`: a PHP extension (C, Linux only, PHP 7.0–8.5) that rewrites the process title of php-fpm workers while they serve a request (e.g. `php-fpm: pool www (1.2.3.4 -> GET https://host:443/uri)`) and restores FPM's own title (`php-fpm: pool www`) at request end. The format comes from the `fpm_proctitle.format` ini directive, which supports placeholders (`%p` pool, `%s` scheme, `%{FCGI_PARAM}`, `%P` pid, `%%`). See README.md for user docs.

`php-src/` (PHP source checkout with git tags, e.g. `php-7.0.0`) and `php-pecl-proctitle/` (the old broken extension this replaces) are reference material only. They are gitignored and must not be modified.

## Commands

```sh
phpize && ./configure && make          # build -> modules/fpm_proctitle.so
make test NO_INTERACTION=1             # phpt tests (CLI only)
make test TESTS=tests/001-load.phpt    # single phpt
tests/integration/run.sh               # real php-fpm on the host + ./modules/fpm_proctitle.so
tests/integration/run.sh docker php:7.4-fpm   # build + phpt + integration inside one image
tests/integration/run.sh matrix        # php:7.0/7.1/7.4/8.0/8.3/8.5-fpm (override with IMAGES="...")
```

CI (`.github/workflows/ci.yml`) has three jobs:
- `run.sh docker php:X-fpm` for every minor version from 7.0 to 8.5;
- a ZTS build + phpt run (no php-fpm in the `-zts` images);
- a real `pie install` from a `path` repository. Composer symlinks path repos, so the job uses a writable copy without `.git`.

The phpt tests can only check the CLI (where the extension is a no-op). The real behaviour is covered by the integration tests:
- `run.sh` starts php-fpm from `pool.conf.in`, with two pools: `www` and `api` (the latter has a per-pool `php_admin_value` format).
- `check.php` sends requests through `fcgi_client.php`, a standalone FastCGI client in PHP. Per-request ini overrides go through the `PHP_VALUE` FastCGI param.
- `title.php` reports `/proc/self/cmdline`, `getenv('FOO')` and the size of the argv+environ area.
- New behaviour should get a `check(...)` in `check.php`.
- Test code must stay PHP 7.0-compatible: `title.php` runs inside the old images.

## Packaging (PIE / Packagist)

- `composer.json` declares `type: php-ext` and `php-ext.extension-name: fpm_proctitle`. The name is required because the package name contains a hyphen. It also sets `os-families: ["linux"]`. The `require.php` range must be updated when a new PHP minor is supported.
- Versions come from git tags. Keep `PHP_FPM_PROCTITLE_VERSION` in `php_fpm_proctitle.h` in sync with the tag.
- PIE builds from the git archive. Anything not needed to build goes under `export-ignore` in `.gitattributes`.
- PIE runs plain `./configure`. That works because phpize builds enable the extension without `--enable-fpm-proctitle`, so don't add required configure flags.

## Architecture (fpm_proctitle.c)

- FPM's master (`fpm_env_init_main` in `php-src/sapi/fpm/fpm/fpm_env.c`) relocates `environ` to the heap and uses the whole contiguous argv+environ area, starting at `argv[0]`, as its title buffer. Workers inherit this through fork. `fpm_*` symbols are **not exported** from the php-fpm binary, so the extension writes into that same buffer itself.
- **MINIT** runs in the master *before* `fpm_init()`, so environ is not relocated yet. It only records the bounds from `/proc/self/stat` (fields 48–51, parsed after the last `)`), and only if `arg_start == sapi_module.executable_location`. It is a no-op unless `sapi_module.name == "fpm-fcgi"`.
- **`pt_init_worker`** runs lazily on the first RINIT whenever `getpid()` changes, i.e. after fork. It:
  - saves the idle title and derives the pool name from it (`"php-fpm: pool " NAME`);
  - picks the capacity: the full area; or argv only, if some `environ[i]` still points inside the area; or `strlen` of the current title as a fallback;
  - clamps the capacity to 4095, the most Linux shows in `/proc/<pid>/cmdline`.
- **RINIT** is skipped when `SG(server_context)` is NULL (e.g. `php-fpm -i`). It expands the format into `pt_work`, reading values via `sapi_module.getenv` (no allocation; cast for the const difference between 7.x and 8.x) and replacing control characters with `?`. Then `pt_write` writes it, zero-filling only up to the previous length.
- **RSHUTDOWN** restores the idle title.

## Compatibility notes

- A single source file targets PHP 7.0+. Use `PHP_VERSION_ID` guards: for example, `zend_bool` no longer exists in recent PHP, so the header defines `fpm_proctitle_bool`. Avoid `--EXTENSIONS--` in phpt files, because old `run-tests.php` doesn't support it; use `--SKIPIF--` with `extension_loaded` instead.
- In zsh, `git show "$tag:path"` breaks, because `:s` is a history modifier. Use `"${tag}:path"`.
