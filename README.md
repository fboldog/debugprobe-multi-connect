# Debugprobe

Firmware source for the Raspberry Pi Debug Probe SWD/UART accessory. Can also be run on a Raspberry Pi Pico or Pico 2.

[Raspberry Pi Debug Probe product page](https://www.raspberrypi.com/products/debug-probe/)

[Raspberry Pi Pico product page](https://www.raspberrypi.com/products/raspberry-pi-pico/)

[Raspberry Pi Pico 2 product page](https://www.raspberrypi.com/products/raspberry-pi-pico-2/)

## Documentation

Debug Probe documentation can be found at the [Raspberry Pi documentation](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html#about-the-debug-probe) and in the [Getting Started with Pico PDF](https://pip.raspberrypi.com/documents/RP-008276-DS).

## Hacking

For the purpose of making changes or studying of the code, you may want to compile the code yourself.

First, clone the repository:
```bash
git clone https://github.com/raspberrypi/debugprobe
cd debugprobe
```

Initialize and update the submodules:
```bash
 git submodule update --init --recursive
```

Then create and switch to the build directory:
```bash
 mkdir build
 cd build
```

If your environment doesn't contain `PICO_SDK_PATH`, then either add it to your environment variables with `export PICO_SDK_PATH=/path/to/sdk` or add `-DPICO_SDK_PATH=/path/to/sdk` to the arguments to CMake below. `PICO_SDK_PATH` must point to a version 2.3.0 or greater install.

Run cmake and build the code:
```bash
 cmake ..
 make
```

Done! You should now have a `debugprobe.uf2` that you can upload to your Debug Probe via the UF2 bootloader.

## Building for the Pico 1

If you want to create the version that runs on the Pico, then you need to invoke `cmake` in the sequence above with the `-DPICO_BOARD=pico` option:
```bash
cmake -DPICO_BOARD=pico ..
```

This will build with the configuration for the Pico and call the output program `debugprobe_on_pico.uf2`, as opposed to `debugprobe.uf2` for the accessory hardware.

Note that if you first ran through the whole sequence to compile for the Debug Probe, then you don't need to start back at the top. You can just go back to the `cmake` step and start from there.

## Building for the Pico 2

If using an existing debugprobe clone:
- You must completely regenerate your build directory, or use a different one.
- You must also sync and update submodules.

```bash
git submodule sync
git submodule update --init --recursive
mkdir build-pico2
cd build-pico2
cmake -DPICO_BOARD=pico2 ../
```

This will build with the configuration for the Pico 2 and call the output program `debugprobe_on_pico2.uf2`.

## Multiple SWD channels and UARTs

One Pico can serve several targets at once. Each SWD channel has its own pins and PIO state machine and appears on the host as its own CMSIS-DAP v2 USB interface, so a separate OpenOCD (or other CMSIS-DAP debugger) instance can attach to each one. Each target UART appears as its own CDC-ACM serial port.

```bash
# SWD-only: 4 SWD channels, 1 UART
cmake -DPICO_BOARD=pico -DPROBE_NUM_CHANNELS=4 ..
# Paired: one SWD channel + one UART per target
cmake -DPICO_BOARD=pico  -DPROBE_NUM_CHANNELS=3 -DPROBE_NUM_UARTS=3 ..   # RP2040: 3 pairs
cmake -DPICO_BOARD=pico2 -DPROBE_NUM_CHANNELS=4 -DPROBE_NUM_UARTS=4 ..   # RP2350: 4 pairs
cmake -DPICO_BOARD=waveshare_rp2350_pizero -DPROBE_NUM_CHANNELS=4 -DPROBE_NUM_UARTS=4 ..   # Waveshare RP2350-PiZero
```
Both settings default to `1`, which builds the stock firmware. The output file name gets `_<n>ch` and/or `_<n>uart` suffixes, e.g. `debugprobe_on_pico2_4ch_4uart.uf2`.

### Limits

* **PIO state machines:** every SWD channel needs one. Every UART beyond the two hardware UARTs runs on PIO and needs two (TX + RX). One state machine is always kept for AutoBaud. So `SWD + 2 × PIO UARTs ≤ 7` on RP2040 and `≤ 11` on RP2350.
* **USB endpoints:** `(SWD − 1) + 2 × (UART − 1) ≤ 10`.
* Configurations over either limit fail at compile time.

### Pico pinouts

With `PROBE_NUM_UARTS=1` (`include/board_pico_config.h`, SWD-only layout, up to 8 channels):

| Channel | SWCLK | SWDIO | nRESET |
|---------|-------|-------|--------|
| 0       | GP2   | GP3   | GP1    |
| 1       | GP6   | GP7   | GP8    |
| 2       | GP9   | GP10  | GP11   |
| 3       | GP12  | GP13  | GP14   |
| 4       | GP16  | GP17  | GP18   |
| 5       | GP19  | GP20  | GP21   |
| 6       | GP22  | GP26  | GP27   |
| 7 (Pico 2 only) | GP15 | GP28 | - |

The UART stays on GP4 (TX) / GP5 (RX).

With `PROBE_NUM_UARTS>1` (paired layout, up to 4 pairs):

| Pair | SWCLK | SWDIO | nRESET | UART TX | UART RX | UART type |
|------|-------|-------|--------|---------|---------|-----------|
| 0    | GP2   | GP3   | GP1    | GP4     | GP5     | hardware (uart1) |
| 1    | GP9   | GP10  | GP11   | GP12    | GP13    | hardware (uart0) |
| 2    | GP14  | GP15  | GP16   | GP17    | GP18    | PIO |
| 3    | GP19  | GP20  | GP21   | GP22    | GP26    | PIO (Pico 2 only) |

Pair 0 is the classic Debugprobe-on-Pico pinout. To use other pins, edit `PROBE_CHANNEL_PINS` and `PROBE_UART_PIN_TABLE` in the board header:

* **SWD entry:** `PROBE_PINS(swclk, swdio, swdi, swdioen, reset)`, with `-1` for unused pins.
* **UART entry:** `PROBE_UART(tx, rx)`, or `PROBE_UART_PINS_FULL(tx, rx, cts, rts, dtr, hwfc)` to add RTS/DTR GPIOs or hardware flow control.

A UART uses a hardware UART automatically when its pins are a hardware TX/RX pair and that UART is still free. Hardware pairs are GPIO 4n / 4n+1, and on RP2350 also GPIO 4n+2 / 4n+3 (UART_AUX, e.g. GP14/GP15). Otherwise the UART runs on PIO.

UART limitations:
* PIO UARTs support only 8N1.
* AutoBaud and the UART activity LEDs only apply to UART 0.
* The firmware's debug output (TX only, on the board's default UART) is disabled when a probe channel or target UART uses its TX pin. When `PROBE_DEBUG_LOG` is enabled and the pin is free, that UART is kept for the log, and a target UART that would have used it runs on PIO instead. Without the log, a target UART may take that UART, which also disables the debug output. That output is compiled out by default; enable it with `-DPROBE_DEBUG_LOG=1` (`2`: + debug messages, `3`: + packet dumps).

### Waveshare RP2350-PiZero pinout

Build with `-DPICO_BOARD=waveshare_rp2350_pizero` and connect the host to the native USB-C port marked "USB", not the PIO-USB port. The 40-pin header uses Raspberry Pi BCM numbering, which matches the RP2350 GPIO numbers. Pins are grouped so that each target uses adjacent header pins. The board has no user LED.

Firmware debug log: build with `-DPROBE_DEBUG_LOG=1` (2 adds debug messages, 3 adds packet dumps) and connect a USB-serial adapter's RX to GP24 (header pin 18) and GND to pin 20, at 115200 8N1. The log is TX only and uses uart1, so in the paired layout UART 1 runs on PIO instead of uart1. The log is available in the paired layout and in the SWD-only layout with up to 4 channels, because channel 4 uses GP24 as nRESET.

With `PROBE_NUM_UARTS>1` (paired layout, up to 4 pairs). Header pin numbers are in brackets:

| Pair | SWCLK | SWDIO | nRESET | UART TX | UART RX | GND | UART type |
|------|-------|-------|--------|---------|---------|-----|-----------|
| 0 | GP2 (3) | GP3 (5) | GP4 (7) | GP14 (8) | GP15 (10) | 6, 9 | hardware (uart0) |
| 1 | GP17 (11) | GP27 (13) | GP18 (12) | GP22 (15) | GP23 (16) | 14 | hardware (uart1) |
| 2 | GP10 (19) | GP9 (21) | GP11 (23) | GP25 (22) | GP8 (24) | 20, 25 | PIO |
| 3 | GP19 (35) | GP26 (37) | GP16 (36) | GP20 (38) | GP21 (40) | 39 | PIO |

With `PROBE_NUM_UARTS=1` (SWD-only layout, up to 9 channels), the UART is on GP14 (8) / GP15 (10), the Raspberry Pi console pins:

| Channel | SWCLK | SWDIO | nRESET |
|---------|-------|-------|--------|
| 0 | GP2 (3) | GP3 (5) | GP4 (7) |
| 1 | GP17 (11) | GP27 (13) | GP18 (12) |
| 2 | GP10 (19) | GP9 (21) | GP11 (23) |
| 3 | GP19 (35) | GP26 (37) | GP16 (36) |
| 4 | GP22 (15) | GP23 (16) | GP24 (18) |
| 5 | GP25 (22) | GP8 (24) | GP7 (26) |
| 6 | GP5 (29) | GP6 (31) | GP13 (33) |
| 7 | GP20 (38) | GP21 (40) | GP12 (32) |
| 8 | GP0 (27) | GP1 (28) | - |

### USB interfaces

| Function | USB interface | Interface string |
|----------|---------------|------------------|
| SWD channel 0 | 0 | `CMSIS-DAP v2 Interface 0` |
| UART 0 | 1 (+2) | `CDC-ACM UART Interface 0` |
| SWD channel *n* (n ≥ 1) | 3 + *n* | `CMSIS-DAP v2 Interface n` |
| UART *n* (n ≥ 1) | 3 + SWD + 2(*n* − 1) | `CDC-ACM UART Interface n` |

Interface 3 is the reset interface. With only one SWD channel or one UART, the names keep their original unnumbered form.

### OpenOCD

To attach OpenOCD to a channel, select the USB interface with `cmsis-dap usb interface`. Give every instance its own GDB/telnet/TCL ports:
```bash
# channel 0
openocd -f interface/cmsis-dap.cfg -c "cmsis-dap usb interface 0" \
        -f target/rp2040.cfg -c "adapter speed 5000"
# channel 1, in a second terminal
openocd -f interface/cmsis-dap.cfg -c "cmsis-dap usb interface 4" \
        -c "gdb_port 3334; telnet_port 4445; tcl_port 6667" \
        -f target/rp2040.cfg -c "adapter speed 5000"
```
If more than one probe is connected, also add `-c "adapter serial <serial>"`.

### Stable serial port names (Linux)

The UARTs show up as `/dev/ttyACM*` in no particular order. The `/dev/serial/by-id/*-if<NN>` names include the interface number. Alternatively, udev rules such as these (e.g. in `/etc/udev/rules.d/99-debugprobe.rules`) match on the interface string:
```
SUBSYSTEM=="tty", ENV{ID_VENDOR_ID}=="2e8a", ENV{ID_MODEL_ID}=="000c", ATTRS{interface}=="CDC-ACM UART Interface 0", SYMLINK+="debugprobe-uart0"
SUBSYSTEM=="tty", ENV{ID_VENDOR_ID}=="2e8a", ENV{ID_MODEL_ID}=="000c", ATTRS{interface}=="CDC-ACM UART Interface 1", SYMLINK+="debugprobe-uart1"
SUBSYSTEM=="tty", ENV{ID_VENDOR_ID}=="2e8a", ENV{ID_MODEL_ID}=="000c", ATTRS{interface}=="CDC-ACM UART Interface 2", SYMLINK+="debugprobe-uart2"
SUBSYSTEM=="tty", ENV{ID_VENDOR_ID}=="2e8a", ENV{ID_MODEL_ID}=="000c", ATTRS{interface}=="CDC-ACM UART Interface 3", SYMLINK+="debugprobe-uart3"
```
Vendor and product are matched with `ENV{...}`, because udev requires all `ATTRS{...}` keys of a rule to match on the same parent device, and `interface` and `idVendor` sit on different ones. With several probes attached, add `ENV{ID_SERIAL_SHORT}=="<serial>"` and use a per-probe link name.

### Throughput

All SWD channels share a single DAP command thread that serves them round-robin, one USB packet at a time. Each channel can run its own SWD clock, but the total command throughput is shared among the active channels. All UARTs are polled by one thread at the rate the fastest open port needs.

## AutoBaud Mode

Mode which automatically detects and sets the UART baud rate as data arrives.

To enable AutoBaud, configure the USB CDC port to the following custom baud rate:
```
9728 (0x2600)
```
> **Note:** Some Linux serial tools cannot set custom baud values. PuTTY on Windows and any terminal that supports arbitrary baud rates works.

Changing the baud rate to any other value disables AutoBaud.

