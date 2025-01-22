#define pragma once

#include <stdio.h>
#include <stdbool.h>

/* Uncomment this to enable debug logging */
#define DEBUG_UIO_INIT

#if defined(DEBUG_UIO_INIT)
#define LOG_UIO_INIT(...) do{ printf("UIO_DRIVER_INIT"); printf(": "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_UIO_INIT(...) do{}while(0)
#endif

#define LOG_UIO_INIT_ERR(...) do{ printf("UIO_DRIVER_INIT"); printf("|ERROR: "); printf(__VA_ARGS__); }while(0)

/* Load all kernel modules required by the driver */
bool load_modules(void);

/* Executes the uio driver program. Does not return (unless there is an error). */
void init_main(void);

/* Helper for loading a kernel module */
bool insmod(const char *module_path);
