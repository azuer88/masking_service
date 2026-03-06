CC       := gcc
CFLAGS   := -Wall -Wextra -Wpedantic -O2 -Iinclude -Ivendor
LDFLAGS  := -lm -lpthread

SRC_DIR    := src
BIN_DIR    := bin
VENDOR_DIR := vendor

SERVICE_SRCS := $(SRC_DIR)/main.c $(SRC_DIR)/image_proc.c
CLIENT_SRCS  := $(SRC_DIR)/client.c

SERVICE_BIN  := $(BIN_DIR)/masking_service
CLIENT_BIN   := $(BIN_DIR)/masking_client

STB_IMAGE       := $(VENDOR_DIR)/stb_image.h
STB_IMAGE_WRITE := $(VENDOR_DIR)/stb_image_write.h

STB_BASE_URL := https://raw.githubusercontent.com/nothings/stb/master

.PHONY: all setup clean install uninstall

all: check-deps $(SERVICE_BIN) $(CLIENT_BIN)

## ── Dependency check ────────────────────────────────────────────────────────

check-deps:
	@if [ ! -f $(STB_IMAGE) ] || [ ! -f $(STB_IMAGE_WRITE) ]; then \
	    echo ""; \
	    echo "ERROR: stb vendor headers are missing."; \
	    echo "Run  'make setup'  to download them, then retry."; \
	    echo ""; \
	    exit 1; \
	fi

## ── Vendor setup ────────────────────────────────────────────────────────────

setup:
	@mkdir -p $(VENDOR_DIR)
	curl -fsSL $(STB_BASE_URL)/stb_image.h       -o $(STB_IMAGE)
	curl -fsSL $(STB_BASE_URL)/stb_image_write.h -o $(STB_IMAGE_WRITE)
	@echo "Vendor headers downloaded to $(VENDOR_DIR)/"

## ── Build targets ───────────────────────────────────────────────────────────

$(BIN_DIR):
	@mkdir -p $@

$(SERVICE_BIN): $(SERVICE_SRCS) include/protocol.h src/image_proc.h \
                $(STB_IMAGE) $(STB_IMAGE_WRITE) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SERVICE_SRCS) -o $@ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_SRCS) include/protocol.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $@

## ── Install / Uninstall ─────────────────────────────────────────────────────

install: all
	install -Dm 755 $(SERVICE_BIN) /usr/local/bin/masking_service
	install -Dm 755 $(CLIENT_BIN)  /usr/local/bin/masking_client
	install -Dm 644 systemd/masking_service.service \
	    /etc/systemd/system/masking_service.service
	@echo ""
	@echo "Installed.  To enable and start the service:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now masking_service"

uninstall:
	systemctl stop masking_service 2>/dev/null || true
	systemctl disable masking_service 2>/dev/null || true
	rm -f /usr/local/bin/masking_service
	rm -f /usr/local/bin/masking_client
	rm -f /etc/systemd/system/masking_service.service
	systemctl daemon-reload

## ── Clean ───────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BIN_DIR)
