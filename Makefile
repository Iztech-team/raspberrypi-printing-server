# Convenience Makefile that wraps CMake.  Just run `make`.

BUILD_DIR ?= build
JOBS      ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all release debug vendor clean install uninstall run rebuild

all: release

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

release: vendor | $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release ..
	cd $(BUILD_DIR) && $(MAKE) -j$(JOBS)

debug: vendor | $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Debug ..
	cd $(BUILD_DIR) && $(MAKE) -j$(JOBS)

# Download bundled dependencies (mongoose, cJSON, stb_image) on first build.
vendor:
	@test -f vendor/mongoose.c  || ./scripts/fetch-vendor.sh
	@test -f vendor/cJSON.c     || ./scripts/fetch-vendor.sh
	@test -f vendor/stb_image.h || ./scripts/fetch-vendor.sh

rebuild: clean all

clean:
	rm -rf $(BUILD_DIR)

install: release
	cd $(BUILD_DIR) && sudo $(MAKE) install
	sudo systemctl daemon-reload
	@echo
	@echo "Installed. Enable + start with:"
	@echo "   sudo systemctl enable --now printer-server"

uninstall:
	cd $(BUILD_DIR) && sudo $(MAKE) uninstall 2>/dev/null || true
	sudo rm -f /usr/local/bin/printer-server
	sudo rm -f /lib/systemd/system/printer-server.service
	sudo systemctl daemon-reload

run: release
	cd $(BUILD_DIR) && ./printer-server -c ../config.json
