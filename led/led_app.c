#include <sys/types.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RK_LED_RED_PATH    "/dev/led_red"
#define RK_LED_GREEN_PATH  "/dev/led_green"
#define RK_LED_YELLOW_PATH "/dev/led_yellow"

const char* led_path[] = { RK_LED_RED_PATH, RK_LED_YELLOW_PATH, RK_LED_GREEN_PATH };

#define ARRAY_SIZE(arr)   (sizeof(arr) / sizeof((arr)[0]))

static void led_blingbling(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("open");
        exit(1);
    }
    for (int i = 0; i <  5; i++) {
        write(fd, "1", 1);
        sleep(1);
        write(fd, "0", 1);
        sleep(1);
    }
    close(fd);
}

int main(int argc, char *argv[])
{
    for (int i = 0; i < ARRAY_SIZE(led_path); i++)
        led_blingbling(led_path[i]);

    return 0;
}
