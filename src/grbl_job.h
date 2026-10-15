/*
  grbl_job.h - running G-code from a USB stick: the shared block between the
  M4 (USB host, file system) and the M7 (Grbl)

  Part of Grbl-Giga, a port of Grbl 1.1 to the Arduino GIGA R1 WiFi.

  Grbl is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version. Distributed WITHOUT ANY WARRANTY; see the GNU GPL for details.

  -------------------------------------------------------------------------------

  The USB-A port is a USB host on the M4 (src/m4/usbstick.cpp): the M4 mounts
  the stick, keeps a listing of its G-code files here, and on request streams
  a file into the byte ring below. Grbl on the M7 (src/job.c) takes lines out
  of the ring as its planner has room for them - the same path lines from the
  PC take - and decides everything about the job: when it may start, the
  check pass, pauses, errors, the end.

      USB stick -> M4: FAT, fread -> ring[] in SRAM4 -> M7: parser, planner

  Lives right after the display block (grbl_shm.h, 0x3800C000) in SRAM4,
  which the M7 maps non-cacheable and the M4 has no data cache for. The ring
  has one writer and one reader, each owning its own counter; counters are
  byte totals since the file was opened, so wr - rd is the fill and no lock is
  needed, only ordering (a barrier between the data and the counter).
*/

#ifndef grbl_job_h
#define grbl_job_h

#include <stdint.h>

#define GRBL_JOB_ADDR       0x3800D000UL
#define GRBL_JOB_MAGIC      0x4A4F4231UL    // "JOB1" - the M4 has set the block up
#define GJOB_MAX_FILES      40
#define GJOB_NAME_LEN       48              // with the terminating 0
#define GJOB_RING_SIZE      8192            // power of two

// usb_state (M4)
#define GJOB_USB_OFF        0   // host not started yet
#define GJOB_USB_NONE       1   // no stick
#define GJOB_USB_MOUNTING   2   // a stick, being mounted
#define GJOB_USB_READY      3   // mounted, listing valid
#define GJOB_USB_ERROR      4   // a stick, but it could not be mounted (usb_err)
#define GJOB_USB_FORMATTING 5   // being formatted ($FF=ERASE, FORMAT on the display)

// usb_fs: what a stick that cannot be mounted holds (M4, a look at its first sectors)
#define GJOB_FS_UNKNOWN     0   // not looked at / nothing recognised
#define GJOB_FS_UNREADABLE  1   // its first sector cannot be read
#define GJOB_FS_FAT         2   // FAT after all (damaged?)
#define GJOB_FS_EXFAT       3   // exFAT after all (damaged?)
#define GJOB_FS_NTFS        4
#define GJOB_FS_GPT         5   // GPT, first partition not recognised
#define GJOB_FS_NONE        6   // no partition table, no file system

// Requests from the M7 (req_code)
#define GJOB_REQ_OPEN       1   // open req_name and stream it into the ring
#define GJOB_REQ_CLOSE      2   // stop streaming, close the file
#define GJOB_REQ_RESCAN     3   // read the directory again
#define GJOB_REQ_UPLOAD_END 5   // the file being uploaded is complete (req_time: its date, 0 = none)
#define GJOB_REQ_WRITE      6   // a file req_name for the stick follows through the ring (M7 writes)
#define GJOB_REQ_UP_ABORT   7   // the upload was given up: drop what has come so far
#define GJOB_REQ_DELETE     8   // delete req_name from the stick
#define GJOB_REQ_FORMAT     9   // format the stick: FAT32 up to 32 GB, exFAT above (req_time 1: exFAT)
#define GJOB_REQ_SECTOR     10  // diagnostics: sector req_time of the stick into ring[0..511]

// ack_status of the requests (0 = ok)
#define GJOB_ERR_NO_STICK   -1  // nothing mounted
#define GJOB_ERR_OPEN       -2  // no such file / cannot be created
#define GJOB_ERR_REQ        -3  // unknown request
#define GJOB_ERR_WRITE      -4  // writing failed (stick full, pulled out)
#define GJOB_ERR_NAME       -6  // not a G-code file name the listing would show
#define GJOB_ERR_RENAME     -7  // written, but it could not take its name
#define GJOB_ERR_FORMAT     -8  // formatting failed

// Job state (M7, job_state)
#define GJOB_IDLE           0   // no job
#define GJOB_OPENING        1   // waiting for the M4 to open the file
#define GJOB_CHECK          2   // check pass ($C): parsing without motion
#define GJOB_RUN            3   // running
#define GJOB_STOPPING       4   // STOP: holding, then reset
#define GJOB_DONE           5   // finished (the last one)
#define GJOB_FAILED         6   // refused or stopped by an error (job_err)
#define GJOB_ABORTED        7   // reset, alarm, STOP or stick removed

typedef struct {
  char     name[GJOB_NAME_LEN];
  uint32_t size;            // bytes
  uint32_t mtime;           // FAT date << 16 | FAT time
} gjob_file_t;

typedef struct {
  volatile uint32_t magic;          // GRBL_JOB_MAGIC once the M4 has initialised the block

  // ---- the stick and its listing (M4) ----
  volatile uint8_t  usb_state;      // GJOB_USB_*
  volatile uint8_t  n_files;        // entries in file[], sorted by name
  volatile uint16_t list_seq;       // changes with every new listing
  volatile int32_t  usb_err;        // mount / read error (negative errno), 0 = none
  volatile uint8_t  list_more;      // more files on the stick than file[] holds
  volatile uint8_t  usb_fs;         // GJOB_FS_*: what is on a stick that cannot be mounted
  volatile uint8_t  usb_ptype;      // its first partition's type byte (0 = no partition table)
  volatile uint8_t  usb_fat;        // mounted: FatFs's FS_FAT12/16/32 (1-3) or FS_EXFAT (4)
  volatile uint32_t usb_size_mb;    // mounted: size of the file system, MB
  volatile uint32_t usb_free_mb;    // mounted: free space, MB (with every listing)
  gjob_file_t       file[GJOB_MAX_FILES];

  // ---- requests: M7 -> M4 ----
  volatile uint8_t  req_seq;        // M7 increments after writing the request
  volatile uint8_t  req_code;       // GJOB_REQ_*
  volatile uint8_t  pad1[2];
  char              req_name[GJOB_NAME_LEN];
  volatile uint32_t req_time;       // UPLOAD_END: the file's date (FAT date << 16 | FAT time), 0 = none;
                                    // FORMAT: 1 = exFAT whatever the size

  // ---- answers: M4 -> M7 ----
  volatile uint8_t  ack_seq;        // = req_seq once handled
  volatile int8_t   ack_status;     // 0 ok, < 0 error
  volatile uint8_t  pad2[2];
  volatile uint32_t f_size;         // the file opened
  volatile uint32_t f_mtime;

  // ---- the stream: M4 writes, M7 reads ----
  volatile uint32_t wr;             // bytes put into the ring since the file was opened
  volatile uint32_t rd;             // bytes taken out (M7)
  volatile uint8_t  eof;            // M4: the whole file is in the ring
  volatile uint8_t  rd_err;         // M4: reading failed or the stick went away
  volatile uint8_t  pad3[2];
  volatile uint32_t rate;           // M4: read rate of the last file, bytes/s
  volatile uint32_t read_ms;        // M4: time it took to read it (0 while not done)

  // ---- the job, for the display (M7) ----
  volatile uint8_t  job_state;      // GJOB_IDLE ...
  volatile uint8_t  job_checked;    // the run was preceded by a check pass (0) or not (1: known file)
  volatile uint8_t  job_err;        // Grbl status code that stopped it, 0 = none
  volatile uint8_t  job_alarm;      // alarm that aborted it, 0 = none
  char              job_name[GJOB_NAME_LEN];
  volatile uint32_t job_size;       // bytes
  volatile uint32_t job_pos;        // bytes executed (parsed)
  volatile uint32_t job_line;       // lines parsed so far
  volatile uint32_t job_lines;      // lines in the file (from the check pass, 0 = unknown)
  volatile uint32_t job_err_line;   // line of job_err
  volatile uint32_t job_start_ms;   // M7 time the run started
  volatile uint32_t job_end_ms;     // M7 time it ended (0 while running)
  volatile float    job_min[3];     // X Y Z range of the program (check pass, work coordinates)
  volatile float    job_max[3];

  uint8_t           ring[GJOB_RING_SIZE];
} grbl_job_t;

#define GRBL_JOB  ((grbl_job_t *)GRBL_JOB_ADDR)

#endif
