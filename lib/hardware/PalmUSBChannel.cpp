#include "PalmUSBChannel.h"

#ifdef CONFIG_USB_VISOR_HOST_ENABLED

#include <algorithm>
#include <string.h>

#include <esp_system.h>
#include <usb/usb_helpers.h>

#include "fnUsbHost.h"

#include "../../include/debug.h"

#define TX_TIMEOUT_MS    1000
#define CTRL_TIMEOUT_MS  500
#define OUT_BUFFER_SIZE  2048

// Handspring vendor requests (Linux drivers/usb/serial/visor.h, pilot-link)
#define VISOR_REQUEST_BYTES_AVAILABLE       0x01
#define VISOR_GET_CONNECTION_INFORMATION    0x03
#define VISOR_CONNECTION_INFO_SIZE          6   // num_ports u16, 2 x {function, port}

#define MAX_FIFO_PAYLOAD 64
typedef struct {
    size_t length;
    uint8_t data[MAX_FIFO_PAYLOAD];
} FIFOPacket;

typedef struct {
    usb_host_client_event_t type;
    uint8_t address;
    usb_device_handle_t dev;
} VisorEvent;

/*-----------------------------------------------------------------------
  Callbacks: all run on the client task, inside usb_host_client_handle_events
-----------------------------------------------------------------------*/

static void clientEventForwarder(const usb_host_client_event_msg_t *event, void *arg)
{
    ((PalmUSBChannel *)arg)->clientEvent(event);
}

static void inForwarder(usb_transfer_t *transfer)
{
    ((PalmUSBChannel *)transfer->context)->inDone(transfer);
}

static void semaphoreGiver(usb_transfer_t *transfer)
{
    xSemaphoreGive((SemaphoreHandle_t)transfer->context);
}

static void outForwarder(usb_transfer_t *transfer)
{
    ((PalmUSBChannel *)transfer->context)->outDone(transfer);
}

void PalmUSBChannel::outDone(usb_transfer_t *transfer)
{
    _outPending = false;
    xSemaphoreGive(_outDone);
}

void PalmUSBChannel::clientEvent(const usb_host_client_event_msg_t *event)
{
    // Opening a device needs control transfers, whose completions are
    // delivered on this very task -- so hand the work to the worker task.
    VisorEvent ev = {};
    ev.type = event->event;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        ev.address = event->new_dev.address;
    else
        ev.dev = event->dev_gone.dev_hdl;
    xQueueSend(_events, &ev, 0);
}

void PalmUSBChannel::inDone(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED)
    {
        if (transfer->actual_num_bytes > 0)
        {
            FIFOPacket pkt;
            pkt.length = std::min((size_t)transfer->actual_num_bytes, (size_t)MAX_FIFO_PAYLOAD);
            memcpy(pkt.data, transfer->data_buffer, pkt.length);
            if (xQueueSend(_rxQueue, &pkt, 0) != pdTRUE)
                Debug_printv("Visor: receive queue full, %u bytes dropped", (unsigned)pkt.length);
        }
        // Keep the endpoint polled: one packet per transfer, so every packet
        // completes even though the Visor sends no zero-length packets
        if (_connected && usb_host_transfer_submit(transfer) == ESP_OK)
            return;
    }
    else if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
             transfer->status != USB_TRANSFER_STATUS_CANCELED)
    {
        Debug_printv("Visor: IN transfer failed, status %d", transfer->status);
    }
    _inFlight--;
}

/*-----------------------------------------------------------------------
  Tasks
-----------------------------------------------------------------------*/

void PalmUSBChannel::clientTask()
{
    while (true)
        usb_host_client_handle_events(_client, portMAX_DELAY);
}

static void clientTaskForwarder(void *arg)
{
    ((PalmUSBChannel *)arg)->clientTask();
}

void PalmUSBChannel::workerTask()
{
    VisorEvent ev;

    while (true)
    {
        xQueueReceive(_events, &ev, portMAX_DELAY);
        if (ev.type == USB_HOST_CLIENT_EVENT_NEW_DEV)
        {
            if (!_dev)
                openDevice(ev.address);
        }
        else if (ev.dev == _dev)
        {
            Debug_printv("Visor: disconnected");
            closeDevice();
        }
    }
}

static void workerTaskForwarder(void *arg)
{
    ((PalmUSBChannel *)arg)->workerTask();
}

/*-----------------------------------------------------------------------
  Device
-----------------------------------------------------------------------*/

bool PalmUSBChannel::findEndpoints(const usb_config_desc_t *config)
{
    int offset = 0;
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config, 0, 0, &offset);
    uint8_t firstIn = 0, firstOut = 0;
    uint16_t firstInMps = 0;

    if (!intf)
        return false;
    _interface = intf->bInterfaceNumber;
    _inEp = _outEp = 0;

    for (int i = 0; i < intf->bNumEndpoints; i++)
    {
        int epOffset = offset;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, config->wTotalLength, &epOffset);
        if (!ep || (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
            continue;

        bool in = ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK;
        uint8_t num = ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK;
        if (in)
        {
            if (!firstIn)
            {
                firstIn = ep->bEndpointAddress;
                firstInMps = USB_EP_DESC_GET_MPS(ep);
            }
            if (num == _endpointNumber)
            {
                _inEp = ep->bEndpointAddress;
                _inMps = USB_EP_DESC_GET_MPS(ep);
            }
        }
        else
        {
            if (!firstOut)
                firstOut = ep->bEndpointAddress;
            if (num == _endpointNumber)
                _outEp = ep->bEndpointAddress;
        }
    }

    // Fall back to the first bulk pair
    if (!_inEp || !_outEp)
    {
        _inEp = firstIn;
        _inMps = firstInMps;
        _outEp = firstOut;
    }
    return _inEp && _outEp && _inMps;
}

// Send a vendor IN request to an endpoint and wait for it; the answer is
// not needed, only that the Visor has been asked
bool PalmUSBChannel::vendorRequestIn(uint8_t request, uint16_t index, uint16_t length)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)_ctrl->data_buffer;

    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_VENDOR |
                           USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
    setup->bRequest = request;
    setup->wValue = 0;
    setup->wIndex = index;
    setup->wLength = length;
    _ctrl->num_bytes = sizeof(usb_setup_packet_t) + length;
    _ctrl->device_handle = _dev;
    _ctrl->bEndpointAddress = 0;
    _ctrl->callback = semaphoreGiver;
    _ctrl->context = _ctrlDone;

    if (usb_host_transfer_submit_control(_client, _ctrl) != ESP_OK)
        return false;
    if (xSemaphoreTake(_ctrlDone, pdMS_TO_TICKS(CTRL_TIMEOUT_MS)) != pdTRUE)
        return false;
    return _ctrl->status == USB_TRANSFER_STATUS_COMPLETED;
}

void PalmUSBChannel::openDevice(uint8_t address)
{
    usb_device_handle_t dev;
    const usb_device_desc_t *desc;
    const usb_config_desc_t *config;

    if (usb_host_device_open(_client, address, &dev) != ESP_OK)
        return;
    usb_host_get_device_descriptor(dev, &desc);

    if ((_expected_vid && desc->idVendor != _expected_vid) ||
        (_expected_pid && desc->idProduct != _expected_pid))
    {
        Debug_printv("Visor: ignoring USB device %04X:%04X", desc->idVendor, desc->idProduct);
        usb_host_device_close(_client, dev);
        return;
    }

    if (usb_host_get_active_config_descriptor(dev, &config) != ESP_OK || !findEndpoints(config))
    {
        Debug_printv("Visor: no bulk endpoint pair on %04X:%04X", desc->idVendor, desc->idProduct);
        usb_host_device_close(_client, dev);
        return;
    }

    if (usb_host_interface_claim(_client, dev, _interface, 0) != ESP_OK)
    {
        Debug_printv("Visor: could not claim interface %u", _interface);
        usb_host_device_close(_client, dev);
        return;
    }
    _dev = dev;

    // The Handspring handshake, as palm-sync and Linux's visor driver do it.
    // Failures are fine: the byte stream works without it.
    bool info = vendorRequestIn(VISOR_GET_CONNECTION_INFORMATION, _outEp & 0x0F, VISOR_CONNECTION_INFO_SIZE);
    bool avail = vendorRequestIn(VISOR_REQUEST_BYTES_AVAILABLE, _outEp & 0x0F, 2);

    _connected = true;
    for (int i = 0; i < VISOR_IN_TRANSFERS; i++)
    {
        _in[i]->device_handle = _dev;
        _in[i]->bEndpointAddress = _inEp;
        _in[i]->num_bytes = _inMps;
        _in[i]->callback = inForwarder;
        _in[i]->context = this;
        if (usb_host_transfer_submit(_in[i]) == ESP_OK)
            _inFlight++;
    }

    Debug_printv("Visor: connected %04X:%04X, bulk IN 0x%02X (%u) OUT 0x%02X, info %s/%s",
                 desc->idVendor, desc->idProduct, _inEp, _inMps, _outEp,
                 info ? "ok" : "no", avail ? "ok" : "no");
}

void PalmUSBChannel::closeDevice()
{
    _connected = false;
    if (!_dev)
        return;

    // Cancel what is still queued, then wait for the callbacks to finish
    usb_host_endpoint_halt(_dev, _inEp);
    usb_host_endpoint_flush(_dev, _inEp);
    usb_host_endpoint_halt(_dev, _outEp);
    usb_host_endpoint_flush(_dev, _outEp);
    for (int i = 0; i < 50 && (_inFlight > 0 || _outPending); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (_inFlight > 0 || _outPending)
        Debug_printv("Visor: %d transfers still pending at close", (int)_inFlight);

    usb_host_interface_release(_client, _dev, _interface);
    usb_host_device_close(_client, _dev);
    _dev = nullptr;
}

/*-----------------------------------------------------------------------
  Channel
-----------------------------------------------------------------------*/

void PalmUSBChannel::begin()
{
    _events = xQueueCreate(8, sizeof(VisorEvent));
    _rxQueue = xQueueCreate(2048 / MAX_FIFO_PAYLOAD, sizeof(FIFOPacket));
    _ctrlDone = xSemaphoreCreateBinary();
    _outDone = xSemaphoreCreateBinary();
    if (!_events || !_rxQueue || !_ctrlDone || !_outDone)
    {
        Debug_printv("could not create Visor queues, free internal/total heap: %lu/%lu",
                     esp_get_free_internal_heap_size(), esp_get_free_heap_size());
        abort();
    }

    for (int i = 0; i < VISOR_IN_TRANSFERS; i++)
        ESP_ERROR_CHECK(usb_host_transfer_alloc(MAX_FIFO_PAYLOAD, 0, &_in[i]));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(OUT_BUFFER_SIZE, 0, &_out));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &_ctrl));

    bool host_was_already_up = !usbHostEnsureInstalled(_service_priority);

    usb_host_client_config_t client_config = {};
    client_config.is_synchronous = false;
    client_config.max_num_event_msg = 5;
    client_config.async.client_event_callback = clientEventForwarder;
    client_config.async.callback_arg = this;
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &_client));

    if (xTaskCreate(clientTaskForwarder, "Visor-client", 4096, this, _service_priority, NULL) != pdTRUE ||
        xTaskCreate(workerTaskForwarder, "Visor-worker", 4096, this, _service_priority, NULL) != pdTRUE)
    {
        Debug_printv("could not create Visor USB tasks");
        abort();
    }

    // A device that enumerated before this client registered is never
    // announced to it (see fnUsbHost.h)
    if (host_was_already_up)
        usbHostRecycleRootPort();

    Debug_printv("Visor: waiting for a Palm app to open the USB Library");
}

void PalmUSBChannel::end()
{
}

void PalmUSBChannel::setServicePriority(UBaseType_t priority)
{
    _service_priority = priority;

    TaskHandle_t h;
    if ((h = xTaskGetHandle("usb_lib")) != NULL)
        vTaskPrioritySet(h, priority);
    if ((h = xTaskGetHandle("Visor-client")) != NULL)
        vTaskPrioritySet(h, priority);
    if ((h = xTaskGetHandle("Visor-worker")) != NULL)
        vTaskPrioritySet(h, priority);
}

void PalmUSBChannel::updateFIFO()
{
    FIFOPacket pkt;
    size_t old_len;

    while (xQueueReceive(_rxQueue, &pkt, 0))
    {
        old_len = _fifo.size();
        _fifo.resize(old_len + pkt.length);
        memcpy(&_fifo[old_len], pkt.data, pkt.length);
    }
}

size_t PalmUSBChannel::dataOut(const void *buffer, size_t length)
{
    const uint8_t *p = (const uint8_t *)buffer;
    size_t sent = 0, n;

    while (sent < length)
    {
        // Link down, or the Visor stopped taking data: drop the rest
        if (!_connected || _outPending)
            return sent;

        n = std::min(length - sent, (size_t)OUT_BUFFER_SIZE);
        memcpy(_out->data_buffer, p + sent, n);
        _out->num_bytes = n;
        _out->device_handle = _dev;
        _out->bEndpointAddress = _outEp;
        _out->callback = outForwarder;
        _out->context = this;

        xSemaphoreTake(_outDone, 0); // drop a completion from a timed-out write
        _outPending = true;
        if (usb_host_transfer_submit(_out) != ESP_OK)
        {
            _outPending = false;
            return sent;
        }
        if (xSemaphoreTake(_outDone, pdMS_TO_TICKS(TX_TIMEOUT_MS)) != pdTRUE)
        {
            // Still queued: outDone() clears _outPending when it completes
            // or is flushed at close; until then writes are dropped
            Debug_printv("Visor: write timed out");
            return sent;
        }
        if (_out->status != USB_TRANSFER_STATUS_COMPLETED)
            return sent;
        sent += _out->actual_num_bytes;
    }
    return sent;
}

#endif /* CONFIG_USB_VISOR_HOST_ENABLED */
