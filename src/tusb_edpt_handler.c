/**
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_edpt_handler.h"
#include "DAP.h"
#include "probe.h"
#include "semphr.h"

/*
 * One CMSIS-DAP v2 bulk interface per SWD channel. Each has its own
 * endpoints and request/response ring buffers; a single DAP thread serves
 * all channels round-robin, switching the probe/DAP context per packet.
 */
typedef struct {
	uint8_t itf_num;
	uint8_t out_ep_addr;
	uint8_t in_ep_addr;
	bool opened;
	buffer_t request;	// host -> probe
	buffer_t response;	// probe -> host
} dap_channel_t;

static dap_channel_t dap_channels[PROBE_NUM_CHANNELS];

static uint8_t _rhport;

static SemaphoreHandle_t edpt_spoon;

#define IDX(n) ((n) % DAP_PACKET_COUNT)
#define WR_IDX(x) IDX((x).wptr)
#define RD_IDX(x) IDX((x).rptr)

#define SLOT_PTR(x, n) (&((x).data[IDX(n)][0]))
#define WR_SLOT_PTR(x) SLOT_PTR(x, (x).wptr)
#define RD_SLOT_PTR(x) SLOT_PTR(x, (x).rptr)

bool buffer_full(buffer_t *buffer)
{
	return (buffer->wptr + 1) % DAP_PACKET_COUNT == buffer->rptr % DAP_PACKET_COUNT;
}

bool buffer_empty(buffer_t *buffer)
{
	return buffer->wptr == buffer->rptr;
}

static void channel_reset(dap_channel_t *ch)
{
	memset(ch, 0, sizeof(*ch));
	// Initialse full/empty flags
	ch->response.wasEmpty = true;
	ch->request.wasEmpty = true;
}

static dap_channel_t *channel_for_ep(uint8_t ep_addr)
{
	for (uint i = 0; i < PROBE_NUM_CHANNELS; i++) {
		dap_channel_t *ch = &dap_channels[i];
		if (ch->opened && (ch->out_ep_addr == ep_addr || ch->in_ep_addr == ep_addr))
			return ch;
	}
	return NULL;
}

// Defer setup to .reset() / .open()
void dap_edpt_init(void) {
	edpt_spoon = xSemaphoreCreateMutex();
	xSemaphoreGive(edpt_spoon);
}

// This only gets called if the SOF watchdog times out
bool dap_edpt_deinit(void)
{
	if (edpt_spoon != NULL)
		vSemaphoreDelete(edpt_spoon);
	edpt_spoon = NULL;
	return true;
}

void dap_edpt_reset(uint8_t __unused rhport)
{
	probe_info("dap_edpt_reset\n");
	// Linux resets us twice in succession
	for (uint i = 0; i < PROBE_NUM_CHANNELS; i++)
		channel_reset(&dap_channels[i]);
}

char * dap_cmd_string[] = {
	[ID_DAP_Info               ] = "DAP_Info",
	[ID_DAP_HostStatus         ] = "DAP_HostStatus",
	[ID_DAP_Connect            ] = "DAP_Connect",
	[ID_DAP_Disconnect         ] = "DAP_Disconnect",
	[ID_DAP_TransferConfigure  ] = "DAP_TransferConfigure",
	[ID_DAP_Transfer           ] = "DAP_Transfer",
	[ID_DAP_TransferBlock      ] = "DAP_TransferBlock",
	[ID_DAP_TransferAbort      ] = "DAP_TransferAbort",
	[ID_DAP_WriteABORT         ] = "DAP_WriteABORT",
	[ID_DAP_Delay              ] = "DAP_Delay",
	[ID_DAP_ResetTarget        ] = "DAP_ResetTarget",
	[ID_DAP_SWJ_Pins           ] = "DAP_SWJ_Pins",
	[ID_DAP_SWJ_Clock          ] = "DAP_SWJ_Clock",
	[ID_DAP_SWJ_Sequence       ] = "DAP_SWJ_Sequence",
	[ID_DAP_SWD_Configure      ] = "DAP_SWD_Configure",
	[ID_DAP_SWD_Sequence       ] = "DAP_SWD_Sequence",
	[ID_DAP_JTAG_Sequence      ] = "DAP_JTAG_Sequence",
	[ID_DAP_JTAG_Configure     ] = "DAP_JTAG_Configure",
	[ID_DAP_JTAG_IDCODE        ] = "DAP_JTAG_IDCODE",
	[ID_DAP_SWO_Transport      ] = "DAP_SWO_Transport",
	[ID_DAP_SWO_Mode           ] = "DAP_SWO_Mode",
	[ID_DAP_SWO_Baudrate       ] = "DAP_SWO_Baudrate",
	[ID_DAP_SWO_Control        ] = "DAP_SWO_Control",
	[ID_DAP_SWO_Status         ] = "DAP_SWO_Status",
	[ID_DAP_SWO_ExtendedStatus ] = "DAP_SWO_ExtendedStatus",
	[ID_DAP_SWO_Data           ] = "DAP_SWO_Data",
	[ID_DAP_QueueCommands      ] = "DAP_QueueCommands",
	[ID_DAP_ExecuteCommands    ] = "DAP_ExecuteCommands",
};


uint16_t dap_edpt_open(uint8_t __unused rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
	// This has an *implicit return* if fails. .open() is called for each interface on the device on usb SET_CONFIGURATION(nr)
	TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == itf_desc->bInterfaceClass &&
			DAP_INTERFACE_SUBCLASS == itf_desc->bInterfaceSubClass &&
			DAP_INTERFACE_PROTOCOL == itf_desc->bInterfaceProtocol, 0);

	int chan = usb_dap_channel_for_itf(itf_desc->bInterfaceNumber);
	TU_VERIFY(chan >= 0, 0);

	uint16_t const drv_len = sizeof(tusb_desc_interface_t) + (itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
	TU_VERIFY(max_len >= drv_len, 0);

	dap_channel_t *ch = &dap_channels[chan];
	channel_reset(ch);
	ch->itf_num = itf_desc->bInterfaceNumber;
	_rhport = rhport;

	// Initialising the OUT endpoint

	tusb_desc_endpoint_t *edpt_desc = (tusb_desc_endpoint_t *) (itf_desc + 1);
	uint8_t ep_addr = edpt_desc->bEndpointAddress;

	ch->out_ep_addr = ep_addr;

	// The OUT endpoint requires a call to usbd_edpt_xfer to initialise the endpoint, giving tinyUSB a buffer to consume when a transfer occurs at the endpoint
	usbd_edpt_open(rhport, edpt_desc);
	usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(ch->request), DAP_PACKET_SIZE);

	// Initiliasing the IN endpoint

	edpt_desc++;
	ep_addr = edpt_desc->bEndpointAddress;

	ch->in_ep_addr = ep_addr;

	// The IN endpoint doesn't need a transfer to initialise it, as this will be done by the main loop of dap_thread
	usbd_edpt_open(rhport, edpt_desc);

	ch->opened = true;

	return drv_len;
}

bool dap_edpt_control_xfer_cb(uint8_t __unused rhport, uint8_t stage, tusb_control_request_t const *request)
{
	return false;
}

// Manage response buffer read and request buffer write indices
bool dap_edpt_xfer_cb(uint8_t __unused rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
	const uint8_t ep_dir = tu_edpt_dir(ep_addr);
	dap_channel_t *ch = channel_for_ep(ep_addr);

	if (ch == NULL || xferred_bytes > DAP_PACKET_SIZE)
		return false;

	if(ep_dir == TUSB_DIR_IN)
	{
		buffer_t *resp = &ch->response;

		xSemaphoreTake(edpt_spoon, portMAX_DELAY);
		resp->rptr++;
		// This checks that the buffer was not empty in DAP thread, which means the next buffer was not queued up for the in endpoint callback
		// So, queue up the buffer at the new read index, since we expect read to catch up to write at this point.
		// It is possible for the read index to be multiple spaces behind the write index (if the USB callbacks are lagging behind dap thread),
		// so we account for this by only setting wasEmpty to true if the next callback will empty the buffer
		if(!resp->wasEmpty)
		{
			usbd_edpt_xfer(rhport, ep_addr, RD_SLOT_PTR(*resp), resp->data_len[RD_IDX(*resp)]);
			resp->wasEmpty = (resp->rptr + 1) == resp->wptr;
		}
		xSemaphoreGive(edpt_spoon);
	} else {
		buffer_t *req = &ch->request;

		xSemaphoreTake(edpt_spoon, portMAX_DELAY);
		// Only queue the next buffer in the out callback if the buffer is not full
		// If full, we set the wasFull flag, which will be checked by dap thread
		if(!buffer_full(req))
		{
			req->wptr++;
			usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(*req), DAP_PACKET_SIZE);
			req->wasFull = false;
		}
		else {
			req->wasFull = true;
		}
		xSemaphoreGive(edpt_spoon);
	}

	//  Wake up DAP thread after processing the callback
	xTaskNotify(dap_taskhandle, 0, eSetValueWithOverwrite);
	return true;
}

/*
 * Process at most one request packet on a channel. Returns true if a packet
 * was processed, false if the channel has nothing (ready) to do.
 */
static bool dap_service_channel(uint chan)
{
	dap_channel_t *ch = &dap_channels[chan];
	buffer_t *req = &ch->request;
	buffer_t *resp = &ch->response;
	uint16_t resp_len;

	if (!ch->opened || req->rptr == req->wptr)
		return false;

	// Wait for the host to drain responses before producing more
	if (buffer_full(resp))
		return false;

	/*
	 * Atomic command support - buffer QueueCommands, but don't process them
	 * until a non-QueueCommands packet is seen. Rather than blocking the
	 * (shared) DAP thread, leave the channel alone until the batch is
	 * complete. If the batch can't fit in the request buffer, run it anyway.
	 */
	if (*RD_SLOT_PTR(*req) == ID_DAP_QueueCommands) {
		bool full = req->wasFull;
		// When full, one more received packet sits uncounted in the write slot
		uint32_t end = req->wptr + (full ? 1 : 0);
		uint32_t n = req->rptr;
		while (n != end && *SLOT_PTR(*req, n) == ID_DAP_QueueCommands)
			n++;
		if (n == end && !full) {
			probe_info("DAP%u wait for queued cmds\n", chan);
			return false;
		}
		for (uint32_t m = req->rptr; m != n; m++) {
			probe_info("%lu %lu DAP%u queued cmd %s len %02x\n", req->wptr, m, chan,
				   dap_cmd_string[*SLOT_PTR(*req, m)], *(SLOT_PTR(*req, m) + 1));
			*SLOT_PTR(*req, m) = ID_DAP_ExecuteCommands;
		}
	}

	// Read a single packet from the USB buffer into the DAP Request buffer
	probe_info("%lu %lu DAP%u cmd %s len %02x\n",
		   req->wptr, req->rptr, chan,
		   dap_cmd_string[*RD_SLOT_PTR(*req)], *(RD_SLOT_PTR(*req)+1));

	probe_select_channel(chan);
	resp_len = DAP_ExecuteCommand(RD_SLOT_PTR(*req), WR_SLOT_PTR(*resp)) & 0xffff;

	xSemaphoreTake(edpt_spoon, portMAX_DELAY);
	req->rptr++;
	// If the buffer was full in the out callback, queue up another buffer for the endpoint
	// to consume now that the slot just processed is free.
	if (req->wasFull)
	{
		req->wptr++;
		usbd_edpt_xfer(_rhport, ch->out_ep_addr, WR_SLOT_PTR(*req), DAP_PACKET_SIZE);
		req->wasFull = false;
	}
	xSemaphoreGive(edpt_spoon);

	probe_info("%lu %lu DAP%u resp %s len %u\n",
		   resp->wptr, resp->rptr, chan,
		   dap_cmd_string[*WR_SLOT_PTR(*resp)], resp_len);

	resp->data_len[WR_IDX(*resp)] = resp_len;
	//  Suspend the scheduler to avoid stale values/race conditions between threads
	xSemaphoreTake(edpt_spoon, portMAX_DELAY);

	if(buffer_empty(resp))
	{
		resp->wptr++;
		usbd_edpt_xfer(_rhport, ch->in_ep_addr, RD_SLOT_PTR(*resp), resp->data_len[RD_IDX(*resp)]);
	} else {
		resp->wptr++;
		// The In callback needs to check this flag to know when to queue up the next buffer.
		resp->wasEmpty = false;
	}
	xSemaphoreGive(edpt_spoon);

	return true;
}

void dap_thread(void *ptr)
{
	uint32_t cmd;
	bool busy;
	do
	{
		// Wait for usb CB wake
		xTaskNotifyWait(0, 0xFFFFFFFFu, &cmd, 1);

		// One packet per channel per pass, so channels share the DAP thread fairly
		do {
			busy = false;
			for (uint chan = 0; chan < PROBE_NUM_CHANNELS; chan++)
				busy |= dap_service_channel(chan);
		} while (busy);
	} while (1);
}

usbd_class_driver_t const _dap_edpt_driver =
{
		.init = dap_edpt_init,
		.deinit = dap_edpt_deinit,
		.reset = dap_edpt_reset,
		.open = dap_edpt_open,
		.control_xfer_cb = dap_edpt_control_xfer_cb,
		.xfer_cb = dap_edpt_xfer_cb,
		.sof = NULL,
#if CFG_TUSB_DEBUG >= 2
		.name = "DAP ENDPOINT"
#endif
};

static usbd_class_driver_t const _edpt_drivers[] = {
	_dap_edpt_driver,
	usb_reset_interface_driver,
};

// Add the custom driver to the tinyUSB stack
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
	*driver_count = sizeof(_edpt_drivers) / sizeof(_edpt_drivers[0]);
	return _edpt_drivers;
}
