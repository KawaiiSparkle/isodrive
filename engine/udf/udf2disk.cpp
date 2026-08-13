/* udf2disk — extract UDF/ISO9660 into a FAT32 or exFAT disk image.
 * If any file is >= 4 GiB, use exFAT; otherwise FAT32.
 * Userspace only (FatFs) — no kernel UDF/exFAT mount required.
 */
#define _FILE_OFFSET_BITS 64
#include "udf.h"
#include "ff.h"
#include "fatimg.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static void die(const char* m) {
  std::fprintf(stderr, "udf2disk: %s\n", m);
}

static std::string parent_of(const std::string& p) {
  auto s = p.rfind('/');
  if (s == std::string::npos || s == 0) return {};
  return p.substr(0, s);
}

static FRESULT ensure_dir(const std::string& path) {
  if (path.empty()) return FR_OK;
  ensure_dir(parent_of(path));
  std::string fp = "/" + path;
  FRESULT r = f_mkdir(fp.c_str());
  if (r == FR_EXIST) return FR_OK;
  return r;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "Usage: udf2disk <input.iso> [output.img]\n"
                 "       udf2disk --list <input.iso>\n"
                 "Extract UDF (or ISO9660/Joliet) into FAT32/exFAT.\n");
    return 1;
  }
  bool list_only = false;
  std::string in, out;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--list" || a == "-l") list_only = true;
    else if (in.empty()) in = a;
    else if (out.empty()) out = a;
  }
  if (in.empty()) return 1;

  UdfImage img;
  if (!img.open(in)) {
    die("cannot open / not UDF or ISO9660");
    return 1;
  }
  std::vector<UdfFile> files;
  if (!img.list(files)) {
    die("failed to list filesystem");
    return 1;
  }
  std::printf("source=%s format=%s files=%zu bytes=%llu max_file=%llu\n",
              in.c_str(), img.is_udf() ? "UDF" : "ISO9660",
              img.file_count(),
              (unsigned long long)img.total_bytes(),
              (unsigned long long)img.max_file_size());

  if (list_only) {
    for (const auto& f : files) {
      std::printf("%s %llu %s\n", f.is_dir ? "D" : "F",
                  (unsigned long long)f.size, f.path.c_str());
    }
    return 0;
  }

  const uint64_t LIM = 4ull * 1024 * 1024 * 1024 - 65536;
  bool use_exfat = img.max_file_size() >= LIM;
  std::printf("target_fs=%s\n", use_exfat ? "exFAT" : "FAT32");

  if (out.empty()) {
    out = in + (use_exfat ? ".exfat.img" : ".fat32.img");
    auto slash = in.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? in : in.substr(slash + 1);
    out = "/data/adb/isodriveplus/images/" + base +
          (use_exfat ? ".exfat.img" : ".fat32.img");
  }

  uint64_t need = img.total_bytes() + img.total_bytes() / 8 + 64ull * 1024 * 1024;
  if (need < 64ull * 1024 * 1024) need = 64ull * 1024 * 1024;
  need = (need + 1024 * 1024 - 1) & ~(1024ull * 1024 - 1);

  /* sparse create */
  int tfd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (tfd < 0) {
    die("cannot create output");
    return 1;
  }
  if (lseek(tfd, (off_t)need - 1, SEEK_SET) < 0 || write(tfd, "", 1) != 1) {
    die("cannot grow output (disk full?)");
    ::close(tfd);
    return 1;
  }
  ::close(tfd);

  if (fatimg_open(out.c_str(), 0) != 0) {
    die("fatimg_open failed");
    return 1;
  }

  uint8_t work[64 * 1024];
  MKFS_PARM opt{};
  opt.fmt = use_exfat ? FM_EXFAT : FM_FAT32;
  opt.n_fat = 1;
  opt.align = 0;
  opt.n_root = 0;
  opt.au_size = 0;
  FRESULT fr = f_mkfs("", &opt, work, sizeof work);
  if (fr != FR_OK) {
    std::fprintf(stderr, "f_mkfs failed (%d)\n", (int)fr);
    fatimg_close();
    return 1;
  }
  FATFS fs;
  fr = f_mount(&fs, "", 1);
  if (fr != FR_OK) {
    std::fprintf(stderr, "f_mount failed (%d)\n", (int)fr);
    fatimg_close();
    return 1;
  }
  f_setlabel(use_exfat ? "VENTOY" : "USBDRIVE");

  size_t done = 0;
  for (const auto& f : files) {
    if (f.path.empty()) continue;
    if (f.is_dir) {
      ensure_dir(f.path);
      continue;
    }
    ensure_dir(parent_of(f.path));
    std::string fp = "/" + f.path;
    FIL fpw;
    fr = f_open(&fpw, fp.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
      std::fprintf(stderr, "skip create %s (%d)\n", f.path.c_str(), (int)fr);
      continue;
    }
    bool ok = img.extract_file(f.path, [&](const void* p, size_t n) {
      UINT bw = 0;
      FRESULT w = f_write(&fpw, p, (UINT)n, &bw);
      return w == FR_OK && bw == n;
    });
    f_close(&fpw);
    done++;
    if ((done % 50) == 0) {
      std::printf("... %zu / %zu files\n", done, img.file_count());
    }
    if (!ok) std::fprintf(stderr, "extract incomplete: %s\n", f.path.c_str());
  }
  f_unmount("");
  fatimg_close();
  std::printf("wrote %s (%llu bytes, %s)\n", out.c_str(),
              (unsigned long long)need, use_exfat ? "exFAT" : "FAT32");
  std::puts(out.c_str());
  return 0;
}
