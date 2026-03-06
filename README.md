# masking_service

## How it works

**Protocol** — `MaskRequest` / `MaskResponse` structs over a Unix stream socket at `/run/masking_service/masking.sock`.

**Blurring algorithm** (`image_proc.c`):
1. Load the target image (any format stb supports: JPEG, PNG, BMP, etc.)
2. Load the mask as grayscale; nearest-neighbour resize if dimensions differ
3. Compute a **Gaussian-approximated blur** (3 passes of box blur via 2-D integral images — O(w×h) per pass, any radius)
4. Per-pixel lerp: `output = original × (1 − mask/255) + blurred × (mask/255)`
5. Write output (format inferred from extension; defaults to PNG)

**Service** (`main.c`) — spawns a detached pthread per connection, logs to syslog.

---

## Quick start

```bash
# 1. Download stb vendor headers
make setup

# 2. Build
make

# 3. Install + enable
sudo make install
sudo useradd -r -s /sbin/nologin masking   # required by the unit file
sudo systemctl daemon-reload
sudo systemctl enable --now masking_service

# 4. Send a request
masking_client photo.jpg mask.png output.png 20
#              ^target    ^mask    ^output     ^blur radius
```

**Mask convention:** white pixels (255) → fully blurred, black (0) → original kept, grey values → partial blend.
