/*
  usbstick.cpp - the USB stick on the USB-A port (M4 core)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The USB-A port is a USB host (OTG_HS with its full-speed PHY, VBUS switched
  by PA15), run here on the M4 so that Grbl on the M7 never sees a USB host
  interrupt. Everything happens in one thread of its own, above the display's
  priority (it mostly waits for USB transfers):

    - waits for a stick, mounts its file system (FAT16, FAT32 or exFAT, with
      an MBR or a GPT partition table - FatFs R0.15 of src/m4/fatfs, as
      mbed's own FatFs knows neither exFAT nor GPT), lists the G-code files
      of the root directory into the shared block (grbl_job.h);
    - on a request from the M7 opens a file and keeps the shared ring filled
      with it, in whole sectors where it can, until the end of the file;
    - notices the stick going away, unmounts, and tells the M7 if a file was
      being read.

  The M7 decides everything else (src/job.c). Files from the PC ($FW, job.c)
  come the other way through the same ring and are written to the stick:
  first under a temporary name, renamed when complete (an interrupted upload
  leaves the old file as it was), dated with the date the PC gives - the
  PC file's - so that a later sync can tell which files have changed.
*/

#if defined(CORE_CM4)

#include <Arduino.h>
#include <mbed.h>
#include <string.h>
#include <ctype.h>
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "usbhost/USBHostMSD.h"
#include "../grbl_job.h"
#include "usbstick.h"

static FATFS fat_fs;
// Drive 0 is the stick; partition 0 = the first one with a file system (or
// none), 1 while format_stick() formats the partition it has made.
extern "C" { PARTITION VolToPart[FF_VOLUMES] = { { 0, 0 } }; }
static USBHostMSD *msd = nullptr;
static bool disk_ok = false;             // the stick answered its init
static rtos::Thread *usb_thread = nullptr;
static bool mounted = false;

// The file being streamed.
static FIL cur_file;
static bool streaming = false;
static uint32_t open_ms = 0;
static uint8_t req_seen = 0;

// Upload to the stick: written under UP_TMP, renamed when complete.
#define UP_TMP  "_upload.tmp"
static FIL up_file;
static bool uploading = false;
static char up_name[GJOB_NAME_LEN] = "";     // the name it takes when complete
static bool up_open = false;             // up_file is open
static bool up_failed = false;           // a write failed: the rest is dropped
static uint8_t up_buf[4096];             // whole sectors for FatFs
static uint32_t up_fill = 0;

// The date FatFs stamps on what it writes: the M4 has no clock that knows
// the date, so it is the last date the PC gave (a file's), and an uploaded
// file gets its own date afterwards with f_utime().
static DWORD fat_now = ((DWORD)(2025 - 1980) << 25) | (1UL << 21) | (1UL << 16);


// ---------------------------------------------------------------------------
// FatFs disk glue: drive 0 is the stick, 512-byte sectors.
// ---------------------------------------------------------------------------

extern "C" DSTATUS disk_status(BYTE pdrv)
{
  return (pdrv == 0 && msd && disk_ok) ? 0 : STA_NOINIT;
}

extern "C" DSTATUS disk_initialize(BYTE pdrv)
{
  if (pdrv != 0 || !msd) { return STA_NOINIT; }
  disk_ok = (msd->init() == 0) && (msd->get_read_size() == 512);
  return disk_ok ? 0 : STA_NOINIT;
}

extern "C" DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
  if (pdrv != 0 || !disk_ok) { return RES_NOTRDY; }
  return msd->read(buff, (mbed::bd_addr_t)sector * 512, (mbed::bd_size_t)count * 512) == 0 ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
  if (pdrv != 0 || !disk_ok) { return RES_NOTRDY; }
  return msd->program(buff, (mbed::bd_addr_t)sector * 512, (mbed::bd_size_t)count * 512) == 0 ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  if (pdrv != 0 || !disk_ok) { return RES_NOTRDY; }
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;                  // every write has gone out already
    case GET_SECTOR_COUNT: *(LBA_t *)buff = (LBA_t)(msd->size() / 512); return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 2048; return RES_OK;   // f_mkfs aligns to 1 MB, as Windows does
  }
  return RES_PARERR;
}

extern "C" DWORD get_fattime(void) { return fat_now; }


static bool g_code_name(const char *name)
{
  static const char *const ext[] = { ".nc", ".ngc", ".gcode", ".gc", ".tap", ".txt", ".cnc", 0 };
  if (name[0] == '.') { return false; }             // hidden files, macOS "._" files
  const char *dot = strrchr(name, '.');
  if (!dot) { return false; }
  for (int i = 0; ext[i]; i++) {
    if (strcasecmp(dot, ext[i]) == 0) { return true; }
  }
  return false;
}

static int name_cmp(const gjob_file_t *a, const gjob_file_t *b) { return strcasecmp(a->name, b->name); }

static bool fat_info(const char *name, uint32_t *size, uint32_t *mtime)
{
  FILINFO fi;
  if (f_stat(name, &fi) != FR_OK) { *size = 0; *mtime = 0; return false; }
  *size = (uint32_t)fi.fsize;
  *mtime = ((uint32_t)fi.fdate << 16) | fi.ftime;
  return true;
}

// Reads the root directory into the shared listing.
static void scan(void)
{
  grbl_job_t *j = GRBL_JOB;
  static gjob_file_t list[GJOB_MAX_FILES];
  uint8_t n = 0, more = 0;
  if (mounted) {
    static FATFS_DIR dir;
    static FILINFO fi;
    if (f_opendir(&dir, "/") == FR_OK) {
      while ((f_readdir(&dir, &fi) == FR_OK) && fi.fname[0]) {
        if ((fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) || !g_code_name(fi.fname)) { continue; }
        if (strlen(fi.fname) >= GJOB_NAME_LEN) { more = 1; continue; }   // name too long to keep
        if (n >= GJOB_MAX_FILES) { more = 1; continue; }
        strcpy(list[n].name, fi.fname);
        list[n].size = (uint32_t)fi.fsize;
        list[n].mtime = ((uint32_t)fi.fdate << 16) | fi.ftime;
        n++;
      }
      f_closedir(&dir);
    }
  }
  // Insertion sort by name: a few dozen entries.
  for (int i = 1; i < n; i++) {
    gjob_file_t t = list[i];
    int k = i - 1;
    while (k >= 0 && name_cmp(&list[k], &t) > 0) { list[k + 1] = list[k]; k--; }
    list[k + 1] = t;
  }
  if (mounted) {
    // Size and free space: FatFs keeps the free count (FAT32's FSINFO, or
    // its own after the first count), so this is quick but the first time.
    DWORD free_clst;
    FATFS *fs;
    if (f_getfree("", &free_clst, &fs) == FR_OK) {
      uint32_t clst = (uint32_t)fs->csize * 512UL;                        // bytes per cluster
      j->usb_size_mb = (uint32_t)((uint64_t)(fs->n_fatent - 2) * clst / 1048576ULL);
      j->usb_free_mb = (uint32_t)((uint64_t)free_clst * clst / 1048576ULL);
    }
  } else {
    j->usb_size_mb = 0;
    j->usb_free_mb = 0;
  }
  j->n_files = 0;
  __DMB();
  memcpy((void *)j->file, list, sizeof(gjob_file_t) * n);
  j->list_more = more;
  __DMB();
  j->n_files = n;
  j->list_seq = (uint16_t)(j->list_seq + 1);
}

// A name a file may take on the stick and be listed under: no path, none of
// FAT's forbidden characters, a G-code extension.
static bool good_new_name(const char *name)
{
  if (!name[0] || !g_code_name(name)) { return false; }
  for (const char *p = name; *p; p++) {
    if (((uint8_t)*p < ' ') || ((uint8_t)*p >= 0x7F) || strchr("\"*/:<>?\\|", *p)) { return false; }
  }
  size_t n = strlen(name);
  return (name[n - 1] != ' ') && (name[n - 1] != '.');
}

// What is on a stick FatFs cannot mount - for the message.
static uint8_t vbr_kind(const uint8_t *s)
{
  if (memcmp(&s[3], "EXFAT   ", 8) == 0) { return GJOB_FS_EXFAT; }
  if (memcmp(&s[3], "NTFS    ", 8) == 0) { return GJOB_FS_NTFS; }
  if ((memcmp(&s[0x36], "FAT", 3) == 0) || (memcmp(&s[0x52], "FAT32", 5) == 0)) { return GJOB_FS_FAT; }
  return GJOB_FS_UNKNOWN;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void classify(void)
{
  grbl_job_t *j = GRBL_JOB;
  static uint8_t sec[512];
  j->usb_fs = GJOB_FS_UNREADABLE;
  j->usb_ptype = 0;
  if (!disk_ok || (msd->read(sec, 0, 512) != 0)) { return; }
  uint8_t k = vbr_kind(sec);                       // a file system right at sector 0
  if (k != GJOB_FS_UNKNOWN) { j->usb_fs = k; return; }
  if ((sec[510] != 0x55) || (sec[511] != 0xAA)) { j->usb_fs = GJOB_FS_NONE; return; }
  uint8_t type = sec[0x1BE + 4];
  uint32_t lba = le32(&sec[0x1BE + 8]);
  j->usb_ptype = type;
  if (!type || !lba) { j->usb_fs = GJOB_FS_NONE; return; }
  if (type == 0xEE) {
    // GPT: the first partition of the table (header in sector 1).
    j->usb_fs = GJOB_FS_GPT;
    if ((msd->read(sec, 512, 512) != 0) || (memcmp(sec, "EFI PART", 8) != 0)) { return; }
    uint32_t table = le32(&sec[72]);              // the table's first sector (low half)
    if ((msd->read(sec, (mbed::bd_addr_t)table * 512, 512) != 0)) { return; }
    lba = le32(&sec[32]);                         // first entry: its first sector (low half)
    if (!lba) { return; }
  }
  if (msd->read(sec, (mbed::bd_addr_t)lba * 512, 512) != 0) { return; }
  k = vbr_kind(sec);
  if (k != GJOB_FS_UNKNOWN) { j->usb_fs = k; }
}

static void stop_streaming(void)
{
  if (streaming) { f_close(&cur_file); }
  streaming = false;
}

// Keeps the ring as full as the file allows. Returns true if it did something.
static bool fill_ring(void)
{
  grbl_job_t *j = GRBL_JOB;
  if (!streaming) { return false; }
  bool did = false;
  for (int rounds = 0; rounds < 8; rounds++) {
    uint32_t wr = j->wr, rd = j->rd;
    uint32_t free_bytes = GJOB_RING_SIZE - (wr - rd);
    if (free_bytes < 512) { break; }                 // whole sectors only, unless at the end
    uint32_t at = wr & (GJOB_RING_SIZE - 1);
    uint32_t chunk = GJOB_RING_SIZE - at;            // up to the end of the ring
    if (chunk > free_bytes) { chunk = free_bytes; }
    if (chunk > 4096) { chunk = 4096; }
    chunk &= ~511UL;
    if (chunk == 0) { break; }
    UINT got;
    FRESULT fr = f_read(&cur_file, (void *)&j->ring[at], chunk, &got);
    if (fr != FR_OK) {
      j->usb_err = fr;
      j->rd_err = 1;
      stop_streaming();
      return true;
    }
    if (got == 0) {
      uint32_t ms = millis() - open_ms;
      j->read_ms = ms ? ms : 1;
      j->rate = (uint32_t)((uint64_t)wr * 1000ULL / (ms ? ms : 1));
      __DMB();
      j->eof = 1;
      stop_streaming();
      return true;
    }
    __DMB();                                         // data before the counter
    j->wr = wr + (uint32_t)got;
    did = true;
  }
  return did;
}

static void up_flush(void)
{
  if (!up_fill) { return; }
  UINT done = 0;
  if (!up_failed && (!up_open || (f_write(&up_file, up_buf, up_fill, &done) != FR_OK) || (done != up_fill))) {
    up_failed = true;
  }
  up_fill = 0;
}

// Closes and deletes an unfinished upload to the stick.
static void up_discard(void)
{
  if (up_open) {
    f_close(&up_file);
    up_open = false;
    if (mounted) { f_unlink(UP_TMP); }
  }
  up_fill = 0;
}

// An upload: here the M7 writes the ring and this side reads it, onto the
// stick in sector-sized pieces. The ring is always emptied, after a failed
// write too, so that the M7 never waits for it.
static bool take_upload(void)
{
  grbl_job_t *j = GRBL_JOB;
  if (!uploading) { return false; }
  uint32_t wr = j->wr, rd = j->rd;
  __DMB();
  if (wr == rd) { return false; }
  while (rd != wr) {
    uint8_t c = j->ring[rd & (GJOB_RING_SIZE - 1)];
    rd++;
    up_buf[up_fill++] = c;
    if (up_fill == sizeof(up_buf)) {
      __DMB();
      j->rd = rd;                  // room for the M7 while the stick writes
      up_flush();
    }
  }
  __DMB();
  j->rd = rd;
  return true;
}

// The upload is complete: the file on the stick takes its name and date.
static int8_t finish_stick_upload(const char *name, uint32_t fat_time)
{
  up_flush();
  if (!mounted || !up_open || up_failed) { up_discard(); return GJOB_ERR_WRITE; }
  if (fat_time) { fat_now = fat_time; }
  FRESULT fr = f_close(&up_file);
  up_open = false;
  if (fr != FR_OK) { f_unlink(UP_TMP); return GJOB_ERR_WRITE; }
  f_unlink(name);                                 // the old one, if there is one
  if (f_rename(UP_TMP, name) != FR_OK) { f_unlink(UP_TMP); return GJOB_ERR_RENAME; }
  if (fat_time) {                                 // the PC file's date, for a later sync
    FILINFO fi;
    fi.fdate = (WORD)(fat_time >> 16);
    fi.ftime = (WORD)fat_time;
    f_utime(name, &fi);
  }
  return 0;
}

static void st32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// CHS of a sector as MBRs give it (255 heads, 63 sectors); past cylinder 1023
// the "too far" value that says to use the LBA.
static void chs(uint8_t *p, uint32_t lba)
{
  uint32_t c = lba / (255UL * 63UL);
  if (c > 1023) { p[0] = 0xFE; p[1] = 0xFF; p[2] = 0xFF; return; }
  uint32_t h = (lba / 63UL) % 255UL, s = lba % 63UL + 1;
  p[0] = (uint8_t)h;
  p[1] = (uint8_t)(s | ((c >> 2) & 0xC0));
  p[2] = (uint8_t)c;
}

// An MBR as Windows writes one: a disk signature - Windows names the volumes
// of an MBR disk by it and does not mount one on a disk whose signature is 0,
// which is what FatFs's own MBR has - and one partition from 1 MB to the end.
// f_mkfs() then formats that partition and sets its type.
static bool write_mbr(uint32_t n_sect, uint32_t signature)
{
  uint8_t *b = up_buf;                             // free while formatting
  memset(b, 0, 512);
  st32(&b[440], signature);
  uint8_t *pte = &b[446];
  chs(&pte[1], 2048);
  pte[4] = 0x0C;                                   // FAT32 LBA, until f_mkfs() sets the real type
  chs(&pte[5], n_sect - 1);
  st32(&pte[8], 2048);
  st32(&pte[12], n_sect - 2048);
  b[510] = 0x55;
  b[511] = 0xAA;
  if (disk_write(0, b, 0, 1) != RES_OK) { return false; }
  // A GPT from before would contradict the MBR: its two headers go.
  memset(b, 0, 512);
  return (disk_write(0, b, 1, 1) == RES_OK) && (disk_write(0, b, n_sect - 1, 1) == RES_OK);
}

// Formats the stick: one partition over all of it, FAT32 up to 32 GB and
// exFAT above (FatFs's choice, as Windows makes it) or exFAT, then mounts it
// again.
static int8_t format_stick(bool exfat)
{
  grbl_job_t *j = GRBL_JOB;
  if (!msd->connected()) { return GJOB_ERR_NO_STICK; }
  j->usb_state = GJOB_USB_FORMATTING;
  j->n_files = 0;
  if (mounted) { f_mount(nullptr, "", 0); }
  mounted = false;
  disk_ok = false;
  MKFS_PARM opt = { (BYTE)(exfat ? FM_EXFAT : FM_ANY), 2, 0, 0, 0 };   // two FATs, sizes by FatFs
  FRESULT fr = FR_DISK_ERR;
  if (!(disk_initialize(0) & STA_NOINIT)) {
    uint32_t n_sect = (uint32_t)(msd->size() / 512);
    // Something different for every stick and every format: FatFs takes the
    // volume serial number (and GPT GUIDs) from get_fattime().
    uint32_t rnd = ((uint32_t)micros() * 2654435761UL) ^ (millis() << 16) ^ n_sect;
    DWORD date = fat_now;
    fat_now = rnd;
    if ((n_sect < 0x10000000UL) && (n_sect > 65536UL)) {   // up to 128 GB: MBR (above, FatFs makes a GPT)
      if (write_mbr(n_sect, rnd | 1)) {
        VolToPart[0].pt = 1;
        fr = f_mkfs("", &opt, up_buf, sizeof(up_buf));
        VolToPart[0].pt = 0;
      }
    } else {
      fr = f_mkfs("", &opt, up_buf, sizeof(up_buf));
    }
    fat_now = date;
  }
  if (fr == FR_OK) { fr = f_mount(&fat_fs, "", 1); }
  if (fr != FR_OK) {
    j->usb_err = fr;
    classify();
    j->usb_state = GJOB_USB_ERROR;
    scan();
    return GJOB_ERR_FORMAT;
  }
  mounted = true;
  j->usb_err = 0;
  j->usb_fat = fat_fs.fs_type;
  scan();
  j->usb_state = GJOB_USB_READY;
  return 0;
}

static void handle_request(void)
{
  grbl_job_t *j = GRBL_JOB;
  uint8_t seq = j->req_seq;
  if (seq == req_seen) { return; }
  __DMB();
  int8_t status = 0;
  char name[GJOB_NAME_LEN];
  memcpy(name, (const void *)j->req_name, GJOB_NAME_LEN);
  name[GJOB_NAME_LEN - 1] = 0;
  switch (j->req_code) {
    case GJOB_REQ_OPEN:
      stop_streaming();
      up_discard();
      uploading = false;
      j->eof = 0;
      j->rd_err = 0;
      j->read_ms = 0;
      j->rate = 0;
      j->wr = 0;
      j->rd = 0;
      if (!mounted) { status = GJOB_ERR_NO_STICK; break; }
      if (f_open(&cur_file, name, FA_READ) != FR_OK) { status = GJOB_ERR_OPEN; break; }
      {
        uint32_t size, mtime;
        fat_info(name, &size, &mtime);
        j->f_size = size;
        j->f_mtime = mtime;
      }
      streaming = true;
      open_ms = millis();
      break;
    case GJOB_REQ_CLOSE:
      stop_streaming();
      break;
    case GJOB_REQ_RESCAN:
      scan();
      break;
    case GJOB_REQ_UPLOAD_END:
      if (!uploading) { status = GJOB_ERR_WRITE; break; }
      take_upload();
      uploading = false;
      status = finish_stick_upload(up_name, j->req_time);
      scan();
      break;
    case GJOB_REQ_WRITE:
      stop_streaming();
      up_discard();
      uploading = false;
      j->wr = 0;
      j->rd = 0;
      if (!mounted) { status = GJOB_ERR_NO_STICK; break; }
      if (!good_new_name(name)) { status = GJOB_ERR_NAME; break; }
      f_unlink(UP_TMP);              // left over from an upload cut short by a power cut
      if (f_open(&up_file, UP_TMP, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { status = GJOB_ERR_OPEN; break; }
      up_open = true;
      up_failed = false;
      up_fill = 0;
      uploading = true;
      strcpy(up_name, name);
      break;
    case GJOB_REQ_UP_ABORT:
      take_upload();
      up_discard();
      uploading = false;
      break;
    case GJOB_REQ_DELETE:
      stop_streaming();
      if (!mounted) {
        status = GJOB_ERR_NO_STICK;
      } else if (f_unlink(name) != FR_OK) {
        status = GJOB_ERR_OPEN;
      }
      scan();
      break;
    case GJOB_REQ_FORMAT:
      stop_streaming();
      up_discard();
      uploading = false;
      status = format_stick(j->req_time == 1);
      break;
    case GJOB_REQ_SECTOR:                // $FX: a raw sector, mounted or not
      stop_streaming();
      up_discard();
      uploading = false;
      if (!msd->connected()) { status = GJOB_ERR_NO_STICK; break; }
      if (!disk_ok) { disk_initialize(0); }
      if (!disk_ok || (msd->read((void *)j->ring, (mbed::bd_addr_t)j->req_time * 512, 512) != 0)) { status = GJOB_ERR_OPEN; }
      break;
    default:
      status = GJOB_ERR_REQ;
  }
  j->ack_status = status;
  __DMB();
  j->ack_seq = seq;
  req_seen = seq;
}

static void usb_worker(void)
{
  grbl_job_t *j = GRBL_JOB;
  msd = new USBHostMSD();        // starts the host stack and switches VBUS on
  j->usb_state = GJOB_USB_NONE;
  uint32_t last_try = 0;
  for (;;) {
    uint32_t now = millis();
    if (!mounted) {
      if (j->usb_state == GJOB_USB_ERROR) {
        // A stick that cannot be mounted: wait until it is pulled out.
        if (!msd->connected()) { j->usb_state = GJOB_USB_NONE; }
      } else if ((uint32_t)(now - last_try) >= 200) {
        last_try = now;
        if (msd->connect()) {
          j->usb_state = GJOB_USB_MOUNTING;
          disk_ok = false;
          FRESULT err = f_mount(&fat_fs, "", 1);
          if (err == FR_OK) {
            mounted = true;
            j->usb_err = 0;
            j->usb_fat = fat_fs.fs_type;
            scan();
            j->usb_state = GJOB_USB_READY;
          } else {
            j->usb_err = err;
            classify();
            j->usb_state = GJOB_USB_ERROR;
          }
        }
      }
    } else if (!msd->connected()) {
      // Pulled out. A file being read from it cannot be finished any more.
      if (streaming) { j->rd_err = 1; stop_streaming(); }
      // So is a file being written to it: the rest of it is dropped.
      if (up_open) { f_close(&up_file); up_open = false; up_failed = true; }
      f_mount(nullptr, "", 0);
      mounted = false;
      disk_ok = false;
      j->usb_state = GJOB_USB_NONE;
      scan();                    // an empty listing
    }
    handle_request();
    bool busy = fill_ring();
    busy |= take_upload();
    if (!busy) {
      rtos::ThisThread::sleep_for(std::chrono::milliseconds((streaming || uploading) ? 1 : 10));
    }
  }
}

void usbstick_start(void)
{
  grbl_job_t *j = GRBL_JOB;
  // Our half of the block from scratch; the M7 owns the job fields and the
  // request counter, which keep their values.
  j->usb_state = GJOB_USB_OFF;
  j->n_files = 0;
  j->list_more = 0;
  j->usb_err = 0;
  j->wr = 0;
  j->eof = 0;
  j->rd_err = 0;
  j->rate = 0;
  j->read_ms = 0;
  req_seen = j->req_seq;
  j->ack_seq = req_seen;
  __DMB();
  j->magic = GRBL_JOB_MAGIC;
  usb_thread = new rtos::Thread(osPriorityAboveNormal, 8 * 1024, nullptr, "usbstick");
  usb_thread->start(mbed::callback(usb_worker));
}

#endif // CORE_CM4
