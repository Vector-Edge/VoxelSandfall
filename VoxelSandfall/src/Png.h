// Tiny dependency-free PNG writer (uncompressed "stored" deflate blocks) for screenshots.
#pragma once
#include <cstdint>

// `rgba` is w*h*4 bytes, top row first. Alpha is dropped (RGB PNG).
bool writePng(const char* path, int w, int h, const uint8_t* rgba);
