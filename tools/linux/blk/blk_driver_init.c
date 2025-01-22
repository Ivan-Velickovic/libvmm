#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <uio/init.h>

#if defined(BOARD_qemu_virt_aarch64)
static char *uio_blk_driver_args[] = {"uio_blk_driver", "/dev/vda", NULL};
#elif defined(BOARD_odroidc4)
static char *uio_blk_driver_args[] = {"uio_blk_driver", "/dev/mmcblk0", NULL};
#else
#error Need to define board for the uio driver
#endif

#define UIO_BLK_DRIVER_PROGRAM_PATH "/root/uio_blk_driver"
#define VIRTIO_BLK_MODULE_PATH "/modules/virtio_blk.ko"

bool load_modules(void)
{
    LOG_UIO_INIT("Initialising kernel module: %s\n", VIRTIO_BLK_MODULE_PATH);
    if (!insmod(VIRTIO_BLK_MODULE_PATH)) {
        LOG_UIO_INIT_ERR("Failed to initialise kernel module %s: %s\n", VIRTIO_BLK_MODULE_PATH, strerror(errno));
        return false;
    }
    return true;
}

void init_main(void)
{
    LOG_UIO_INIT("Starting UIO block driver...\n");
    execv(UIO_BLK_DRIVER_PROGRAM_PATH, uio_blk_driver_args);
    LOG_UIO_INIT_ERR("UIO block driver exited\n");
}
