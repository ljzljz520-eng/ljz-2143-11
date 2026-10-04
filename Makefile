CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -pthread
SRC_DIR := src
TARGET := visual-window-app
SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/window.c $(SRC_DIR)/renderer.c \
           $(SRC_DIR)/text_layer.c $(SRC_DIR)/bg_manager.c \
           $(SRC_DIR)/image_format.c $(SRC_DIR)/sha256.c
TEST_TARGET := tests-unit

# Per-package pkg-config with hard -l fallback so a missing .pc file does
# not silently drop libraries on minimal toolchains.
PKGS := sdl2 SDL2_image SDL2_ttf libcurl json-c
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs $(PKGS) 2>/dev/null || \
              echo "-lSDL2 -lSDL2_image -lSDL2_ttf -lcurl -ljson-c")
LDLIBS := $(PKG_LIBS) -lm

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

# Unit tests for pure, SDL-independent modules (sha256 + sniff).
$(TEST_TARGET): $(SRC_DIR)/tests/unit_main.c $(SRC_DIR)/sha256.c \
                $(SRC_DIR)/image_format.c
	$(CC) $(CFLAGS) -I$(SRC_DIR) $^ -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(SRC_DIR)/*.o
