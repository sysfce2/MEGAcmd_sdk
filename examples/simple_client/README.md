# simple_client

C++ console app using the public API of MEGA SDK ([include/megaapi.h](../../include/megaapi.h)).

This example logs in to your MEGA account, gets your MEGA filesystem, shows the files/folders in your root folder,
uploads an image and optionally starts a local HTTP proxy server to browse you MEGA account.

Optionally in [simple_client.cpp](simple_client.cpp), more MegaApi calls can be added after the comment `// Add code here to exercise MegaApi`.

## Credentials

Credentials are read from environment variables. Set `MEGA_EMAIL` and `MEGA_PWD` before running:

```
MEGA_EMAIL=you@example.com MEGA_PWD='secret' ./simple_client
```

## Logging

SDK logs are written to `./simple_client.log` by default, in append mode, and are also echoed to stdout. Override the file path with the `MEGA_LOG_FILE` environment variable:

```
MEGA_LOG_FILE=/tmp/streaming-test.log ./simple_client
```

Disable the stdout tee with `MEGA_LOG_STDOUT=0` (also accepts `false`):

```
MEGA_LOG_STDOUT=0 ./simple_client
```
