# sc-ble-bridge
The main goal of this utility is to provide workaround for [steam-for-linux issue](https://github.com/ValveSoftware/steam-for-linux/issues/7697) which makes Valve's Steam Controller unusable in BLE mode with bluez 5.56 or newer installed.
## How does it work?
This utility catches newly connected SC and creates Virtual SC that acts as a bridge between steam and SC. Currently, only one SC connected to your machine at the time is supported. Connecting more will probably result in errors. This utility unfortunately relies on using `sudo` as you not only have to access `/dev/uhid` but also access SC's that should not be readable by normal user so that steam also won't be able to read them.
## Prerequisites
Before starting this utility you have to make sure that real SC connected via BLE won't be readable by steam. The best way of doing this is by creating udev rules. First of all, exclude SC via BLE by adding `ATTRS{idProduct}!="1106"` to all rules matching SC via BLE. You might find them in files called `60-steam-input.rules`, `71-valve-controllers.rules` or other with similar names in `/usr/lib/udev/rules.d/`. Your rule in `steam-input.rules` before changing should look like this:
```
# Valve HID devices over bluetooth hidraw
KERNEL=="hidraw*", KERNELS=="*28DE:*", MODE="0660", TAG+="uaccess"
```
after the change it will look like this:
```
# Valve HID devices over bluetooth hidraw
KERNEL=="hidraw*", KERNELS=="*28DE:*", ATTRS{idProduct}!="1106" MODE="0660", TAG+="uaccess"
```
Now, create new rules file e.g. `69-sc-ble-bridge.rules` with following rules:
```
KERNEL=="hidraw*", KERNELS=="*28DE:1106*", ATTRS{country}=="00", MODE="0600"
KERNEL=="hidraw*", KERNELS=="*28DE:1106*", ATTRS{country}=="01", MODE="0660", TAG+="uaccess"
```
## Requirements
- glib 2.36 or newer - should be available as it required by bluez.
## Building
To build this utility run:
```
make
```
Executable file should appear in `bin` directory.
## Usage
To use this utility run:
```
sudo ./sc-ble-bridge
```
Now you should be able to once again use your SC via BLE.
## TODO
- Support for multiple controllers at the same time
- Better structuring and fix typing
