/* File-backed FatFs disk (512-byte sectors). */
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE
#include "ff.h"
#include "diskio.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int img_seek(FILE* f, uint64_t off) {
  return lseek(fileno(f), (off_t)off, SEEK_SET) == (off_t)off ? 0 : -1;
}

static FILE* g_img;
static uint64_t g_sectors;
static DSTATUS g_stat = STA_NOINIT;

#ifdef __cplusplus
extern "C" {
#endif
int fatimg_open(const char* path, uint64_t size_bytes) {
  g_img = fopen(path, "r+b");
  if (!g_img) g_img = fopen(path, "w+b");
  if (!g_img) return -1;
  if (size_bytes) {
    if (img_seek(g_img, size_bytes - 1) != 0) { fclose(g_img); g_img = NULL; return -1; }
    fputc(0, g_img);
    fflush(g_img);
  }
  off_t sz = lseek(fileno(g_img), 0, SEEK_END);
  if (sz < 512) { fclose(g_img); g_img = NULL; return -1; }
  g_sectors = (uint64_t)sz / 512;
  g_stat = 0;
  return 0;
}

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
void fatimg_close(void) {
  if (g_img) { fflush(g_img); fclose(g_img); g_img = NULL; }
  g_stat = STA_NOINIT;
}
#ifdef __cplusplus
}
#endif

DSTATUS disk_initialize(BYTE pdrv) {
  (void)pdrv;
  return g_img ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
  (void)pdrv;
  return g_stat;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;
  if (!g_img) return RES_NOTRDY;
  if (img_seek(g_img, (uint64_t)sector * 512) != 0) return RES_ERROR;
  if (fread(buff, 512, count, g_img) != count) return RES_ERROR;
  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;
  if (!g_img) return RES_NOTRDY;
  if (img_seek(g_img, (uint64_t)sector * 512) != 0) return RES_ERROR;
  if (fwrite(buff, 512, count, g_img) != count) return RES_ERROR;
  return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  (void)pdrv;
  switch (cmd) {
    case CTRL_SYNC:
      if (g_img) fflush(g_img);
      return RES_OK;
    case GET_SECTOR_COUNT:
      *(LBA_t*)buff = (LBA_t)g_sectors;
      return RES_OK;
    case GET_BLOCK_SIZE:
      *(DWORD*)buff = 1;
      return RES_OK;
    default:
      return RES_PARERR;
  }
}
