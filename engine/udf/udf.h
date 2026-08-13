#pragma once
/* Read-only UDF (and ISO9660 Joliet fallback) from an .iso/.img file. */
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct UdfFile {
  std::string path;   // UTF-8, uses '/'
  bool is_dir = false;
  uint64_t size = 0;
};

class UdfImage {
public:
  bool open(const std::string& path);
  void close();
  bool ok() const { return fd_ >= 0 && (udf_ok_ || iso_ok_); }
  bool is_udf() const { return udf_ok_; }
  uint32_t block_size() const { return bs_; }

  /* Walk every file/dir. Dirs reported before their children. */
  bool list(std::vector<UdfFile>& out);

  /* Read file contents into dest (must be a file, not dir). */
  bool extract_file(const std::string& utf8_path, const std::function<bool(const void*, size_t)>& sink);

  uint64_t max_file_size() const { return max_file_; }
  uint64_t total_bytes() const { return total_bytes_; }
  size_t file_count() const { return file_count_; }

private:
  int fd_ = -1;
  uint32_t bs_ = 2048;
  bool udf_ok_ = false;
  bool iso_ok_ = false;
  uint32_t part_start_ = 0; /* in blocks */
  uint32_t part_len_ = 0;
  uint32_t root_lbn_ = 0;
  uint16_t root_part_ = 0;
  uint64_t max_file_ = 0;
  uint64_t total_bytes_ = 0;
  size_t file_count_ = 0;

  bool read_bytes(uint64_t off, void* buf, size_t n);
  bool read_block(uint32_t lba, void* buf);
  bool read_part_block(uint32_t lbn, void* buf);

  bool probe_udf();
  bool probe_iso9660();
  bool walk_udf(uint32_t icb_lbn, const std::string& prefix, std::vector<UdfFile>& out);
  bool walk_iso(std::vector<UdfFile>& out);

  struct Extent { uint32_t lbn; uint32_t len; };
  bool load_icb(uint32_t lbn, bool& is_dir, uint64_t& size, std::vector<Extent>& exts);
  bool read_extents(const std::vector<Extent>& exts, uint64_t size,
                    const std::function<bool(const void*, size_t)>& sink);
};
