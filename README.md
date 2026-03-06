# masking_service

## How it works

**Protocol** — `MaskRequest` / `MaskResponse` structs over a Unix stream socket at `/run/masking_service/masking.sock`.

**Blurring algorithm** (`image_proc.c`):
1. Load the target image (any format stb supports: JPEG, PNG, BMP, etc.)
2. Load the mask as grayscale; nearest-neighbour resize if dimensions differ
3. Compute a **Gaussian-approximated blur** (3 passes of box blur via 2-D integral images — O(w×h) per pass, any radius)
4. Per-pixel lerp: `output = original × (1 − mask/255) + blurred × (mask/255)`
5. Write output (format inferred from extension; defaults to PNG)

**Service** (`main.c`) — fixed worker pool (default 4, configurable via `MASKING_WORKERS` env var) draining a bounded work queue. Logs to syslog.

---

## Quick start

```bash
# 1. Download stb vendor headers
make setup

# 2. Build
make

# 3. Install + enable
# Note: the unit runs as 'default', which is the OctoPrint user on OctoPi.
# If OctoPrint runs as a different user on your system, edit
# User= and Group= in systemd/masking_service.service before installing.
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now masking_service

# 4. Send a request
masking_client photo.jpg mask.png output.png 20
#              ^target    ^mask    ^output     ^blur radius
```

**Mask convention:** white pixels (255) → fully blurred, black (0) → original kept, grey values → partial blend.

> **Warning:** each worker consumes a full CPU core while processing an image. The default of 4 workers is suitable for a desktop/server. On low-powered hardware (e.g. Raspberry Pi 3/4) this will starve OctoPrint and may cause print failures — set `MASKING_WORKERS=1` or `2` via a systemd drop-in and increase only if the hardware can sustain the load.
