/* IOS, the Wii's I/O processor. The SDK talks to it through IPC requests
 * (open/close/read/write/seek/ioctl/ioctlv on device paths); here
 * __ios_Ipc2 answers them on the host instead of sending them to the
 * Starlet. Pointers inside a request are physical addresses, which MA()
 * accepts directly. */
#include <string.h>

#include "hle.h"

enum { REQ_OPEN = 1, REQ_CLOSE, REQ_READ, REQ_WRITE, REQ_SEEK, REQ_IOCTL, REQ_IOCTLV };

/* IPC result codes. */
#define IPC_OK 0
#define IPC_EINVAL (-4)
#define IPC_ENOENT (-106)
#define IPC_PENDING 0x7FFFFFFF /* internal: never completes (event hooks) */

static const uint32_t kIosFree = 0x802420B0;   /* iosFree(hid, ptr) */
static const uint32_t kIosHeapSda = 0xFFFFA79C; /* r13-relative: hid of the IPC heap */

/* Title ID of PokePark Wii (USA): 00010000-R8AE. */
static const uint32_t kTitleHi = 0x00010000, kTitleLo = 0x52384145;

/* NAND files (hle_nand.c). */
FILE* hle_nand_open(const char* nand, int mode, int32_t* err);
int32_t hle_nand_read(FILE* f, uint32_t dst, uint32_t len);
int32_t hle_nand_write(FILE* f, uint32_t src, uint32_t len);
int32_t hle_nand_seek(FILE* f, int32_t offset, int32_t whence);
int32_t hle_nand_file_stats(FILE* f, uint32_t out);
int32_t hle_nand_ioctl(uint32_t type, uint32_t in, uint32_t out, uint32_t outsize);
int32_t hle_nand_ioctlv(uint32_t type, uint32_t vec, uint32_t nin, uint32_t nout);

/* Open descriptors: a device path, or a NAND file with its host file. */
#define MAX_FDS 32
static char sFds[MAX_FDS][64];
static FILE* sFiles[MAX_FDS];

static int alloc_fd(const char* path) {
    int i;
    for (i = 0; i < MAX_FDS; i++) {
        if (!sFds[i][0]) {
            strncpy(sFds[i], path, sizeof(sFds[i]) - 1);
            sFiles[i] = NULL;
            return i;
        }
    }
    return -1;
}

static const char* fd_path(int32_t fd) {
    return fd >= 0 && fd < MAX_FDS && sFds[fd][0] ? sFds[fd] : NULL;
}

/* Devices the game opens. Anything else is logged and refused; that
 * includes /dev/sdio/slot0 (the SD card's file system is answered above
 * it, in hle_sd.c) and /dev/usb/oh1/57e/305 (no Bluetooth adapter; Wii
 * Remote input comes from hle_wpad.c). */
static const char* const kDevices[] = {
    "/dev/stm/immediate", "/dev/stm/eventhook", "/dev/fs", "/dev/es", "/dev/di",
    "/dev/net/kd/request", "/dev/net/ncd/manage",
    "/dev/net/ip/top", "/dev/net/kd/time", "/dev/wl0", "/dev/usb/kbd", "/dev/usb/hid",
};

static int32_t ios_open(uint32_t req) {
    char path[64];
    size_t i;
    hle_read_str(LD32(req + 0x0C), path, sizeof(path));
    for (i = 0; i < sizeof(kDevices) / sizeof(kDevices[0]); i++) {
        if (strcmp(path, kDevices[i]) == 0) {
            int fd = alloc_fd(path);
            HLE_LOG("IOS_Open(\"%s\") = %d", path, fd);
            return fd;
        }
    }
    {
        int32_t err;
        FILE* f = hle_nand_open(path, (int)LD32(req + 0x10), &err);
        int fd;
        if (!f) {
            HLE_LOG("IOS_Open(\"%s\", %u) = %d", path, LD32(req + 0x10), err);
            return err;
        }
        fd = alloc_fd(path);
        if (fd < 0) {
            fclose(f);
            return -117; /* too many open files */
        }
        sFiles[fd] = f;
        return fd;
    }
}

/* /dev/di, the disc drive. Commands take a 0x20-byte block whose first
 * word is the command << 24; reads give the length in word 1 and the
 * offset / 4 in word 2. Results: 1 for success, 2 for a drive error. */
#define DI_OK 1
#define DI_ERROR 2

static int32_t di_ioctl(uint32_t type, uint32_t in, uint32_t out, uint32_t outsize) {
    HLE_LOG("/dev/di 0x%02X (%08X %08X), in %08X, out %08X+%X", type, LD32(in + 4), LD32(in + 8), in, out, outsize);
    if (out && outsize) memset(MA(out), 0, outsize);
    switch (type) {
    case 0x70: /* ReadDiskID */
        memcpy(MA(out), hle_dvd_disc_id(), outsize < 0x20 ? outsize : 0x20);
        return DI_OK;
    case 0x71: /* Read, from the open data partition */
        return hle_dvd_read((uint64_t)LD32(in + 8) << 2, LD32(in + 4), out) ? DI_OK : DI_ERROR;
    case 0x88: /* GetCoverStatus: 2 = closed with a disc inside */
        if (outsize >= 4) ST32(out, 2);
        return DI_OK;
    case 0x12: /* Inquiry */
        if (outsize >= 8) {
            ST32(out, 0x00000002);
            ST32(out + 4, 0x20060526);
        }
        return DI_OK;
    case 0xE0: /* RequestError: no error */
    case 0x7A: /* GetCoverRegister */
    case 0x7E: /* GetLength */
    case 0x86: /* ClearCoverInterrupt */
    case 0x8A: /* Reset */
    case 0x8C: /* ClosePartition */
    case 0x8E: /* EnableDvdVideo */
    case 0xE3: /* StopMotor */
    case 0xE4: /* AudioBufferConfig */
        return DI_OK;
    }
    HLE_WARN("/dev/di command 0x%X: unhandled, returning success", type);
    return DI_OK;
}

static int32_t ios_ioctl(uint32_t req, const char* dev) {
    uint32_t type = LD32(req + 0x0C), in = LD32(req + 0x10), insize = LD32(req + 0x14);
    uint32_t out = LD32(req + 0x18), outsize = LD32(req + 0x1C);
    (void)insize;
    if (strcmp(dev, "/dev/fs") == 0) return hle_nand_ioctl(type, in, out, outsize);
    if (strcmp(dev, "/dev/stm/eventhook") == 0 && type == 0x1000) {
        /* Completes on a power/reset button press, which never happens. */
        return IPC_PENDING;
    }
    if (strcmp(dev, "/dev/di") == 0) return di_ioctl(type, in, out, outsize);
    HLE_WARN("IOS_Ioctl(\"%s\", 0x%X, in %u bytes, out %u bytes): unhandled, returning 0", dev, type, insize, outsize);
    if (out && outsize) memset(MA(out), 0, outsize);
    return IPC_OK;
}

static int32_t ios_ioctlv(uint32_t req, const char* dev) {
    uint32_t type = LD32(req + 0x0C), nin = LD32(req + 0x10), nout = LD32(req + 0x14);
    uint32_t vec = LD32(req + 0x18);
#define VEC_BASE(i) LD32(vec + 8 * (i))
#define VEC_LEN(i) LD32(vec + 8 * (i) + 4)
    if (strcmp(dev, "/dev/fs") == 0) return hle_nand_ioctlv(type, vec, nin, nout);
    if (strcmp(dev, "/dev/es") == 0) {
        switch (type) {
        case 32: /* GetTitleID */
            if (nout >= 1 && VEC_LEN(nin) >= 8) {
                ST32(VEC_BASE(nin), kTitleHi);
                ST32(VEC_BASE(nin) + 4, kTitleLo);
                return IPC_OK;
            }
            break;
        case 29: { /* GetDataDir */
            static const char kDir[] = "/title/00010000/52384145/data";
            if (nout >= 1) {
                memcpy(MA(VEC_BASE(nin)), kDir, sizeof(kDir));
                return IPC_OK;
            }
            break;
        }
        }
    }
    if (strcmp(dev, "/dev/di") == 0 && type == 0x8B && nout >= 1) {
        /* OpenPartition: return the partition's TMD and no error. */
        hle_dvd_tmd(VEC_BASE(nin), VEC_LEN(nin));
        if (nout >= 2) ST32(VEC_BASE(nin + 1), 0);
        return DI_OK;
    }
    HLE_WARN("IOS_Ioctlv(\"%s\", 0x%X, %u in, %u out): unhandled, returning 0", dev, type, nin, nout);
    {
        uint32_t i;
        for (i = nin; i < nin + nout; i++) {
            if (VEC_LEN(i)) memset(MA(VEC_BASE(i)), 0, VEC_LEN(i));
        }
    }
    return IPC_OK;
#undef VEC_BASE
#undef VEC_LEN
}

static int32_t ios_handle(uint32_t req) {
    uint32_t type = LD32(req);
    int32_t fd = (int32_t)LD32(req + 8);
    const char* dev;
    if (type == REQ_OPEN) return ios_open(req);
    dev = fd_path(fd);
    if (!dev) {
        HLE_WARN("IOS request %u on bad fd %d", type, fd);
        return IPC_EINVAL;
    }
    if (sFiles[fd]) {
        FILE* f = sFiles[fd];
        switch (type) {
        case REQ_CLOSE:
            fclose(f);
            sFiles[fd] = NULL;
            sFds[fd][0] = '\0';
            return IPC_OK;
        case REQ_READ: return hle_nand_read(f, LD32(req + 0x0C), LD32(req + 0x10));
        case REQ_WRITE: return hle_nand_write(f, LD32(req + 0x0C), LD32(req + 0x10));
        case REQ_SEEK: return hle_nand_seek(f, (int32_t)LD32(req + 0x0C), (int32_t)LD32(req + 0x10));
        case REQ_IOCTL:
            if (LD32(req + 0x0C) == 11) return hle_nand_file_stats(f, LD32(req + 0x18)); /* GetFileStats */
            break;
        }
        HLE_WARN("request %u on NAND file \"%s\": unhandled", type, dev);
        return -101;
    }
    switch (type) {
    case REQ_CLOSE:
        HLE_LOG("IOS_Close(%d \"%s\")", fd, dev);
        sFds[fd][0] = '\0';
        return IPC_OK;
    case REQ_IOCTL:
        return ios_ioctl(req, dev);
    case REQ_IOCTLV:
        return ios_ioctlv(req, dev);
    default:
        HLE_WARN("IOS request %u on \"%s\": unhandled, returning 0", type, dev);
        return IPC_OK;
    }
}

static void free_request(ppc_t* c, uint32_t req) {
    hle_call(c, kIosFree, LD32(c->r[13] + kIosHeapSda), req);
}

/* s32 __ios_Ipc2(IPCRequestEx* req, IPCAsyncCallback cb): sends a
 * request. Without a callback it waits and returns the result; with one
 * it returns 0 and the callback gets (result, callbackArg) later. */
void f_802408F0(ppc_t* c) {
    uint32_t req = c->r[3], cb = c->r[4];
    int32_t ret;
    if (!req) {
        c->r[3] = (uint32_t)IPC_EINVAL;
        return;
    }
    ret = ios_handle(req);
    if (ret == IPC_PENDING) {
        if (!cb) HLE_WARN("synchronous IOS request will never complete");
        c->r[3] = 0;
        return;
    }
    ST32(req + 4, (uint32_t)ret);
    if (cb) {
        hle_post(cb, (uint32_t)ret, LD32(req + 0x24), free_request, req);
        c->r[3] = 0;
    } else {
        free_request(c, req);
        c->r[3] = (uint32_t)ret;
    }
}
