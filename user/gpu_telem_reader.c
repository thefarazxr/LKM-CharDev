/* user/gpu_telem_reader.c — C user-space consumer for /dev/gpu_telem
 *
 * Demonstrates blocking / event-driven reads against the kernel ring buffer.
 *
 * Read path:
 *   poll()  — registers with kernel wait queue; returns when POLLIN is set
 *   read()  — delivers one or more struct gpu_sample (48 bytes each)
 *             blocks if ring is empty (unless O_NONBLOCK)
 *
 * Usage:
 *   ./gpu_telem_reader                 stream forever
 *   ./gpu_telem_reader -n 20           print 20 samples then exit
 *   ./gpu_telem_reader /dev/gpu_telem  explicit device path
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "../include/gpu_telem_uapi.h"

#define BATCH 16

static volatile int g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

static void print_header(void)
{
	printf("%-7s  %-20s  %-9s  %-8s  %-10s  %-10s  %s\n",
	       "idx", "timestamp_ns", "temp_°C", "fan_rpm",
	       "power_W", "core_MHz", "throttle");
	printf("%-7s  %-20s  %-9s  %-8s  %-10s  %-10s  %s\n",
	       "-------", "--------------------", "---------", "--------",
	       "----------", "----------", "--------");
}

static void print_sample(uint64_t idx, const struct gpu_sample *s)
{
	char temp[16], throttle[48];

	if (s->temp_millideg == -1)
		snprintf(temp, sizeof(temp), "n/a");
	else
		snprintf(temp, sizeof(temp), "%7.2f", s->temp_millideg / 1000.0);

	throttle[0] = '\0';
	if (s->throttle_reasons & GPU_THROTTLE_POWER)   strcat(throttle, "PWR ");
	if (s->throttle_reasons & GPU_THROTTLE_THERMAL) strcat(throttle, "THM ");
	if (s->throttle_reasons & GPU_THROTTLE_CURRENT) strcat(throttle, "CUR ");
	if (s->throttle_reasons & GPU_THROTTLE_UTIL)    strcat(throttle, "UTL ");
	if (!throttle[0]) strcpy(throttle, "-");

	printf("%-7" PRIu64 "  %-20" PRIu64 "  %-9s  %-8u  %-10.3f  %-10u  %s\n",
	       idx, s->timestamp_ns, temp, s->fan_rpm,
	       s->power_uw / 1e6, s->core_freq_mhz, throttle);
}

int main(int argc, char *argv[])
{
	const char *dev = "/dev/gpu_telem";
	long max = -1;
	int opt, fd;
	uint64_t total = 0;
	struct pollfd pfd;
	struct gpu_sample batch[BATCH];
	uint32_t ring_sz = 0, interval = 0;

	while ((opt = getopt(argc, argv, "n:h")) != -1) {
		switch (opt) {
		case 'n': max = strtol(optarg, NULL, 10); break;
		case 'h':
			printf("Usage: %s [-n count] [device]\n", argv[0]);
			return 0;
		default:
			fprintf(stderr, "Usage: %s [-n count] [device]\n", argv[0]);
			return 1;
		}
	}
	if (optind < argc)
		dev = argv[optind];

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, "  Hint: sudo insmod gpu_telem.ko\n");
		return 1;
	}

	ioctl(fd, GPU_TELEM_GET_RING_SIZE,   &ring_sz);
	ioctl(fd, GPU_TELEM_GET_INTERVAL_MS, &interval);
	printf("Device   : %s\n", dev);
	printf("Ring     : %u samples  Interval: %u ms\n\n", ring_sz, interval);
	print_header();

	pfd.fd = fd;
	pfd.events = POLLIN;

	while (g_running && (max < 0 || (long)total < max)) {
		int ready;
		ssize_t n;
		size_t count, want, i;

		/*
		 * poll() puts this process to sleep in the kernel wait queue
		 * (gdev.wq) until the collector calls wake_up_interruptible()
		 * after a ring_push().  1 s timeout lets us check g_running.
		 */
		ready = poll(&pfd, 1, 1000);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("poll"); break;
		}
		if (ready == 0 || !(pfd.revents & POLLIN))
			continue;

		want = BATCH;
		if (max >= 0) {
			size_t rem = (size_t)(max - (long)total);
			if (rem < want) want = rem;
		}

		n = read(fd, batch, want * sizeof(struct gpu_sample));
		if (n < 0) {
			if (errno == EINTR) continue;
			perror("read"); break;
		}
		if (n == 0) break;

		count = (size_t)n / sizeof(struct gpu_sample);
		for (i = 0; i < count; i++)
			print_sample(total + i, &batch[i]);
		total += count;
		fflush(stdout);
	}

	close(fd);
	printf("\n%s: %" PRIu64 " samples delivered.\n", argv[0], total);
	return 0;
}
