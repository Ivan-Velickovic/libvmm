#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uio/init.h>

#define ALSA_CONFIG_DIR "/alsa"
#define UIO_SND_DRIVER_LOGFILE "/user_sound"
#define UIO_SND_DRIVER_PROGRAM_PATH "/root/uio_snd_driver"
#define NUM_VIRTIO_MODULES 16

static const char *module_paths[] = {
    "/modules/nls_base.ko", "/modules/soundcore.ko", "/modules/snd-intel-dspcfg.ko",
    "/modules/ledtrig-audio.ko", "/modules/led-class.ko", "/modules/usbcore.ko",
    "/modules/snd.ko", "/modules/snd-timer.ko", "/modules/snd-pcm.ko", "/modules/snd-hda-core.ko",
    "/modules/snd-hda-codec.ko", "/modules/snd-hda-codec-generic.ko", "/modules/snd-hda-intel.ko",
    "/modules/snd-hda-codec-realtek.ko", "/modules/snd-ctl-led.ko", "/modules/virtio_snd.ko"
};

static char *uio_snd_driver_args[] = {"user_sound.elf", "default", "hw:0,0", NULL};

static bool setup_logfile(void)
{
    int fd = open(UIO_SND_DRIVER_LOGFILE, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) {
        LOG_UIO_INIT_ERR("Failed to open log file\n");
        return false;
    }

    // Redirect stdout and stderr to log file.
    int err = dup2(fd, STDOUT_FILENO);
    if (err < 0) {
        LOG_UIO_INIT_ERR("Failed to redirect stdout to log file\n");
        close(fd);
        return false;
    }

    err = dup2(fd, STDERR_FILENO);
    if (err < 0) {
        LOG_UIO_INIT_ERR("Failed to redirect stderr to log file\n");
        close(fd);
        return false;
    }

    err = close(fd);
    if (err < 0) {
        LOG_UIO_INIT_ERR("Failed to close log file\n");
        return false;
    }

    return true;
}

bool load_modules(void)
{
    for (int i = 0; i < NUM_VIRTIO_MODULES; i++) {
        LOG_UIO_INIT("Initialising kernel module: %s\n", module_paths[i]);
        if (!insmod(module_paths[i])) {
            LOG_UIO_INIT_ERR("Failed to initialise kernel module %s: %s\n", module_paths[i], strerror(errno));
            return false;
        }
    }
    return true;
}

void init_main(void)
{
    if (setenv("ALSA_CONFIG_DIR", ALSA_CONFIG_DIR, 1) != 0) {
        LOG_UIO_INIT_ERR("Failed to set environment variable ALSA_CONFIG_DIR: %s", strerror(errno));
        return;
    }

    LOG_UIO_INIT("Starting UIO sound driver...\n");
    if (!setup_logfile()) {
        return;
    }

    execv(UIO_SND_DRIVER_PROGRAM_PATH, uio_snd_driver_args);
    LOG_UIO_INIT_ERR("UIO sound driver exited\n");
}
