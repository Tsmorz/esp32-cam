#include "storage.h"
#include "options.h"
#include "strbuf.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

static constexpr const char *DIR = "/WCAM";
static constexpr uint32_t SPI_HZ = 16000000; // jumper wires: not the max
static constexpr uint64_t KEEP_FREE = 2 * 1024 * 1024; // "full" below this

struct Entry {
  uint32_t seq, size, time; // time: Unix seconds, 0 = unknown
  FileKind kind;
};

static SPIClass *_spi = nullptr;
static bool _wired = false, _mounted = false;
static uint64_t _total = 0, _used = 0;
static std::vector<Entry> _files; // ascending seq
static uint32_t _next = 1;
static std::mutex _m;

static void name_of(const Entry &e, char *out, size_t n) {
  snprintf(out, n, "%s_%05lu.%s", e.kind == FileKind::Photo ? "IMG" : "VID",
           (unsigned long)e.seq, e.kind == FileKind::Photo ? "JPG" : "MOV");
}

// "IMG_00012.JPG" → seq 12, Photo
static bool parse(const char *name, uint32_t *seq, FileKind *kind) {
  unsigned long s;
  char ext[4];
  int used = 0;
  if (sscanf(name, "IMG_%lu.%3s%n", &s, ext, &used) == 2 && !name[used] &&
      !strcmp(ext, "JPG"))
    *kind = FileKind::Photo;
  else if (sscanf(name, "VID_%lu.%3s%n", &s, ext, &used) == 2 && !name[used] &&
           !strcmp(ext, "MOV"))
    *kind = FileKind::Clip;
  else
    return false;
  *seq = (uint32_t)s;
  return s > 0;
}

bool storage_init() {
  const Options &o = options();
  _wired = o.sd_sck >= 0 && o.sd_miso >= 0 && o.sd_mosi >= 0 && o.sd_cs >= 0;
  if (!_wired)
    return false;
  _spi = new SPIClass(FSPI);
  _spi->begin(o.sd_sck, o.sd_miso, o.sd_mosi, o.sd_cs);
  // A new card is formatted by itself (FAT): no computer needed
  if (!SD.begin(o.sd_cs, *_spi, SPI_HZ, "/sd", 5, true)) {
    Serial.println("[sd] no card answering on the configured pins");
    return false;
  }
  _mounted = true;
  SD.mkdir(DIR);
  File dir = SD.open(DIR);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    Entry e;
    if (!f.isDirectory() && parse(f.name(), &e.seq, &e.kind)) {
      e.size = f.size();
      e.time = (uint32_t)f.getLastWrite();
      _files.push_back(e);
    }
    f.close();
  }
  dir.close();
  std::sort(_files.begin(), _files.end(),
            [](const Entry &a, const Entry &b) { return a.seq < b.seq; });
  if (!_files.empty())
    _next = _files.back().seq + 1;
  _total = SD.totalBytes();
  _used = SD.usedBytes();
  Serial.printf("[sd] %u files, %llu of %llu MB used\n",
                (unsigned)_files.size(), _used >> 20, _total >> 20);
  return true;
}

const char *storage_state() {
  if (!_wired)
    return "none";
  if (!_mounted)
    return "missing";
  return storage_ready() ? "ok" : "full";
}

bool storage_ready() { return _mounted && _total - _used > KEEP_FREE; }
uint64_t storage_total() { return _total; }
uint64_t storage_used() { return _used; }
fs::FS &storage_fs() { return SD; }

uint32_t storage_new_file(FileKind kind, char *path, size_t n) {
  if (!storage_ready())
    return 0;
  std::lock_guard<std::mutex> l(_m);
  Entry e = {_next++, 0, 0, kind};
  char name[24];
  name_of(e, name, sizeof(name));
  snprintf(path, n, "%s/%s", DIR, name);
  return e.seq;
}

void storage_added(uint32_t seq, FileKind kind, uint32_t size) {
  const time_t now = time(nullptr);
  std::lock_guard<std::mutex> l(_m);
  _files.push_back({seq, size, now > 1600000000 ? (uint32_t)now : 0, kind});
  _used += size;
}

size_t storage_list_json(char *buf, size_t size, int offset, int limit) {
  std::lock_guard<std::mutex> l(_m);
  size_t pos = 0;
  appendf(buf, size, pos, "{\"total\":%u,\"files\":[", (unsigned)_files.size());
  int shown = 0;
  for (int i = (int)_files.size() - 1 - offset; i >= 0 && shown < limit;
       i--, shown++) {
    char name[24];
    name_of(_files[i], name, sizeof(name));
    appendf(buf, size, pos, "%s{\"name\":\"%s\",\"size\":%lu,\"time\":%lu}",
            shown ? "," : "", name, (unsigned long)_files[i].size,
            (unsigned long)_files[i].time);
  }
  appendf(buf, size, pos, "]}");
  return pos;
}

bool storage_path(const char *name, char *path, size_t n) {
  uint32_t seq;
  FileKind kind;
  if (!_mounted || !parse(name, &seq, &kind))
    return false;
  snprintf(path, n, "%s/%s", DIR, name);
  return true;
}

bool storage_delete(const char *name) {
  char path[40];
  uint32_t seq;
  FileKind kind;
  if (!storage_path(name, path, sizeof(path)) || !parse(name, &seq, &kind))
    return false;
  std::lock_guard<std::mutex> l(_m);
  auto it = std::find_if(_files.begin(), _files.end(), [&](const Entry &e) {
    return e.seq == seq && e.kind == kind;
  });
  if (it == _files.end() || !SD.remove(path))
    return false;
  _used -= std::min<uint64_t>(_used, it->size);
  _files.erase(it);
  return true;
}

int storage_delete_all() {
  if (!_mounted)
    return 0;
  std::lock_guard<std::mutex> l(_m);
  int n = 0;
  for (const Entry &e : _files) {
    char name[24], path[40];
    name_of(e, name, sizeof(name));
    snprintf(path, sizeof(path), "%s/%s", DIR, name);
    n += SD.remove(path);
  }
  _files.clear();
  _used = SD.usedBytes();
  return n;
}
