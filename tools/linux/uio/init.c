#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <uio/init.h>

static void shutdown(void)
{
    LOG_UIO_INIT("Shutting down...\n");
    reboot(RB_POWER_OFF);
}

static bool init_mounts()
{
    LOG_UIO_INIT("Mounting devtmpfs to /dev of type devtmpfs\n");
    int err = mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    if (err < 0) {
        LOG_UIO_INIT_ERR("Failed to mount devtmpfs on /dev: %s\n", strerror(errno));
        return false;
    }

    LOG_UIO_INIT("Mounting sysfs to /sys of type sysfs\n");
    err = mount("sysfs", "/sys", "sysfs", 0, NULL);
    if (err < 0) {
        LOG_UIO_INIT_ERR("Failed to mount sysfs on /sys: %s\n", strerror(errno));
        return false;
    }

    return true;
}

bool insmod(const char *module_path)
{
    int fd = open(module_path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    int err = syscall(__NR_finit_module, fd, "");
    if (err != 0) {
        close(fd);
        return false;
    }

    err = close(fd);
    if (err != 0) {
        return false;
    }

    return true;
}

int main()
{
    if (getpid() != 1) {
        LOG_UIO_INIT_ERR("init is not running as pid 1\n");
        shutdown();
    }

    if (chdir("/")) {
        LOG_UIO_INIT_ERR("Failed to cd to \'/\': %s\n", strerror(errno));
        shutdown();
    }

    if (!load_modules()) {
        shutdown();
    }

    if (!init_mounts()) {
        shutdown();
    }

    init_main();
    shutdown();

    return EXIT_FAILURE;
}
