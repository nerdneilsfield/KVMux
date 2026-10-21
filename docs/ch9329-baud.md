# Configure the CH9329 baud rate on Linux

Use `scripts/ch9329_baud.py` to read or change the baud rate stored in a CH9329 through its existing serial connection. The procedure is for enclosed CH9329 devices whose serial interface is already in protocol transmission mode; it does not require access to `SET`, `CFG0`, `CFG1` or other configuration pins.

The script accepts the nine baud rates listed in the [CH9329 V1.1 datasheet, section 5](https://atta.szlcsc.com/upload/public/pdf/source/20240313/4552A4F439D1C1C57C5F4F80A48274B1.pdf):

```text
1200 2400 4800 9600 14400 19200 38400 57600 115200
```

The default configuration is `9600` baud, 8 data bits, no parity and 1 stop bit (8N1). The CH9329 V1.1 manual states that `115200` is unavailable with 3.3 V supply; use `57600` unless the module documentation confirms that `115200` is supported.

## Prerequisites

- Linux with permission to open the CH9329 serial device.
- Python 3.8 or later and `pyserial` (Ubuntu/Debian installation):

  ```sh
  sudo apt install python3-serial
  ```

- The CH9329 must be connected to a host serial adapter that exposes a device such as `/dev/ttyUSB0` or `/dev/ttyCH341USB0`.
- Stop KVMux and the relay before opening the serial device with the script.

Run the commands from the repository root. Use the baud rate at which KVMux currently connects to this device. `Configured baud:` reports the saved parameter; after a write, the active serial rate remains unchanged until the chip powers on again.

## Read the current configuration

Run the script without `--set-baud`:

```sh
python3 scripts/ch9329_baud.py /dev/ttyUSB0 --baud 9600
```

Replace `/dev/ttyUSB0` with the actual device. Replace `9600` with the rate currently configured in the CH9329.

Output similar to the following confirms that the device answered the protocol request:

```text
Configured baud: 9600
```

## Change the baud rate

To change a CH9329 currently configured for `9600` to `57600`, run:

```sh
python3 scripts/ch9329_baud.py /dev/ttyUSB0 \
  --baud 9600 \
  --set-baud 57600
```

The script performs these operations:

1. Reads the 50-byte parameter block with `CMD_GET_PARA_CFG` (`0x08`).
2. Checks that the serial mode is protocol transmission mode.
3. Changes the 4-byte big-endian baud-rate field and converts hardware-selected modes to their software equivalents, preserving the other parameter bytes.
4. Writes the parameter block with `CMD_SET_PARA_CFG` (`0x09`).
5. Prints instructions to power-cycle the device and read back the new setting.

The protocol specifies that saved parameters take effect at the next power-on. The script leaves that step to you rather than sending a software reset. After it prints `Saved baud`, disconnect all power sources to the enclosed device, then reconnect them. A device with two USB cables may still receive power when only one cable is unplugged. Verify the setting with the new rate:

```sh
python3 scripts/ch9329_baud.py /dev/ttyUSB0 --baud 57600
```

Expected output:

```text
Configured baud: 57600
```

Without `--set-baud`, the script only reads the configuration. If the chip is already at the requested rate, the script skips the write.

## Start KVMux relay at the same rate

The Linux relay changes the host serial-port setting; it does not configure the CH9329. After verifying the chip, start the relay with the same rate:

```sh
build/linux-debug-headless/kvmux-relay --debug --serve \
  --device "<capture-device-id>" \
  --serial /dev/ttyUSB0 \
  --baud 57600
```

The GUI must use the same rate when it connects to CH9329 hardware locally. For a remote session, only the relay needs the hardware-side serial setting.

## Protocol-mode requirement

The script can configure an enclosed device through the serial cable when `CMD_GET_PARA_CFG` responds. The CH9329 must already be using software or hardware-selected protocol transmission mode (`0x00` or `0x80`). The script converts the returned hardware-selected mode values to the equivalent software values required by `CMD_SET_PARA_CFG`; it does not change the USB keyboard/mouse function.

ASCII mode and pass-through mode do not expose this binary configuration interface. Such devices need the vendor's configuration method; this script does not switch them into protocol mode. A timeout alone cannot distinguish the wrong baud rate, wrong address, disconnected device or incompatible mode. No parameter write is sent unless a valid configuration response has been received.

## Command reference

```text
python3 scripts/ch9329_baud.py <serial-device> [--baud CURRENT] [--set-baud TARGET] [--address ADDRESS]
```

| Option | Default | Description |
| --- | --- | --- |
| `<serial-device>` | none | Linux serial device path, for example `/dev/ttyUSB0`. |
| `--baud CURRENT` | `9600` | Rate used to open the serial link before the command. |
| `--set-baud TARGET` | read-only | Saves one of the supported rates listed above. |
| `--address ADDRESS` | `0` | CH9329 address in decimal or `0x` notation. Broadcast address `0xFF` is rejected. |

The script exits with status `0` after a successful read or acknowledged write, `1` for a dependency, serial or protocol error, and `2` for invalid arguments. An acknowledged write confirms that the chip accepted the parameters; the separate read after power-cycling verifies operation at the new rate.

If the write times out, its outcome is unknown. Power-cycle the device and use a read-only command at the requested new rate, then at the old rate if needed, before attempting another write.

## Protocol details

The script uses the WCH CH9329 serial protocol frame:

```text
57 AB | ADDR | CMD | LEN | DATA | SUM
```

`SUM` is the low 8 bits of the sum of every preceding frame byte. The baud-rate field is bytes `[3:7]` of the 50-byte `CMD_GET_PARA_CFG` data block, with the most significant byte first. The protocol reference is [WCH CH9329 serial communication protocol](https://www.wch.cn/uploads/file/20190508/1557278355473027.pdf).
