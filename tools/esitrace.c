/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * esitrace - protocol tracer for the EPSON Perfection V39 (GT-S650, 04b8:013d)
 *
 * Loads Epson's own ESC/I interpreter (esxi010c.dll, x64) and gives it USB
 * read/write callbacks that talk to \\.\UsbscanN and log every byte.  The
 * host side then speaks ordinary ESC/I to the interpreter (INTWrite/INTRead),
 * so the log shows how high-level ESC/I maps onto the raw device protocol.
 *
 * Build (x64):  clang -O1 -o esitrace.exe esitrace.c
 *
 * Usage:
 *   esitrace [-d \\.\Usbscan0] [-l log.txt] [-i esxi010c.dll] info
 *   esitrace ... scan <dpi> <color|gray|line> <x> <y> <w> <h> out.pnm
 *   esitrace ... raw  <hex bytes to write> <bytes to read>   (no interpreter)
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef int (*usb_cb)(void *buf, int len);
typedef int (*int_init_fn)(usb_cb rd, usb_cb wr);
typedef int (*int_rw_fn)(void *buf, int len);
typedef void (*int_close_fn)(void);

static HANDLE dev = INVALID_HANDLE_VALUE;
static FILE *logf, *rawf;

/* binary capture: per transfer  dir(1: '<' or '>') len(4, LE) data */
static void rawrec(char dir, const void *p, int n)
{
    if (!rawf || n <= 0)
        return;
    fputc(dir, rawf);
    fwrite(&n, 4, 1, rawf);
    fwrite(p, 1, n, rawf);
}
static int_rw_fn INTRead, INTWrite;
static int_close_fn INTClose;
static LARGE_INTEGER t0, freq;
static unsigned long long raw_in, raw_out;
static int quiet_data;  /* suppress dumping of big image reads */

static double now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / freq.QuadPart;
}

static void dump(const char *tag, const uint8_t *p, int n)
{
    int max = (quiet_data && n > 64) ? 32 : 512;
    fprintf(logf, "%9.4f %s %6d:", now(), tag, n);
    for (int i = 0; i < n && i < max; i++)
        fprintf(logf, " %02x", p[i]);
    if (n > max) {
        fprintf(logf, " ... tail:");
        for (int i = n - 16; i < n; i++)
            fprintf(logf, " %02x", p[i]);
    }
    fputc('\n', logf);
    fflush(logf);
}

static int xfer(int write, void *buf, int len, DWORD timeout_ms)
{
    OVERLAPPED ov = {0};
    DWORD got = 0;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL ok = write ? WriteFile(dev, buf, len, NULL, &ov)
                    : ReadFile(dev, buf, len, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        fprintf(logf, "  !! %s error %lu\n", write ? "write" : "read", GetLastError());
        CloseHandle(ov.hEvent);
        return -1;
    }
    if (WaitForSingleObject(ov.hEvent, timeout_ms) != WAIT_OBJECT_0) {
        CancelIo(dev);
        GetOverlappedResult(dev, &ov, &got, TRUE);
        fprintf(logf, "  !! %s timeout (%lu of %d)\n", write ? "write" : "read", got, len);
        CloseHandle(ov.hEvent);
        return -1;
    }
    if (!GetOverlappedResult(dev, &ov, &got, FALSE)) {
        fprintf(logf, "  !! %s result error %lu\n", write ? "write" : "read", GetLastError());
        got = (DWORD)-1;
    }
    CloseHandle(ov.hEvent);
    return (int)got;
}

/* callbacks handed to the interpreter: raw bulk traffic */
static int cb_read(void *buf, int len)
{
    int done = 0;
    while (done < len) {
        int n = xfer(0, (uint8_t *)buf + done, len - done, 20000);
        if (n <= 0)
            break;
        done += n;
    }
    raw_in += done;
    rawrec('<', buf, done);
    dump("  usb<", buf, done);
    return done == len;
}

static int cb_write(void *buf, int len)
{
    dump("  usb>", buf, len);
    rawrec('>', buf, len);
    int n = xfer(1, buf, len, 20000);
    raw_out += len;
    return n == len;
}

/* ESC/I level (what a classic epson2-style host would send) */
static int esci_w(const void *buf, int len)
{
    dump("ESCI>", buf, len);
    int r = INTWrite((void *)buf, len);
    if (!r)
        fprintf(logf, "  !! INTWrite returned %d\n", r);
    return r;
}

static int esci_r(void *buf, int len)
{
    int r = INTRead(buf, len);
    dump("ESCI<", buf, len);
    if (!r)
        fprintf(logf, "  !! INTRead returned %d\n", r);
    return r;
}

static int esci_cmd_ack(uint8_t a, uint8_t b)
{
    uint8_t c[2] = {a, b}, ack = 0;
    esci_w(c, 2);
    esci_r(&ack, 1);
    return ack == 0x06;
}

/* "info block" reply: STX, status, len16 LE, data */
static int esci_info(uint8_t a, uint8_t b, uint8_t *out, int *outlen)
{
    uint8_t c[2] = {a, b}, h[4];
    esci_w(c, 2);
    esci_r(h, 4);
    int n = h[2] | (h[3] << 8);
    if (h[0] != 0x02 || n > 4096) {
        fprintf(logf, "  !! unexpected header\n");
        return 0;
    }
    if (n)
        esci_r(out, n);
    *outlen = n;
    return 1;
}

static int esci_fixed(uint8_t a, uint8_t b, uint8_t *out, int n)
{
    uint8_t c[2] = {a, b};
    esci_w(c, 2);
    return esci_r(out, n);
}

static void cmd_info(void)
{
    uint8_t buf[4096];
    int n;
    fprintf(logf, "## ESC @ (initialize)\n");
    esci_cmd_ack(0x1b, '@');
    fprintf(logf, "## ESC I (identity)\n");
    esci_info(0x1b, 'I', buf, &n);
    fprintf(logf, "## ESC F (status)\n");
    esci_info(0x1b, 'F', buf, &n);
    fprintf(logf, "## ESC f (extended status)\n");
    esci_info(0x1b, 'f', buf, &n);
    fprintf(logf, "## FS I (extended identity, 80 bytes)\n");
    esci_fixed(0x1c, 'I', buf, 80);
    fprintf(logf, "## FS F (scanner status, 16 bytes)\n");
    esci_fixed(0x1c, 'F', buf, 16);
    fprintf(logf, "## FS S (scanning parameters, 64 bytes)\n");
    esci_fixed(0x1c, 'S', buf, 64);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int cmd_scan(int dpi, int mode, int x, int y, int w, int h, const char *out)
{
    uint8_t p[64] = {0}, hdr[14], ack = 0x06;

    esci_cmd_ack(0x1b, '@');

    put32(p + 0, dpi);
    put32(p + 4, dpi);
    put32(p + 8, x);
    put32(p + 12, y);
    put32(p + 16, w);
    put32(p + 20, h);
    int color = mode == 1;
    p[24] = color ? 0x13 : 0x00;   /* ESC C: 0x13 = RGB pixel sequence */
    p[25] = mode == 2 ? 1 : 8;                     /* ESC D: bit depth */
    p[26] = 0;                     /* ESC e: option control (flatbed) */
    p[27] = 0;                     /* ESC g: scan mode */
    p[28] = 0;                     /* ESC d: block line number */
    p[29] = 0x03;                  /* ESC Z: gamma (3 or 4 accepted) */
    p[30] = 0;                     /* ESC L: brightness */
    p[31] = 0x00;                  /* ESC M: color correction */
    p[32] = 0x00;                  /* ESC B: halftone */
    p[33] = 0x80;                  /* ESC t: threshold */
    p[34] = 0;                     /* ESC s */
    p[35] = 0;                     /* ESC Q: sharpness */
    p[36] = 0;                     /* ESC K: mirroring */
    p[37] = 0;                     /* ESC N: film type */
    p[38] = 0;                     /* ESC m: lamp mode */

    fprintf(logf, "## FS W (set scanning parameters)\n");
    if (!esci_cmd_ack(0x1c, 'W'))
        fprintf(logf, "  !! FS W not acked\n");
    esci_w(p, 64);
    esci_r(&ack, 1);
    if (ack != 0x06) {
        fprintf(logf, "  !! parameters rejected (%02x)\n", ack);
        return 1;
    }
    {
        uint8_t s[64];
        fprintf(logf, "## FS S (read back)\n");
        esci_fixed(0x1c, 'S', s, 64);
    }

    fprintf(logf, "## FS G (start scan)\n");
    esci_w("\x1cG", 2);
    esci_r(hdr, 14);
    uint32_t bs = get32(hdr + 2), bn = get32(hdr + 6), ls = get32(hdr + 10);
    fprintf(logf, "   status %02x block %u x %u, last %u\n", hdr[1], bs, bn, ls);
    if (hdr[0] != 0x02)
        return 1;

    FILE *f = fopen(out, "wb");
    fprintf(f, "P%d\n%d %d\n255\n", color ? 6 : 5, w, h);
    uint8_t *blk = malloc((bs > ls ? bs : ls) + 1);
    quiet_data = 1;
    for (uint32_t i = 0; i <= bn; i++) {
        uint32_t n = (i == bn) ? ls : bs;
        if (n == 0)
            break;
        esci_r(blk, n + 1);
        fwrite(blk, 1, n, f);
        fprintf(logf, "   block %u: %u bytes, error code %02x\n", i, n, blk[n]);
        if (i != bn) {
            esci_w(&ack, 1);
        }
    }
    quiet_data = 0;
    fclose(f);
    free(blk);
    return 0;
}

static int hexbytes(const char *s, uint8_t *out)
{
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == ',')
            s++;
        if (!*s)
            break;
        out[n++] = (uint8_t)strtoul(s, (char **)&s, 16);
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *path = "\\\\.\\Usbscan0", *logname = "esitrace.log";
    const char *dll = "C:\\Windows\\System32\\esxi010c.dll";
    const char *mutexname = "Global\\Es_Mutex_0002";
    int i = 1;

    for (; i < argc && argv[i][0] == '-'; i += 2) {
        if (argv[i][1] == 'd') path = argv[i + 1];
        else if (argv[i][1] == 'm') mutexname = argv[i + 1];
        else if (argv[i][1] == 'r') rawf = fopen(argv[i + 1], "wb");
        else if (argv[i][1] == 'l') logname = argv[i + 1];
        else if (argv[i][1] == 'i') dll = argv[i + 1];
    }
    if (i >= argc) {
        fprintf(stderr, "usage: esitrace [-d dev] [-l log] [-i dll] info|scan|raw ...\n");
        return 2;
    }
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    logf = fopen(logname, "w");

    /* Epson's WIA driver polls the scanner (status + buttons) once a second
     * from the WIA service.  It serialises access with this named mutex;
     * hold it so the poller cannot interleave commands with ours. */
    HANDLE mtx = OpenMutexA(SYNCHRONIZE, FALSE, mutexname);
    if (mtx) {
        DWORD w = WaitForSingleObject(mtx, 10000);
        fprintf(logf, "## holding %s (%lu)\n", mutexname, w);
    } else {
        fprintf(logf, "## no %s (%lu), WIA poller may interfere\n", mutexname, GetLastError());
    }

    dev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                      FILE_FLAG_OVERLAPPED, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open %s: %lu\n", path, GetLastError());
        return 1;
    }
    {
        /* usbscan.sys blocks inside ReadFile regardless of OVERLAPPED, so
         * use its own per-handle timeout (seconds): read, write, event */
        ULONG to[3] = {strcmp(argv[i], "drain") ? 30 : 3, 10, 10};
        DWORD ret;
        DeviceIoControl(dev, CTL_CODE(0x8000, 0x80b, METHOD_BUFFERED, FILE_ANY_ACCESS),
                        to, sizeof to, NULL, 0, &ret, NULL);
    }

    if (!strcmp(argv[i], "usbinfo")) {
        /* IOCTL_GET_DEVICE_DESCRIPTOR / IOCTL_GET_PIPE_CONFIGURATION */
        USHORT dd[4];
        struct { ULONG n; struct { USHORT mps; UCHAR ep, interval; ULONG type; } p[8]; } pc;
        DWORD ret;
        if (DeviceIoControl(dev, CTL_CODE(0x8000, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS),
                            NULL, 0, dd, sizeof dd, &ret, NULL))
            printf("vid %04x pid %04x bcdDevice %04x\n", dd[0], dd[1], dd[2]);
        if (DeviceIoControl(dev, CTL_CODE(0x8000, 0x80a, METHOD_BUFFERED, FILE_ANY_ACCESS),
                            NULL, 0, &pc, sizeof pc, &ret, NULL))
            for (ULONG k = 0; k < pc.n && k < 8; k++)
                printf("pipe %lu: ep 0x%02x type %lu (1=iso 2=bulk 3=int) maxpacket %u interval %u\n",
                       k, pc.p[k].ep, pc.p[k].type, pc.p[k].mps, pc.p[k].interval);
        return 0;
    }
    if (!strcmp(argv[i], "drain")) {
        static uint8_t b[65536];
        int n, total = 0;
        while ((n = xfer(0, b, sizeof b, 2000)) > 0) {
            dump("  usb<", b, n);
            total += n;
        }
        printf("drained %d bytes\n", total);
        fclose(logf);
        return 0;
    }
    if (!strcmp(argv[i], "raw")) {
        uint8_t b[4096];
        int n = hexbytes(argv[i + 1], b);
        cb_write(b, n);
        int want = atoi(argv[i + 2]);
        if (want) {
            int got = xfer(0, b, want, 3000);
            if (got > 0)
                dump("  usb<", b, got);
        }
        fclose(logf);
        return 0;
    }

    HMODULE m = LoadLibraryA(dll);
    if (!m) {
        fprintf(stderr, "cannot load %s: %lu\n", dll, GetLastError());
        return 1;
    }
    int_init_fn INTInit = (int_init_fn)GetProcAddress(m, "INTInit");
    INTRead = (int_rw_fn)GetProcAddress(m, "INTRead");
    INTWrite = (int_rw_fn)GetProcAddress(m, "INTWrite");
    INTClose = (int_close_fn)GetProcAddress(m, "INTClose");
    fprintf(logf, "## INTInit\n");
    if (!INTInit(cb_read, cb_write)) {
        fprintf(stderr, "INTInit failed\n");
        return 1;
    }

    int rc = 0;
    if (!strcmp(argv[i], "info")) {
        cmd_info();
    } else if (!strcmp(argv[i], "scan") && argc - i >= 8) {
        rc = cmd_scan(atoi(argv[i + 1]), !strcmp(argv[i + 2], "color") ? 1 : !strcmp(argv[i + 2], "line") ? 2 : 0,
                      atoi(argv[i + 3]), atoi(argv[i + 4]), atoi(argv[i + 5]),
                      atoi(argv[i + 6]), argv[i + 7]);
    }
    fprintf(logf, "## INTClose\n");
    INTClose();
    if (rawf)
        fclose(rawf);
    fprintf(logf, "## raw totals: out %llu, in %llu bytes\n", raw_out, raw_in);
    fclose(logf);
    CloseHandle(dev);
    return rc;
}
