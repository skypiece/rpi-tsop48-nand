/*
    Raspberry Pi / 360-Clip based 8-bit NAND reader

    Copyright (C) 2012  pharos

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

//#define DEBUG 1

#define PAGE_SIZE 2112
#define MAX_WAIT_READ_BUSY	1000000

#define BCM2708_PERI_BASE	0x20000000
#define GPIO_BASE	 	(BCM2708_PERI_BASE + 0x200000)

#define N_WRITE_PROTECT		0 // pulled up by RPi, this is useful
#define N_READ_BUSY		1 // pulled up by RPi, this is also useful

// rest of GPIOs have been chose arbitrarily, with the only constraint of not using
// GPIO 14 (TXD)/GPIO 15 (RXD)/06 (GND) on P1. instead I use GND on P2 header, pin 8

// IMPORTANT: BE VERY CAREFUL TO CONNECT VCC TO P1-01 (3.3V) AND *NOT* P1-02 (5V) !!

#define N_WRITE_ENABLE 		21
#define ADDRESS_LATCH_ENABLE	4
#define COMMAND_LATCH_ENABLE	17
#define N_READ_ENABLE		18
#define N_CHIP_ENABLE		22

int data_to_gpio_map[8] = { 23, 24, 25, 8, 7, 10, 9, 11 }; // 23 is NAND IO 0, etc.

volatile unsigned int *gpio;

inline void INP_GPIO(int g)
{
#ifdef DEBUG
	printf("setting direction of GPIO#%d to input\n", g);
#endif
	(*(gpio+((g)/10)) &= ~(7<<(((g)%10)*3)));
}

inline void OUT_GPIO(int g)
{
	INP_GPIO(g);
#ifdef DEBUG
	printf("setting direction of GPIO#%d to output\n", g);
#endif
	*(gpio+((g)/10)) |= (1<<(((g)%10)*3));
}

inline void GPIO_SET_1(int g)
{
#ifdef DEBUG
	printf("setting GPIO#%d to 1\n", g);
#endif
	*(gpio +  7)  = 1 << g;
}

inline void GPIO_SET_0(int g)
{
#ifdef DEBUG
	printf("setting GPIO#%d to 0\n", g);
#endif
	*(gpio + 10)  = 1 << g;
}

inline int GPIO_READ(int g)
{
	int x = (*(gpio + 13) & (1 << g)) >> g;
#ifdef DEBUG
	printf("GPIO#%d reads as %d\n", g, x);
#endif
	return x;
}

inline void set_data_direction_in(void)
{
	int i;
#ifdef DEBUG
	printf("data direction => IN\n");
#endif
	for (i = 0; i < 8; i++)
		INP_GPIO(data_to_gpio_map[i]);
}

inline void set_data_direction_out(void)
{
	int i;
#ifdef DEBUG
	printf("data direction => OUT\n");
#endif
	for (i = 0; i < 8; i++)
		OUT_GPIO(data_to_gpio_map[i]);
}

inline int GPIO_DATA8_IN(void)
{
	int i, data;
	for (i = data = 0; i < 8; i++, data = data << 1) {
		data |= GPIO_READ(data_to_gpio_map[7 - i]);
	}
	data >>= 1;
#ifdef DEBUG
	printf("GPIO_DATA8_IN: data=%02x\n", data);
#endif
	return data;
}

inline void GPIO_DATA8_OUT(int data)
{
	int i;
#ifdef DEBUG
	printf("GPIO_DATA8_OUT: data=%02x\n", data);
#endif
	for (i = 0; i < 8; i++, data >>= 1) {
		if (data & 1)
			GPIO_SET_1(data_to_gpio_map[i]);
		else
			GPIO_SET_0(data_to_gpio_map[i]);
	}
}

int delay = 1;
int shortpause()
{
	int i;
	volatile static int dontcare = 0;
	for (i = 0; i < delay; i++) {
		dontcare++;
	}
}

int main(int argc, char **argv)
{ 
	int mem_fd;

	if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC)) < 0) {
		perror("open /dev/mem, are you root?");
		return -1;
	}

	if ((gpio = (volatile unsigned int *) mmap((caddr_t) 0x13370000, 4096, PROT_READ|PROT_WRITE,
						MAP_SHARED|MAP_FIXED, mem_fd, GPIO_BASE)) == MAP_FAILED) {
		perror("mmap GPIO_BASE");
		close(mem_fd);
		return -1;
	}

	INP_GPIO(N_READ_BUSY);

	OUT_GPIO(N_WRITE_PROTECT);
	GPIO_SET_1(N_WRITE_PROTECT);

	OUT_GPIO(N_READ_ENABLE);
	GPIO_SET_1(N_READ_ENABLE);

	OUT_GPIO(N_WRITE_ENABLE);
	GPIO_SET_1(N_WRITE_ENABLE);

	OUT_GPIO(COMMAND_LATCH_ENABLE);
	GPIO_SET_0(COMMAND_LATCH_ENABLE);

	OUT_GPIO(ADDRESS_LATCH_ENABLE);
	GPIO_SET_0(ADDRESS_LATCH_ENABLE);

	OUT_GPIO(N_CHIP_ENABLE);
	GPIO_SET_0(N_CHIP_ENABLE);

	if (argc < 3) {
usage:
		GPIO_SET_1(N_CHIP_ENABLE);
		printf("usage: %s <delay> <command> ...\n" \
			"\t<delay> is used to slow down operations (50 should work, increase in case of bad reads)\n" \
			"\tthis program assumes PAGE_SIZE == %d (this can be changed at the top of the source)\n" \
			"available commands:\n" \
			"\tread_id (no arguments) : read the 5-byte device ID\n" \
			"\tread_full <page number> <# of pages> <output filename> : read N pages including spare\n" \
			"\tread_data <page number> <# of pages> <output filename> : read N pages, discard spare\n",
			argv[0], PAGE_SIZE);
		close(mem_fd);
		return -1;
	}

	delay = atoi(argv[1]);
	if (delay < 20) {
		printf("delay must be >= 20\n");
		return -1;
	}

	if (strcmp(argv[2], "read_id") == 0) {
		return read_id(NULL);
	}

	if (strcmp(argv[2], "read_full") == 0) {
		if (argc != 6) goto usage;
		if (atoi(argv[4]) <= 0) {
			printf("# of pages must be > 0\n");
			return -1;
		}
		return read_pages(atoi(argv[3]), atoi(argv[4]), argv[5], 1);
	}

	if (strcmp(argv[2], "read_data") == 0) {
		if (argc != 6) goto usage;
		if (atoi(argv[4]) <= 0) {
			printf("# of pages must be > 0\n");
			return -1;
		}
		return read_pages(atoi(argv[3]), atoi(argv[4]), argv[5], 0);
	}

	printf("unknown command '%s'\n", argv[2]);
	goto usage;
	return 0;
}

void error_msg(char *msg)
{
	printf("%s\nbe sure to check wiring, and check that pressure is applied on both sides of 360 Clip\n" \
		"sometimes it is required to move slightly the 360 Clip in case of a false contact\n", msg);
}

int read_id(unsigned char id[5])
{
	int i;
	unsigned char buf[5];

	GPIO_SET_1(COMMAND_LATCH_ENABLE);
	shortpause();
	GPIO_SET_0(N_WRITE_ENABLE);
	set_data_direction_out(); GPIO_DATA8_OUT(0x90); // Read ID byte 1
	shortpause();
	GPIO_SET_1(N_WRITE_ENABLE);
	shortpause(); set_data_direction_in();
	GPIO_SET_0(COMMAND_LATCH_ENABLE);
	shortpause();

	GPIO_SET_1(ADDRESS_LATCH_ENABLE);
	GPIO_SET_0(N_WRITE_ENABLE);
	set_data_direction_out(); GPIO_DATA8_OUT(0x00); // Read ID byte 2
	shortpause();
	GPIO_SET_1(N_WRITE_ENABLE);
	shortpause(); set_data_direction_in();
	GPIO_SET_0(ADDRESS_LATCH_ENABLE);
	shortpause();

	for (i = 0; i < 5; i++) {
		GPIO_SET_0(N_READ_ENABLE);
		shortpause();
		GPIO_SET_1(N_READ_ENABLE);
		buf[i] = GPIO_DATA8_IN();
		shortpause();
	}
	if (id != NULL)
		memcpy(id, buf, 5);
	else {
		printf("id = ");
		for (i = 0; i < 5; i++)
			printf("%02x ", buf[i]);
		printf("\n");
	}
	if (buf[0] == buf[1] && buf[1] == buf[2] && buf[2] == buf[3] && buf[3] == buf[4]) {
		error_msg("all five ID bytes are identical, this is not normal");
		return -1;
	}
	return 0;
}

inline int page_to_address(int page, int address_byte_index)
{
	switch(address_byte_index) {
	case 2:
		return page & 0xff;
	case 3:
		return (page >>  8) & 0xff;
	case 4:
		return (page >> 16) & 0xff;
	default:
		return 0;
	}
}

int send_command_address(int cmd1, int cmd2, int page)
{
	int i;

	set_data_direction_out();

	GPIO_SET_1(COMMAND_LATCH_ENABLE);
	shortpause(); GPIO_SET_0(N_WRITE_ENABLE);
	GPIO_DATA8_OUT(cmd1);
	shortpause(); GPIO_SET_1(N_WRITE_ENABLE);
	shortpause(); GPIO_SET_0(COMMAND_LATCH_ENABLE);
	shortpause();

	GPIO_SET_1(ADDRESS_LATCH_ENABLE);
	for (i = 0; i < 5; i++) {
		GPIO_SET_0(N_WRITE_ENABLE);
		GPIO_DATA8_OUT(page_to_address(page, i));
		shortpause();
		GPIO_SET_1(N_WRITE_ENABLE);
		shortpause();
	}
	GPIO_SET_0(ADDRESS_LATCH_ENABLE);
	shortpause();

	GPIO_SET_1(COMMAND_LATCH_ENABLE);
	shortpause(); GPIO_SET_0(N_WRITE_ENABLE);
	GPIO_DATA8_OUT(cmd2);
	shortpause(); GPIO_SET_1(N_WRITE_ENABLE);
	shortpause(); GPIO_SET_0(COMMAND_LATCH_ENABLE);
	shortpause();

	return 0;
}

int read_pages(int first_page_number, int number_of_pages, char *outfile, int write_spare)
{
	int page, i, n, retry_count;
	unsigned char id[5], id2[5];
	unsigned char buf[PAGE_SIZE * 2];
	FILE *badlog, *f = fopen(outfile, "w+");
	if (f == NULL) {
		perror("fopen output file");
		return -1;
	}
	if ((badlog = fopen("bad.log", "w+")) == NULL) {
		perror("fopen bad.log");
		return -1;
	}
	if (GPIO_READ(N_READ_BUSY) == 0) {
		error_msg("N_READ_BUSY should be 1 (pulled up), but reads as 0. make sure the NAND is powered on");
		return -1;
	}

	if (read_id(id) < 0)
		return -1;
	printf("NAND ID: ");
	for (i = 0; i < 5; i++) {
		printf("%02x ", id[i]);
	}
	printf("\nif this ID is incorrect, press Ctrl-C NOW to abort (3s timeout)\n");
	sleep(3);

	for (retry_count = 0, page = first_page_number*2; page < (first_page_number + number_of_pages)*2; page++) {
	retry:
		read_id(id2);
		if (memcmp(id, id2, 5) != 0) {
			error_msg("NAND ID has changed! make sure not to move the 360 Clip during operation. retrying\n");
			goto retry;
		}
		printf("reading page %d\n", page << 1);
		send_command_address(0x00, 0x30, page >> 1);
		for (i = 0; i < MAX_WAIT_READ_BUSY; i++) {
			if (GPIO_READ(N_READ_BUSY) == 0)
				break;
		}
		if (i == MAX_WAIT_READ_BUSY) {
			printf("N_READ_BUSY was not brought to 0 by NAND in time, retrying\n");
			goto retry;
		}
		set_data_direction_in();
		for (i = 0; i < MAX_WAIT_READ_BUSY; i++) {
			if (GPIO_READ(N_READ_BUSY) == 1)
				break;
		}
		if (i == MAX_WAIT_READ_BUSY) {
			printf("N_READ_BUSY was not brought to 1 by NAND in time, retrying\n");
			goto retry;
		}
		n = PAGE_SIZE*(page & 1);
		for (i = 0; i < PAGE_SIZE; i++) {
			GPIO_SET_0(N_READ_ENABLE);
			shortpause();
			GPIO_SET_1(N_READ_ENABLE);
			buf[i + n] = GPIO_DATA8_IN();
			shortpause();
		}
		if (!n) /* read the page again to ensure correct operation, bit 0 in page used for this purpose */
			continue;

		if (memcmp(buf, buf + PAGE_SIZE, PAGE_SIZE) != 0) {
			if (retry_count < 5) {
				printf("page failed to read correctly! retrying\n");
				retry_count++;
				page = page & ~1;
				goto retry;
			}
			printf("too many retries. perhaps bad block?\n");
			fprintf(badlog, "page %d seems to be bad\n", page >> 1);
			retry_count = 0;
		}
		if (write_spare) {
			if (fwrite(buf, PAGE_SIZE, 1, f) != 1) {
				perror("fwrite");
				return -1;
			}
		}
		else {
			if (fwrite(buf, 512 * (PAGE_SIZE / 512), 1, f) != 1) {
				perror("fwrite");
				return -1;
			}
		}
	}
}
