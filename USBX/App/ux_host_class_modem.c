/*
 * ux_host_class_modem.c — USBX host class for SIMCom composite USB modems
 *
 * Adapted from ux_host_class_gser (Generic Serial, Qualcomm/Sierra Wireless).
 *
 * Architecture (device-level activation via PID/VID match):
 *   - QUERY matches by VID/PID.
 *   - ACTIVATE receives the whole device.  It calls SET_CONFIGURATION,
 *     then scans ALL interfaces.  Each interface that exposes bulk IN + OUT
 *     endpoints gets its own independent UX_HOST_CLASS_MODEM instance.
 *   - Interfaces without bulk endpoints (e.g. Wireless Controller, HID)
 *     are silently skipped.
 *   - Global table ux_host_class_modem_instances[] lets the application
 *     look up any activated interface by index or interface number.
 */

#define UX_SOURCE_CODE

#include "ux_api.h"
#include "ux_host_stack.h"
#include "ux_host_class_modem.h"
#include "elog.h"

#define TAG "MODEM"

/* ---------- Global state ---------------------------------------------- */

UX_HOST_CLASS_MODEM  *ux_host_class_modem_instances[UX_HOST_CLASS_MODEM_MAX_INSTANCES];
ULONG                 ux_host_class_modem_count = 0;
UX_HOST_CLASS        *ux_host_class_modem_class_ptr = UX_NULL;

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
 */
static UINT modem_activate_one(UX_HOST_CLASS_MODEM *modem)
{
    UINT status;

    status = modem_endpoints_get(modem);
    if (status != UX_SUCCESS)
        return status;

    status = _ux_host_semaphore_create(&modem->semaphore, "ux_modem_sem", 1);
    if (status != UX_SUCCESS)
        return status;

    _ux_host_stack_class_instance_create(modem->class_ptr, (VOID *)modem);
    modem->state = UX_HOST_CLASS_MODEM_STATE_LIVE;

    if (ux_host_class_modem_count < UX_HOST_CLASS_MODEM_MAX_INSTANCES)
        ux_host_class_modem_instances[ux_host_class_modem_count++] = modem;

    elog_d(TAG, "LIVE ifnum=%u in=%p out=%p [%lu]",
          (unsigned)modem->interface->ux_interface_descriptor.bInterfaceNumber,
          (void *)modem->bulk_in, (void *)modem->bulk_out,
          (unsigned long)ux_host_class_modem_count);

    return UX_SUCCESS;
}

/* ---------- ENTRY ---------------------------------------------------- */

UINT ux_host_class_modem_entry(UX_HOST_CLASS_COMMAND *command)
{
    switch (command->ux_host_class_command_request)
    {

    /* ---- QUERY: match by VID/PID, skip ECM/RNDIS interfaces ---- */
    case UX_HOST_CLASS_COMMAND_QUERY:
        if ((command->ux_host_class_command_usage == UX_HOST_CLASS_COMMAND_USAGE_PIDVID) &&
            (command->ux_host_class_command_vid  == UX_HOST_CLASS_MODEM_VID) &&
            (command->ux_host_class_command_pid  == UX_HOST_CLASS_MODEM_PID))
        {
            UX_INTERFACE *iface = (UX_INTERFACE *)command->ux_host_class_command_container;
            UINT cls = iface->ux_interface_descriptor.bInterfaceClass;
            UINT sub = iface->ux_interface_descriptor.bInterfaceSubClass;

            /* Let CDC-ECM class handle ECM control (0x02/0x06) and data (0x0A/0x00) */
            if ((cls == 0x02 && sub == 0x06) || (cls == 0x0A && sub == 0x00))
                return UX_NO_CLASS_MATCH;
            /* Skip RNDIS control (0xE0/0x01) — no driver */
            if (cls == 0xE0 && sub == 0x01)
                return UX_NO_CLASS_MATCH;

            return UX_SUCCESS;
        }
        return UX_NO_CLASS_MATCH;

    /* ---- ACTIVATE: device-level, scan all interfaces ---- */
    case UX_HOST_CLASS_COMMAND_ACTIVATE:
        {
            UX_DEVICE           *device;
            UX_CONFIGURATION    *configuration;
            UX_INTERFACE        *iface;
            UX_HOST_CLASS       *class_ptr;
            UX_HOST_CLASS_MODEM *modem;
            UINT                 status;
            ULONG                if_idx;

            device    = (UX_DEVICE *)command->ux_host_class_command_container;
            class_ptr = command->ux_host_class_command_class_ptr;

            /* Cache class pointer for event callback identification. */
            if (ux_host_class_modem_class_ptr == UX_NULL)
                ux_host_class_modem_class_ptr = class_ptr;

            elog_d(TAG, "ACTIVATE device VID=0x%04X PID=0x%04X",
                  (unsigned)device->ux_device_descriptor.idVendor,
                  (unsigned)device->ux_device_descriptor.idProduct);

            /* SET_CONFIGURATION (no-op if already configured). */
            {
                UX_HOST_CLASS_MODEM tmp;
                tmp.device = device;
                status = _ux_host_stack_device_configuration_get(device, 0, &configuration);
                if (status != UX_SUCCESS)
                    return status;
                status = _ux_host_stack_device_configuration_select(configuration);
                if (status != UX_SUCCESS)
                {
                    elog_e(TAG, "configure failed 0x%02X", status);
                    return status;
                }
            }

            /* Get the configuration to access interfaces. */
            status = _ux_host_stack_device_configuration_get(device, 0, &configuration);
            if (status != UX_SUCCESS)
                return status;

            /* Scan ALL interfaces in this configuration. */
            for (if_idx = 0; ; if_idx++)
            {
                status = _ux_host_stack_configuration_interface_get(
                             configuration, if_idx, 0, &iface);
                if (status != UX_SUCCESS)
                    break;  /* No more interfaces */

                elog_d(TAG, "  ifnum=%u class=0x%02X sub=0x%02X ep=%u",
                      (unsigned)iface->ux_interface_descriptor.bInterfaceNumber,
                      (unsigned)iface->ux_interface_descriptor.bInterfaceClass,
                      (unsigned)iface->ux_interface_descriptor.bInterfaceSubClass,
                      (unsigned)iface->ux_interface_descriptor.bNumEndpoints);

                /* Skip ECM/RNDIS interfaces — same filter as QUERY */
                {
                    UINT cls = iface->ux_interface_descriptor.bInterfaceClass;
                    UINT sub = iface->ux_interface_descriptor.bInterfaceSubClass;
                    if ((cls == 0x02 && sub == 0x06) || (cls == 0x0A && sub == 0x00) ||
                        (cls == 0xE0 && sub == 0x01))
                        continue;
                }

                /* Allocate per-interface instance. */
                modem = (UX_HOST_CLASS_MODEM *)
                    _ux_utility_memory_allocate(UX_NO_ALIGN, UX_REGULAR_MEMORY,
                                                sizeof(UX_HOST_CLASS_MODEM));
                if (modem == UX_NULL)
                    continue;

                modem->class_ptr = class_ptr;
                modem->device    = device;
                modem->interface = iface;

                status = modem_activate_one(modem);
                if (status != UX_SUCCESS)
                {
                    elog_d(TAG, "  SKIP ifnum=%u (0x%02X)",
                          (unsigned)iface->ux_interface_descriptor.bInterfaceNumber, status);
                    _ux_utility_memory_free(modem);
                    continue;
                }

                /* Notify application for each activated interface. */
                if (_ux_system_host->ux_system_host_change_function != UX_NULL)
                    _ux_system_host->ux_system_host_change_function(
                        UX_DEVICE_INSERTION, class_ptr, (VOID *)modem);
            }

            elog_d(TAG, "ACTIVATE done — %lu instance(s)",
                  (unsigned long)ux_host_class_modem_count);
            return (ux_host_class_modem_count > 0) ? UX_SUCCESS : UX_NO_CLASS_MATCH;
        }

    /* ---- DEACTIVATE: device removed, destroy all its instances ---- */
    case UX_HOST_CLASS_COMMAND_DEACTIVATE:
        {
            UX_HOST_CLASS_MODEM *modem;
            UX_HOST_CLASS_MODEM *inst;
            UX_DEVICE           *device;
            ULONG                i;

            modem = (UX_HOST_CLASS_MODEM *)command->ux_host_class_command_instance;
            if (modem == UX_NULL)
                return UX_ERROR;

            device = modem->device;

            elog_d(TAG, "DEACTIVATE device — removing all instances");

            /* Walk the global table backwards so removal-by-swap is safe. */
            for (i = ux_host_class_modem_count; i > 0; i--)
            {
                inst = ux_host_class_modem_instances[i - 1];
                if (inst == UX_NULL || inst->device != device)
                    continue;

                /* Stop async RX if running (prevents callback use-after-free). */
                if (inst->rx_state != UX_HOST_CLASS_MODEM_RX_STOPPED)
                    ux_host_class_modem_reception_stop(inst);

                inst->state = UX_HOST_CLASS_MODEM_STATE_SHUTDOWN;

                if (inst->bulk_out)
                    _ux_host_stack_endpoint_transfer_abort(inst->bulk_out);
                if (inst->bulk_in)
                    _ux_host_stack_endpoint_transfer_abort(inst->bulk_in);

                _ux_host_semaphore_delete(&inst->semaphore);
                _ux_host_stack_class_instance_destroy(inst->class_ptr, (VOID *)inst);

                /* Remove from global table (swap with last). */
                ux_host_class_modem_instances[i - 1] =
                    ux_host_class_modem_instances[--ux_host_class_modem_count];
                ux_host_class_modem_instances[ux_host_class_modem_count] = UX_NULL;

                elog_d(TAG, "  removed ifnum=%u",
                      (unsigned)inst->interface->ux_interface_descriptor.bInterfaceNumber);

                /* Notify application. */
                if (_ux_system_host->ux_system_host_change_function != UX_NULL)
                    _ux_system_host->ux_system_host_change_function(
                        UX_DEVICE_REMOVAL, inst->class_ptr, (VOID *)inst);

                _ux_utility_memory_free(inst);
            }

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
    if (modem == NULL || modem->rx_state != UX_HOST_CLASS_MODEM_RX_STARTED)
        return;

    if (transfer_request->ux_transfer_request_completion_code != UX_SUCCESS)
    {
        modem->rx_state = UX_HOST_CLASS_MODEM_RX_STOPPED;
        return;
    }

    actual = transfer_request->ux_transfer_request_actual_length;
    if (actual > 0)
        lwrb_write(&modem->rx_rb, modem->rx_xfer_buf, actual);

    /* Re-arm */
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

    modem->rx_state = UX_HOST_CLASS_MODEM_RX_STOPPED;
    _ux_host_stack_endpoint_transfer_abort(modem->bulk_in);

    /* Free bounce buffer. */
    if (modem->rx_xfer_buf != UX_NULL)
    {
        _ux_utility_memory_free(modem->rx_xfer_buf);
        modem->rx_xfer_buf = UX_NULL;
    }

    elog_d(TAG, "RX stopped ifnum=%lu",
          (unsigned long)ux_host_class_modem_ifnum(modem));
    return UX_SUCCESS;
}
