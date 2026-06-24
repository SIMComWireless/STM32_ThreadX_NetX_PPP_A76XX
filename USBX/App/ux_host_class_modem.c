/*
 * ux_host_class_modem.c — USBX host class for SIMCom composite USB modems
 *
 * Adapted from ux_host_class_gser (Generic Serial, Qualcomm/Sierra Wireless).
 *
 * Architecture (interface-level activation via CSP match):
 *   - QUERY matches Class=0xFF (vendor-specific) interfaces via CSP.
 *   - ACTIVATE receives a single UX_INTERFACE* and creates one instance.
 *   - CDC-ECM handles Class=0x02/0x0A (ECM) interfaces independently.
 *   - Global table ux_host_class_modem_instances[] lets the application
 *     look up any activated interface by index or interface number.
 */

#define UX_SOURCE_CODE

#include "ux_api.h"
#include "ux_host_stack.h"
#include "ux_host_class_modem.h"
#include "bsp_usb.h"
#include "elog.h"

#define TAG "MODEM"

/* ---------- Global state ---------------------------------------------- */

UX_HOST_CLASS_MODEM  *ux_host_class_modem_instances[UX_HOST_CLASS_MODEM_MAX_INSTANCES];
ULONG                 ux_host_class_modem_count = 0;
UX_HOST_CLASS        *ux_host_class_modem_class_ptr = UX_NULL;
ULONG                 ux_host_class_modem_detected_pid = 0;

/* Supported PID list */
static const ULONG modem_pid_list[] = UX_HOST_CLASS_MODEM_PID_LIST;

/* ---------- Internal helpers ----------------------------------------- */

/*
 * modem_endpoints_get — find bulk IN and bulk OUT endpoints on one interface.
 * Returns UX_SUCCESS if both found, UX_ENDPOINT_HANDLE_UNKNOWN otherwise.
 */
static UINT modem_endpoints_get(UX_HOST_CLASS_MODEM *modem)
{
    UX_ENDPOINT *endpoint;
    ULONG        ep_idx;
    UINT         status;
    ULONG        num_ep;

    num_ep = modem->interface->ux_interface_descriptor.bNumEndpoints;

    for (ep_idx = 0; ep_idx < num_ep; ep_idx++)
    {
        status = _ux_host_stack_interface_endpoint_get(
                     modem->interface, ep_idx, &endpoint);
        if (status != UX_SUCCESS)
            continue;

        if ((endpoint->ux_endpoint_descriptor.bmAttributes & UX_MASK_ENDPOINT_TYPE)
            != UX_BULK_ENDPOINT)
            continue;

        if ((endpoint->ux_endpoint_descriptor.bEndpointAddress & UX_ENDPOINT_DIRECTION)
            == UX_ENDPOINT_OUT && modem->bulk_out == UX_NULL)
        {
            endpoint->ux_endpoint_transfer_request.ux_transfer_request_type = UX_REQUEST_OUT;
            modem->bulk_out = endpoint;
        }
        else if ((endpoint->ux_endpoint_descriptor.bEndpointAddress & UX_ENDPOINT_DIRECTION)
                 == UX_ENDPOINT_IN && modem->bulk_in == UX_NULL)
        {
            endpoint->ux_endpoint_transfer_request.ux_transfer_request_type = UX_REQUEST_IN;
            modem->bulk_in = endpoint;
        }
    }

    if (modem->bulk_in == UX_NULL || modem->bulk_out == UX_NULL)
        return UX_ENDPOINT_HANDLE_UNKNOWN;

    return UX_SUCCESS;
}

/*
 * modem_activate_one — create a per-interface instance.
 * Called for each interface that has bulk endpoints.
 *
 * If an instance with the same interface number already exists (e.g. from
 * a previous enumeration before MCU reset), the old instance is replaced.
 * This prevents the instance table from overflowing on re-enumeration.
 */
static UINT modem_activate_one(UX_HOST_CLASS_MODEM *modem)
{
    UINT status;
    ULONG ifnum;
    ULONG i;

    status = modem_endpoints_get(modem);
    if (status != UX_SUCCESS)
        return status;

    ifnum = modem->interface->ux_interface_descriptor.bInterfaceNumber;

    /* Check for duplicate — replace old instance if same ifnum exists.
     * This handles re-enumeration after MCU reset where the modem stays
     * powered and re-enumerates without a proper disconnect event. */
    for (i = 0; i < ux_host_class_modem_count; i++)
    {
        UX_HOST_CLASS_MODEM *old = ux_host_class_modem_instances[i];
        if (old != UX_NULL &&
            old->interface->ux_interface_descriptor.bInterfaceNumber == ifnum)
        {
            elog_w(TAG, "REPLACE ifnum=%u — old instance %p replaced by %p",
                  (unsigned)ifnum, (void *)old, (void *)modem);

            /* Stop async RX on old instance to prevent use-after-free */
            if (old->rx_state != UX_HOST_CLASS_MODEM_RX_STOPPED)
                ux_host_class_modem_reception_stop(old);

            old->state = UX_HOST_CLASS_MODEM_STATE_SHUTDOWN;
            _ux_host_semaphore_delete(&old->rx_data_sem);
            _ux_host_semaphore_delete(&old->semaphore);
            _ux_host_stack_class_instance_destroy(old->class_ptr, (VOID *)old);

            /* Notify BSP USB to clear cached pointer before freeing */
            bsp_usb_notify_deactivated((uint8_t)ifnum);

            /* Free old instance memory — prevents leak on re-enumeration */
            _ux_utility_memory_free(old);

            /* Replace in table (don't increment count) */
            ux_host_class_modem_instances[i] = modem;
            goto activate;
        }
    }

    /* No duplicate — add to table */
    if (ux_host_class_modem_count >= UX_HOST_CLASS_MODEM_MAX_INSTANCES)
    {
        elog_e(TAG, "Instance table full (%lu) — cannot add ifnum=%u",
              (unsigned long)ux_host_class_modem_count, (unsigned)ifnum);
        return UX_MEMORY_INSUFFICIENT;
    }
    ux_host_class_modem_instances[ux_host_class_modem_count++] = modem;

activate:
    status = _ux_host_semaphore_create(&modem->semaphore, "ux_modem_sem", 1);
    if (status != UX_SUCCESS)
        return status;

    /* Data-available semaphore — posted by RX callback, waited by read() */
    status = _ux_host_semaphore_create(&modem->rx_data_sem, "ux_modem_rx", 0);
    if (status != UX_SUCCESS)
        return status;

    _ux_host_stack_class_instance_create(modem->class_ptr, (VOID *)modem);
    modem->state = UX_HOST_CLASS_MODEM_STATE_LIVE;

    elog_d(TAG, "LIVE ifnum=%u in=%p out=%p [%lu]",
          (unsigned)ifnum,
          (void *)modem->bulk_in, (void *)modem->bulk_out,
          (unsigned long)ux_host_class_modem_count);

    return UX_SUCCESS;
}

/* ---------- ENTRY ---------------------------------------------------- */

UINT ux_host_class_modem_entry(UX_HOST_CLASS_COMMAND *command)
{
    switch (command->ux_host_class_command_request)
    {

    /* ---- QUERY: CSP match for vendor-specific (0xFF) interfaces ---- */
    case UX_HOST_CLASS_COMMAND_QUERY:
        if (command->ux_host_class_command_usage == UX_HOST_CLASS_COMMAND_USAGE_CSP)
        {
            UINT cls = command->ux_host_class_command_class;

            /* Match vendor-specific interfaces only */
            if (cls != 0xFF)
                return UX_NO_CLASS_MATCH;

            /* Check VID/PID from device descriptor */
            {
                UX_INTERFACE *iface = (UX_INTERFACE *)command->ux_host_class_command_container;
                UX_DEVICE *device = iface->ux_interface_configuration->ux_configuration_device;
                ULONG vid = device->ux_device_descriptor.idVendor;
                ULONG pid = device->ux_device_descriptor.idProduct;
                ULONG i;

                if (vid != UX_HOST_CLASS_MODEM_VID)
                    return UX_NO_CLASS_MATCH;

                for (i = 0; i < UX_HOST_CLASS_MODEM_PID_COUNT; i++)
                {
                    if (pid == modem_pid_list[i])
                    {
                        /* Store detected PID for application use */
                        ux_host_class_modem_detected_pid = pid;
                        elog_d(TAG, "MATCH VID=0x%04lX PID=0x%04lX", vid, pid);
                        return UX_SUCCESS;
                    }
                }
            }
        }
        return UX_NO_CLASS_MATCH;

    /* ---- ACTIVATE: per-interface ---- */
    case UX_HOST_CLASS_COMMAND_ACTIVATE:
        {
            UX_INTERFACE        *iface;
            UX_DEVICE           *device;
            UX_HOST_CLASS       *class_ptr;
            UX_HOST_CLASS_MODEM *modem;
            UINT                 status;

            iface     = (UX_INTERFACE *)command->ux_host_class_command_container;
            device    = iface->ux_interface_configuration->ux_configuration_device;
            class_ptr = command->ux_host_class_command_class_ptr;

            /* Cache class pointer for event callback identification. */
            if (ux_host_class_modem_class_ptr == UX_NULL)
                ux_host_class_modem_class_ptr = class_ptr;

            elog_d(TAG, "ACTIVATE ifnum=%u class=0x%02X sub=0x%02X ep=%u",
                  (unsigned)iface->ux_interface_descriptor.bInterfaceNumber,
                  (unsigned)iface->ux_interface_descriptor.bInterfaceClass,
                  (unsigned)iface->ux_interface_descriptor.bInterfaceSubClass,
                  (unsigned)iface->ux_interface_descriptor.bNumEndpoints);

            /* Allocate per-interface instance. */
            modem = (UX_HOST_CLASS_MODEM *)
                _ux_utility_memory_allocate(UX_NO_ALIGN, UX_REGULAR_MEMORY,
                                            sizeof(UX_HOST_CLASS_MODEM));
            if (modem == UX_NULL)
                return UX_MEMORY_INSUFFICIENT;

            modem->class_ptr = class_ptr;
            modem->device    = device;
            modem->interface = iface;

            status = modem_activate_one(modem);
            if (status != UX_SUCCESS)
            {
                elog_d(TAG, "  SKIP ifnum=%u (0x%02X)",
                      (unsigned)iface->ux_interface_descriptor.bInterfaceNumber, status);
                _ux_utility_memory_free(modem);
                return status;
            }

            /* Notify application. */
            if (_ux_system_host->ux_system_host_change_function != UX_NULL)
                _ux_system_host->ux_system_host_change_function(
                    UX_DEVICE_INSERTION, class_ptr, (VOID *)modem);

            return UX_SUCCESS;
        }

    /* ---- DEACTIVATE: single instance removed ---- */
    case UX_HOST_CLASS_COMMAND_DEACTIVATE:
        {
            UX_HOST_CLASS_MODEM *modem;
            ULONG                i;
            ULONG                ifnum;

            modem = (UX_HOST_CLASS_MODEM *)command->ux_host_class_command_instance;
            if (modem == UX_NULL)
                return UX_ERROR;

            ifnum = modem->interface->ux_interface_descriptor.bInterfaceNumber;
            elog_d(TAG, "DEACTIVATE ifnum=%u", (unsigned)ifnum);

            /* Notify BSP USB to clear cached modem pointer BEFORE freeing.
             * Prevents use-after-free in bsp_usb read/write functions. */
            bsp_usb_notify_deactivated((uint8_t)ifnum);

            /* Stop async RX if running (prevents callback use-after-free). */
            if (modem->rx_state != UX_HOST_CLASS_MODEM_RX_STOPPED)
                ux_host_class_modem_reception_stop(modem);

            modem->state = UX_HOST_CLASS_MODEM_STATE_SHUTDOWN;

            if (modem->bulk_out)
                _ux_host_stack_endpoint_transfer_abort(modem->bulk_out);
            if (modem->bulk_in)
                _ux_host_stack_endpoint_transfer_abort(modem->bulk_in);

            _ux_host_semaphore_delete(&modem->rx_data_sem);
            _ux_host_semaphore_delete(&modem->semaphore);
            _ux_host_stack_class_instance_destroy(modem->class_ptr, (VOID *)modem);

            /* Remove from global table (swap-with-last). */
            for (i = 0; i < ux_host_class_modem_count; i++)
            {
                if (ux_host_class_modem_instances[i] == modem)
                {
                    ux_host_class_modem_instances[i] =
                        ux_host_class_modem_instances[--ux_host_class_modem_count];
                    ux_host_class_modem_instances[ux_host_class_modem_count] = UX_NULL;
                    break;
                }
            }

            elog_d(TAG, "  removed ifnum=%u [%lu left]",
                  (unsigned)ifnum,
                  (unsigned long)ux_host_class_modem_count);

            /* Notify application. */
            if (_ux_system_host->ux_system_host_change_function != UX_NULL)
                _ux_system_host->ux_system_host_change_function(
                    UX_DEVICE_REMOVAL, modem->class_ptr, (VOID *)modem);

            _ux_utility_memory_free(modem);
            return UX_SUCCESS;
        }

    default:
        return UX_FUNCTION_NOT_SUPPORTED;
    }
}

/* ---------- READ (bulk IN, blocking) --------------------------------- */

UINT ux_host_class_modem_read(UX_HOST_CLASS_MODEM *modem,
                               UCHAR *buf, ULONG len, ULONG *actual)
{
    UX_TRANSFER *xfer;
    UINT         status;
    ULONG        chunk;

    if (modem == UX_NULL || buf == UX_NULL || actual == UX_NULL)
        return UX_INVALID_PARAMETER;

    if (modem->state != UX_HOST_CLASS_MODEM_STATE_LIVE)
        return UX_HOST_CLASS_INSTANCE_UNKNOWN;

    status = _ux_host_semaphore_get(&modem->semaphore, UX_WAIT_FOREVER);
    if (status != UX_SUCCESS)
        return status;

    *actual = 0;
    xfer = &modem->bulk_in->ux_endpoint_transfer_request;

    while (len > 0)
    {
        chunk = (len > xfer->ux_transfer_request_maximum_length)
              ? xfer->ux_transfer_request_maximum_length : len;

        xfer->ux_transfer_request_data_pointer     = buf;
        xfer->ux_transfer_request_requested_length  = chunk;

        status = _ux_host_stack_transfer_request(xfer);
        if (status != UX_SUCCESS)
        {
            _ux_host_semaphore_put(&modem->semaphore);
            return status;
        }

        status = _ux_host_semaphore_get(
                     &xfer->ux_transfer_request_semaphore,
                     UX_MS_TO_TICK(UX_HOST_CLASS_MODEM_TRANSFER_TIMEOUT));
        if (status != UX_SUCCESS)
        {
            _ux_host_stack_transfer_request_abort(xfer);
            *actual += xfer->ux_transfer_request_actual_length;
            _ux_host_semaphore_put(&modem->semaphore);
            xfer->ux_transfer_request_completion_code = UX_TRANSFER_TIMEOUT;
            return UX_TRANSFER_TIMEOUT;
        }

        *actual += xfer->ux_transfer_request_actual_length;

        if (chunk != xfer->ux_transfer_request_actual_length)
        {
            _ux_host_semaphore_put(&modem->semaphore);
            return UX_SUCCESS;
        }

        buf += chunk;
        len -= chunk;
    }

    _ux_host_semaphore_put(&modem->semaphore);
    return UX_SUCCESS;
}

/* ---------- WRITE (bulk OUT, blocking) ------------------------------- */

UINT ux_host_class_modem_write(UX_HOST_CLASS_MODEM *modem,
                                UCHAR *buf, ULONG len, ULONG *actual)
{
    UX_TRANSFER *xfer;
    UINT         status;
    ULONG        chunk;

    if (modem == UX_NULL || buf == UX_NULL || actual == UX_NULL)
        return UX_INVALID_PARAMETER;

    if (modem->state != UX_HOST_CLASS_MODEM_STATE_LIVE)
        return UX_HOST_CLASS_INSTANCE_UNKNOWN;

    status = _ux_host_semaphore_get(&modem->semaphore, UX_WAIT_FOREVER);
    if (status != UX_SUCCESS)
        return status;

    *actual = 0;
    xfer = &modem->bulk_out->ux_endpoint_transfer_request;

    while (len > 0)
    {
        chunk = (len > xfer->ux_transfer_request_maximum_length)
              ? xfer->ux_transfer_request_maximum_length : len;

        xfer->ux_transfer_request_data_pointer     = buf;
        xfer->ux_transfer_request_requested_length  = chunk;

        status = _ux_host_stack_transfer_request(xfer);
        if (status != UX_SUCCESS)
        {
            _ux_host_semaphore_put(&modem->semaphore);
            return status;
        }

        status = _ux_host_semaphore_get(
                     &xfer->ux_transfer_request_semaphore,
                     UX_MS_TO_TICK(UX_HOST_CLASS_MODEM_TRANSFER_TIMEOUT));
        if (status != UX_SUCCESS)
        {
            _ux_host_stack_transfer_request_abort(xfer);
            *actual += xfer->ux_transfer_request_actual_length;
            _ux_host_semaphore_put(&modem->semaphore);
            xfer->ux_transfer_request_completion_code = UX_TRANSFER_TIMEOUT;
            return UX_TRANSFER_TIMEOUT;
        }

        *actual += xfer->ux_transfer_request_actual_length;

        if (chunk != xfer->ux_transfer_request_actual_length)
        {
            _ux_host_semaphore_put(&modem->semaphore);
            return UX_SUCCESS;
        }

        buf += chunk;
        len -= chunk;
    }

    _ux_host_semaphore_put(&modem->semaphore);
    return UX_SUCCESS;
}

/* ---------- ASYNC RECEPTION (callback → lwrb) ------------------------ */

/*
 * modem_rx_callback — USBX transfer completion callback (ISR context).
 * Copies received data from the bounce buffer into the lwrb and re-arms.
 */
static VOID modem_rx_callback(UX_TRANSFER *transfer_request)
{
    UX_HOST_CLASS_MODEM *modem;
    ULONG                actual;

    modem = (UX_HOST_CLASS_MODEM *)transfer_request->ux_transfer_request_class_instance;
    if (modem == NULL)
        return;

    /* Check aborting first — volatile, so ISR always sees the latest value.
     * If reception_stop is in progress, do NOT touch the transfer or lwrb. */
    if (modem->rx_aborting)
        return;

    if (modem->rx_state != UX_HOST_CLASS_MODEM_RX_STARTED)
        return;

    if (transfer_request->ux_transfer_request_completion_code != UX_SUCCESS)
    {
        modem->rx_state = UX_HOST_CLASS_MODEM_RX_STOPPED;
        return;
    }

    actual = transfer_request->ux_transfer_request_actual_length;
    if (actual > 0)
    {
        lwrb_write(&modem->rx_rb, modem->rx_xfer_buf, actual);
        /* Signal waiting read() that data is available */
        _ux_host_semaphore_put(&modem->rx_data_sem);
    }

    /* Re-arm only if not aborting (double-check after potential ISR preemption) */
    if (modem->rx_aborting)
        return;

    transfer_request->ux_transfer_request_requested_length = modem->rx_block_size;
    _ux_host_stack_transfer_request(transfer_request);
}

/*
 * ux_host_class_modem_reception_start — start non-blocking async reception.
 *
 * rx_buf / rx_buf_size — application-owned buffer for lwrb (must stay valid).
 * block_size — bytes per USB bulk IN transfer (e.g. 512).
 *
 * A bounce buffer is allocated internally for USB DMA.  After start,
 * read data with: lwrb_read(&modem->rx_rb, buf, len)
 */
UINT ux_host_class_modem_reception_start(UX_HOST_CLASS_MODEM *modem,
                                          UCHAR *rx_buf, ULONG rx_buf_size,
                                          ULONG block_size)
{
    UX_TRANSFER *xfer;
    UINT         status;

    if (modem == UX_NULL || rx_buf == UX_NULL || block_size == 0)
        return UX_INVALID_PARAMETER;

    if (modem->state != UX_HOST_CLASS_MODEM_STATE_LIVE)
        return UX_HOST_CLASS_INSTANCE_UNKNOWN;

    if (modem->rx_state != UX_HOST_CLASS_MODEM_RX_STOPPED)
        return UX_ERROR;

    /* Allocate bounce buffer for USB DMA (separate from lwrb storage). */
    modem->rx_xfer_buf = (UCHAR *)_ux_utility_memory_allocate(
                             UX_NO_ALIGN, UX_CACHE_SAFE_MEMORY, block_size);
    if (modem->rx_xfer_buf == UX_NULL)
        return UX_MEMORY_INSUFFICIENT;

    lwrb_init(&modem->rx_rb, rx_buf, rx_buf_size);
    modem->rx_block_size = block_size;

    xfer = &modem->rx_transfer;
    memset(xfer, 0, sizeof(*xfer));
    xfer->ux_transfer_request_endpoint            = modem->bulk_in;
    xfer->ux_transfer_request_type                = UX_REQUEST_IN;
    xfer->ux_transfer_request_data_pointer        = modem->rx_xfer_buf;
    xfer->ux_transfer_request_requested_length    = block_size;
    xfer->ux_transfer_request_class_instance      = (VOID *)modem;
    xfer->ux_transfer_request_completion_function  = modem_rx_callback;

    modem->rx_aborting = 0;  /* clear before enabling RX */
    modem->rx_state = UX_HOST_CLASS_MODEM_RX_STARTED;

    status = _ux_host_stack_transfer_request(xfer);
    if (status != UX_SUCCESS)
    {
        modem->rx_state = UX_HOST_CLASS_MODEM_RX_STOPPED;
        _ux_utility_memory_free(modem->rx_xfer_buf);
        modem->rx_xfer_buf = UX_NULL;
        return status;
    }

    elog_d(TAG, "RX started ifnum=%lu blk=%lu",
          (unsigned long)ux_host_class_modem_ifnum(modem),
          (unsigned long)block_size);
    return UX_SUCCESS;
}

/*
 * ux_host_class_modem_reception_stop — stop async reception.
 */
UINT ux_host_class_modem_reception_stop(UX_HOST_CLASS_MODEM *modem)
{
    if (modem == UX_NULL)
        return UX_INVALID_PARAMETER;

    if (modem->rx_state == UX_HOST_CLASS_MODEM_RX_STOPPED)
        return UX_SUCCESS;

    /* Set aborting flag first — volatile, visible to ISR immediately.
     * Prevents modem_rx_callback from re-arming the transfer even if
     * the ISR fires between setting rx_state and calling abort. */
    modem->rx_aborting = 1;

    /* Set STOPPED — prevents modem_rx_callback from re-arming.
     * Must happen before the abort, because abort may invoke the callback
     * synchronously from thread context. */
    modem->rx_state = UX_HOST_CLASS_MODEM_RX_STOPPED;

    /* Abort the in-flight transfer.  This halts the HCD channel and
     * invokes modem_rx_callback with completion_code != UX_SUCCESS,
     * which returns without re-arming.  After this call returns, no
     * ISR will access the bounce buffer. */
    _ux_host_stack_endpoint_transfer_abort(modem->bulk_in);

    /* Now safe to free the bounce buffer — no in-flight DMA. */
    if (modem->rx_xfer_buf != UX_NULL)
    {
        _ux_utility_memory_free(modem->rx_xfer_buf);
        modem->rx_xfer_buf = UX_NULL;
    }

    elog_d(TAG, "RX stopped ifnum=%lu",
          (unsigned long)ux_host_class_modem_ifnum(modem));
    return UX_SUCCESS;
}
