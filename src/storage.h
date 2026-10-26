#pragma once
#include <FS.h>
#include <stddef.h>
#include <stdint.h>

// Photos and clips on a memory card (e.g. Adafruit's XTSD SD-NAND breakout)
// on SPI, wired to the pins in options(). Files live in /WCAM as
// IMG_00001.JPG and VID_00002.MOV, numbered in one sequence. The list is
// kept in RAM, so the dashboard's gallery doesn't walk the card each time.
//
// Not encrypted (the user's choice): photos are private through the web
// server (password, not over the hotspot), not on the card itself.

enum class FileKind : uint8_t { Photo = 'I', Clip = 'V' };

bool storage_init();         // after options_init(); mounts if wired
const char *storage_state(); // "none" (not wired), "missing", "ok" or "full"
bool storage_ready();        // mounted with room for more
uint64_t storage_total();
uint64_t storage_used();

// A new file to write: fills path ("/WCAM/IMG_00012.JPG"), returns its
// sequence number (0 if not ready). Call storage_added() once it's complete.
uint32_t storage_new_file(FileKind kind, char *path, size_t n);
void storage_added(uint32_t seq, FileKind kind, uint32_t size);
fs::FS &storage_fs();

// Gallery, newest first: {"total":N,"files":[{"name","size","time"}]}
size_t storage_list_json(char *buf, size_t size, int offset, int limit);
// name = "IMG_00012.JPG"; false if it isn't one of ours
bool storage_path(const char *name, char *path, size_t n);
bool storage_delete(const char *name);
int storage_delete_all();
