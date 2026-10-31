/*
 * SANE backend for the EPSON Perfection V39 / GT-S650 (USB 04b8:013d).
 *
 * Implements the device protocol documented in ../PROTOCOL.md directly:
 * firmware upload (4.2), init (6), AFE/LED/shading calibration (7.4),
 * positioning and image transfer (7.1-7.5) and host-side unscrambling,
 * cropping and down-sampling (7.2).  No Epson code is used; the firmware
 * image (esfw010c.bin) must be supplied by the user.
 *
 * Built out of tree against libusb-1.0 and the public SANE headers; the
 * exported entry points carry the sane_epsonv39_ prefix so the dll
 * meta-backend can load it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>
#include <sane/sane.h>
#include <sane/saneopts.h>

#define BACKEND_NAME "epsonv39"
#define V39_VID 0x04b8
#define V39_PID 0x013d
#define EP_OUT 0x02
#define EP_IN 0x81
#define ACK 0x06

#define BUILD 1

#define ENTRY(name) sane_epsonv39_##name

/* ------------------------------------------------------------------ */
/* debug                                                               */

static int dbg_level;

static void DBG(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void DBG(int lvl, const char *fmt, ...)
{
    va_list ap;
    if (lvl > dbg_level)
        return;
    {
        static double t0;
        struct timespec ts;
        double t;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (t0 == 0)
            t0 = t;
        fprintf(stderr, "[" BACKEND_NAME " %8.3f] ", t - t0);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* per-resolution tables (PROTOCOL.md 7.2, 7.5, ref/)                  */

typedef struct {
    int dpi;
    int W;            /* pixels per raw line (full sensor)          */
    int k;            /* phase groups per sensor half               */
    int dummy;        /* dummy pixels at the end of each half       */
    uint32_t reg21;   /* 1E 21 line timing                          */
    int mcal;         /* motor mode, calibration move               */
    int mimg_c;       /* motor mode, image scan colour              */
    int mimg_g;       /* motor mode, image scan gray (0 = n/a)      */
    int soff_c;       /* start-position offset (1/1200")            */
    int soff_g;
    int b9;           /* 1E 41 byte 9: AFE/LED/dark/image passes    */
    int b9w;          /* 1E 41 byte 9: white pass                   */
    int b10_c;        /* 1E 41 byte 10, colour                      */
    int b10_g;        /* 1E 41 byte 10, gray (white pass uses 1)    */
    int lamp;         /* 1E 11 parameter                            */
    int duty0;        /* LED start duty                             */
    int v_c, v_g;     /* raw lines per output line                  */
    int dark_reads_c, dark_lines_c;   /* colour dark pass split     */
    int white_c, white_g;             /* white pass lines           */
    uint32_t m1a[3], m1b[3];          /* 1E 22 mode 1 timing        */
    uint32_t m2b;                     /* 1E 22 mode 2 timing        */
    uint16_t c;                       /* 1E 22 third field (x3)     */
} hwres_t;

static const hwres_t hwres[] = {
    { 300, 2592, 1, 4, 0x210, 0x10, 0x09, 0x0B, 65, 64, 1, 4, 2, 1, 4, 135, 2, 4,
      1, 32, 64, 192, { 0xD6, 0x41E, 0xA90 }, { 0x2F3, 0x965, 0xA91 }, 0xA55, 0x2A3 },
    { 600, 5168, 1, 0, 0x480, 0x0F, 0x08, 0x0A, 65, 65, 1, 2, 2, 2, 3, 320, 1, 1,
      1, 32, 32, 96, { 0xD6, 0x874, 0x1792 }, { 0x5D7, 0x14F5, 0x1793 }, 0x1757, 0x640 },
    { 1200, 10336, 2, 0, 0x900, 0x0E, 0x05, 0x06, 174, 174, 1, 2, 1, 1, 2, 640, 1, 1,
      2, 16, 32, 96, { 0xD6, 0xFF4, 0x2E12 }, { 0xAD7, 0x28F5, 0x2E13 }, 0x2DD7, 0xC80 },
    { 2400, 20672, 4, 0, 0x1200, 0x0D, 0x03, 0x04, 174, 174, 2, 2, 1, 1, 1, 900, 1, 1,
      3, 10, 32, 96, { 0xD6, 0x160C, 0x405A }, { 0xEE7, 0x3935, 0x405B }, 0x401F, 0x1194 },
    { 4800, 41344, 8, 0, 0x2400, 0x0C, 0x01, 0x00, 174, 174, 4, 2, 1, 1, 0, 1800, 1, 1,
      5, 6, 32, 96, { 0xD6, 0x2B24, 0x7FA2 }, { 0x1CF7, 0x7175, 0x7FA3 }, 0x7F67, 0x2328 },
};
#define N_HWRES (int)(sizeof(hwres) / sizeof(hwres[0]))

static const uint16_t afe_gain = 0x3A;
static const uint16_t w43_cal[6] = { 0x8000, 0x8000, 0x8000, 0x7FDB, 0x7FE2, 0x8000 };
static const uint16_t w43_img[6] = { 0x7FDB, 0x7FE2, 0x8000, 0x7FDB, 0x7FE2, 0x8000 };

#define MAX_X_MM 215.9   /* 8.5"  */
#define MAX_Y_MM 297.18  /* 11.7" */

static const SANE_Int res_list[] = { 9, 75, 100, 150, 200, 300, 600, 1200, 2400, 4800 };

/* ------------------------------------------------------------------ */
/* options                                                             */

enum {
    OPT_NUM_OPTS = 0,
    OPT_MODE_GROUP,
    OPT_MODE,
    OPT_RESOLUTION,
    OPT_PREVIEW,
    OPT_THRESHOLD,
    OPT_CAL_CACHE,
    OPT_GEOMETRY_GROUP,
    OPT_TL_X,
    OPT_TL_Y,
    OPT_BR_X,
    OPT_BR_Y,
    OPT_SENSORS_GROUP,
    OPT_BUTTON_SCAN,     /* 1E 85 value 1: Start */
    OPT_BUTTON_COPY,     /* 2: Copy */
    OPT_BUTTON_EMAIL,    /* 3: Scan to E-mail */
    OPT_BUTTON_PDF,      /* 4: PDF */
    NUM_OPTIONS
};

static SANE_String_Const mode_list[] = {
    SANE_VALUE_SCAN_MODE_COLOR, SANE_VALUE_SCAN_MODE_GRAY, SANE_VALUE_SCAN_MODE_LINEART, NULL
};

static const SANE_Range x_range = { SANE_FIX(0), SANE_FIX(MAX_X_MM), 0 };
static const SANE_Range y_range = { SANE_FIX(0), SANE_FIX(MAX_Y_MM), 0 };
static const SANE_Range thr_range = { 0, 255, 1 };

/* ------------------------------------------------------------------ */
/* device and scan state                                               */

typedef struct {
    /* geometry and format, computed by calc_params() */
    const hwres_t *hw;
    int out_dpi;
    int colour_dev;      /* device scans colour (3 planes)               */
    int out_mode;        /* 0 colour, 1 gray, 2 lineart                  */
    int planes;          /* device planes in the image pass              */
    int v;               /* raw lines per hw line                        */
    int hw_x0;           /* first hw pixel (after the fixed x offset)    */
    int hw_w;            /* hw pixels needed                             */
    int hw_h;            /* hw lines needed                              */
    double y_in;         /* top edge, inches                             */
    int out_w, out_h;    /* output pixels / lines                        */
    int bpl;             /* output bytes per line                        */
    int raw_lines;       /* lines in the final 1E 41                     */
    int raw_line_bytes;
    int lpt;
} scanparm_t;

typedef struct dev_s {
    struct dev_s *next;
    char name[64];
    uint8_t bus, addr;
    SANE_Device sane;
} v39dev_t;

typedef struct {
    libusb_device_handle *h;
    v39dev_t *dev;

    SANE_Option_Descriptor opt[NUM_OPTIONS];
    SANE_Word val[NUM_OPTIONS];
    char mode[32];

    scanparm_t sp;

    /* last 1E 85 reading, shared by the button options */
    uint8_t btn;
    double btn_time;

    /* calibration results */
    uint16_t duty[3];
    uint16_t afe[2];
    uint16_t w42[9];
    int shade_sel, shade_b15;

    /* scanning */
    int scanning;
    volatile int cancel;
    pthread_t thread;
    int thread_running;
    SANE_Status thread_status;
    int transfers_left;

    /* output queue (ring buffer) */
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    uint8_t *q;
    size_t qcap, qhead, qlen;
    int qdone;
    size_t delivered, total;
} v39_t;

static libusb_context *usbctx;
static v39dev_t *devices;
static const SANE_Device **devlist;
static char fw_path_conf[PATH_MAX];

static const char *status_str(SANE_Status st)
{
    switch (st) {
    case SANE_STATUS_GOOD: return "good";
    case SANE_STATUS_CANCELLED: return "cancelled";
    case SANE_STATUS_DEVICE_BUSY: return "device busy";
    case SANE_STATUS_INVAL: return "invalid argument";
    case SANE_STATUS_IO_ERROR: return "I/O error";
    case SANE_STATUS_NO_MEM: return "out of memory";
    default: return "error";
    }
}

/* ------------------------------------------------------------------ */
/* USB transport                                                       */

#define TIMEOUT_CMD 30000
#define TIMEOUT_IMG 30000
#define TIMEOUT_FW 60000

static SANE_Status usb_write_t(v39_t *s, const void *buf, int len, int timeout);
static SANE_Status usb_write(v39_t *s, const void *buf, int len)
{
    return usb_write_t(s, buf, len, TIMEOUT_CMD);
}

static SANE_Status usb_write_t(v39_t *s, const void *buf, int len, int timeout)
{
    int done = 0, r;
    r = libusb_bulk_transfer(s->h, EP_OUT, (unsigned char *)buf, len, &done, timeout);
    if (r < 0 || done != len) {
        DBG(1, "usb write failed: %s (%d/%d)\n", libusb_error_name(r), done, len);
        return SANE_STATUS_IO_ERROR;
    }
    if (dbg_level >= 5) {
        int i;
        fprintf(stderr, "[" BACKEND_NAME "] >");
        for (i = 0; i < len && i < 48; i++)
            fprintf(stderr, " %02x", ((const uint8_t *)buf)[i]);
        fprintf(stderr, len > 48 ? " ...(%d)\n" : "\n", len);
    }
    return SANE_STATUS_GOOD;
}

/* one bulk IN transfer of exactly len bytes (the device ends with a short packet) */
static SANE_Status usb_read(v39_t *s, void *buf, int len, int timeout)
{
    int got = 0, r;
    r = libusb_bulk_transfer(s->h, EP_IN, buf, len, &got, timeout);
    if (r < 0 || got != len) {
        DBG(1, "usb read failed: %s (%d/%d)\n", libusb_error_name(r), got, len);
        return SANE_STATUS_IO_ERROR;
    }
    if (dbg_level >= 5) {
        int i;
        fprintf(stderr, "[" BACKEND_NAME "] <");
        for (i = 0; i < len && i < 48; i++)
            fprintf(stderr, " %02x", ((const uint8_t *)buf)[i]);
        fprintf(stderr, len > 48 ? " ...(%d)\n" : "\n", len);
    }
    return SANE_STATUS_GOOD;
}

#define CHK(x) do { SANE_Status st_ = (x); if (st_ != SANE_STATUS_GOOD) return st_; } while (0)

static SANE_Status expect_ack_t(v39_t *s, int timeout)
{
    uint8_t a;
    CHK(usb_read(s, &a, 1, timeout));
    if (a != ACK) {
        DBG(1, "expected ACK, got %02x\n", a);
        return SANE_STATUS_IO_ERROR;
    }
    return SANE_STATUS_GOOD;
}

static SANE_Status expect_ack(v39_t *s)
{
    uint8_t a;
    CHK(usb_read(s, &a, 1, TIMEOUT_CMD));
    if (a != ACK) {
        DBG(1, "expected ACK, got %02x\n", a);
        return SANE_STATUS_IO_ERROR;
    }
    return SANE_STATUS_GOOD;
}

static SANE_Status op2(v39_t *s, uint8_t a, uint8_t b)
{
    uint8_t op[2] = { a, b };
    return usb_write(s, op, 2);
}

/* shape Q */
static SANE_Status cmd_query(v39_t *s, uint8_t a, uint8_t b, void *reply, int n)
{
    CHK(op2(s, a, b));
    return usb_read(s, reply, n, TIMEOUT_CMD);
}

/* shape A */
static SANE_Status cmd_action(v39_t *s, uint8_t a, uint8_t b)
{
    CHK(op2(s, a, b));
    return expect_ack(s);
}

/* shape S */
static SANE_Status cmd_set(v39_t *s, uint8_t a, uint8_t b, const void *p, int n)
{
    CHK(op2(s, a, b));
    CHK(expect_ack(s));
    CHK(usb_write(s, p, n));
    return expect_ack(s);
}

/* shape SG */
static SANE_Status cmd_setget(v39_t *s, uint8_t a, uint8_t b, const void *p, int n,
                              void *reply, int m)
{
    CHK(op2(s, a, b));
    CHK(expect_ack(s));
    CHK(usb_write(s, p, n));
    return usb_read(s, reply, m, TIMEOUT_CMD);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static SANE_Status set_u8(v39_t *s, uint8_t op, uint8_t v) { return cmd_set(s, 0x1E, op, &v, 1); }

static SANE_Status get_status(v39_t *s, uint8_t *st)
{
    uint8_t r[2];
    CHK(cmd_query(s, 0x1B, 0x03, r, 2));
    *st = r[0];
    return SANE_STATUS_GOOD;
}

static SANE_Status wait_idle(v39_t *s, double timeout)
{
    double t0 = now_s();
    uint8_t st;
    for (;;) {
        CHK(get_status(s, &st));
        if (!(st & 0x40))
            return SANE_STATUS_GOOD;
        if (now_s() - t0 > timeout) {
            DBG(1, "timeout waiting for idle (status %02x)\n", st);
            return SANE_STATUS_DEVICE_BUSY;
        }
        sleep_ms(50);
    }
}

/*
 * After 1E 05 the busy bit (0x40) can take a moment to appear.  Wait until
 * the move has visibly started (or 300 ms have passed), then until idle.
 */
static SANE_Status wait_move(v39_t *s)
{
    double t0 = now_s();
    uint8_t st;
    for (;;) {
        CHK(get_status(s, &st));
        if ((st & 0x40) || now_s() - t0 > 0.3)
            break;
        sleep_ms(10);
    }
    DBG(3, "move %s after %.0f ms\n", (st & 0x40) ? "started" : "not seen", (now_s() - t0) * 1000);
    return wait_idle(s, 30);
}

static SANE_Status mem_write(v39_t *s, uint8_t type, uint32_t addr, const void *data, uint32_t len)
{
    uint8_t hdr[10] = { type, 0 };
    put32(hdr + 2, addr);
    put32(hdr + 6, len);
    CHK(op2(s, 0x1E, 0x84));
    CHK(expect_ack(s));
    CHK(usb_write(s, hdr, 10));
    CHK(usb_write(s, data, len));
    return expect_ack(s);
}

static SANE_Status mem_read(v39_t *s, uint8_t type, uint32_t addr, void *data, uint32_t len)
{
    uint8_t hdr[10] = { type, 0 };
    put32(hdr + 2, addr);
    put32(hdr + 6, len);
    return cmd_setget(s, 0x1E, 0x83, hdr, 10, data, len);
}

static SANE_Status move(v39_t *s, uint16_t mode, uint32_t steps)
{
    uint8_t p[6];
    put16(p, mode);
    put32(p + 2, steps);
    return cmd_set(s, 0x1E, 0x01, p, 6);
}

static SANE_Status set_reg21(v39_t *s, uint32_t v)
{
    uint8_t p[4];
    put32(p, v);
    return cmd_set(s, 0x1E, 0x21, p, 4);
}

static SANE_Status set_words(v39_t *s, uint8_t op, const uint16_t *w, int n)
{
    uint8_t p[32];
    int i;
    for (i = 0; i < n; i++)
        put16(p + 2 * i, w[i]);
    return cmd_set(s, 0x1E, op, p, 2 * n);
}

static SANE_Status set_afe(v39_t *s, uint16_t offL, uint16_t offR)
{
    uint16_t w[9] = { offL, offL, offL, offR, offR, offR, afe_gain, afe_gain, afe_gain };
    return set_words(s, 0x31, w, 9);
}

/* 1E 22 LED/exposure block (PROTOCOL.md 5.4, 7.5) */
static SANE_Status set_led(v39_t *s, const hwres_t *hw, int mode, const uint16_t duty[3], int flag)
{
    uint8_t p[40];
    int i;
    memset(p, 0, sizeof p);
    put16(p, mode);
    for (i = 0; i < 3; i++) {
        put32(p + 2 + 4 * i, mode == 1 ? hw->m1a[i] : 0xD6);
        put32(p + 14 + 4 * i, mode == 1 ? hw->m1b[i] : hw->m2b);
        put16(p + 26 + 2 * i, hw->c);
        put16(p + 32 + 2 * i, duty ? duty[i] : 0);
    }
    put16(p + 38, flag);
    return cmd_set(s, 0x1E, 0x22, p, 40);
}

/* 1E 41 set-scan: bytes 8..17 given as b[10] */
static SANE_Status set_scan(v39_t *s, const hwres_t *hw, uint32_t lines, const uint8_t b[10], int lpt)
{
    uint8_t p[22];
    memset(p, 0, sizeof p);
    put16(p, 0x15F);
    put16(p + 2, 0x15F + hw->W);
    put32(p + 4, lines);
    memcpy(p + 8, b, 10);
    p[18] = lpt;
    return cmd_set(s, 0x1E, 0x41, p, 22);
}

static int calc_lpt(int lines, int line_bytes)
{
    int l = 65535 / line_bytes;
    if (l < 1)
        l = 1;
    if (l > lines)
        l = lines;
    if (l > 255)
        l = 255;
    return l;
}

/*
 * Run one calibration pass: 1E 41 (unless already sent), 1E 47, read all
 * transfers into buf (lines * line_bytes, trailers dropped).
 */
static SANE_Status read_pass(v39_t *s, const hwres_t *hw, int lines, const uint8_t b[10],
                             int line_bytes, uint8_t *buf)
{
    int lpt = calc_lpt(lines, line_bytes), left = lines;
    uint8_t *tmp;
    SANE_Status st;

    CHK(set_scan(s, hw, lines, b, lpt));
    CHK(wait_idle(s, 30));
    CHK(op2(s, 0x1E, 0x47));
    tmp = malloc((size_t)lpt * line_bytes + 8);
    if (!tmp)
        return SANE_STATUS_NO_MEM;
    while (left > 0) {
        int n = left < lpt ? left : lpt;
        st = usb_read(s, tmp, n * line_bytes + 8, TIMEOUT_IMG);
        if (st != SANE_STATUS_GOOD) {
            free(tmp);
            return st;
        }
        memcpy(buf, tmp, (size_t)n * line_bytes);
        buf += (size_t)n * line_bytes;
        left -= n;
    }
    free(tmp);
    /* the carriage may still be moving; the device ignores commands until idle */
    return wait_idle(s, 30);
}

/* ------------------------------------------------------------------ */
/* firmware (PROTOCOL.md 4.2)                                          */

static int try_load_fw(const char *path, uint8_t **out)
{
    FILE *f;
    uint8_t *buf;
    long n;
    if (!path || !*path)
        return 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    buf = malloc(0x10200);
    n = buf ? (long)fread(buf, 1, 0x10200, f) : 0;
    fclose(f);
    if (n < 0x10100 || memcmp(buf, "EDLA", 4) != 0) {
        DBG(1, "%s: not an EDLA firmware image\n", path);
        free(buf);
        return 0;
    }
    DBG(2, "firmware: %s\n", path);
    *out = buf;
    return 1;
}

static SANE_Status upload_firmware(v39_t *s)
{
    char p[PATH_MAX];
    const char *env;
    uint8_t *fw = NULL, sum = 0;
    uint8_t st;
    int i, found = 0;
    SANE_Status r;
    double t0;

    env = getenv("SANE_EPSONV39_FIRMWARE");
    found = try_load_fw(env, &fw) || try_load_fw(fw_path_conf, &fw);
    if (!found) {
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg) {
            snprintf(p, sizeof p, "%s/sane/epsonv39/esfw010c.bin", xdg);
            found = try_load_fw(p, &fw);
        }
        if (!found && home) {
            snprintf(p, sizeof p, "%s/.local/share/sane/epsonv39/esfw010c.bin", home);
            found = try_load_fw(p, &fw);
        }
    }
    if (!found)
        found = try_load_fw("/usr/share/sane/epsonv39/esfw010c.bin", &fw) ||
                try_load_fw("/usr/local/share/sane/epsonv39/esfw010c.bin", &fw) ||
                try_load_fw("/usr/share/iscan/esfw010c.bin", &fw) ||
                try_load_fw("/usr/lib/firmware/epson/esfw010c.bin", &fw);
    if (!found) {
        DBG(0, "firmware esfw010c.bin not found; set 'firmware' in epsonv39.conf\n");
        return SANE_STATUS_INVAL;
    }

    for (i = 0; i < 0x10000; i++)
        sum += fw[0x100 + i];

    /*
     * The boot loader is slow: the image write and the checksum ACK can take
     * several seconds.  A timeout here leaves the device mid-download, and
     * only a replug recovers it, so use generous timeouts throughout.
     */
    DBG(1, "uploading firmware\n");
    t0 = now_s();
    r = cmd_action(s, 0x1B, 0x06);
    if (r == SANE_STATUS_GOOD) r = usb_write_t(s, fw + 0x4C, 4, TIMEOUT_FW);
    if (r == SANE_STATUS_GOOD) r = usb_write_t(s, fw + 0x100, 65520, TIMEOUT_FW);
    if (r == SANE_STATUS_GOOD) r = usb_write_t(s, fw + 0x100 + 65520, 16, TIMEOUT_FW);
    if (r == SANE_STATUS_GOOD) r = usb_write_t(s, &sum, 1, TIMEOUT_FW);
    DBG(2, "firmware sent in %.2f s\n", now_s() - t0);
    if (r == SANE_STATUS_GOOD) r = expect_ack_t(s, TIMEOUT_FW);
    DBG(2, "firmware checksum ACK after %.2f s\n", now_s() - t0);
    if (r == SANE_STATUS_GOOD) r = usb_write_t(s, "\x1b\x16", 2, TIMEOUT_FW);
    if (r == SANE_STATUS_GOOD) r = expect_ack_t(s, TIMEOUT_FW);
    if (r == SANE_STATUS_GOOD) {
        uint8_t go = 0x80;
        r = usb_write_t(s, &go, 1, TIMEOUT_FW);
    }
    if (r == SANE_STATUS_GOOD) r = expect_ack_t(s, TIMEOUT_FW);
    DBG(2, "firmware started after %.2f s\n", now_s() - t0);
    free(fw);
    if (r != SANE_STATUS_GOOD)
        return r;

    /*
     * After start the firmware initialises (status 1A, often with 0x40 set
     * while the carriage homes).  Commands other than status/identify sent
     * before bit 0x02 clears may never be ACKed, which desynchronises the
     * protocol.  Wait for 18 (bounded; continue if 0x02 sticks).
     */
    t0 = now_s();
    {
        uint8_t last = 0xFF;
        for (;;) {
            CHK(get_status(s, &st));
            if (st != last) {
                DBG(2, "firmware boot: status %02x at %.2f s\n", st, now_s() - t0);
                last = st;
            }
            if ((st & 0x10) && !(st & 0x42))
                break;
            if (now_s() - t0 > 30) {
                if (!(st & 0x10)) {
                    DBG(1, "firmware did not start (status %02x)\n", st);
                    return SANE_STATUS_IO_ERROR;
                }
                DBG(1, "status still %02x after firmware start; continuing\n", st);
                break;
            }
            sleep_ms(50);
        }
    }
    sleep_ms(200);
    return SANE_STATUS_GOOD;
}

/* firmware check + ESC @ equivalent (PROTOCOL.md 6) */
static SANE_Status device_init(v39_t *s)
{
    uint8_t st, buf[32], lut[256];
    int i;

    CHK(get_status(s, &st));
    DBG(2, "status %02x\n", st);
    if (!(st & 0x10))
        CHK(upload_firmware(s));
    /* commands other than status are ignored while the carriage moves */
    CHK(wait_idle(s, 60));
    {
        double t0 = now_s();
        CHK(get_status(s, &st));
        while ((st & 0x02) && now_s() - t0 < 30) {
            sleep_ms(50);
            CHK(get_status(s, &st));
        }
        if (st & 0x02)
            DBG(1, "status bit 0x02 still set (%02x)\n", st);
    }

    CHK(cmd_query(s, 0x1B, 0x13, buf, 28));
    buf[28] = 0;
    DBG(1, "device: '%s'\n", (char *)buf);

    CHK(wait_idle(s, 30));
    CHK(mem_read(s, 0x0B, 0, buf, 10));
    for (i = 0; i < 256; i++)
        lut[i] = i;
    for (i = 0; i < 3; i++)
        CHK(mem_write(s, 0x03, 0x021FFC00 + 0x100 * i, lut, 256));
    {
        uint8_t p[4] = { 0x0B, 0, 4, 0 };
        CHK(op2(s, 0x1E, 0xE4));
        CHK(expect_ack(s));
        CHK(usb_write(s, p, 4));
        CHK(expect_ack(s));
        CHK(usb_read(s, buf, 4, TIMEOUT_CMD));
    }
    return cmd_action(s, 0x1B, 0x40);
}

/* ------------------------------------------------------------------ */
/* calibration maths (PROTOCOL.md 7.4)                                 */

static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* min/max over 8-pixel box averages within each half of a line mean */
static void box8(const int64_t *m, int W, int want_max, int64_t res[2])
{
    int h;
    for (h = 0; h < 2; h++) {
        int n = W / 2, x;
        int64_t acc = 0, best = want_max ? INT64_MIN : INT64_MAX;
        for (x = 0; x < n; x++) {
            acc += m[2 * x + h];
            if (x >= 8)
                acc -= m[2 * (x - 8) + h];
            if (x >= 7) {
                int64_t b = acc / 8;
                if (want_max ? b > best : b < best)
                    best = b;
            }
        }
        res[h] = best;
    }
}

#define AFE_LO 0x380
#define AFE_HI 0x480

/* program offsets, read 4 dark lines, return the per-half measure (7.4) */
static SANE_Status afe_measure(v39_t *s, const hwres_t *hw, int b10,
                               const uint16_t off[2], int64_t meas[2])
{
    const int lines = 4, lb = hw->W * 2;
    uint8_t blk[10] = { 0x00, hw->b9, b10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80, 0x00 };
    uint8_t *buf = malloc((size_t)lines * lb);
    int64_t *m = malloc(sizeof(int64_t) * hw->W);
    int x, l;
    SANE_Status st;

    if (!buf || !m) {
        free(buf);
        free(m);
        return SANE_STATUS_NO_MEM;
    }
    st = set_afe(s, off[0], off[1]);
    if (st == SANE_STATUS_GOOD)
        st = read_pass(s, hw, lines, blk, lb, buf);
    if (st == SANE_STATUS_GOOD) {
        for (x = 0; x < hw->W; x++) {
            int64_t a = 0;
            for (l = 0; l < lines; l++)
                a += rd16(buf + (size_t)l * lb + 2 * x);
            m[x] = a / lines;
        }
        box8(m, hw->W, 0, meas);
        DBG(3, "AFE offsets %02x/%02x -> %lld/%lld\n", off[0], off[1],
            (long long)meas[0], (long long)meas[1]);
    }
    free(buf);
    free(m);
    return st;
}

/* successive-approximation search per sensor half, starting at 0x80 */
static SANE_Status afe_search(v39_t *s, const hwres_t *hw, int b10)
{
    uint16_t off[2] = { 0x80, 0x80 }, bit[2] = { 0x40, 0x40 };
    int done[2] = { 0, 0 }, iter, h;
    SANE_Status st = SANE_STATUS_GOOD;

    for (iter = 0; iter < 10 && !(done[0] && done[1]); iter++) {
        int64_t meas[2];
        st = afe_measure(s, hw, b10, off, meas);
        if (st != SANE_STATUS_GOOD)
            return st;
        for (h = 0; h < 2; h++) {
            if (done[h])
                continue;
            if ((meas[h] >= AFE_LO && meas[h] <= AFE_HI) || bit[h] == 0) {
                done[h] = 1;
                continue;
            }
            if (meas[h] > AFE_HI)
                off[h] &= ~(bit[h] << 1);
            off[h] |= bit[h];
            bit[h] >>= 1;
        }
    }
    s->afe[0] = off[0];
    s->afe[1] = off[1];
    DBG(2, "AFE offsets: L=%02x R=%02x\n", off[0], off[1]);
    return set_afe(s, off[0], off[1]);
}

/* one read at the current offsets; *ok = both halves inside the window */
static SANE_Status afe_verify(v39_t *s, const hwres_t *hw, int b10, int *ok)
{
    int64_t meas[2];
    CHK(afe_measure(s, hw, b10, s->afe, meas));
    *ok = meas[0] >= AFE_LO && meas[0] <= AFE_HI && meas[1] >= AFE_LO && meas[1] <= AFE_HI;
    return SANE_STATUS_GOOD;
}

static SANE_Status led_search(v39_t *s, const hwres_t *hw, int b10, int *reads)
{
    const int lines = 4, lb = hw->W * 3;
    uint8_t blk[10] = { 0x01, hw->b9, b10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00 };
    uint8_t *buf = malloc((size_t)lines * lb);
    int64_t *m = malloc(sizeof(int64_t) * hw->W);
    int iter, c, x, l;
    SANE_Status st = SANE_STATUS_GOOD;

    if (!buf || !m) {
        free(buf);
        free(m);
        return SANE_STATUS_NO_MEM;
    }
    for (iter = 0; iter < 8; iter++) {
        int ok = 1;
        st = set_led(s, hw, 2, s->duty, 0);
        if (st == SANE_STATUS_GOOD)
            st = read_pass(s, hw, lines, blk, lb, buf);
        if (st != SANE_STATUS_GOOD)
            break;
        for (c = 0; c < 3; c++) {
            int64_t r[2], peak;
            /* targets: 0x698000/g ... with g = max(1E 43 word, 0x8000) = 0x8000 */
            const int lo = 0x698000 / 0x8000, tgt = 0x6B8000 / 0x8000, hi = 0x6D8000 / 0x8000;
            for (x = 0; x < hw->W; x++) {
                int64_t a = 0;
                for (l = 0; l < lines; l++)
                    a += buf[(size_t)l * lb + (size_t)c * hw->W + x];
                m[x] = a / lines;
            }
            box8(m, hw->W, 1, r);
            peak = r[0] > r[1] ? r[0] : r[1];
            DBG(3, "LED %c duty %u peak %lld\n", "RGB"[c], s->duty[c], (long long)peak);
            if (peak < lo || peak > hi) {
                long d = peak > 0 ? ((long)s->duty[c] * tgt + peak / 2) / peak : (long)s->duty[c] * 2;
                if (d < 1)
                    d = 1;
                if (d > hw->c)
                    d = hw->c;
                if (d != s->duty[c])
                    ok = 0;
                s->duty[c] = d;
            }
        }
        if (ok)
            break;
    }
    free(buf);
    free(m);
    if (reads)
        *reads = iter + 1;
    DBG(2, "LED duties: %u %u %u (%d reads)\n", s->duty[0], s->duty[1], s->duty[2], iter + 1);
    return st;
}

/* builds the shading table from 16-bit dark/white passes (raw pixel order) */
static SANE_Status shading(v39_t *s, const hwres_t *hw, int P,
                           const uint8_t *dark, int dlines,
                           const uint8_t *white, int wlines, uint16_t **table)
{
    const int W = hw->W;
    const size_t lb = (size_t)W * P * 2;
    int64_t *A = calloc(W, sizeof(int64_t));
    int64_t *B = malloc(sizeof(int64_t) * W * P);
    int64_t *g = malloc(sizeof(int64_t) * W);
    uint16_t *T = malloc(sizeof(uint16_t) * W * P);
    int64_t blk[2], amax[2], rng, base[3] = { 0, 0, 0 }, rngb = 0;
    int sel = 0, sha = 0, shb = 0, x, l, c, gi, n;

    if (!A || !B || !g || !T) {
        free(A); free(B); free(g); free(T);
        return SANE_STATUS_NO_MEM;
    }
    for (l = 0; l < dlines; l++)
        for (c = 0; c < P; c++)
            for (x = 0; x < W; x++)
                A[x] += rd16(dark + l * lb + ((size_t)c * W + x) * 2);
    for (x = 0; x < W; x++)
        A[x] /= (int64_t)dlines * P;
    blk[0] = blk[1] = INT64_MAX;
    amax[0] = amax[1] = INT64_MIN;
    for (x = 0; x < W; x++) {
        if (A[x] < blk[x & 1]) blk[x & 1] = A[x];
        if (A[x] > amax[x & 1]) amax[x & 1] = A[x];
    }
    rng = amax[0] - blk[0] > amax[1] - blk[1] ? amax[0] - blk[0] : amax[1] - blk[1];
    if (rng < 0x20) sel = 3;
    else if (rng < 0x40) sel = 2;
    else if (rng < 0x80) sel = 1;
    else if (rng < 0x100) sel = 0;
    else {
        sel = 0;
        for (sha = 1; sha < 8 && rng >= (0x100LL << sha); sha++)
            ;
    }
    for (x = 0; x < W; x++)
        A[x] -= blk[x & 1];

    n = wlines / 4;
    for (c = 0; c < P; c++) {
        int64_t bmin = INT64_MAX, bmax = INT64_MIN;
        for (x = 0; x < W; x++)
            B[c * W + x] = 0;
        for (gi = 0; gi < 4; gi++) {
            for (x = 0; x < W; x++)
                g[x] = 0;
            for (l = gi * n; l < (gi + 1) * n; l++)
                for (x = 0; x < W; x++)
                    g[x] += rd16(white + l * lb + ((size_t)c * W + x) * 2);
            for (x = 0; x < W; x++) {
                int64_t v = g[x] / n;
                if (v > B[c * W + x])
                    B[c * W + x] = v;
            }
        }
        for (x = 0; x < W; x++) {
            int64_t v = (B[c * W + x] - A[x]) & 0xFFFF;
            B[c * W + x] = v;
            if (v < bmin) bmin = v;
            if (v > bmax) bmax = v;
        }
        base[c] = bmin;
        if (bmax - bmin > rngb)
            rngb = bmax - bmin;
    }
    while (rngb >= (1LL << (8 + sel + shb)))
        shb++;
    for (c = 0; c < P; c++)
        for (x = 0; x < W; x++)
            T[c * W + x] = (uint16_t)((((B[c * W + x] - base[c]) >> shb) << (8 - sel)) | (A[x] >> sha));

    for (c = 0; c < 3; c++)
        s->w42[c] = (uint16_t)(P == 3 ? base[c] : base[0]);
    for (c = 0; c < 3; c++) {
        s->w42[3 + 2 * c] = (uint16_t)blk[0];
        s->w42[4 + 2 * c] = (uint16_t)blk[1];
    }
    s->shade_sel = sel;
    s->shade_b15 = (shb << 4) | sha;
    DBG(2, "shading: black %lld/%lld base %04x %04x %04x sel %d b15 %02x\n",
        (long long)blk[0], (long long)blk[1], s->w42[0], s->w42[1], s->w42[2], sel, s->shade_b15);
    free(A); free(B); free(g);
    *table = T;
    return SANE_STATUS_GOOD;
}

/* ------------------------------------------------------------------ */
/* scan parameters                                                     */

static const hwres_t *find_hw(int dpi)
{
    int i;
    for (i = 0; i < N_HWRES; i++)
        if (hwres[i].dpi == dpi)
            return &hwres[i];
    return NULL;
}

static void calc_params(v39_t *s)
{
    scanparm_t *p = &s->sp;
    double tlx = SANE_UNFIX(s->val[OPT_TL_X]) / 25.4, tly = SANE_UNFIX(s->val[OPT_TL_Y]) / 25.4;
    double brx = SANE_UNFIX(s->val[OPT_BR_X]) / 25.4, bry = SANE_UNFIX(s->val[OPT_BR_Y]) / 25.4;
    int dpi = s->val[OPT_RESOLUTION];
    int hwdpi, avail;

    if (brx < tlx) { double t = brx; brx = tlx; tlx = t; }
    if (bry < tly) { double t = bry; bry = tly; tly = t; }

    p->out_mode = !strcmp(s->mode, SANE_VALUE_SCAN_MODE_COLOR) ? 0
                : !strcmp(s->mode, SANE_VALUE_SCAN_MODE_GRAY) ? 1 : 2;
    if (s->val[OPT_PREVIEW])
        dpi = 75;
    p->out_dpi = dpi;
    hwdpi = dpi < 300 ? 300 : dpi;
    p->hw = find_hw(hwdpi);
    /* gray at 4800 dpi: no captured motor mode, scan colour and convert */
    p->colour_dev = p->out_mode == 0 || p->hw->mimg_g == 0;
    p->planes = p->colour_dev ? 3 : 1;
    p->v = p->colour_dev ? p->hw->v_c : p->hw->v_g;

    p->out_w = (int)((brx - tlx) * dpi + 0.5);
    p->out_h = (int)((bry - tly) * dpi + 0.5);
    if (p->out_w < 1) p->out_w = 1;
    if (p->out_h < 1) p->out_h = 1;
    if (p->out_mode == 2)
        p->out_w = (p->out_w + 7) & ~7;

    p->hw_x0 = (int)(tlx * hwdpi + 0.5);
    p->hw_w = (int)(((long long)p->out_w * hwdpi + dpi - 1) / dpi);
    p->hw_h = (int)(((long long)p->out_h * hwdpi + dpi - 1) / dpi);
    avail = 2 * (p->hw->W / 2 - p->hw->dummy) - 17 * hwdpi / 300;
    if (p->hw_x0 + p->hw_w > avail) {
        p->hw_x0 = avail - p->hw_w;
        if (p->hw_x0 < 0) {
            p->hw_x0 = 0;
            p->hw_w = avail;
            p->out_w = (int)((long long)avail * dpi / hwdpi);
            if (p->out_mode == 2)
                p->out_w &= ~7;
        }
    }
    p->y_in = tly;
    if (tly * 1200 + (double)p->hw_h * 1200 / hwdpi > MAX_Y_MM / 25.4 * 1200 + 1) {
        p->hw_h = (int)((MAX_Y_MM / 25.4 - tly) * hwdpi);
        p->out_h = (int)((long long)p->hw_h * dpi / hwdpi);
        if (p->out_h < 1) p->out_h = 1;
    }
    p->bpl = p->out_mode == 0 ? 3 * p->out_w : p->out_mode == 1 ? p->out_w : p->out_w / 8;
    p->raw_lines = p->hw_h * p->v;
    p->raw_line_bytes = p->hw->W * p->planes;
    p->lpt = calc_lpt(p->raw_lines, p->raw_line_bytes);
}

/* ------------------------------------------------------------------ */
/* output queue                                                        */

static int q_put(v39_t *s, const uint8_t *d, size_t n)
{
    pthread_mutex_lock(&s->mtx);
    while (n > 0) {
        size_t tail, chunk;
        while (s->qlen == s->qcap && !s->cancel)
            pthread_cond_wait(&s->cond, &s->mtx);
        if (s->cancel) {
            pthread_mutex_unlock(&s->mtx);
            return -1;
        }
        tail = (s->qhead + s->qlen) % s->qcap;
        chunk = s->qcap - s->qlen;
        if (chunk > s->qcap - tail)
            chunk = s->qcap - tail;
        if (chunk > n)
            chunk = n;
        memcpy(s->q + tail, d, chunk);
        s->qlen += chunk;
        d += chunk;
        n -= chunk;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->mtx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* image reader thread                                                 */

typedef struct {
    v39_t *s;
    int *map;            /* raw byte index (within a plane) per hw pixel  */
    uint32_t *vacc;      /* vertical accumulation over v raw lines        */
    int vcount;
    uint32_t *dacc;      /* down-sampling accumulation (planes x out_w)   */
    int dcount, hw_row, out_row;
    uint8_t *line;       /* hw line (planes x hw_w)                        */
    uint8_t *out;
} pipe_t;

static int emit_line(pipe_t *pp, const uint8_t *hwline)
{
    v39_t *s = pp->s;
    scanparm_t *p = &s->sp;
    int x, c, P = p->planes, hwdpi = p->hw->dpi;
    const uint8_t *src = hwline;
    uint8_t *tmp = NULL;

    if (pp->out_row >= p->out_h)
        return 0;

    if (p->out_dpi < hwdpi) {
        /* area down-sampling from 300 dpi */
        int row_end = (int)((long long)(pp->out_row + 1) * hwdpi / p->out_dpi);
        for (c = 0; c < P; c++)
            for (x = 0; x < p->out_w; x++) {
                int x0 = (int)((long long)x * hwdpi / p->out_dpi);
                int x1 = (int)((long long)(x + 1) * hwdpi / p->out_dpi), i;
                uint32_t a = 0;
                for (i = x0; i < x1 && i < p->hw_w; i++)
                    a += hwline[c * p->hw_w + i];
                pp->dacc[c * p->out_w + x] += a / (x1 - x0);
            }
        pp->dcount++;
        pp->hw_row++;
        if (pp->hw_row < row_end)
            return 0;
        tmp = pp->line + (size_t)P * p->hw_w;   /* scratch after the hw line */
        for (c = 0; c < P; c++)
            for (x = 0; x < p->out_w; x++) {
                uint32_t *a = &pp->dacc[c * p->out_w + x];
                tmp[c * p->out_w + x] = (*a + pp->dcount / 2) / pp->dcount;
                *a = 0;
            }
        pp->dcount = 0;
        src = tmp;
    }
    /* src: P planes of out_w pixels */
    {
        int W = p->out_dpi < hwdpi ? p->out_w : p->hw_w;
        uint8_t *o = pp->out;
        if (p->out_mode == 0) {
            for (x = 0; x < p->out_w; x++) {
                o[3 * x] = src[x];
                o[3 * x + 1] = src[W + x];
                o[3 * x + 2] = src[2 * W + x];
            }
        } else {
            for (x = 0; x < p->out_w; x++) {
                int g = P == 3 ? (src[x] + src[W + x] + src[2 * W + x] + 1) / 3 : src[x];
                if (p->out_mode == 1)
                    o[x] = g;
                else {
                    if (!(x & 7))
                        o[x >> 3] = 0;
                    if (g < s->val[OPT_THRESHOLD])
                        o[x >> 3] |= 0x80 >> (x & 7);
                }
            }
        }
        pp->out_row++;
        return q_put(s, o, p->bpl);
    }
}

static int process_raw_line(pipe_t *pp, const uint8_t *raw)
{
    scanparm_t *p = &pp->s->sp;
    int c, x, P = p->planes, W = p->hw->W;

    for (c = 0; c < P; c++) {
        const uint8_t *pl = raw + (size_t)c * W;
        uint32_t *a = pp->vacc + (size_t)c * p->hw_w;
        for (x = 0; x < p->hw_w; x++)
            a[x] += pl[pp->map[x]];
    }
    if (++pp->vcount < p->v)
        return 0;
    for (c = 0; c < P * p->hw_w; c++) {
        pp->line[c] = (pp->vacc[c] + p->v / 2) / p->v;
        pp->vacc[c] = 0;
    }
    pp->vcount = 0;
    return emit_line(pp, pp->line);
}

static void *reader_thread(void *arg)
{
    v39_t *s = arg;
    scanparm_t *p = &s->sp;
    pipe_t pp;
    int left = p->raw_lines, i;
    int N = p->hw->W / 2, k = p->hw->k, half_out = N - p->hw->dummy;
    int xoff = 17 * p->hw->dpi / 300;
    size_t tsize = (size_t)p->lpt * p->raw_line_bytes + 8;
    uint8_t *tbuf = malloc(tsize);
    SANE_Status st = SANE_STATUS_GOOD;

    memset(&pp, 0, sizeof pp);
    pp.s = s;
    pp.map = malloc(sizeof(int) * p->hw_w);
    pp.vacc = calloc((size_t)p->planes * p->hw_w, sizeof(uint32_t));
    pp.dacc = calloc((size_t)p->planes * p->out_w, sizeof(uint32_t));
    pp.line = malloc((size_t)p->planes * (p->hw_w + p->out_w));
    pp.out = malloc(p->bpl + 1);
    if (!tbuf || !pp.map || !pp.vacc || !pp.dacc || !pp.line || !pp.out) {
        st = SANE_STATUS_NO_MEM;
        goto out;
    }
    /* unscramble map (PROTOCOL.md 7.2): unscrambled pixel u -> raw byte */
    for (i = 0; i < p->hw_w; i++) {
        int u = p->hw_x0 + xoff + i, half = 0, pos;
        if (u >= half_out) {
            half = 1;
            u -= half_out;
        }
        pos = (u % k) * (N / k) + u / k;
        pp.map[i] = 2 * pos + half;
    }

    while (left > 0) {
        int n = left < p->lpt ? left : p->lpt;
        int got = 0, r;
        if (s->cancel) {
            st = SANE_STATUS_CANCELLED;
            break;
        }
        r = libusb_bulk_transfer(s->h, EP_IN, tbuf, n * p->raw_line_bytes + 8, &got, TIMEOUT_IMG);
        if (r < 0 || got != n * p->raw_line_bytes + 8) {
            DBG(1, "image read failed: %s (%d/%d)\n", libusb_error_name(r), got,
                n * p->raw_line_bytes + 8);
            st = SANE_STATUS_IO_ERROR;
            break;
        }
        s->transfers_left--;
        DBG(4, "transfer %d lines, trailer %02x %02x %02x %02x %02x %02x %02x %02x\n", n,
            tbuf[got - 8], tbuf[got - 7], tbuf[got - 6], tbuf[got - 5],
            tbuf[got - 4], tbuf[got - 3], tbuf[got - 2], tbuf[got - 1]);
        for (i = 0; i < n; i++)
            if (process_raw_line(&pp, tbuf + (size_t)i * p->raw_line_bytes) < 0) {
                st = SANE_STATUS_CANCELLED;
                break;
            }
        if (st != SANE_STATUS_GOOD)
            break;
        left -= n;
    }
    /* pad if the device delivered fewer lines than promised (should not happen) */
    if (st == SANE_STATUS_GOOD && pp.out_row < p->out_h) {
        DBG(1, "padding %d missing lines\n", p->out_h - pp.out_row);
        memset(pp.out, p->out_mode == 2 ? 0 : 0xFF, p->bpl);
        while (pp.out_row < p->out_h && q_put(s, pp.out, p->bpl) == 0)
            pp.out_row++;
    }
out:
    free(tbuf);
    free(pp.map);
    free(pp.vacc);
    free(pp.dacc);
    free(pp.line);
    free(pp.out);
    pthread_mutex_lock(&s->mtx);
    s->thread_status = st;
    s->qdone = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mtx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* calibration cache in device RAM                                     */

/*
 * Epson's interpreter keeps a calibration cache in device RAM (type 09):
 * 12 bytes of data at 0x021FFF00 and a 16-byte key at 0x021FFF10.  RAM is
 * lost on power-off, so the cache dies with the firmware.  We use the same
 * two regions with our own tagged format, so neither driver can mistake the
 * other's entry for a hit (PROTOCOL.md 5.2):
 *
 *   data: u16 duty[3], u16 afe[2], u16 0
 *   key : "V39S", u16 version, u16 hw dpi, u8 colour, u8 0, u16 0,
 *         u32 FNV-1a over data and key bytes 0..11
 */
#define CACHE_DATA 0x021FFF00
#define CACHE_KEY  0x021FFF10
#define CACHE_VERSION 1

static uint32_t fnv1a(const uint8_t *a, size_t n, const uint8_t *b, size_t m)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ a[i]) * 16777619u;
    for (i = 0; i < m; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void cache_build(const v39_t *s, const hwres_t *hw, int colour, uint8_t data[12], uint8_t key[16])
{
    int i;
    memset(data, 0, 12);
    memset(key, 0, 16);
    for (i = 0; i < 3; i++)
        put16(data + 2 * i, s->duty[i]);
    put16(data + 6, s->afe[0]);
    put16(data + 8, s->afe[1]);
    memcpy(key, "V39S", 4);
    put16(key + 4, CACHE_VERSION);
    put16(key + 6, hw->dpi);
    key[8] = colour ? 1 : 0;
    put32(key + 12, fnv1a(data, 12, key, 12));
}

/* returns 1 and fills s->duty / s->afe on a valid entry for this mode */
static int cache_load(v39_t *s, const hwres_t *hw, int colour)
{
    uint8_t data[12], key[16], want_data[12], want_key[16];
    int i;
    if (mem_read(s, 0x09, CACHE_KEY, key, 16) != SANE_STATUS_GOOD ||
        mem_read(s, 0x09, CACHE_DATA, data, 12) != SANE_STATUS_GOOD)
        return 0;
    if (memcmp(key, "V39S", 4) != 0) {
        DBG(2, "calibration cache: no entry of ours\n");
        return 0;
    }
    if (rd16(key + 4) != CACHE_VERSION || rd16(key + 6) != hw->dpi || key[8] != (colour ? 1 : 0)) {
        DBG(2, "calibration cache: entry for %u dpi %s, need %d dpi %s\n", rd16(key + 6),
            key[8] ? "colour" : "gray", hw->dpi, colour ? "colour" : "gray");
        return 0;
    }
    if ((uint32_t)(key[12] | key[13] << 8 | key[14] << 16 | (uint32_t)key[15] << 24) !=
        fnv1a(data, 12, key, 12)) {
        DBG(1, "calibration cache: checksum mismatch\n");
        return 0;
    }
    for (i = 0; i < 3; i++) {
        s->duty[i] = rd16(data + 2 * i);
        if (s->duty[i] == 0 || s->duty[i] > hw->c)
            return 0;
    }
    s->afe[0] = rd16(data + 6);
    s->afe[1] = rd16(data + 8);
    if (s->afe[0] > 0xFF || s->afe[1] > 0xFF)
        return 0;
    cache_build(s, hw, colour, want_data, want_key);   /* sanity: round trip */
    if (memcmp(want_data, data, 12) != 0)
        return 0;
    DBG(2, "calibration cache: hit (AFE %02x/%02x, duties %u %u %u)\n",
        s->afe[0], s->afe[1], s->duty[0], s->duty[1], s->duty[2]);
    return 1;
}

static SANE_Status cache_store(v39_t *s, const hwres_t *hw, int colour)
{
    uint8_t data[12], key[16];
    cache_build(s, hw, colour, data, key);
    /* data first, key last: a half-written entry never validates */
    CHK(mem_write(s, 0x09, CACHE_DATA, data, 12));
    return mem_write(s, 0x09, CACHE_KEY, key, 16);
}

/* ------------------------------------------------------------------ */
/* scan sequence (PROTOCOL.md 7.1)                                     */

static SANE_Status do_calibrate_and_start(v39_t *s)
{
    scanparm_t *p = &s->sp;
    const hwres_t *hw = p->hw;
    const int colour = p->colour_dev;
    const int b10 = colour ? hw->b10_c : hw->b10_g;
    const int ledmode_img = colour ? 2 : 1;     /* gray scans use the mode-1 block */
    const uint16_t zero9[9] = { 0 };
    uint8_t rec[42], r6[6], r4[4];
    uint8_t *dark = NULL, *white = NULL;
    uint16_t *table = NULL;
    int P = p->planes, dlines, wlines, reads, per, i;
    size_t dlb = (size_t)hw->W * P * 2;
    SANE_Status st;
    double t0 = now_s();

    CHK(wait_idle(s, 60));

    /* 1. parameter record (bookkeeping) */
    CHK(cmd_query(s, 0x1E, 0x53, rec, 42));
    memset(rec, 0, sizeof rec);
    put32(rec, hw->dpi);
    put32(rec + 4, hw->dpi * p->v);
    put32(rec + 8, p->hw_x0);
    put32(rec + 12, (uint32_t)(p->y_in * hw->dpi * p->v + 0.5));
    put32(rec + 16, hw->W);
    put32(rec + 20, p->raw_lines);
    rec[24] = colour ? 0x13 : 0x00;
    rec[25] = 8;
    rec[29] = 3;
    rec[33] = 0x80;
    CHK(cmd_set(s, 0x1E, 0x57, rec, 42));

    /* 2. prepare */
    CHK(set_u8(s, 0xA2, 1));
    {
        uint8_t z = 0;
        CHK(cmd_setget(s, 0x1E, 0x87, &z, 1, r6, 6));
    }
    {
        uint8_t a[4];
        put32(a, 0x0037F844);
        CHK(cmd_setget(s, 0x1B, 0x25, a, 4, r4, 4));
    }

    /*
     * 3. AFE offsets and LED exposure.  Full calibration runs AFE, LED,
     * AFE as Epson's interpreter does.  With a cache entry for this mode,
     * one dark read checks the cached offsets and the LED search starts
     * from the cached duties, so it normally stops after its first read.
     */
    {
        int hit = s->val[OPT_CAL_CACHE] && cache_load(s, hw, colour);
        int afe_ok = 0, reads = 0;

        if (!hit) {
            for (i = 0; i < 3; i++)
                s->duty[i] = hw->duty0;
            s->afe[0] = s->afe[1] = 0x80;
        }
        CHK(set_afe(s, s->afe[0], s->afe[1]));
        CHK(set_u8(s, 0xA2, 0));
        CHK(set_u8(s, 0x5A, 0));
        CHK(set_u8(s, 0x11, hw->lamp));
        sleep_ms(30);
        CHK(set_reg21(s, hw->reg21));
        CHK(set_led(s, hw, colour ? 2 : 1, NULL, 0));
        CHK(set_words(s, 0x42, zero9, 9));
        CHK(set_words(s, 0x43, w43_cal, 6));
        CHK(set_led(s, hw, 1, s->duty, 0));
        if (hit)
            CHK(afe_verify(s, hw, b10, &afe_ok));
        if (!afe_ok) {
            if (hit)
                DBG(2, "calibration cache: AFE out of window, searching\n");
            CHK(afe_search(s, hw, b10));
        }

        CHK(set_u8(s, 0xA2, 1));
        CHK(set_u8(s, 0x5A, 0));
        CHK(set_reg21(s, hw->reg21));
        CHK(set_words(s, 0x42, zero9, 9));
        CHK(set_words(s, 0x43, w43_cal, 6));
        CHK(led_search(s, hw, b10, &reads));

        if (!hit || !afe_ok) {
            /* second AFE pass of the full calibration */
            CHK(set_u8(s, 0xA2, 0));
            CHK(set_u8(s, 0x5A, 0));
            CHK(set_reg21(s, hw->reg21));
            CHK(set_words(s, 0x42, zero9, 9));
            CHK(set_words(s, 0x43, w43_cal, 6));
            CHK(set_led(s, hw, 1, s->duty, 0));
            CHK(afe_search(s, hw, b10));
        }
        if (!hit || !afe_ok || reads > 1) {
            SANE_Status cs = cache_store(s, hw, colour);
            if (cs != SANE_STATUS_GOOD)
                return cs;
        }
        DBG(1, "calibration: %s\n", !hit ? "full" : afe_ok && reads == 1 ? "cached" : "cached, refreshed");
    }

    /* 4. dark pass (LEDs off: flag 1) */
    if (colour) {
        reads = hw->dark_reads_c;
        per = hw->dark_lines_c;
    } else {
        reads = 1;
        per = 32;
    }
    dlines = reads * per;
    wlines = colour ? hw->white_c : hw->white_g;
    dark = malloc(dlb * dlines);
    white = malloc(dlb * wlines);
    if (!dark || !white) {
        st = SANE_STATUS_NO_MEM;
        goto fail;
    }
    st = set_u8(s, 0xA2, 0);
    if (!st) st = set_u8(s, 0x5A, 0);
    if (!st) st = set_reg21(s, hw->reg21);
    if (!st) st = set_led(s, hw, ledmode_img, s->duty, 1);
    if (!st) st = set_words(s, 0x42, zero9, 9);
    if (!st) st = set_words(s, 0x43, w43_cal, 6);
    for (i = 0; i < reads && !st; i++) {
        uint8_t b[10] = { 0x00, hw->b9, b10, 0x00, 0x00, colour ? 0 : 1, 0x00, 0x00, 0x80, 0x00 };
        st = read_pass(s, hw, per, b, (int)dlb, dark + dlb * per * i);
    }
    if (st)
        goto fail;

    /* white pass over the calibration strip */
    {
        uint16_t w[9] = { 0 };
        uint8_t z = 0;
        uint8_t b[10] = { 0x00, hw->b9w, colour ? 3 : 1, 0x40, 0x00, colour ? 0 : 1, 0x00, 0x00, 0x80, 0x80 };
        int64_t bl[2] = { INT64_MAX, INT64_MAX };
        size_t x;
        /* black levels for 1E 42 during the white pass: per-half minimum of the dark mean */
        int64_t *A = calloc(hw->W, sizeof(int64_t));
        if (!A) {
            st = SANE_STATUS_NO_MEM;
            goto fail;
        }
        for (i = 0; i < dlines * P; i++)
            for (x = 0; x < (size_t)hw->W; x++)
                A[x] += rd16(dark + (size_t)i * hw->W * 2 + 2 * x);
        for (x = 0; x < (size_t)hw->W; x++) {
            A[x] /= (int64_t)dlines * P;
            if (A[x] < bl[x & 1])
                bl[x & 1] = A[x];
        }
        free(A);
        for (i = 0; i < 3; i++) {
            w[3 + 2 * i] = bl[0];
            w[4 + 2 * i] = bl[1];
        }
        st = cmd_setget(s, 0x1E, 0x87, &z, 1, r6, 6);
        if (!st) st = set_u8(s, 0xA2, 1);
        if (!st) st = set_u8(s, 0x5A, 0);
        if (!st) st = set_reg21(s, hw->reg21);
        if (!st) st = set_led(s, hw, ledmode_img, s->duty, 0);
        if (!st) st = set_words(s, 0x42, w, 9);
        if (!st) st = set_words(s, 0x43, w43_cal, 6);
        if (!st) st = move(s, hw->mcal, 48);
        if (!st) st = read_pass(s, hw, wlines, b, (int)dlb, white);
        if (st)
            goto fail;
    }

    /* 5. shading table */
    st = shading(s, hw, P, dark, dlines, white, wlines, &table);
    if (st)
        goto fail;
    free(dark);
    free(white);
    dark = white = NULL;
    st = mem_write(s, 0x06, 0x02012000, table, (uint32_t)hw->W * P * 2);
    free(table);
    table = NULL;
    if (st)
        goto fail;

    /* 6. position */
    {
        uint8_t a[4], mm = colour ? hw->mimg_c : hw->mimg_g;
        int soff = colour ? hw->soff_c : hw->soff_g;
        uint32_t steps = (uint32_t)(p->y_in * 1200 + 0.5) + soff;
        put32(a, 0x0037F844);
        CHK(cmd_setget(s, 0x1B, 0x25, a, 4, r4, 4));
        CHK(set_u8(s, 0x5A, 1));
        CHK(cmd_setget(s, 0x1B, 0x25, a, 4, r4, 4));
        CHK(cmd_setget(s, 0x1E, 0x08, &mm, 1, r4, 4));
        CHK(move(s, 0x1000, steps));
        CHK(cmd_action(s, 0x1E, 0x05));
        CHK(wait_move(s));
    }

    /* 7. final setup */
    {
        uint8_t mm = colour ? hw->mimg_c : hw->mimg_g;
        uint32_t steps = (uint32_t)(((long long)p->hw_h * 1200 + hw->dpi - 1) / hw->dpi);
        uint8_t b[10] = { 0x01, hw->b9, b10, 0x80 | s->shade_sel, 0x80, colour ? 0 : 1, 0x00,
                          s->shade_b15, 0x80, 0xA0 };
        CHK(set_reg21(s, hw->reg21));
        CHK(set_led(s, hw, ledmode_img, s->duty, 0));
        CHK(set_words(s, 0x42, s->w42, 9));
        CHK(set_words(s, 0x43, w43_img, 6));
        CHK(move(s, mm, steps));
        CHK(set_scan(s, hw, p->raw_lines, b, p->lpt));
        CHK(set_u8(s, 0xA2, 1));
        CHK(wait_idle(s, 30));
    }

    /* 8. start */
    DBG(2, "calibration done in %.2f s; scanning %d raw lines (%d x %d out)\n",
        now_s() - t0, p->raw_lines, p->out_w, p->out_h);
    s->transfers_left = (p->raw_lines + p->lpt - 1) / p->lpt;
    return op2(s, 0x1E, 0x47);

fail:
    free(dark);
    free(white);
    free(table);
    return st;
}

/* ------------------------------------------------------------------ */
/* config and discovery                                                */

static void read_config(void)
{
    char path[PATH_MAX], line[PATH_MAX + 32];
    const char *dirs = getenv("SANE_CONFIG_DIR");
    FILE *f = NULL;

    if (dirs) {
        char *copy = strdup(dirs), *save = NULL, *d;
        for (d = strtok_r(copy, ":", &save); d && !f; d = strtok_r(NULL, ":", &save)) {
            snprintf(path, sizeof path, "%s/" BACKEND_NAME ".conf", d);
            f = fopen(path, "r");
        }
        free(copy);
    }
    if (!f)
        f = fopen("/etc/sane.d/" BACKEND_NAME ".conf", "r");
    if (!f)
        f = fopen("/usr/local/etc/sane.d/" BACKEND_NAME ".conf", "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *l = line, *e;
        while (*l == ' ' || *l == '\t')
            l++;
        e = l + strlen(l);
        while (e > l && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        if (!*l || *l == '#')
            continue;
        if (!strncmp(l, "firmware", 8) && (l[8] == ' ' || l[8] == '\t')) {
            l += 8;
            while (*l == ' ' || *l == '\t')
                l++;
            snprintf(fw_path_conf, sizeof fw_path_conf, "%s", l);
            DBG(2, "config: firmware %s\n", fw_path_conf);
        }
    }
    fclose(f);
}

static void free_devices(void)
{
    while (devices) {
        v39dev_t *n = devices->next;
        free(devices);
        devices = n;
    }
    free(devlist);
    devlist = NULL;
}

static void probe_devices(void)
{
    libusb_device **list;
    ssize_t n, i;
    int cnt = 0;
    v39dev_t *d;

    free_devices();
    n = libusb_get_device_list(usbctx, &list);
    for (i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != V39_VID || desc.idProduct != V39_PID)
            continue;
        d = calloc(1, sizeof *d);
        if (!d)
            break;
        d->bus = libusb_get_bus_number(list[i]);
        d->addr = libusb_get_device_address(list[i]);
        snprintf(d->name, sizeof d->name, "libusb:%03u:%03u", d->bus, d->addr);
        d->sane.name = d->name;
        d->sane.vendor = "Epson";
        d->sane.model = "Perfection V39";
        d->sane.type = "flatbed scanner";
        d->next = devices;
        devices = d;
        cnt++;
    }
    if (n >= 0)
        libusb_free_device_list(list, 1);
    devlist = calloc(cnt + 1, sizeof *devlist);
    for (i = 0, d = devices; d && devlist; d = d->next)
        devlist[i++] = &d->sane;
}

/* ------------------------------------------------------------------ */
/* SANE API                                                            */

SANE_Status ENTRY(init)(SANE_Int *version_code, SANE_Auth_Callback authorize)
{
    const char *e = getenv("SANE_DEBUG_EPSONV39");
    (void)authorize;
    dbg_level = e ? atoi(e) : 0;
    DBG(1, "init, build %d\n", BUILD);
    if (version_code)
        *version_code = SANE_VERSION_CODE(SANE_CURRENT_MAJOR, 0, BUILD);
    if (libusb_init(&usbctx) != 0)
        return SANE_STATUS_IO_ERROR;
    read_config();
    return SANE_STATUS_GOOD;
}

void ENTRY(exit)(void)
{
    free_devices();
    if (usbctx)
        libusb_exit(usbctx);
    usbctx = NULL;
}

SANE_Status ENTRY(get_devices)(const SANE_Device ***list, SANE_Bool local_only)
{
    (void)local_only;
    probe_devices();
    if (!devlist)
        return SANE_STATUS_NO_MEM;
    *list = devlist;
    return SANE_STATUS_GOOD;
}

static void init_options(v39_t *s)
{
    SANE_Option_Descriptor *o = s->opt;
    int i;
    memset(o, 0, sizeof s->opt);
    for (i = 0; i < NUM_OPTIONS; i++) {
        o[i].size = sizeof(SANE_Word);
        o[i].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    }
    o[OPT_NUM_OPTS].name = SANE_NAME_NUM_OPTIONS;
    o[OPT_NUM_OPTS].title = SANE_TITLE_NUM_OPTIONS;
    o[OPT_NUM_OPTS].desc = SANE_DESC_NUM_OPTIONS;
    o[OPT_NUM_OPTS].type = SANE_TYPE_INT;
    o[OPT_NUM_OPTS].cap = SANE_CAP_SOFT_DETECT;
    s->val[OPT_NUM_OPTS] = NUM_OPTIONS;

    o[OPT_MODE_GROUP].title = SANE_I18N("Scan Mode");
    o[OPT_MODE_GROUP].type = SANE_TYPE_GROUP;
    o[OPT_MODE_GROUP].cap = 0;
    o[OPT_MODE_GROUP].size = 0;

    o[OPT_MODE].name = SANE_NAME_SCAN_MODE;
    o[OPT_MODE].title = SANE_TITLE_SCAN_MODE;
    o[OPT_MODE].desc = SANE_DESC_SCAN_MODE;
    o[OPT_MODE].type = SANE_TYPE_STRING;
    o[OPT_MODE].size = sizeof s->mode;
    o[OPT_MODE].constraint_type = SANE_CONSTRAINT_STRING_LIST;
    o[OPT_MODE].constraint.string_list = mode_list;
    strcpy(s->mode, SANE_VALUE_SCAN_MODE_COLOR);

    o[OPT_RESOLUTION].name = SANE_NAME_SCAN_RESOLUTION;
    o[OPT_RESOLUTION].title = SANE_TITLE_SCAN_RESOLUTION;
    o[OPT_RESOLUTION].desc = SANE_DESC_SCAN_RESOLUTION;
    o[OPT_RESOLUTION].type = SANE_TYPE_INT;
    o[OPT_RESOLUTION].unit = SANE_UNIT_DPI;
    o[OPT_RESOLUTION].constraint_type = SANE_CONSTRAINT_WORD_LIST;
    o[OPT_RESOLUTION].constraint.word_list = res_list;
    s->val[OPT_RESOLUTION] = 300;

    o[OPT_PREVIEW].name = SANE_NAME_PREVIEW;
    o[OPT_PREVIEW].title = SANE_TITLE_PREVIEW;
    o[OPT_PREVIEW].desc = SANE_DESC_PREVIEW;
    o[OPT_PREVIEW].type = SANE_TYPE_BOOL;
    s->val[OPT_PREVIEW] = SANE_FALSE;

    o[OPT_THRESHOLD].name = SANE_NAME_THRESHOLD;
    o[OPT_THRESHOLD].title = SANE_TITLE_THRESHOLD;
    o[OPT_THRESHOLD].desc = SANE_DESC_THRESHOLD;
    o[OPT_THRESHOLD].type = SANE_TYPE_INT;
    o[OPT_THRESHOLD].constraint_type = SANE_CONSTRAINT_RANGE;
    o[OPT_THRESHOLD].constraint.range = &thr_range;
    o[OPT_THRESHOLD].cap |= SANE_CAP_INACTIVE;
    s->val[OPT_THRESHOLD] = 128;

    o[OPT_CAL_CACHE].name = "calibration-cache";
    o[OPT_CAL_CACHE].title = SANE_I18N("Calibration cache");
    o[OPT_CAL_CACHE].desc = SANE_I18N("Reuse the AFE offsets and LED exposure measured by an earlier "
                                      "scan in the same mode, kept in scanner RAM until power-off. "
                                      "A quick check read still runs; turn this off to force a full "
                                      "calibration.");
    o[OPT_CAL_CACHE].type = SANE_TYPE_BOOL;
    o[OPT_CAL_CACHE].cap |= SANE_CAP_ADVANCED;
    s->val[OPT_CAL_CACHE] = SANE_TRUE;

    o[OPT_GEOMETRY_GROUP].title = SANE_I18N("Geometry");
    o[OPT_GEOMETRY_GROUP].type = SANE_TYPE_GROUP;
    o[OPT_GEOMETRY_GROUP].cap = 0;
    o[OPT_GEOMETRY_GROUP].size = 0;

    for (i = OPT_TL_X; i <= OPT_BR_Y; i++) {
        o[i].type = SANE_TYPE_FIXED;
        o[i].unit = SANE_UNIT_MM;
        o[i].constraint_type = SANE_CONSTRAINT_RANGE;
        o[i].constraint.range = (i == OPT_TL_X || i == OPT_BR_X) ? &x_range : &y_range;
    }
    o[OPT_TL_X].name = SANE_NAME_SCAN_TL_X;
    o[OPT_TL_X].title = SANE_TITLE_SCAN_TL_X;
    o[OPT_TL_X].desc = SANE_DESC_SCAN_TL_X;
    o[OPT_TL_Y].name = SANE_NAME_SCAN_TL_Y;
    o[OPT_TL_Y].title = SANE_TITLE_SCAN_TL_Y;
    o[OPT_TL_Y].desc = SANE_DESC_SCAN_TL_Y;
    o[OPT_BR_X].name = SANE_NAME_SCAN_BR_X;
    o[OPT_BR_X].title = SANE_TITLE_SCAN_BR_X;
    o[OPT_BR_X].desc = SANE_DESC_SCAN_BR_X;
    o[OPT_BR_Y].name = SANE_NAME_SCAN_BR_Y;
    o[OPT_BR_Y].title = SANE_TITLE_SCAN_BR_Y;
    o[OPT_BR_Y].desc = SANE_DESC_SCAN_BR_Y;
    o[OPT_SENSORS_GROUP].name = SANE_NAME_SENSORS;
    o[OPT_SENSORS_GROUP].title = SANE_TITLE_SENSORS;
    o[OPT_SENSORS_GROUP].type = SANE_TYPE_GROUP;
    o[OPT_SENSORS_GROUP].cap = SANE_CAP_ADVANCED;
    o[OPT_SENSORS_GROUP].size = 0;
    {
        static const struct { int opt; const char *name, *title, *desc; } btn[] = {
            { OPT_BUTTON_SCAN, SANE_NAME_SCAN, SANE_TITLE_SCAN, SANE_DESC_SCAN },
            { OPT_BUTTON_COPY, SANE_NAME_COPY, SANE_TITLE_COPY, SANE_DESC_COPY },
            { OPT_BUTTON_EMAIL, SANE_NAME_EMAIL, SANE_TITLE_EMAIL, SANE_DESC_EMAIL },
            { OPT_BUTTON_PDF, SANE_NAME_PDF, SANE_TITLE_PDF, SANE_DESC_PDF },
        };
        for (i = 0; i < 4; i++) {
            o[btn[i].opt].name = btn[i].name;
            o[btn[i].opt].title = btn[i].title;
            o[btn[i].opt].desc = btn[i].desc;
            o[btn[i].opt].type = SANE_TYPE_BOOL;
            o[btn[i].opt].cap = SANE_CAP_SOFT_DETECT | SANE_CAP_HARD_SELECT | SANE_CAP_ADVANCED;
            s->val[btn[i].opt] = SANE_FALSE;
        }
    }

    s->val[OPT_TL_X] = 0;
    s->val[OPT_TL_Y] = 0;
    s->val[OPT_BR_X] = SANE_FIX(MAX_X_MM);
    s->val[OPT_BR_Y] = SANE_FIX(MAX_Y_MM);
}

SANE_Status ENTRY(open)(SANE_String_Const name, SANE_Handle *handle)
{
    v39dev_t *d;
    v39_t *s;
    libusb_device **list;
    ssize_t n, i;
    int r;
    SANE_Status st;

    if (!devices)
        probe_devices();
    for (d = devices; d; d = d->next)
        if (!name || !*name || !strcmp(name, d->name))
            break;
    if (!d)
        return SANE_STATUS_INVAL;

    s = calloc(1, sizeof *s);
    if (!s)
        return SANE_STATUS_NO_MEM;
    s->dev = d;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cond, NULL);

    n = libusb_get_device_list(usbctx, &list);
    for (i = 0; i < n; i++)
        if (libusb_get_bus_number(list[i]) == d->bus && libusb_get_device_address(list[i]) == d->addr)
            break;
    r = i < n ? libusb_open(list[i], &s->h) : LIBUSB_ERROR_NO_DEVICE;
    if (n >= 0)
        libusb_free_device_list(list, 1);
    if (r != 0) {
        DBG(1, "cannot open %s: %s\n", d->name, libusb_error_name(r));
        free(s);
        return r == LIBUSB_ERROR_ACCESS ? SANE_STATUS_ACCESS_DENIED : SANE_STATUS_IO_ERROR;
    }
    libusb_set_auto_detach_kernel_driver(s->h, 1);
    r = libusb_claim_interface(s->h, 0);
    if (r != 0) {
        DBG(1, "cannot claim interface: %s\n", libusb_error_name(r));
        libusb_close(s->h);
        free(s);
        return SANE_STATUS_DEVICE_BUSY;
    }
    st = device_init(s);
    if (st != SANE_STATUS_GOOD) {
        libusb_release_interface(s->h, 0);
        libusb_close(s->h);
        free(s);
        return st;
    }
    init_options(s);
    *handle = s;
    return SANE_STATUS_GOOD;
}

static void stop_scan(v39_t *s);

void ENTRY(close)(SANE_Handle h)
{
    v39_t *s = h;
    stop_scan(s);
    libusb_release_interface(s->h, 0);
    libusb_close(s->h);
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cond);
    free(s);
}

const SANE_Option_Descriptor *ENTRY(get_option_descriptor)(SANE_Handle h, SANE_Int n)
{
    v39_t *s = h;
    if (n < 0 || n >= NUM_OPTIONS)
        return NULL;
    return &s->opt[n];
}

static SANE_Word nearest_res(SANE_Word v)
{
    int i, best = res_list[1];
    for (i = 1; i <= res_list[0]; i++)
        if (abs(res_list[i] - v) < abs(best - v))
            best = res_list[i];
    return best;
}

SANE_Status ENTRY(control_option)(SANE_Handle h, SANE_Int n, SANE_Action action,
                                  void *val, SANE_Int *info)
{
    v39_t *s = h;
    SANE_Option_Descriptor *o;
    if (info)
        *info = 0;
    if (n < 0 || n >= NUM_OPTIONS)
        return SANE_STATUS_INVAL;
    o = &s->opt[n];
    if (o->type == SANE_TYPE_GROUP)
        return SANE_STATUS_INVAL;

    if (action == SANE_ACTION_GET_VALUE) {
        if (n >= OPT_BUTTON_SCAN && n <= OPT_BUTTON_PDF) {
            /*
             * 1E 85 returns the number of the button held down right now
             * (1 Start, 2 Copy, 3 E-mail, 4 PDF, 0 none); it does not latch,
             * so pollers should read several times a second.  No USB
             * traffic is allowed while the image is streaming.
             */
            uint8_t b = 0;
            if (!s->scanning) {
                /* one query serves all four options for 50 ms */
                double t = now_s();
                if (t - s->btn_time > 0.05) {
                    SANE_Status st = cmd_query(s, 0x1E, 0x85, &s->btn, 1);
                    if (st != SANE_STATUS_GOOD)
                        return st;
                    s->btn_time = t;
                    if (s->btn)
                        DBG(4, "button %u\n", s->btn);
                }
                b = s->btn;
            }
            *(SANE_Word *)val = b == n - OPT_BUTTON_SCAN + 1 ? SANE_TRUE : SANE_FALSE;
            return SANE_STATUS_GOOD;
        }
        if (n == OPT_MODE)
            strcpy(val, s->mode);
        else
            *(SANE_Word *)val = s->val[n];
        return SANE_STATUS_GOOD;
    }
    if (s->scanning)
        return SANE_STATUS_DEVICE_BUSY;
    if (action != SANE_ACTION_SET_VALUE)
        return SANE_STATUS_INVAL;
    if (!(o->cap & SANE_CAP_SOFT_SELECT) || (o->cap & SANE_CAP_INACTIVE))
        return SANE_STATUS_INVAL;

    switch (n) {
    case OPT_MODE: {
        int i, ok = 0;
        for (i = 0; mode_list[i]; i++)
            if (!strcmp(val, mode_list[i]))
                ok = 1;
        if (!ok)
            return SANE_STATUS_INVAL;
        snprintf(s->mode, sizeof s->mode, "%s", (const char *)val);
        if (!strcmp(s->mode, SANE_VALUE_SCAN_MODE_LINEART))
            s->opt[OPT_THRESHOLD].cap &= ~SANE_CAP_INACTIVE;
        else
            s->opt[OPT_THRESHOLD].cap |= SANE_CAP_INACTIVE;
        if (info)
            *info |= SANE_INFO_RELOAD_OPTIONS | SANE_INFO_RELOAD_PARAMS;
        break;
    }
    case OPT_RESOLUTION: {
        SANE_Word v = nearest_res(*(SANE_Word *)val);
        if (v != *(SANE_Word *)val && info)
            *info |= SANE_INFO_INEXACT;
        s->val[n] = v;
        if (info)
            *info |= SANE_INFO_RELOAD_PARAMS;
        break;
    }
    case OPT_THRESHOLD: {
        SANE_Word v = *(SANE_Word *)val;
        if (v < 0 || v > 255)
            return SANE_STATUS_INVAL;
        s->val[n] = v;
        break;
    }
    case OPT_CAL_CACHE:
        s->val[n] = *(SANE_Word *)val ? SANE_TRUE : SANE_FALSE;
        break;
    case OPT_PREVIEW:
        s->val[n] = *(SANE_Word *)val ? SANE_TRUE : SANE_FALSE;
        if (info)
            *info |= SANE_INFO_RELOAD_PARAMS;
        break;
    case OPT_TL_X:
    case OPT_TL_Y:
    case OPT_BR_X:
    case OPT_BR_Y: {
        SANE_Word v = *(SANE_Word *)val;
        const SANE_Range *r = o->constraint.range;
        if (v < r->min) v = r->min;
        if (v > r->max) v = r->max;
        if (v != *(SANE_Word *)val && info)
            *info |= SANE_INFO_INEXACT;
        s->val[n] = v;
        if (info)
            *info |= SANE_INFO_RELOAD_PARAMS;
        break;
    }
    default:
        return SANE_STATUS_INVAL;
    }
    return SANE_STATUS_GOOD;
}

SANE_Status ENTRY(get_parameters)(SANE_Handle h, SANE_Parameters *params)
{
    v39_t *s = h;
    if (!s->scanning)
        calc_params(s);
    params->format = s->sp.out_mode == 0 ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
    params->last_frame = SANE_TRUE;
    params->bytes_per_line = s->sp.bpl;
    params->pixels_per_line = s->sp.out_w;
    params->lines = s->sp.out_h;
    params->depth = s->sp.out_mode == 2 ? 1 : 8;
    return SANE_STATUS_GOOD;
}

SANE_Status ENTRY(start)(SANE_Handle h)
{
    v39_t *s = h;
    SANE_Status st;

    if (s->scanning)
        return SANE_STATUS_DEVICE_BUSY;
    calc_params(s);
    s->cancel = 0;
    s->qhead = s->qlen = 0;
    s->qdone = 0;
    s->delivered = 0;
    s->total = (size_t)s->sp.bpl * s->sp.out_h;
    s->thread_status = SANE_STATUS_GOOD;
    s->transfers_left = 0;
    s->qcap = 32u << 20;
    if (s->qcap < (size_t)s->sp.bpl * 4)
        s->qcap = (size_t)s->sp.bpl * 4;
    free(s->q);
    s->q = malloc(s->qcap);
    if (!s->q)
        return SANE_STATUS_NO_MEM;

    DBG(1, "start: %s %d dpi (hw %d), %d x %d px, y %.3f\"\n", s->mode, s->sp.out_dpi,
        s->sp.hw->dpi, s->sp.out_w, s->sp.out_h, s->sp.y_in);
    s->scanning = 1;
    st = do_calibrate_and_start(s);
    if (st != SANE_STATUS_GOOD) {
        DBG(1, "scan setup failed: %s\n", status_str(st));
        s->scanning = 0;
        return st;
    }
    if (pthread_create(&s->thread, NULL, reader_thread, s) != 0) {
        s->cancel = 1;
        stop_scan(s);
        return SANE_STATUS_NO_MEM;
    }
    s->thread_running = 1;
    return SANE_STATUS_GOOD;
}

SANE_Status ENTRY(read)(SANE_Handle h, SANE_Byte *buf, SANE_Int max, SANE_Int *len)
{
    v39_t *s = h;
    size_t n;

    *len = 0;
    if (!s->scanning)
        return s->cancel ? SANE_STATUS_CANCELLED : SANE_STATUS_EOF;
    pthread_mutex_lock(&s->mtx);
    while (s->qlen == 0 && !s->qdone && !s->cancel)
        pthread_cond_wait(&s->cond, &s->mtx);
    if (s->cancel) {
        pthread_mutex_unlock(&s->mtx);
        stop_scan(s);
        return SANE_STATUS_CANCELLED;
    }
    if (s->qlen == 0) {
        SANE_Status st = s->thread_status;
        pthread_mutex_unlock(&s->mtx);
        stop_scan(s);
        return st == SANE_STATUS_GOOD ? SANE_STATUS_EOF : st;
    }
    n = s->qlen < (size_t)max ? s->qlen : (size_t)max;
    if (n > s->qcap - s->qhead)
        n = s->qcap - s->qhead;
    memcpy(buf, s->q + s->qhead, n);
    s->qhead = (s->qhead + n) % s->qcap;
    s->qlen -= n;
    s->delivered += n;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mtx);
    *len = (SANE_Int)n;
    return SANE_STATUS_GOOD;
}

/* ends a scan: joins the reader, cancels and drains the device if needed */
static void stop_scan(v39_t *s)
{
    if (!s->scanning)
        return;
    if (s->thread_running) {
        pthread_mutex_lock(&s->mtx);
        if (s->cancel)
            pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mtx);
        pthread_join(s->thread, NULL);
        s->thread_running = 0;
    }
    if (s->transfers_left > 0) {
        uint8_t junk[16384];
        int got, r, i;
        DBG(1, "cancelling scan (%d transfers left)\n", s->transfers_left);
        op2(s, 0x1E, 0x18);
        /* drain whatever the device still sends */
        for (i = 0; i < 100000; i++) {
            r = libusb_bulk_transfer(s->h, EP_IN, junk, sizeof junk, &got, 1000);
            if (r != 0 && r != LIBUSB_ERROR_OVERFLOW)
                break;
        }
        s->transfers_left = 0;
        wait_idle(s, 30);
    }
    free(s->q);
    s->q = NULL;
    s->scanning = 0;
}

void ENTRY(cancel)(SANE_Handle h)
{
    v39_t *s = h;
    if (!s->scanning)
        return;
    pthread_mutex_lock(&s->mtx);
    s->cancel = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mtx);
    stop_scan(s);
}

SANE_Status ENTRY(set_io_mode)(SANE_Handle h, SANE_Bool non_blocking)
{
    (void)h;
    return non_blocking ? SANE_STATUS_UNSUPPORTED : SANE_STATUS_GOOD;
}

SANE_Status ENTRY(get_select_fd)(SANE_Handle h, SANE_Int *fd)
{
    (void)h;
    (void)fd;
    return SANE_STATUS_UNSUPPORTED;
}
