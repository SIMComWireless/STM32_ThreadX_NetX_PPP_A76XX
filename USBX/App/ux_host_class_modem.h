/*
 * ux_host_class_modem.h — USBX host class for SIMCom composite USB modems
 *
 * Adapted from ux_host_class_gser (Generic Serial).
 *
 * Architecture (interface-level activation via CSP match):
 *   - QUERY matches Class=0xFF (vendor-specific) interfaces.
 *   - ACTIVATE receives a single UX_INTERFACE* and creates one instance.
 *   - CDC-ECM handles Class=0x02/0x0A (ECM) interfaces independently.
 *   - Global table ux_host_class_modem_instances[] lets the application
 *     look up any activated interface by index or interface number.
 *
 * Async reception: each interface can start non-blocking reception that
 * delivers received data to a lwrb ring buffer via USBX transfer callback.
 * A separate bounce buffer is used for USB transfers to avoid data corruption
 * (the lwrb internal buffer and USB DMA buffer must not overlap).
 */

#ifndef UX_HOST_CLASS_MODEM_H
#define UX_HOST_CLASS_MODEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ux_api.h"
#include "lwrb.h"

/* SIMCom USB identifiers — override in ux_user.h or compiler -D if needed */
#ifndef UX_HOST_CLASS_MODEM_VID
#define UX_HOST_CLASS_MODEM_VID                         0x1E0E
#endif
#ifndef UX_HOST_CLASS_MODEM_PID
#define UX_HOST_CLASS_MODEM_PID                         0x9011
#endif

/* Configuration */
#ifndef UX_HOST_CLASS_MODEM_MAX_INSTANCES
#define UX_HOST_CLASS_MODEM_MAX_INSTANCES               6
#endif

#ifndef UX_HOST_CLASS_MODEM_TRANSFER_TIMEOUT
#define UX_HOST_CLASS_MODEM_TRANSFER_TIMEOUT            300000
#endif

#define UX_HOST_CLASS_MODEM_STATE_IDLE                  0
#define UX_HOST_CLASS_MODEM_STATE_LIVE                  1
#define UX_HOST_CLASS_MODEM_STATE_SHUTDOWN              2

/* Reception states */
#define UX_HOST_CLASS_MODEM_RX_STOPPED                  0
#define UX_HOST_CLASS_MODEM_RX_STARTED                  1

/* ------------------------------------------------------------------ */
/*  Per-interface instance                                             */
/* ------------------------------------------------------------------ */

typedef struct UX_HOST_CLASS_MODEM_STRUCT
{
    struct UX_HOST_CLASS_MODEM_STRUCT   *next;
    UX_HOST_CLASS                       *class_ptr;
    UX_DEVICE                           *device;
    UX_INTERFACE                        *interface;
    UX_ENDPOINT                         *bulk_in;
    UX_ENDPOINT                         *bulk_out;
    UX_SEMAPHORE                         semaphore;
    UINT                                 state;

    /* Async reception (non-blocking, callback → lwrb) */
    lwrb_t                               rx_rb;           /* ring buffer handle        */
    UCHAR                               *rx_xfer_buf;     /* bounce buffer for USB DMA */
    UX_TRANSFER                          rx_transfer;     /* dedicated transfer for RX */
    volatile UINT                        rx_state;        /* UX_HOST_CLASS_MODEM_RX_*  */
    ULONG                                rx_block_size;   /* bytes per USB transfer    */
} UX_HOST_CLASS_MODEM;

/* ------------------------------------------------------------------ */
/*  Global instance table                                              */
/* ------------------------------------------------------------------ */

extern UX_HOST_CLASS_MODEM  *ux_host_class_modem_instances[];
extern ULONG                 ux_host_class_modem_count;
extern UX_HOST_CLASS        *ux_host_class_modem_class_ptr;

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

UINT  ux_host_class_modem_entry(UX_HOST_CLASS_COMMAND *command);

/* Blocking read (polling, semaphore-protected) */
UINT  ux_host_class_modem_read(UX_HOST_CLASS_MODEM *modem,
                                UCHAR *buf, ULONG len, ULONG *actual);

/* Blocking write (polling, semaphore-protected) */
UINT  ux_host_class_modem_write(UX_HOST_CLASS_MODEM *modem,
                                 UCHAR *buf, ULONG len, ULONG *actual);

/*
 * Start non-blocking async reception.
 *
 * rx_buf / rx_buf_size — application-allocated buffer for the lwrb.
 * block_size — bytes per USB bulk transfer (e.g. 512 or 1024).
 *
 * After start, data arrives via USBX transfer callback and is written
 * into the lwrb.  Application reads with lwrb_read(&modem->rx_rb, ...).
 *
 * NOTE: rx_buf must remain valid until reception is stopped.
 */
UINT  ux_host_class_modem_reception_start(UX_HOST_CLASS_MODEM *modem,
                                           UCHAR *rx_buf, ULONG rx_buf_size,
                                           ULONG block_size);

/* Stop async reception */
UINT  ux_host_class_modem_reception_stop(UX_HOST_CLASS_MODEM *modem);

/* ------------------------------------------------------------------ */
/*  Inline helpers                                                     */
/* ------------------------------------------------------------------ */

static inline ULONG ux_host_class_modem_ifnum(UX_HOST_CLASS_MODEM *m)
{
    if (m && m->interface)
        return m->interface->ux_interface_descriptor.bInterfaceNumber;
    return 0xFF;
}

static inline UX_HOST_CLASS_MODEM *ux_host_class_modem_find(ULONG ifnum)
{
    ULONG i;
    for (i = 0; i < ux_host_class_modem_count; i++)
    {
        if (ux_host_class_modem_ifnum(ux_host_class_modem_instances[i]) == ifnum)
            return ux_host_class_modem_instances[i];
    }
    return NULL;
}

/* Convenience wrappers — look up by ifnum, then delegate to per-instance API.
 * Returns UX_INVALID_PARAMETER if the interface is not found. */

static inline UINT ux_host_class_modem_read_if(ULONG ifnum,
                                                UCHAR *buf, ULONG len, ULONG *actual)
{
    UX_HOST_CLASS_MODEM *m = ux_host_class_modem_find(ifnum);
    if (m == NULL) return UX_INVALID_PARAMETER;
    return ux_host_class_modem_read(m, buf, len, actual);
}

static inline UINT ux_host_class_modem_write_if(ULONG ifnum,
                                                 UCHAR *buf, ULONG len, ULONG *actual)
{
    UX_HOST_CLASS_MODEM *m = ux_host_class_modem_find(ifnum);
    if (m == NULL) return UX_INVALID_PARAMETER;
    return ux_host_class_modem_write(m, buf, len, actual);
}

static inline UINT ux_host_class_modem_rx_start_if(ULONG ifnum,
                                                     UCHAR *rb, ULONG rb_size, ULONG blk)
{
    UX_HOST_CLASS_MODEM *m = ux_host_class_modem_find(ifnum);
    if (m == NULL) return UX_INVALID_PARAMETER;
    return ux_host_class_modem_reception_start(m, rb, rb_size, blk);
}

static inline UINT ux_host_class_modem_rx_stop_if(ULONG ifnum)
{
    UX_HOST_CLASS_MODEM *m = ux_host_class_modem_find(ifnum);
    if (m == NULL) return UX_INVALID_PARAMETER;
    return ux_host_class_modem_reception_stop(m);
}

#ifdef __cplusplus
}
#endif
#endif
