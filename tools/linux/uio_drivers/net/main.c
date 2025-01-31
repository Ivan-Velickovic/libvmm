/*
 * Copyright 2024, UNSW
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uio/libuio.h>

#include "log.h"

#include <ethernet_config.h>
#include <uio/net.h>

#define ARGC_REQURED 2

/* Redefine this based on your sDDF version... */
#define NET_DATA_REGION_BYTES NET_DATA_REGION_SIZE

/* IMPORTANT: This driver currently assumes the network interface
   has an "Ethernet-like" MTU, you should change this if your net inf is
   different. */
char frame[ETH_FRAME_LEN];

/* Event queue for polling */
#define MAX_EVENTS 16 // Arbitrary length
struct epoll_event events[MAX_EVENTS];

#define PAGE_SIZE_4K 0x1000

/* Raw socket FD to send/recv network frames */
int sock_fd;

/* UIO FD to access the sDDF control and data queues */
int uio_sddf_net_queues_fd;
char *sddf_net_queues_vaddr;
net_queue_handle_t rx_queue;
net_queue_handle_t tx_queue;

char *rx_free_drv;
char *rx_active_drv;
char *tx_free_drv;
char *tx_active_drv;
char *rx_data_drv;
char *tx_datas_drv[NUM_NETWORK_CLIENTS];

/* UIO FDs to wait for TX/RX interrupts from VMM */
int uio_sddf_net_tx_incoming_fd;
int uio_sddf_net_rx_incoming_fd;

/* UIO FDs to signal TX/RX to VMM */
int uio_sddf_net_tx_outgoing_fd;
int uio_sddf_net_rx_outgoing_fd;
char *sddf_net_tx_outgoing_irq_fault_vaddr;
char *sddf_net_rx_outgoing_irq_fault_vaddr;

/* Polling FD to wait for events from the TX/RX UIO FD */
int epoll_fd;

/* UIO FDs to access the physical addresses of the sDDF data segment
   so we can deduct it from the offset in control queue to access the data */
int uio_sddf_vmm_net_info_passing_fd;
vmm_net_info_t *vmm_info_passing;

struct ifreq ifr;

int create_promiscuous_socket(const char *net_inf)
{
    // See https://man7.org/linux/man-pages/man7/packet.7.html for more details on these operations
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd == -1) {
        perror("create_promiscuous_socket(): socket()");
        LOG_NET_ERR("could not create socket.\n");
        exit(EXIT_FAILURE);
    }

    strncpy(ifr.ifr_name, net_inf, IFNAMSIZ);
    if (ioctl(sockfd, SIOCGIFINDEX, &ifr) == -1) {
        perror("create_promiscuous_socket(): ioctl()");
        LOG_NET_ERR("could not get network interface index.\n");
        exit(EXIT_FAILURE);
    }
    int ifindex = ifr.ifr_ifindex;

    struct packet_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(sockfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) == -1) {
        perror("create_promiscuous_socket(): setsockopt()");
        LOG_NET_ERR("could not set net device to promiscuous mode.\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_ll bind_address;
    memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sll_family   = AF_PACKET;
    bind_address.sll_protocol = htons(ETH_P_ALL);
    bind_address.sll_ifindex  = ifindex;
    if (bind(sockfd, (struct sockaddr *)&bind_address, sizeof(bind_address)) == -1) {
        perror("create_promiscuous_socket(): bind()");
        LOG_NET_ERR("could not set net device to promiscuous mode.\n");
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

int create_epoll(void)
{
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("create_epoll(): epoll_create1()");
        LOG_NET_ERR("can't create the epoll fd.\n");
        exit(EXIT_FAILURE);
    }
    return epoll_fd;
}

void bind_fd_to_epoll(int fd, int epollfd)
{
    struct epoll_event sock_event;
    sock_event.events = EPOLLIN;
    sock_event.data.fd = fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &sock_event) == -1) {
        perror("bind_fd_to_epoll(): epoll_ctl()");
        LOG_NET_ERR("can't register fd %d to epoll fd %d.\n", fd, epollfd);
        exit(EXIT_FAILURE);
    }
}

int open_uio(const char *abs_path)
{
    int fd = open(abs_path, O_RDWR);
    if (fd == -1) {
        perror("open_uio(): open()");
        LOG_NET_ERR("can't open uio @ %s.\n", abs_path);
        exit(EXIT_FAILURE);
    }
    return fd;
}

char *map_uio(uint64_t length, int uiofd)
{
    void *base = (char *) mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, uiofd, 0);
    if (base == MAP_FAILED) {
        perror("map_uio(): mmap()");
        LOG_NET_ERR("can't mmap uio fd %d\n", uiofd);
        exit(EXIT_FAILURE);
    }
    return (char *) base;
}

void uio_interrupt_ack(int uiofd)
{
    uint32_t enable = 1;
    if (write(uiofd, &enable, sizeof(uint32_t)) != sizeof(uint32_t)) {
        perror("uio_interrupt_ack(): write()");
        LOG_NET_ERR("Failed to write enable/ack interrupts on uio fd %d\n", uiofd);
        exit(EXIT_FAILURE);
    }
}

void tx_process(void)
{
    bool processed_tx = false;

    net_request_signal_active(&tx_queue);
    while (!net_queue_empty_active(&tx_queue)) {
        processed_tx = true;

        net_buff_desc_t tx_buffer;
        if (net_dequeue_active(&tx_queue, &tx_buffer) != 0) {
            LOG_NET_ERR("couldn't dequeue active TX buffer, sddf err is %d, quitting.\n");
            exit(EXIT_FAILURE);
        }

        // Workout which client it is from, so we can get the offset in data region
        uintptr_t dma_tx_addr = tx_buffer.io_or_offset;
        uintptr_t tx_data_offset;
        int tx_client;
        bool tx_data_paddr_found = false;
        for (int i = 0; i < NUM_NETWORK_CLIENTS; i++) {
            if (dma_tx_addr >= vmm_info_passing->tx_paddrs[i] &&
                dma_tx_addr < (vmm_info_passing->tx_paddrs[i] + NET_DATA_REGION_BYTES)) {

                tx_data_offset = dma_tx_addr - vmm_info_passing->tx_paddrs[i];
                tx_client = i;
                tx_data_paddr_found = true;
            }
        }

        if (!tx_data_paddr_found) {
            LOG_NET_ERR("couldn't find corresponding client for DMA addr %p, quitting.\n", dma_tx_addr);
            exit(EXIT_FAILURE);
        }

        char *tx_data_base = tx_datas_drv[tx_client];
        char *tx_data = (char *)((uintptr_t) tx_data_base + tx_data_offset);

        // Blocking send!
        struct sockaddr_ll sa;
        memset(&sa, 0, sizeof(sa));
        sa.sll_family = AF_PACKET;
        sa.sll_protocol = htons(ETH_P_ALL);
        sa.sll_ifindex = ifr.ifr_ifindex;
        sa.sll_halen    = ETH_ALEN;

        int sent_bytes = sendto(sock_fd, tx_data, tx_buffer.len, 0, (struct sockaddr *)&sa, sizeof(sa));
        if (sent_bytes != tx_buffer.len) {
            perror("tx_process(): sendto()");
            LOG_NET_ERR("TX sent %d != expected %d. qutting.\n", sent_bytes, tx_buffer.len);
            exit(EXIT_FAILURE);
        }

        if (net_enqueue_free(&tx_queue, tx_buffer) != 0) {
            LOG_NET_ERR("Couldn't return free TX buffer into the sddf tx free queue. quiting\n");
            exit(EXIT_FAILURE);
        }
    }

    // net_request_signal_active(&tx_queue);
    if (processed_tx && net_require_signal_free(&tx_queue)) {
        *sddf_net_tx_outgoing_irq_fault_vaddr = 0;
    }
}

int bytes_available_in_socket(void)
{
    int bytes_available;
    if (ioctl(sock_fd, FIONREAD, &bytes_available) == -1) {
        perror("bytes_available_in_socket(): ioctl()");
        LOG_NET_ERR("Couldn't poll the socket to see whether there is data available. quiting\n");
        exit(EXIT_FAILURE);
    }
    return bytes_available;
}

void rx_process(void)
{
    bool processed_rx = false;

    // Poll the socket and receive all frames until there is nothing to receive or the free queue is empty.
    net_request_signal_free(&rx_queue);
    while (bytes_available_in_socket() && !net_queue_empty_free(&rx_queue)) {
        processed_rx = true;

        net_buff_desc_t buffer;
        int err = net_dequeue_free(&rx_queue, &buffer);
        if (err) {
            LOG_NET_ERR("couldn't dequeue a free RX buf\n");
            exit(EXIT_FAILURE);
        }

        // Write frame out to temp buffer
        int num_bytes = recv(sock_fd, &frame[0], sizeof(frame), 0);
        if (num_bytes < 0) {
            perror("rx_process(): recv()");
            LOG_NET_ERR("couldnt recv from raw sock\n");
            exit(EXIT_FAILURE);
        }

        // Convert DMA addr from virtualiser to offset then mem copy
        uintptr_t offset = buffer.io_or_offset - vmm_info_passing->rx_paddr;
        char *buf_in_sddf_rx_data = (char *)((uintptr_t) rx_data_drv + offset);
        for (uint64_t i = 0; i < num_bytes; i++) {
            buf_in_sddf_rx_data[i] = frame[i];
        }

        // Enqueue it to the active queue
        buffer.len = num_bytes;
        if (net_enqueue_active(&rx_queue, buffer) != 0) {
            LOG_NET_ERR("couldn't enqueue active RX buffer, quitting.\n");
            exit(EXIT_FAILURE);
        }
    }

    if (processed_rx && net_require_signal_active(&rx_queue)) {
        *sddf_net_rx_outgoing_irq_fault_vaddr = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc != ARGC_REQURED) {
        LOG_NET("usage: ./uio_net_driver <network_interface>");
        exit(EXIT_FAILURE);
    } else {
        LOG_NET("*** Starting up\n");
        LOG_NET("*** Network interface: %s\n", argv[1]);
    }

    LOG_NET("*** Setting up raw promiscuous socket\n");
    sock_fd = create_promiscuous_socket(argv[1]);

    LOG_NET("*** Creating epoll\n");
    epoll_fd = create_epoll();

    LOG_NET("*** Binding socket to epoll\n");
    bind_fd_to_epoll(sock_fd, epoll_fd);

    LOG_NET("*** Mapping in sDDF control and data queues\n");
    uio_sddf_net_queues_fd = open_uio(UIO_PATH_SDDF_NET_CONTROL_AND_DATA_QUEUES);
    //                                        tx active+free + rx active+free + common rx data and per client tx data
    uint64_t sddf_net_control_and_data_size = (NET_DATA_REGION_BYTES * 4) + (NET_DATA_REGION_BYTES *
                                                                             (1 + NUM_NETWORK_CLIENTS));
    sddf_net_queues_vaddr = map_uio(sddf_net_control_and_data_size, uio_sddf_net_queues_fd);

    LOG_NET("*** Total control + data size is %p\n", sddf_net_control_and_data_size);

    LOG_NET("*** Setting up sDDF control and data queues\n");
    rx_free_drv   = sddf_net_queues_vaddr;
    rx_active_drv = (char *)((uint64_t) rx_free_drv + NET_DATA_REGION_BYTES);
    tx_free_drv   = (char *)((uint64_t) rx_active_drv + NET_DATA_REGION_BYTES);
    tx_active_drv = (char *)((uint64_t) tx_free_drv + NET_DATA_REGION_BYTES);
    rx_data_drv   = (char *)((uint64_t) tx_active_drv + NET_DATA_REGION_BYTES);
    for (int i = 0; i < NUM_NETWORK_CLIENTS; i++) {
        tx_datas_drv[i] = (char *)((uint64_t) rx_data_drv + (NET_DATA_REGION_BYTES * (i + 1)));
    }

    net_queue_init(&rx_queue, (net_queue_t *)rx_free_drv, (net_queue_t *)rx_active_drv, NET_RX_QUEUE_CAPACITY_DRIV);
    net_queue_init(&tx_queue, (net_queue_t *)tx_free_drv, (net_queue_t *)tx_active_drv, NET_TX_QUEUE_CAPACITY_DRIV);

    LOG_NET("*** rx_free_drv   = %p\n", rx_free_drv);
    LOG_NET("*** rx_active_drv = %p\n", rx_active_drv);
    LOG_NET("*** tx_free_drv   = %p\n", tx_free_drv);
    LOG_NET("*** tx_active_drv = %p\n", tx_active_drv);
    LOG_NET("*** rx_data_drv   = %p\n", rx_data_drv);
    for (int i = 0; i < NUM_NETWORK_CLIENTS; i++) {
        LOG_NET("*** tx_data_drv cli%d = %p\n", i, tx_datas_drv[i]);
    }

    LOG_NET("*** Setting up UIO TX and RX interrupts from VMM \"incoming\"\n");
    uio_sddf_net_tx_incoming_fd = open_uio(UIO_PATH_SDDF_NET_INCOMING_TX_IRQ);
    uio_sddf_net_rx_incoming_fd = open_uio(UIO_PATH_SDDF_NET_INCOMING_RX_IRQ);
    uio_interrupt_ack(uio_sddf_net_tx_incoming_fd);
    uio_interrupt_ack(uio_sddf_net_rx_incoming_fd);

    LOG_NET("*** Binding UIO TX and RX incoming interrupts to epoll\n"); // So we can block on them, instead of polling.
    bind_fd_to_epoll(uio_sddf_net_tx_incoming_fd, epoll_fd);
    bind_fd_to_epoll(uio_sddf_net_rx_incoming_fd, epoll_fd);

    LOG_NET("*** Setting up UIO data passing between VMM and us\n");
    uio_sddf_vmm_net_info_passing_fd = open_uio(UIO_PATH_SDDF_NET_SHARED_DATA);
    vmm_info_passing = (vmm_net_info_t *) map_uio(PAGE_SIZE_4K, uio_sddf_vmm_net_info_passing_fd);
    LOG_NET("*** RX paddr: %p\n", vmm_info_passing->rx_paddr);
    for (int i = 0; i < NUM_NETWORK_CLIENTS; i++) {
        LOG_NET("*** TX cli%d paddr: %p\n", i, vmm_info_passing->tx_paddrs[i]);
    }

    LOG_NET("*** Setting up UIO TX and RX interrupts to VMM \"outgoing\"\n");
    uio_sddf_net_tx_outgoing_fd = open_uio(UIO_PATH_SDDF_NET_TX_FAULT_TO_VMM);
    uio_sddf_net_rx_outgoing_fd = open_uio(UIO_PATH_SDDF_NET_RX_FAULT_TO_VMM);
    // These vaddrs will fault on physical addresses of the UIO regions.
    sddf_net_tx_outgoing_irq_fault_vaddr = map_uio(PAGE_SIZE_4K, uio_sddf_net_tx_outgoing_fd);
    sddf_net_rx_outgoing_irq_fault_vaddr = map_uio(PAGE_SIZE_4K, uio_sddf_net_rx_outgoing_fd);

    LOG_NET("*** Waiting for RX virt to boot up\n");
    while (net_queue_empty_free(&rx_queue));

    LOG_NET("*** All initialisation successful, now sending all pending TX active before we block on events\n");
    tx_process();

    LOG_NET("*** All pending TX active have been sent thru the raw sock, entering event loop now.\n");
    LOG_NET("*** You won't see any output from UIO Net anymore. Unless there is a warning or error.\n");
    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n_events == -1) {
            perror("main(): epoll_wait():");
            LOG_NET_ERR("epoll wait failed\n");
            exit(EXIT_FAILURE);
        }
        if (n_events == MAX_EVENTS) {
            LOG_NET_WARN("epoll_wait() returned MAX_EVENTS, there maybe dropped events!\n");
        }

        for (int i = 0; i < n_events; i++) {
            if (!(events[i].events & EPOLLIN)) {
                LOG_NET_WARN("got non EPOLLIN event on fd %d\n", events[i].data.fd);
                continue;
            }

            if (events[i].data.fd == sock_fd) {
                // Oh hey got a frame from the network device!
                rx_process();
            } else if (events[i].data.fd == uio_sddf_net_tx_incoming_fd) {
                // Got virt TX ntfn from VMM, send it thru the raw socket
                tx_process();
                uio_interrupt_ack(uio_sddf_net_tx_incoming_fd);
            } else if (events[i].data.fd == uio_sddf_net_rx_incoming_fd) {
                // Got RX virt ntfn from VMM, the free RX queue got filled!
                rx_process();
                uio_interrupt_ack(uio_sddf_net_rx_incoming_fd);
            } else {
                LOG_NET_WARN("epoll_wait() returned event on unknown fd %d\n", events[i].data.fd);
            }
        }
    }

    LOG_NET_WARN("Exit\n");
    return 0;
}
