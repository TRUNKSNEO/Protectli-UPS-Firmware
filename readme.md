# PL02 Firmware
This is the firwmare for the Protectli UPS. It uses zephyr RTOS running on the ESP32 and an RP2040.

## Getting Started
Before getting started, make sure you have a proper Zephyr development
environment. You can follow the official
[Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

```shell
west init -m git@github.com:o7-machinehum/pl02-fw.git --mr main pl02-fw
cd pl02-fw
west update
```

### Build & Run - ESP32/RP2040
The application can be built by running:

```shell
west build -b pl02_esp32 esp32-app --build-dir build/esp32
west build -b pl02_rp2040 rp2040-app --build-dir build/rp2040
```

Once you have built the application you can flash it by running:

```shell
west flash --esp-device /dev/tty.usbserial-0001 --build-dir build/esp32
west flash --runner jlink --build-dir build/rp2040
```

If flashing doesn't work on the ESP32, you might need to reduce the baud rate. Append `--esp-baud-rate 460800` to the end of the flash command.

```shell
# If you don't have a jlink (programmer). You can use this.
sudo picotool load build/rp2040/zephyr/zephyr.elf
```

### Build & Run - STM32
The STM32 doesn't not used zephyr, it's a libopencm3 project.
```shell
git submodule init
git submodule update
```

Install Deps
```shell
sudo pacman -S arm-none-eabi-gcc openocd # Or whatever your OS is
```

Building and flashing
```shell
cd stm32-app
make
make flash
```

## ESP32 Networking
To connect the ESP32 to a WiFi network start by inserting the USB cable into board, so should see a serial device appear at `/dev/ttyUSBx`, or COMx for Windows. Open a shell here using minicom or otherwise.

``` bash
# Scan for networks

uart:~$ wifi scan
Scan requested

Num  | SSID                             (len) | Chan (Band)   | RSSI | Security
1    | Turnip_WiFi                      11    | 1    (2.4GHz) | -57  | WPA2-PSK
2    | you-kids-get-off-my-lan          23    | 11   (2.4GHz) | -59  | WPA2-PSK
3    | hnm-68027                        9     | 1    (2.4GHz) | -73  | WPA2-PSK
4    | Sunrise_2.4GHz_CF0D50            21    | 1    (2.4GHz) | -88  | WPA2-PSK
5    | Sunrise_2.4GHz_6CD280            21    | 1    (2.4GHz) | -90  | WPA2-PSK
6    | Spalenring-Garage                17    | 6    (2.4GHz) | -90  | WPA2-PSK
7    | DIRECT-48-HP M28 LaserJet        25    | 6    (2.4GHz) | -91  | WPA2-PSK

```

``` bash
# Connect to network
uart:~$ wifi connect "you-kids-get-off-my-lan" 4 Password1234
Connection requested
Connected
[00:00:31.616,000] <inf> net_dhcpv4: Received: 192.168.1.109

```

You should be able to ping the device on the network.
``` bash
uart:~$ net ping 192.168.1.2
PING 192.168.1.2
28 bytes from 192.168.1.2 to 192.168.1.109: icmp_seq=1 ttl=64 time=14 ms
28 bytes from 192.168.1.2 to 192.168.1.109: icmp_seq=2 ttl=64 time=7 ms
28 bytes from 192.168.1.2 to 192.168.1.109: icmp_seq=3 ttl=64 time=4 ms
```

Various other networking things
``` bash
uart:~$ net ipv4
IPv4 support                              : enabled
IPv4 fragmentation support                : disabled
Max number of IPv4 network interfaces in the system          : 1
Max number of unicast IPv4 addresses per network interface   : 1
Max number of multicast IPv4 addresses per network interface : 1

IPv4 addresses for interface 1 (0x3ffb2708) (Ethernet)
====================================================
Type            State           Lifetime (sec)  Address
DHCP    preferred       192.168.1.109/255.255.255.0
uart:~$ net arp
     Interface  Link              Address
[ 0] 1          A0:B5:49:B3:E5:B0 192.168.1.2
```
