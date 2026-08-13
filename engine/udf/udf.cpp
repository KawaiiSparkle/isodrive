#include "udf.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

inline uint16_t r16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t r32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}
inline uint64_t r64(const uint8_t* p) {
  return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

/* OSTA CS0 compressed unicode → UTF-8 */
std::string dstring_to_utf8(const uint8_t* p, size_t n) {
  if (!p || n == 0) return {};
  /* dstring: last byte is length of used bytes */
  size_t used = p[n - 1];
  if (used == 0 || used > n - 1) used = n;
  if (used < 1) return {};
  uint8_t cmp = p[0];
  std::string out;
  if (cmp == 8) {
    for (size_t i = 1; i < used; i++) {
      unsigned char c = p[i];
      if (c < 0x80) out.push_back((char)c);
      else {
        out.push_back((char)(0xC0 | (c >> 6)));
        out.push_back((char)(0x80 | (c & 0x3F)));
      }
    }
  } else if (cmp == 16) {
    for (size_t i = 1; i + 1 < used; i += 2) {
      uint16_t u = (uint16_t)((p[i] << 8) | p[i + 1]); /* OSTA-16 is big-endian */
      if (u < 0x80) out.push_back((char)u);
      else if (u < 0x800) {
        out.push_back((char)(0xC0 | (u >> 6)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      } else {
        out.push_back((char)(0xE0 | (u >> 12)));
        out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      }
    }
  } else {
    /* raw */
    out.assign(reinterpret_cast<const char*>(p), used);
  }
  return out;
}

std::string fid_name(const uint8_t* fid, size_t fid_len, uint8_t l_fi, uint16_t l_iu) {
  if (l_fi == 0) return {};
  const uint8_t* s = fid + 38 + l_iu;
  if ((size_t)(s - fid) + l_fi > fid_len) return {};
  uint8_t cmp = s[0];
  std::string out;
  if (cmp == 8) {
    out.assign(reinterpret_cast<const char*>(s + 1), l_fi - 1);
  } else if (cmp == 16) {
    for (size_t i = 1; i + 1 < l_fi; i += 2) {
      uint16_t u = (uint16_t)((s[i] << 8) | s[i + 1]);
      if (u < 0x80) out.push_back((char)u);
      else if (u < 0x800) {
        out.push_back((char)(0xC0 | (u >> 6)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      } else {
        out.push_back((char)(0xE0 | (u >> 12)));
        out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      }
    }
  } else {
    out.assign(reinterpret_cast<const char*>(s), l_fi);
  }
  return out;
}

} // namespace

bool UdfImage::read_bytes(uint64_t off, void* buf, size_t n) {
  if (fd_ < 0) return false;
  if (lseek(fd_, (off_t)off, SEEK_SET) != (off_t)off) return false;
  uint8_t* p = (uint8_t*)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd_, p + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

bool UdfImage::read_block(uint32_t lba, void* buf) {
  return read_bytes((uint64_t)lba * bs_, buf, bs_);
}

bool UdfImage::read_part_block(uint32_t lbn, void* buf) {
  return read_block(part_start_ + lbn, buf);
}

void UdfImage::close() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  udf_ok_ = iso_ok_ = false;
}

bool UdfImage::open(const std::string& path) {
  close();
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ < 0) return false;
  bs_ = 2048;
  if (probe_udf()) {
    udf_ok_ = true;
    return true;
  }
  bs_ = 2048;
  if (probe_iso9660()) {
    iso_ok_ = true;
    return true;
  }
  close();
  return false;
}

bool UdfImage::probe_udf() {
  /* AVDP typically at sector 256 */
  std::vector<uint8_t> buf(4096);
  const uint32_t try_bs[] = {2048, 512};
  for (uint32_t tbs : try_bs) {
    bs_ = tbs;
    if (!read_bytes(256ull * tbs, buf.data(), 512)) continue;
    if (r16(buf.data()) != 2) continue; /* TAG_AVDP */
    uint32_t mvds_len = r32(buf.data() + 16);
    uint32_t mvds_loc = r32(buf.data() + 20);
    if (mvds_len < 16 || mvds_loc == 0) continue;

    bool have_pd = false, have_lvd = false;
    uint32_t fsd_lbn = 0;
    uint16_t fsd_part = 0;
    uint32_t nsec = mvds_len / tbs;
    if (nsec == 0) nsec = 16;
    for (uint32_t i = 0; i < nsec + 4; i++) {
      if (!read_block(mvds_loc + i, buf.data())) break;
      uint16_t tag = r16(buf.data());
      if (tag == 8) break; /* Terminating */
      if (tag == 5) {      /* Partition Descriptor */
        part_start_ = r32(buf.data() + 188);
        part_len_ = r32(buf.data() + 192);
        have_pd = true;
      } else if (tag == 6) { /* Logical Volume Descriptor */
        uint32_t lbs = r32(buf.data() + 212);
        if (lbs == 512 || lbs == 1024 || lbs == 2048) bs_ = lbs;
        /* LogicalVolumeContentsUse: long_ad of FSD at offset 248 */
        fsd_lbn = r32(buf.data() + 248 + 4); /* lb_addr.lbn */
        fsd_part = r16(buf.data() + 248 + 8);
        have_lvd = true;
      }
    }
    if (!have_pd || !have_lvd) continue;

    /* File Set Descriptor */
    if (!read_part_block(fsd_lbn, buf.data())) continue;
    if (r16(buf.data()) != 256) {
      /* try absolute */
      if (!read_block(part_start_ + fsd_lbn, buf.data())) continue;
      if (r16(buf.data()) != 256) continue;
    }
    root_lbn_ = r32(buf.data() + 400 + 4);
    root_part_ = r16(buf.data() + 400 + 8);
    (void)fsd_part;
    return true;
  }
  return false;
}

bool UdfImage::load_icb(uint32_t lbn, bool& is_dir, uint64_t& size, std::vector<Extent>& exts) {
  std::vector<uint8_t> buf(bs_);
  if (!read_part_block(lbn, buf.data())) return false;
  uint16_t tag = r16(buf.data());
  size_t fe_off = 0;
  if (tag == 261) { /* File Entry */
    uint32_t l_ea = r32(buf.data() + 168);
    uint32_t l_ad = r32(buf.data() + 172);
    size = r64(buf.data() + 56);
    uint8_t ftype = buf[176 + 1]; /* icbtag FileType at 16+1? ICBTAG at offset 16 */
    /* ICBTAG: 16 bytes at offset 16; FileType is byte 11 of ICBTAG → offset 27 */
    ftype = buf[27];
    is_dir = (ftype == 4);
    fe_off = 176 + l_ea;
    const uint8_t* ad = buf.data() + fe_off;
    uint8_t flags = buf[16 + 1] & 7; /* Flags in ICBTAG low 3 bits = alloc desc type? */
    /* ICBTAG Flags at offset 18 (uint16): bits 0-2 = ad type */
    flags = r16(buf.data() + 18) & 7;
    if (flags == 3) {
      /* in-ICB data */
      exts.clear();
      return true;
    }
    size_t step = (flags == 0) ? 8 : 16;
    for (uint32_t o = 0; o + step <= l_ad; o += step) {
      uint32_t len = r32(ad + o) & 0x3FFFFFFF;
      uint32_t loc = (step == 8) ? r32(ad + o + 4) : r32(ad + o + 4);
      if (len == 0) continue;
      exts.push_back({loc, len});
    }
    return true;
  }
  if (tag == 266) { /* Extended File Entry */
    uint32_t l_ea = r32(buf.data() + 208);
    uint32_t l_ad = r32(buf.data() + 212);
    size = r64(buf.data() + 56);
    uint8_t ftype = buf[27];
    is_dir = (ftype == 4);
    uint16_t flags = r16(buf.data() + 18) & 7;
    if (flags == 3) {
      exts.clear();
      return true;
    }
    const uint8_t* ad = buf.data() + 216 + l_ea;
    size_t step = (flags == 0) ? 8 : 16;
    for (uint32_t o = 0; o + step <= l_ad; o += step) {
      uint32_t len = r32(ad + o) & 0x3FFFFFFF;
      uint32_t loc = r32(ad + o + 4);
      if (len) exts.push_back({loc, len});
    }
    return true;
  }
  return false;
}

bool UdfImage::read_extents(const std::vector<Extent>& exts, uint64_t size,
                            const std::function<bool(const void*, size_t)>& sink) {
  uint64_t left = size;
  std::vector<uint8_t> buf(bs_);
  for (const auto& e : exts) {
    uint32_t remain = e.len;
    uint32_t lbn = e.lbn;
    while (remain && left) {
      if (!read_part_block(lbn, buf.data())) return false;
      size_t chunk = bs_;
      if (chunk > remain) chunk = remain;
      if (chunk > left) chunk = (size_t)left;
      if (!sink(buf.data(), chunk)) return false;
      remain -= (uint32_t)chunk;
      left -= chunk;
      lbn++;
    }
  }
  return true;
}

bool UdfImage::walk_udf(uint32_t icb_lbn, const std::string& prefix, std::vector<UdfFile>& out) {
  bool is_dir = false;
  uint64_t sz = 0;
  std::vector<Extent> exts;
  if (!load_icb(icb_lbn, is_dir, sz, exts)) return false;
  if (!is_dir) {
    UdfFile f;
    f.path = prefix;
    f.is_dir = false;
    f.size = sz;
    out.push_back(f);
    if (sz > max_file_) max_file_ = sz;
    total_bytes_ += sz;
    file_count_++;
    return true;
  }
  if (!prefix.empty()) {
    UdfFile d;
    d.path = prefix;
    d.is_dir = true;
    out.push_back(d);
  }
  /* slurp directory data */
  std::vector<uint8_t> data;
  data.reserve((size_t)std::min<uint64_t>(sz, 64 * 1024 * 1024));
  if (exts.empty() && sz) {
    /* in-ICB: re-read FE and copy tail — skip for simplicity if empty extents + size */
  }
  if (!read_extents(exts, sz, [&](const void* p, size_t n) {
        data.insert(data.end(), (const uint8_t*)p, (const uint8_t*)p + n);
        return true;
      }))
    return false;

  size_t off = 0;
  while (off + 38 <= data.size()) {
    const uint8_t* fid = data.data() + off;
    if (r16(fid) != 257) {
      /* padding */
      if (fid[0] == 0) { off++; continue; }
      break;
    }
    uint8_t fileChar = fid[18];
    uint8_t l_fi = fid[19];
    uint16_t l_iu = r16(fid + 36);
    uint32_t icb = r32(fid + 20 + 4);
    size_t rec = 38 + l_iu + l_fi;
    rec = (rec + 3) & ~size_t(3);
    if (off + rec > data.size()) break;
    bool parent = (fileChar & 0x08) != 0;
    bool deleted = (fileChar & 0x04) != 0;
    if (!parent && !deleted && l_fi) {
      std::string name = fid_name(fid, rec, l_fi, l_iu);
      if (!name.empty() && name != "." && name != "..") {
        std::string child = prefix.empty() ? name : (prefix + "/" + name);
        walk_udf(icb, child, out);
      }
    }
    off += rec;
  }
  return true;
}

/* --- ISO9660 / Joliet fallback (PVD + SVD escape %/@) --- */
bool UdfImage::probe_iso9660() {
  uint8_t sec[2048];
  if (!read_bytes(16 * 2048, sec, 2048)) return false;
  return sec[0] == 1 && memcmp(sec + 1, "CD001", 5) == 0;
}

static void iso_name(const uint8_t* p, int len, bool joliet, std::string& out) {
  out.clear();
  if (joliet) {
    for (int i = 0; i + 1 < len; i += 2) {
      uint16_t u = (uint16_t)((p[i] << 8) | p[i + 1]);
      if (u == 0 || u == ';') break;
      if (u < 0x80) out.push_back((char)u);
      else if (u < 0x800) {
        out.push_back((char)(0xC0 | (u >> 6)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      } else {
        out.push_back((char)(0xE0 | (u >> 12)));
        out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      }
    }
  } else {
    for (int i = 0; i < len; i++) {
      char c = (char)p[i];
      if (c == ';' || c == 0) break;
      if (c == '.') {
        /* strip trailing empty extension later */
      }
      out.push_back(c);
    }
    if (out.size() >= 2 && out[out.size() - 2] == '.' && out.back() == ';')
      out.resize(out.size() - 2);
    auto sc = out.find(';');
    if (sc != std::string::npos) out.resize(sc);
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
}

bool UdfImage::walk_iso(std::vector<UdfFile>& out) {
  uint8_t sec[2048];
  uint32_t root_lba = 0, root_len = 0;
  bool joliet = false;
  for (int s = 16; s < 32; s++) {
    if (!read_bytes((uint64_t)s * 2048, sec, 2048)) break;
    if (sec[0] == 255) break;
    if (memcmp(sec + 1, "CD001", 5) != 0) continue;
    if (sec[0] == 2 && sec[88] == 0x25 && sec[89] == 0x2F &&
        (sec[90] == 0x40 || sec[90] == 0x43 || sec[90] == 0x45)) {
      joliet = true;
      root_lba = sec[158] | (sec[159] << 8) | (sec[160] << 16) | (sec[161] << 24);
      root_len = sec[166] | (sec[167] << 8) | (sec[168] << 16) | (sec[169] << 24);
    } else if (sec[0] == 1 && root_lba == 0) {
      root_lba = sec[158] | (sec[159] << 8) | (sec[160] << 16) | (sec[161] << 24);
      root_len = sec[166] | (sec[167] << 8) | (sec[168] << 16) | (sec[169] << 24);
    }
  }
  if (!root_lba) return false;

  struct Item { uint32_t lba, len; bool dir; std::string path; };
  std::vector<Item> q;
  q.push_back({root_lba, root_len, true, ""});
  std::vector<uint8_t> dirbuf;
  while (!q.empty()) {
    Item it = q.back();
    q.pop_back();
    if (it.dir && !it.path.empty()) {
      UdfFile d;
      d.path = it.path;
      d.is_dir = true;
      out.push_back(d);
    }
    dirbuf.resize(it.len);
    if (!read_bytes((uint64_t)it.lba * 2048, dirbuf.data(), it.len)) continue;
    size_t o = 0;
    while (o < dirbuf.size()) {
      uint8_t rec = dirbuf[o];
      if (rec == 0) {
        o = (o / 2048 + 1) * 2048;
        continue;
      }
      if (o + rec > dirbuf.size()) break;
      uint32_t lba = dirbuf[o + 2] | (dirbuf[o + 3] << 8) | (dirbuf[o + 4] << 16) | (dirbuf[o + 5] << 24);
      uint32_t len = dirbuf[o + 10] | (dirbuf[o + 11] << 8) | (dirbuf[o + 12] << 16) | (dirbuf[o + 13] << 24);
      uint8_t flags = dirbuf[o + 25];
      uint8_t nl = dirbuf[o + 32];
      std::string name;
      iso_name(&dirbuf[o + 33], nl, joliet, name);
      bool isdir = (flags & 2) != 0;
      if (!(nl == 1 && (dirbuf[o + 33] == 0 || dirbuf[o + 33] == 1)) && !name.empty()) {
        std::string child = it.path.empty() ? name : (it.path + "/" + name);
        if (isdir) q.push_back({lba, len, true, child});
        else {
          UdfFile f;
          f.path = child;
          f.size = len;
          out.push_back(f);
          if (len > max_file_) max_file_ = len;
          total_bytes_ += len;
          file_count_++;
        }
      }
      o += rec;
    }
  }
  return true;
}

bool UdfImage::list(std::vector<UdfFile>& out) {
  out.clear();
  max_file_ = total_bytes_ = 0;
  file_count_ = 0;
  if (udf_ok_) return walk_udf(root_lbn_, "", out);
  if (iso_ok_) return walk_iso(out);
  return false;
}

bool UdfImage::extract_file(const std::string& utf8_path,
                            const std::function<bool(const void*, size_t)>& sink) {
  std::vector<UdfFile> all;
  /* Locate by walking again — cheap enough vs ISO size. */
  if (udf_ok_) {
    /* DFS search */
    struct St { uint32_t icb; std::string path; };
    std::vector<St> st;
    st.push_back({root_lbn_, ""});
    std::vector<uint8_t> data;
    while (!st.empty()) {
      St cur = st.back();
      st.pop_back();
      bool is_dir = false;
      uint64_t sz = 0;
      std::vector<Extent> exts;
      if (!load_icb(cur.icb, is_dir, sz, exts)) continue;
      if (!is_dir) {
        if (cur.path == utf8_path) return read_extents(exts, sz, sink);
        continue;
      }
      data.clear();
      if (!read_extents(exts, sz, [&](const void* p, size_t n) {
            data.insert(data.end(), (const uint8_t*)p, (const uint8_t*)p + n);
            return true;
          }))
        continue;
      size_t off = 0;
      while (off + 38 <= data.size()) {
        const uint8_t* fid = data.data() + off;
        if (r16(fid) != 257) {
          if (fid[0] == 0) { off++; continue; }
          break;
        }
        uint8_t fileChar = fid[18];
        uint8_t l_fi = fid[19];
        uint16_t l_iu = r16(fid + 36);
        uint32_t icb = r32(fid + 20 + 4);
        size_t rec = (38 + l_iu + l_fi + 3) & ~size_t(3);
        if (off + rec > data.size()) break;
        if (!(fileChar & 0x08) && !(fileChar & 0x04) && l_fi) {
          std::string name = fid_name(fid, rec, l_fi, l_iu);
          if (!name.empty()) {
            std::string child = cur.path.empty() ? name : (cur.path + "/" + name);
            st.push_back({icb, child});
          }
        }
        off += rec;
      }
    }
    return false;
  }
  /* ISO9660 extract */
  uint8_t sec[2048];
  uint32_t root_lba = 0, root_len = 0;
  bool joliet = false;
  for (int s = 16; s < 32; s++) {
    if (!read_bytes((uint64_t)s * 2048, sec, 2048)) break;
    if (sec[0] == 255) break;
    if (memcmp(sec + 1, "CD001", 5) != 0) continue;
    if (sec[0] == 2 && sec[88] == 0x25) {
      joliet = true;
      root_lba = sec[158] | (sec[159] << 8) | (sec[160] << 16) | (sec[161] << 24);
      root_len = sec[166] | (sec[167] << 8) | (sec[168] << 16) | (sec[169] << 24);
    } else if (sec[0] == 1 && !root_lba) {
      root_lba = sec[158] | (sec[159] << 8) | (sec[160] << 16) | (sec[161] << 24);
      root_len = sec[166] | (sec[167] << 8) | (sec[168] << 16) | (sec[169] << 24);
    }
  }
  struct Item { uint32_t lba, len; bool dir; std::string path; };
  std::vector<Item> q{{root_lba, root_len, true, ""}};
  std::vector<uint8_t> dirbuf;
  while (!q.empty()) {
    Item it = q.back();
    q.pop_back();
    dirbuf.resize(it.len);
    if (!read_bytes((uint64_t)it.lba * 2048, dirbuf.data(), it.len)) continue;
    size_t o = 0;
    while (o < dirbuf.size()) {
      uint8_t rec = dirbuf[o];
      if (!rec) { o = (o / 2048 + 1) * 2048; continue; }
      if (o + rec > dirbuf.size()) break;
      uint32_t lba = dirbuf[o + 2] | (dirbuf[o + 3] << 8) | (dirbuf[o + 4] << 16) | (dirbuf[o + 5] << 24);
      uint32_t len = dirbuf[o + 10] | (dirbuf[o + 11] << 8) | (dirbuf[o + 12] << 16) | (dirbuf[o + 13] << 24);
      uint8_t flags = dirbuf[o + 25];
      uint8_t nl = dirbuf[o + 32];
      std::string name;
      iso_name(&dirbuf[o + 33], nl, joliet, name);
      bool isdir = (flags & 2) != 0;
      if (!(nl == 1 && (dirbuf[o + 33] == 0 || dirbuf[o + 33] == 1)) && !name.empty()) {
        std::string child = it.path.empty() ? name : (it.path + "/" + name);
        if (isdir) q.push_back({lba, len, true, child});
        else if (child == utf8_path) {
          uint64_t left = len;
          uint32_t sl = lba;
          while (left) {
            if (!read_bytes((uint64_t)sl * 2048, sec, 2048)) return false;
            size_t n = 2048;
            if (n > left) n = (size_t)left;
            if (!sink(sec, n)) return false;
            left -= n;
            sl++;
          }
          return true;
        }
      }
      o += rec;
    }
  }
  return false;
}
