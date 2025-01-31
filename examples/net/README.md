<!--
     Copyright 2024, UNSW
     SPDX-License-Identifier: CC-BY-SA-4.0
-->

# UIO Networking example

This example show case an Ethernet driver VM running with sDDF's echo server on the Odroid C4. It is a shallow copy that only contains files that needs to be modified to accomodate the Ethernet driver VM. This is a summary of modified files, relative to `/dep/sddf/examples/echo_server/`:
- `board/odroidc4/echo_server.system`: swapped out native Ethernet driver for VM.
- `include/serial_config/serial_config.h`: Hooked VM to serial TX virt.
- `Makefile`: specified `zig cc` for Linux userspace drivers compilation and added libvmm related paths.
- `echo.mk`: swapped out native Ethernet driver build step for VM build steps.

The basic idea is we have a userspace program in Linux that converts sDDF's network TX/RX into corresponding syscalls in a Linux socket.
- The sDDF network control, data regions and notification from virtualisers are mapped into Linux userspace via UIO.
- A promiscuous socket is opened to sniff all incoming frames. When a frame comes through from the socket, we enqueue the data into the appropriate sDDF queues and fault on a pre-determined address so the VMM can notify the RX virtualiser.
- When a client wants to transmit data, we will get TX virt notifications via UIO, then we simply write the data out into the socket.

Even though we already have a native Ethernet driver for Odroid C4, this example is still useful as it could be used to provide Ethernet driver VM on new boards or further developed into a WiFi/Cellular/Bluetooth driver VM.

The example currently works on the following platforms:
* HardKernel Odroid-C4

Things to note when using the network UIO driver:
- You need to map in the control queues as uncached in the guest and native virtualisers. This is because Linux treats UIO regions as device memory thus map them uncached to userspace.

## Compilation
To compile this example, run:
```
make MICROKIT_SDK=<absolute path to 1.4.1 SDK> MICROKIT_BOARD=odroidc4 MICROKIT_CONFIG=release
```

### Boot
On a successful boot, your console will look like this:
```
Starting syslogd: OK
Starting klogd: OK
Running sysctl: OK
Saving random seed: [    4.917610] random: crng init done
OK
Starting network: OK
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
2: eth0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop qlen 1000
    link/ether 00:1e:06:4a:35:e4 brd ff:ff:ff:ff:ff:ff
[    5.149624] meson8b-dwmac ff3f0000.ethernet eth0: PHY [mdio_mux-0.0:00] driver [RTL8211F Gigabit Ethernet] (irq=POLL)
[    5.150295] meson8b-dwmac ff3f0000.ethernet eth0: Register MEM_TYPE_PAGE_POOL RxQ-0
[    5.157634] meson8b-dwmac ff3f0000.ethernet eth0: No Safety Features support found
[    5.157800] meson8b-dwmac ff3f0000.ethernet eth0: PTP not supported by HW
[    5.158231] meson8b-dwmac ff3f0000.ethernet eth0: configuring for phy/rgmii link mode
[   10.312812] meson8b-dwmac ff3f0000.ethernet eth0: Link is Up - 1Gbps/Full - flow control off
ethtool -K eth0 gso off
ethtool -K eth0 gro off
ethtool -K eth0 tx off rx off
UIO(NET): *** Starting up
UIO(NET): *** Network interface: eth0
UIO(NET): *** Setting up raw promiscuous socket
[   13.203873] device eth0 entered promiscuous mode
UIO(NET): *** Creating epoll
UIO(NET): *** Binding socket to epoll
UIO(NET): *** Mapping in sDDF control and data queues
UIO(NET): *** Total control + data size is 0xe00000
UIO(NET): *** Setting up sDDF control and data queues
UIO(NET): *** rx_free_drv   = 0xffff8d905000
UIO(NET): *** rx_active_drv = 0xffff8db05000
UIO(NET): *** tx_free_drv   = 0xffff8dd05000
UIO(NET): *** tx_active_drv = 0xffff8df05000
UIO(NET): *** rx_data_drv   = 0xffff8e105000
UIO(NET): *** tx_data_drv cli0 = 0xffff8e305000
UIO(NET): *** tx_data_drv cli1 = 0xffff8e505000
UIO(NET): *** Setting up UIO TX and RX interrupts from VMM "incoming"
UIO(NET): *** Binding UIO TX and RX incoming interrupts to epoll
UIO(NET): *** Setting up UIO data passing between VMM and us
UIO(NET): *** RX paddr: 0x8000000
UIO(NET): *** TX cli0 paddr: 0x8200000
UIO(NET): *** TX cli1 paddr: 0x8600000
UIO(NET): *** Setting up UIO TX and RX interrupts to VMM "outgoing"
UIO(NET): *** Waiting for RX virt to boot up
UIO(NET): *** All initialisation successful, now sending all pending TX active before we block on events
UIO(NET): *** All pending TX active have been sent thru the raw sock, entering event loop now.
UIO(NET): *** You won't see any output from UIO Net anymore. Unless there is a warning or error.
LWIP|NOTICE: DHCP request for client0 returned IP address: 172.16.1.218
LWIP|NOTICE: DHCP request for client1 returned IP address: 172.16.1.219
```

Though the MAC, IP and memory addreses would be different of course. There will be delay in the tens of seconds between the Linux userspace network driver initialisation and LWIP DHCP completion. This is due to all the components being able to initialise first before the guest can get to userspace. So LWIP will send many DHCP requests without success, with increasing timeout for each request.
