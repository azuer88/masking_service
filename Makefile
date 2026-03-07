CC  := gcc
CXX := g++

CFLAGS   := -Wall -Wextra -Wpedantic -O2 -Iinclude -Ivendor
CXXFLAGS := -Wall -Wextra          -O2 -Iinclude -Ivendor

LDFLAGS  := -lm -lpthread

SRC_DIR    := src
OBJ_DIR    := obj
BIN_DIR    := bin
VENDOR_DIR := vendor

SERVICE_BIN := $(BIN_DIR)/masking_service
CLIENT_BIN  := $(BIN_DIR)/masking_client

STB_IMAGE       := $(VENDOR_DIR)/stb_image.h
STB_IMAGE_WRITE := $(VENDOR_DIR)/stb_image_write.h

STB_BASE_URL := https://raw.githubusercontent.com/nothings/stb/master

## ── Optional GPU backends ────────────────────────────────────────────────────
#
#  Both are detected automatically via pkg-config at build time.
#  Set ACL_PREFIX or OCV_PREFIX on the make command line to override the
#  pkg-config search path if headers/libs are in a non-standard location.

ACL_CFLAGS := $(shell pkg-config --cflags arm-compute-library 2>/dev/null)
ACL_LIBS   := $(shell pkg-config --libs   arm-compute-library 2>/dev/null)

OCV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null \
                   || pkg-config --cflags opencv  2>/dev/null)
OCV_LIBS   := $(shell pkg-config --libs   opencv4 2>/dev/null \
                   || pkg-config --libs   opencv  2>/dev/null)

ifneq ($(ACL_LIBS),)
  CXXFLAGS += -DHAVE_ACL $(ACL_CFLAGS)
  LDFLAGS  += $(ACL_LIBS)
  $(info GPU: ARM Compute Library detected)
endif

ifneq ($(OCV_LIBS),)
  CXXFLAGS += -DHAVE_OPENCV $(OCV_CFLAGS)
  LDFLAGS  += $(OCV_LIBS)
  $(info GPU: OpenCV detected)
endif

## ── Object files ─────────────────────────────────────────────────────────────

SERVICE_OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/image_proc.o
CLIENT_OBJS  := $(OBJ_DIR)/client.o

.PHONY: all setup clean install install-bin install-unit uninstall

all: check-deps $(SERVICE_BIN) $(CLIENT_BIN)

## ── Dependency check ─────────────────────────────────────────────────────────

check-deps:
	@if [ ! -f $(STB_IMAGE) ] || [ ! -f $(STB_IMAGE_WRITE) ]; then \
	    echo ""; \
	    echo "ERROR: stb vendor headers are missing."; \
	    echo "Run  'make setup'  to download them, then retry."; \
	    echo ""; \
	    exit 1; \
	fi

## ── Vendor setup ─────────────────────────────────────────────────────────────

setup:
	@mkdir -p $(VENDOR_DIR)
	curl -fsSL $(STB_BASE_URL)/stb_image.h       -o $(STB_IMAGE)
	curl -fsSL $(STB_BASE_URL)/stb_image_write.h -o $(STB_IMAGE_WRITE)
	@echo "Vendor headers downloaded to $(VENDOR_DIR)/"

## ── Compilation ──────────────────────────────────────────────────────────────

$(OBJ_DIR) $(BIN_DIR):
	@mkdir -p $@

# main.c compiled as C
$(OBJ_DIR)/main.o: $(SRC_DIR)/main.c include/protocol.h \
                   $(SRC_DIR)/image_proc.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# image_proc.cpp compiled as C++ (with optional ACL/OpenCV flags)
# -Wno-missing-field-initializers suppresses warnings from vendor stb headers
$(OBJ_DIR)/image_proc.o: $(SRC_DIR)/image_proc.cpp include/protocol.h \
                          $(SRC_DIR)/image_proc.h \
                          $(STB_IMAGE) $(STB_IMAGE_WRITE) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -Wno-missing-field-initializers -c $< -o $@

# client.c compiled as C
$(OBJ_DIR)/client.o: $(SRC_DIR)/client.c include/protocol.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Link with g++ so the C++ runtime (and ACL/OpenCV C++ symbols) resolves
$(SERVICE_BIN): $(SERVICE_OBJS) | $(BIN_DIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_OBJS) | $(BIN_DIR)
	$(CC) $^ -o $@

## ── Install / Uninstall ──────────────────────────────────────────────────────

install: install-bin install-unit
	@echo ""
	@echo "Installed.  To enable and start the service:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now masking_service"

install-bin: all
	install -Dm 755 $(SERVICE_BIN) /usr/local/bin/masking_service
	install -Dm 755 $(CLIENT_BIN)  /usr/local/bin/masking_client
	@echo "Binaries installed.  Restart the service to apply:"
	@echo "  sudo systemctl restart masking_service"

install-unit:
	install -Dm 644 systemd/masking_service.service \
	    /etc/systemd/system/masking_service.service
	@echo "Unit file installed.  Reload systemd to apply:"
	@echo "  sudo systemctl daemon-reload && sudo systemctl restart masking_service"

uninstall:
	systemctl stop masking_service 2>/dev/null || true
	systemctl disable masking_service 2>/dev/null || true
	rm -f /usr/local/bin/masking_service
	rm -f /usr/local/bin/masking_client
	rm -f /etc/systemd/system/masking_service.service
	systemctl daemon-reload

## ── Clean ────────────────────────────────────────────────────────────────────

clean:
	rm -rf $(BIN_DIR) $(OBJ_DIR)
