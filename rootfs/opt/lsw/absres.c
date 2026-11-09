// absres: set the resolution (units per mm) of a device's ABS_X/ABS_Y axes.
// The NVTCapacitivePen reports resolution 0, so libinput ignores it ("missing tablet
// capabilities: resolution"). The change lives in the kernel's input device until reboot.
// Usage: absres /dev/input/eventN <units-per-mm>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s /dev/input/eventN units-per-mm\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    int res = atoi(argv[2]);
    int axes[] = { ABS_X, ABS_Y };
    for (int i = 0; i < 2; i++) {
        struct input_absinfo a;
        if (ioctl(fd, EVIOCGABS(axes[i]), &a) < 0) { perror("EVIOCGABS"); return 1; }
        a.resolution = res;
        if (ioctl(fd, EVIOCSABS(axes[i]), &a) < 0) { perror("EVIOCSABS"); return 1; }
        printf("axis %d: %d..%d, resolution %d\n", axes[i], a.minimum, a.maximum, a.resolution);
    }
    return 0;
}
