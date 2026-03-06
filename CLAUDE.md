# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build commands

```bash
make setup    # download stb vendor headers (first-time only, requires curl)
make          # build bin/masking_service and bin/masking_client
make clean    # remove bin/
sudo make install    # install to /usr/local/bin + /etc/systemd/system/
sudo make uninstall
```

The build requires no external libraries beyond the C standard library — image I/O is handled by the single-header `vendor/stb_image.h` and `vendor/stb_image_write.h` (downloaded by `make setup`).  Link flags: `-lm -lpthread`.

## Architecture

The project is split into three compilation units:

- **`src/main.c`** — daemon entry point.  Creates the Unix stream socket at `SOCKET_PATH`, then enters an accept loop that pushes client fds onto a bounded work queue.  Worker count is read from the `MASKING_WORKERS` environment variable at startup (default `4`, max `64`); set it in the systemd unit or a drop-in.  The queue depth is a compile-time `QUEUE_SIZE=64`; when full the accept loop blocks, providing natural back-pressure.  Shutdown broadcasts on both condvars, lets workers drain remaining work, then joins all threads.  Logs to syslog (`LOG_DAEMON`).
- **`src/image_proc.c`** — all image processing.  Exposes one public function: `apply_mask_blur()`.  Internally implements a 2-D integral-image box blur (O(w×h) per pass) applied three times to approximate a Gaussian, then lerps between the original and blurred pixel using the mask intensity as alpha.  Mask is resampled to the target's dimensions via nearest-neighbour if they differ.
- **`src/client.c`** — standalone CLI that connects to the socket, sends a `MaskRequest`, and prints the `MaskResponse`.  Not linked into the service.

## IPC protocol

Defined entirely in **`include/protocol.h`**:

- `MaskRequest` — three `PATH_MAX`-length path strings (target, mask, output) plus an `int blur_radius`.  Sent as a single `sizeof(MaskRequest)` binary write.
- `MaskResponse` — `int status` (0 = success, negative `ERR_*` code on failure) plus a 512-byte human-readable message.

The socket lives at `/run/masking_service/masking.sock`.  The systemd unit creates `/run/masking_service/` via `RuntimeDirectory=masking_service`; when running outside systemd, the daemon calls `mkdir(SOCKET_DIR)` itself.

## Mask convention

White (255) pixels in the mask map to fully blurred output; black (0) pixels preserve the original. Grey values produce a proportional blend. The mask is always loaded as 1-channel grayscale regardless of its actual format.

## systemd unit notes

The unit runs as `User=default` / `Group=default` — the same user as OctoPrint/Octolapse — so it has write access to all snapshot paths and `/tmp` directories those processes create. No extra system user is needed.

`ProtectSystem=strict` makes the filesystem read-only by default; `/tmp` is explicitly listed in `ReadWritePaths`. For paths outside `/tmp` (e.g. `/home/default/octolapse`), add them via a drop-in:

```bash
sudo systemctl edit masking_service
```
```ini
[Service]
ReadWritePaths=/home/default/octolapse
```

To change the worker count:
```ini
[Service]
Environment=MASKING_WORKERS=8
```
