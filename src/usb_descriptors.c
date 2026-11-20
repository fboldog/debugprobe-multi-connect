/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2021 Peter Lawrence
 * Copyright (c) 2022 Raspberry Pi Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "tusb.h"
#include "get_serial.h"
#include "probe_config.h"
#include "pico/usb_reset.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
    .bcdUSB             = 0x0210, // USB Specification version 2.1 for BOS
#else
    .bcdUSB             = 0x0110,
#endif
    .bDeviceClass       = 0x00, // Each interface specifies its own
    .bDeviceSubClass    = 0x00, // Each interface specifies its own
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x2E8A, // Pi
    .idProduct          = 0x000c, // CMSIS-DAP Debug Probe
    .bcdDevice          = 0x0231, // Version 02.31
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum
{
  ITF_NUM_PROBE, // Old versions of Keil MDK only look at interface 0
  ITF_NUM_CDC_COM,
  ITF_NUM_CDC_DATA,
  ITF_NUM_RESET,
  ITF_NUM_PROBE_EXTRA, // SWD channels 1..PROBE_NUM_CHANNELS-1
  ITF_NUM_CDC_EXTRA = ITF_NUM_PROBE_EXTRA + PROBE_NUM_CHANNELS - 1, // UARTs 1..PROBE_NUM_UARTS-1, two interfaces each
  ITF_NUM_TOTAL = ITF_NUM_CDC_EXTRA + 2 * (PROBE_NUM_UARTS - 1)
};

#define CDC_NOTIFICATION_EP_NUM 0x81
#define CDC_DATA_OUT_EP_NUM 0x02
#define CDC_DATA_IN_EP_NUM 0x83
#define DAP_OUT_EP_NUM 0x04
#define DAP_IN_EP_NUM 0x85

// SWD channel n (n >= 1) uses OUT/IN endpoint pair number 5 + n, i.e. EP6..EP15
#define DAP_EXTRA_EP_NUM(n) (5 + (n))
// String descriptor index for channel n (n >= 1), following the fixed strings
#define STRID_PROBE_EXTRA(n) (7 + (n))

#define DAP_EXTRA_DESCRIPTOR(n) \
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_PROBE_EXTRA + (n) - 1, STRID_PROBE_EXTRA(n), \
                        DAP_EXTRA_EP_NUM(n), 0x80 | DAP_EXTRA_EP_NUM(n), 64)

// UART n (n >= 1): data endpoint pair and notification IN endpoint after the DAP channels' endpoints
#define CDC_EXTRA_DATA_EP_NUM(n)  (5 + PROBE_NUM_CHANNELS + 2 * ((n) - 1))
#define CDC_EXTRA_NOTIF_EP_NUM(n) (CDC_EXTRA_DATA_EP_NUM(n) + 1)
#define STRID_CDC_EXTRA(n) (7 + (PROBE_NUM_CHANNELS - 1) + (n))

#define CDC_EXTRA_DESCRIPTOR(n) \
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_EXTRA + 2 * ((n) - 1), STRID_CDC_EXTRA(n), \
                     0x80 | CDC_EXTRA_NOTIF_EP_NUM(n), 64, \
                     CDC_EXTRA_DATA_EP_NUM(n), 0x80 | CDC_EXTRA_DATA_EP_NUM(n), 64)

#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V1)
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#define CDC_DESC_OFFSET   (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_RPI_RESET_DESC_LEN + \
                           (PROBE_NUM_CHANNELS - 1) * TUD_VENDOR_DESC_LEN + (PROBE_NUM_UARTS - 1) * TUD_CDC_DESC_LEN)
#define CDC_DESC_OFFSET   (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#endif

int usb_dap_channel_for_itf(uint8_t itf)
{
  if (itf == ITF_NUM_PROBE)
    return 0;
  if (itf >= ITF_NUM_PROBE_EXTRA && itf < ITF_NUM_TOTAL)
    return itf - ITF_NUM_PROBE_EXTRA + 1;
  return -1;
}

static uint8_t const desc_hid_report[] =
{
  TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE)
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t itf)
{
  (void) itf;
  return desc_hid_report;
}

uint8_t desc_configuration[] =
{
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
  // Interface 0
#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V1)
  // HID (named interface)
  TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_PROBE, 4, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), DAP_OUT_EP_NUM, DAP_IN_EP_NUM, CFG_TUD_HID_EP_BUFSIZE, 1),
#elif (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
  // Bulk (named interface)
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_PROBE, 5, DAP_OUT_EP_NUM, DAP_IN_EP_NUM, 64),
#elif (PROBE_DEBUG_PROTOCOL == PROTO_OPENOCD_CUSTOM)
  // Bulk
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_PROBE, 0, DAP_OUT_EP_NUM, DAP_IN_EP_NUM, 64),
#endif
  // Interface 1 + 2
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_COM, 6, CDC_NOTIFICATION_EP_NUM, 64, CDC_DATA_OUT_EP_NUM, CDC_DATA_IN_EP_NUM, 64),
  // Reset interface
  TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, 7),
  // Additional SWD channels
#if PROBE_NUM_CHANNELS > 1
  DAP_EXTRA_DESCRIPTOR(1),
#endif
#if PROBE_NUM_CHANNELS > 2
  DAP_EXTRA_DESCRIPTOR(2),
#endif
#if PROBE_NUM_CHANNELS > 3
  DAP_EXTRA_DESCRIPTOR(3),
#endif
#if PROBE_NUM_CHANNELS > 4
  DAP_EXTRA_DESCRIPTOR(4),
#endif
#if PROBE_NUM_CHANNELS > 5
  DAP_EXTRA_DESCRIPTOR(5),
#endif
#if PROBE_NUM_CHANNELS > 6
  DAP_EXTRA_DESCRIPTOR(6),
#endif
#if PROBE_NUM_CHANNELS > 7
  DAP_EXTRA_DESCRIPTOR(7),
#endif
#if PROBE_NUM_CHANNELS > 8
  DAP_EXTRA_DESCRIPTOR(8),
#endif
#if PROBE_NUM_CHANNELS > 9
  DAP_EXTRA_DESCRIPTOR(9),
#endif
#if PROBE_NUM_CHANNELS > 10
  DAP_EXTRA_DESCRIPTOR(10),
#endif
  // Additional UARTs
#if PROBE_NUM_UARTS > 1
  CDC_EXTRA_DESCRIPTOR(1),
#endif
#if PROBE_NUM_UARTS > 2
  CDC_EXTRA_DESCRIPTOR(2),
#endif
#if PROBE_NUM_UARTS > 3
  CDC_EXTRA_DESCRIPTOR(3),
#endif
#if PROBE_NUM_UARTS > 4
  CDC_EXTRA_DESCRIPTOR(4),
#endif
#if PROBE_NUM_UARTS > 5
  CDC_EXTRA_DESCRIPTOR(5),
#endif
#if PROBE_NUM_UARTS > 6
  CDC_EXTRA_DESCRIPTOR(6),
#endif
#if PROBE_NUM_UARTS > 7
  CDC_EXTRA_DESCRIPTOR(7),
#endif
#if PROBE_NUM_UARTS > 8
  CDC_EXTRA_DESCRIPTOR(8),
#endif
#if PROBE_NUM_UARTS > 9
  CDC_EXTRA_DESCRIPTOR(9),
#endif
#if PROBE_NUM_UARTS > 10
  CDC_EXTRA_DESCRIPTOR(10),
#endif
};

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "Incorrect configuration descriptor size");

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index; // for multiple configurations
  /* Hack in CAP_BREAK support */
  desc_configuration[CDC_DESC_OFFSET + 8 + 9 + 5 + 5 + 4 - 1] = 0x6;
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
char const* string_desc_arr [] =
{
  (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
  "Raspberry Pi", // 1: Manufacturer
  PROBE_PRODUCT_STRING, // 2: Product
  usb_serial,     // 3: Serial, uses flash unique ID
  "CMSIS-DAP v1 Interface", // 4: Interface descriptor for HID transport
#if PROBE_NUM_CHANNELS > 1
  "CMSIS-DAP v2 Interface 0", // 5: Interface descriptor for Bulk transport, SWD channel 0
#else
  "CMSIS-DAP v2 Interface", // 5: Interface descriptor for Bulk transport
#endif
#if PROBE_NUM_UARTS > 1
  "CDC-ACM UART Interface 0", // 6: Interface descriptor for CDC, UART 0
#else
  "CDC-ACM UART Interface", // 6: Interface descriptor for CDC
#endif
  "Reset", // 7: Interface descriptor for Reset
  // 8+: SWD channels 1..n-1. Must contain "CMSIS-DAP" for host tools to recognise them.
#if PROBE_NUM_CHANNELS > 1
  "CMSIS-DAP v2 Interface 1",
#endif
#if PROBE_NUM_CHANNELS > 2
  "CMSIS-DAP v2 Interface 2",
#endif
#if PROBE_NUM_CHANNELS > 3
  "CMSIS-DAP v2 Interface 3",
#endif
#if PROBE_NUM_CHANNELS > 4
  "CMSIS-DAP v2 Interface 4",
#endif
#if PROBE_NUM_CHANNELS > 5
  "CMSIS-DAP v2 Interface 5",
#endif
#if PROBE_NUM_CHANNELS > 6
  "CMSIS-DAP v2 Interface 6",
#endif
#if PROBE_NUM_CHANNELS > 7
  "CMSIS-DAP v2 Interface 7",
#endif
#if PROBE_NUM_CHANNELS > 8
  "CMSIS-DAP v2 Interface 8",
#endif
#if PROBE_NUM_CHANNELS > 9
  "CMSIS-DAP v2 Interface 9",
#endif
#if PROBE_NUM_CHANNELS > 10
  "CMSIS-DAP v2 Interface 10",
#endif
  // Then UARTs 1..n-1
#if PROBE_NUM_UARTS > 1
  "CDC-ACM UART Interface 1",
#endif
#if PROBE_NUM_UARTS > 2
  "CDC-ACM UART Interface 2",
#endif
#if PROBE_NUM_UARTS > 3
  "CDC-ACM UART Interface 3",
#endif
#if PROBE_NUM_UARTS > 4
  "CDC-ACM UART Interface 4",
#endif
#if PROBE_NUM_UARTS > 5
  "CDC-ACM UART Interface 5",
#endif
#if PROBE_NUM_UARTS > 6
  "CDC-ACM UART Interface 6",
#endif
#if PROBE_NUM_UARTS > 7
  "CDC-ACM UART Interface 7",
#endif
#if PROBE_NUM_UARTS > 8
  "CDC-ACM UART Interface 8",
#endif
#if PROBE_NUM_UARTS > 9
  "CDC-ACM UART Interface 9",
#endif
#if PROBE_NUM_UARTS > 10
  "CDC-ACM UART Interface 10",
#endif
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;

  if ( index == 0)
  {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  }else
  {
    // Convert ASCII string into UTF-16

    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];

    // Cap at max char
    chr_count = strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    for(uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

  return _desc_str;
}

/* [incoherent gibbering to make Windows happy] */

//--------------------------------------------------------------------+
// BOS Descriptor
//--------------------------------------------------------------------+

/* Microsoft OS 2.0 registry property descriptor
Per MS requirements https://msdn.microsoft.com/en-us/library/windows/hardware/hh450799(v=vs.85).aspx
device should create DeviceInterfaceGUIDs. It can be done by driver and
in case of real PnP solution device should expose MS "Microsoft OS 2.0
registry property descriptor". Such descriptor can insert any record
into Windows registry per device/configuration/interface. In our case it
will insert "DeviceInterfaceGUIDs" multistring property.


https://developers.google.com/web/fundamentals/native-hardware/build-for-webusb/
(Section Microsoft OS compatibility descriptors)
*/
#define MS_OS_FEATURE_DESC_LEN 0xA0
#define MS_OS_20_DESC_LEN  (0x0A + 0x08 + PROBE_NUM_CHANNELS * MS_OS_FEATURE_DESC_LEN + RPI_RESET_MS_OS_20_DESC_LEN)

#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

uint8_t const desc_bos[] =
{
  // total length, number of device caps
  TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

  // Microsoft OS 2.0 descriptor
  TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1)
};

// WinUSB function subset for one CMSIS-DAP v2 interface
#define MS_OS_20_DAP_FUNCTION(itf) \
  /* Function Subset header: length, type, first interface, reserved, subset length */ \
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), (itf), 0, U16_TO_U8S_LE(MS_OS_FEATURE_DESC_LEN), \
  /* MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID */ \
  U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, \
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* sub-compatible */ \
  /* MS OS 2.0 Registry property descriptor: length, type */ \
  U16_TO_U8S_LE(MS_OS_FEATURE_DESC_LEN-0x08-0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY), \
  U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), /* wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16 */ \
  'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, \
  'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00, \
  U16_TO_U8S_LE(0x0050), /* wPropertyDataLength */ \
  /* bPropertyData "{CDB3B5AD-293B-4663-AA36-1AAE46463776}" as a UTF-16 string (b doesn't mean bytes) */ \
  '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00, '5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00, \
  '2', 0x00, '9', 0x00, '3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00, '3', 0x00, '-', 0x00, \
  'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00, '-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00, \
  '6', 0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, '6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00

uint8_t const desc_ms_os_20[] =
{
  // Set header: length, type, windows version, total length
  U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

  // Configuration subset header: length, type, configuration index, reserved, configuration total length
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN-0x0A),

  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE),
#if PROBE_NUM_CHANNELS > 1
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 0),
#endif
#if PROBE_NUM_CHANNELS > 2
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 1),
#endif
#if PROBE_NUM_CHANNELS > 3
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 2),
#endif
#if PROBE_NUM_CHANNELS > 4
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 3),
#endif
#if PROBE_NUM_CHANNELS > 5
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 4),
#endif
#if PROBE_NUM_CHANNELS > 6
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 5),
#endif
#if PROBE_NUM_CHANNELS > 7
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 6),
#endif
#if PROBE_NUM_CHANNELS > 8
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 7),
#endif
#if PROBE_NUM_CHANNELS > 9
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 8),
#endif
#if PROBE_NUM_CHANNELS > 10
  MS_OS_20_DAP_FUNCTION(ITF_NUM_PROBE_EXTRA + 9),
#endif

  RPI_RESET_MS_OS_20_DESCRIPTOR(ITF_NUM_RESET)
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

uint8_t const * tud_descriptor_bos_cb(void)
{
  return desc_bos;
}
