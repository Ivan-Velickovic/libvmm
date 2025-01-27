/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <dirent.h>
#include <regex.h>
#include <limits.h>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <sddf/blk/queue.h>

#include <uio/libuio.h>
#include <uio/blk.h>

/* Uncomment this to enable debug logging */
// #define DEBUG_UIO

#if defined(DEBUG_UIO)
#define LOG_UIO(...) do{ printf("UIO_DRIVER"); printf(": "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_UIO(...) do{}while(0)
#endif

#define LOG_UIO_ERR(...) do{ printf("UIO_DRIVER"); printf("|ERROR: "); printf(__VA_ARGS__); }while(0)

#define _unused(x) ((void)(x))

#define MAIN_UIO_NUM 0

#define MAX_PATHNAME 64
#define UIO_MAX_MAPS 32
#define UIO_MAX_DEV 16

#define MAX_EVENTS 16

#define UIO_IRQ_MAP 0
#define VMM_NOTIFY_MAP 1

static int main_uio_fd;
static void *maps[UIO_MAX_MAPS];
static uintptr_t maps_phys[UIO_MAX_MAPS];
static int num_maps;

static int curr_map = 0;

static int epoll_fd = -1;

/*
 * Just happily abort if the user can't be bother to provide these functions
 */
__attribute__((weak)) int driver_init(int uio_fd, void **maps,
                                      uintptr_t *maps_phys, int num_maps,
                                      int argc, char **argv) {
  assert(!"UIO driver did not implement driver_init");
  return -1;
}

__attribute__((weak)) void driver_notified(int *event_fds, int num_events) {
  assert(!"UIO driver did not implement driver_notified");
}

void uio_irq_enable() {
  int32_t one = 1;
  int ret = write(main_uio_fd, &one, 4);
  if (ret < 0) {
    LOG_UIO_ERR("writing 1 to device failed with ret val: %d, errno: %d\n", ret,
                errno);
  }
}

void vmm_notify() {
  *(uint8_t *)maps[VMM_NOTIFY_MAP] = 1;
  msync(maps[VMM_NOTIFY_MAP], 1, MS_ASYNC);
}

void bind_fd_to_event_loop(int fd) {
  struct epoll_event sock_event;
  sock_event.events = EPOLLIN;
  sock_event.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &sock_event) == -1) {
    LOG_UIO_ERR("can't register fd %d to epoll.\n", fd);
    exit(1);
  } else {
    LOG_UIO("registered fd %d to epoll\n", fd);
  }
}

static int create_epoll(void) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1) {
    LOG_UIO_ERR("can't create the epoll fd.\n");
    exit(1);
  } else {
    LOG_UIO("created epoll fd %d\n", epoll_fd);
  }
  return epoll_fd;
}

static int uio_num_maps(int uio_num) {
  DIR *dir;
  struct dirent *entry;
  struct stat statbuf;
  regex_t regex;
  int count = 0;

  char path[MAX_PATHNAME];
  int len = snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps", uio_num);
  if (len < 0 || len >= sizeof(path)) {
    LOG_UIO_ERR("Failed to create maps path string\n");
    exit(1);
  }

  /* Compile regex that searches for maps */
  if (regcomp(&regex, "^map[0-9]+$", REG_EXTENDED) != 0) {
    LOG_UIO_ERR("Could not compile regex\n");
    exit(1);
  }

  dir = opendir(path);
  if (dir == NULL) {
    LOG_UIO_ERR("Failed to open uio maps directory\n");
    exit(1);
  }

  /* Read directory entries */
  while ((entry = readdir(dir)) != NULL) {
    char fullPath[MAX_PATHNAME];

    int len =
        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);
    if (len < 0 || len >= sizeof(fullPath)) {
      LOG_UIO_ERR("Failed to create full uio maps path\n");
      exit(1);
    };

    /* Check if entry is a directory */
    if (stat(fullPath, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
      /* Skip over . and .. */
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }

      /* Check if directory name matches regex */
      if (regexec(&regex, entry->d_name, 0, NULL, 0) == 0) {
        count++;
        LOG_UIO("Map found: %s\n", entry->d_name);
      }
    }
  }

  LOG_UIO("Total directories matching 'map[0-9]+': %d\n", count);

  closedir(dir);
  regfree(&regex);

  return count;
}

static int uio_map_size(int uio_num, int map_num) {
  char path[MAX_PATHNAME];
  char buf[MAX_PATHNAME];

  int len = snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map%d/size",
                     uio_num, map_num);
  if (len < 0 || len >= sizeof(path)) {
    LOG_UIO_ERR("Failed to create uio%d map%d size path string\n", uio_num,
                map_num);
    exit(1);
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    LOG_UIO_ERR("Failed to open %s\n", path);
    exit(1);
  }
  ssize_t ret = read(fd, buf, sizeof(buf));
  if (ret < 0) {
    LOG_UIO_ERR("Failed to read map%d size\n", map_num);
    exit(1);
  }
  close(fd);

  int size = strtoul(buf, NULL, 0);
  if (size == 0 || size == ULONG_MAX) {
    LOG_UIO_ERR("Failed to convert map%d size to integer\n", map_num);
    exit(1);
  }

  return size;
}

static uintptr_t uio_map_addr(int uio_num, int map_num) {
  char path[MAX_PATHNAME];
  char buf[MAX_PATHNAME];

  int len = snprintf(path, sizeof(path), "/sys/class/uio/uio%d/maps/map%d/addr",
                     uio_num, map_num);
  if (len < 0 || len >= sizeof(path)) {
    LOG_UIO_ERR("Failed to create uio%d map%d addr path string\n", uio_num,
                map_num);
    exit(1);
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    LOG_UIO_ERR("Failed to open %s\n", path);
    exit(1);
  }
  ssize_t ret = read(fd, buf, sizeof(buf));
  if (ret < 0) {
    LOG_UIO_ERR("Failed to read map%d addr\n", map_num);
    exit(1);
  }
  close(fd);

  uintptr_t ret_addr = strtoul(buf, NULL, 0);
  if (ret_addr == 0 || ret_addr == ULONG_MAX) {
    LOG_UIO_ERR("Failed to convert map%d addr to integer\n", map_num);
    exit(1);
  }

  return ret_addr;
}

static void uio_map_init(int uio_fd, int uio_num) {
  LOG_UIO("Initialising UIO device %d mappings\n", uio_num);

  int curr_num_maps = uio_num_maps(uio_num);
  if (curr_num_maps < 0) {
    LOG_UIO_ERR("Failed to get number of maps\n");
    exit(1);
  }
  if (curr_num_maps == 0) {
    LOG_UIO_ERR("No maps found\n");
    exit(1);
  }

  num_maps += curr_num_maps;

  for (int i = 0; i < curr_num_maps; i++) {
    if (curr_map >= UIO_MAX_MAPS) {
      LOG_UIO_ERR("too many maps, maximum is %d\n", UIO_MAX_MAPS);
      close(uio_fd);
      exit(1);
    }

    int size = uio_map_size(uio_num, i);
    maps_phys[curr_map] = uio_map_addr(uio_num, i);

    if ((maps[curr_map] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                               uio_fd, i * getpagesize())) == NULL) {
      LOG_UIO_ERR("mmap failed, errno: %d\n", errno);
      close(uio_fd);
      exit(1);
    }

    LOG_UIO("mmaped map%d (driver map%d) with 0x%x bytes at %p\n", i, curr_map,
            size, maps[curr_map]);

    curr_map++;
  }
}

int main(int argc, char **argv)
{
  if (argc < 1) {
    printf("Usage: %s [driver_args...]\n", argv[0]);
    return 1;
  }

  for (int uio_num = 0; uio_num < UIO_MAX_DEV; uio_num++) {
    /* Append UIO_NUM to "/dev/uio" to get the full path of the uio device, e.g.
     * "/dev/uio0" */
    char uio_device_name[MAX_PATHNAME];
    int len = snprintf(uio_device_name, sizeof(uio_device_name), "/dev/uio%d",
                       uio_num);
    if (len < 0 || len >= sizeof(uio_device_name)) {
      LOG_UIO_ERR("Failed to create uio device name string\n");
      return 1;
    }

    int uio_fd = open(uio_device_name, O_RDWR);
    if (uio_fd < 0) {
      LOG_UIO("Failed to open %s\n", uio_device_name);
      if (uio_num == MAIN_UIO_NUM) {
        LOG_UIO_ERR("Could not open main UIO device, failing\n");
        return 1;
      } else {
        LOG_UIO("Assuming no more UIO devices\n");
      }
      break;
    }

    /* Initialise uio device mappings. This reads into /sys/class/uio to
     * determine the number of associated devices, their maps and their sizes.
     */
    uio_map_init(uio_fd, uio_num);

    /* Set /dev/uio0 as the interrupt */
    if (uio_num == MAIN_UIO_NUM) {
      LOG_UIO("Setting main uio device to %s\n", uio_device_name);
      main_uio_fd = uio_fd;
    }
  }

  /* Initialise epoll, bind uio fd to it */
  epoll_fd = create_epoll();
  bind_fd_to_event_loop(main_uio_fd);

  /* Initialise driver, we pass the UIO device mappings to the driver,
   * skipping the first one which only contains UIO's irq status, and
   * the second one which contains the uio notify region.
   */
  LOG_UIO("Initialising driver with %d driver specific maps\n", num_maps - 2);
  if (driver_init(main_uio_fd, maps + 2, maps_phys + 2, num_maps - 2, argc - 1,
                  argv + 1) != 0) {
    LOG_UIO_ERR("Failed to initialise driver\n");
    exit(1);
  }

  /* Enable uio interrupt */
  uio_irq_enable();

  struct epoll_event events[MAX_EVENTS];
  int event_fds[MAX_EVENTS];
  while (true) {
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      LOG_UIO_ERR("epoll_wait failed, errno: %d\n", errno);
      exit(1);
    }

    for (int n = 0; n < nfds; n++) {
      event_fds[n] = events[n].data.fd;
      if (event_fds[n] == main_uio_fd) {
        int irq_count;
        int read_ret = read(main_uio_fd, &irq_count, sizeof(irq_count));
        _unused(read_ret);
        assert(read_ret >= 0);
        LOG_UIO("received irq, count: %d\n", irq_count);
      }
    }

    /* wake the guest driver up to do some real works */
    driver_notified(event_fds, nfds);
  }

  return 0;
}

