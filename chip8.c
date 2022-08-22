/* A CHIP-8 emulator.
 * Copyright 2022 Rob King
 * Released under the terms of the GNU General Public License.
 * See LICENSE for details.
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef CURSES_INCLUDE_H
	#define CURSES_INCLUDE_H <curses.h>
#endif

#include CURSES_INCLUDE_H

#define TICKS_PER_SECOND 60
#define NANOS_PER_SECOND 1000000000
#define NANOS_PER_TICK (NANOS_PER_SECOND/60)
#define STACK_SIZE 5
#define MEMORY_SIZE 4096

typedef struct CHIP8 CHIP8;
struct CHIP8{
	uint8_t mem[MEMORY_SIZE];
	uint16_t stack[STACK_SIZE];
	uint16_t pc, sp, i;

	uint8_t delay, sound;
	uint8_t v[16];

	bool dirty, display[32][64];

	int inspertick;
	char *keymap;
};

static void
die(const char *m)
{
	endwin();
	fputs(m, stderr);
	exit(EXIT_FAILURE);
}

static long int
tsdiff(const struct timespec *end, const struct timespec *start)
{
	return (end->tv_sec - start->tv_sec) + 
		(end->tv_nsec - start->tv_nsec) / NANOS_PER_SECOND;
}

static void
sleeptonexttick(const struct timespec *start, const struct timespec *end)
{
	long nanos = NANOS_PER_TICK - tsdiff(end, start);
	if (nanos > 0){
		struct timespec rmtp = {0}, rqtp = {
			.tv_sec = nanos / NANOS_PER_SECOND,
			.tv_nsec = nanos % NANOS_PER_SECOND
		};

		while (nanosleep(&rqtp, &rmtp) != 0){
			memcpy(&rqtp, &rmtp, sizeof(struct timespec));
			memset(&rmtp, 0, sizeof(struct timespec));
		}
	}
}

static void
loadfonts(uint8_t buf[MEMORY_SIZE], uint16_t addr)
{
	static uint8_t font[] = {
		0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
		0x20, 0x60, 0x20, 0x20, 0x70, // 1
		0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
		0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
		0x90, 0x90, 0xF0, 0x10, 0x10, // 4
		0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
		0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
		0xF0, 0x10, 0x20, 0x40, 0x40, // 7
		0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
		0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
		0xF0, 0x90, 0xF0, 0x90, 0x90, // A
		0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
		0xF0, 0x80, 0x80, 0x80, 0xF0, // C
		0xE0, 0x90, 0x90, 0x90, 0xE0, // D
		0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
		0xF0, 0x80, 0xF0, 0x80, 0x80  // F
	};

	if (addr >= MEMORY_SIZE - sizeof(font))
		die("could not load fonts\n");
	memcpy(buf + addr, font, sizeof(font));
}

static size_t
loadrom(const char *filename, uint8_t buf[MEMORY_SIZE], uint16_t addr)
{
	FILE *f = fopen(filename, "rb");
	if (!f)
		die("could not open rom\n");

	size_t n = fread(buf + addr, 1, MEMORY_SIZE - addr, f);
	if (ferror(f))
		die("could not read rom\n");

	fclose(f);
	return n;
}

static void
cls(CHIP8 *vm)
{
	memset(vm->display, 0, sizeof(vm->display));
	clear();
	vm->dirty = true;
}

static void
refreshscreen(CHIP8 *vm)
{
	if (vm->dirty){
		refresh();
		vm->dirty = false;
	}
}

static bool
isbitset(int n, uint8_t b)
{
	return (b<<n)&0x80;
}

static void
setpixel(CHIP8 *vm, uint8_t row, uint8_t col)
{
	chtype c;
	if (vm->display[row][col]){
		vm->v[0xF] = 1;
		vm->display[row][col] = false;
		c = A_NORMAL|' ';
	} else{
		vm->display[row][col] = true;
		c = A_REVERSE|' ';
	}
	mvaddch(row, col, c);
	vm->dirty = true;
}

static void
draw(CHIP8 *vm, uint16_t inst)
{
	uint8_t x = vm->v[(inst&0x0F00)>>8] % 64;
	uint8_t y = vm->v[(inst&0x00F0)>>4] % 32;
	uint8_t n = inst&0x000F;

	vm->v[0xf] = 0;
	for (uint8_t row = 0; row < n && y + row < 32 && vm->i + row < MEMORY_SIZE; row++){
		uint8_t b = vm->mem[vm->i + row];
		for (uint8_t col = 0; col < 8 && x + col < 64; col++){
			if (isbitset(col, b))
				setpixel(vm, y + row, x + col);
		}
	}
}

static void
call(CHIP8 *vm, uint16_t addr)
{
	if (vm->sp >= STACK_SIZE)
		die("stack overflow\n");
	vm->stack[vm->sp++] = vm->pc;
	vm->pc = addr;
}

static void
rts(CHIP8 *vm)
{
	if (!vm->sp)
		die("stack underflow\n");
	vm->pc = vm->stack[--vm->sp];
}

static uint16_t
fetch(CHIP8 *vm)
{
	uint16_t i1 = vm->mem[vm->pc++ % MEMORY_SIZE];
	uint16_t i2 = vm->mem[vm->pc++ % MEMORY_SIZE];
	return (i1<<8)+i2;
}

#define NOKEY 255
#define QUIT 254
static uint8_t
getkeyboard(const CHIP8 *vm)
{
	char *o = NULL;
	int c = getch();
	if (c == 0x1b)
		return QUIT;
	if (c != ERR && (o = strchr(vm->keymap, tolower(c))))
		return (uint8_t)(o - vm->keymap);
	return NOKEY;
}

static void
bcd(CHIP8 *vm, uint8_t vx)
{
	vm->mem[(vm->i+0)%MEMORY_SIZE] = vx / 100; vx %= 100;
	vm->mem[(vm->i+1)%MEMORY_SIZE] = vx /  10; vx %=  10;
	vm->mem[(vm->i+2)%MEMORY_SIZE] = vx /   1; vx %=   1;
}

static void
regdmp(CHIP8 *vm, uint8_t vx)
{
	for (uint8_t i = 0; i <= vx; i++)
		vm->mem[(vm->i + i)%MEMORY_SIZE] = vm->v[i];
}

static void
regld(CHIP8 *vm, uint8_t vx)
{
	for (uint8_t i = 0; i <= vx; i++)
		vm->v[i] = vm->mem[(vm->i + i)%MEMORY_SIZE];
}

static void
initscreen(void)
{
	if (!initscr())
		die("could not open screen\n");
	raw();
	noecho();
	nonl();
	scrollok(stdscr, FALSE);
	nodelay(stdscr, TRUE);
	intrflush(stdscr, FALSE);
	curs_set(0);
}

#define A    (((inst)>>12)&0x0F)
#define B    ((inst)&0x0FF)
#define D    (((inst))&0x0F)
#define I    vm->i
#define V(x) vm->v[x]
#define X    (((inst)>>8)&0x0F)
#define Y    (((inst)>>4)&0x0F)
#define Vx   V(X)
#define Vy   V(((inst)>>4)&0xF)
#define VF   V(0xf)
#define VAL  (inst&0x0FFF)
#define LH   (inst&0x00FF)
#define PC   vm->pc
#define DEQ(op, action) if (inst == (op)) { action ; continue;}
#define DAA(a, action) if (A == a) { action ; continue;}
#define DAD(a, d, action) if (A == a && D == d) { action ; continue;}
#define DAB(a, b, action) if (A == a && B == b) { action ; continue;}

static void
run(CHIP8 *vm)
{
	uint8_t keyreg = 17;
	uint8_t pressed = NOKEY;
	while (pressed != QUIT){
		pressed = getkeyboard(vm);
		if (keyreg < 16 && pressed != NOKEY){
			vm->v[keyreg] = pressed;
			keyreg = 17;
			PC += 2;
		}

		struct timespec start = {0}, end = {0};
		clock_gettime(CLOCK_MONOTONIC, &start);

		for (int i = 0; i < vm->inspertick; i++){
			uint16_t inst = fetch(vm);
			DEQ(0x00E0,    cls(vm))
			DEQ(0x00EE,    rts(vm))
			DAA(0x1,       PC = VAL)
			DAA(0x2,       call(vm, VAL))
			DAA(0x3,       PC += (Vx == LH) * 2)
			DAA(0x4,       PC += (Vx != LH) * 2)
			DAD(0x5, 0x00, PC += (Vx == Vy) * 2)
			DAA(0x6,       Vx = LH)
			DAA(0x7,       Vx += LH)
			DAD(0x8, 0x00, Vx = Vy)
			DAD(0x8, 0x01, Vx |= Vy)
			DAD(0x8, 0x02, Vx &= Vy)
			DAD(0x8, 0x03, Vx ^= Vy)
			DAD(0x8, 0x04, Vx += Vy; VF = (int)Vx + Vy > 0xFF)
			DAD(0x8, 0x05, Vx -= Vy; VF = (int)Vx > Vy)
			DAD(0x8, 0x06, VF = Vx&1; Vx >>= 1)
			DAD(0x8, 0x07, Vx = Vy - Vx)
			DAD(0x8, 0x0E, VF = isbitset(0, Vx); Vx <<= 1)
			DAD(0x9, 0x00, PC += (Vx != Vy) * 2)
			DAA(0xA,       I = VAL)
			DAA(0xB,       PC = VAL + V(0))
			DAA(0xC,       Vx = (rand()%255)&LH)
			DAA(0xD,       draw(vm, inst))
			DAB(0xE, 0x9E, PC += (Vx == pressed) * 2)
			DAB(0xE, 0xA1, PC += (Vx != pressed) * 2)
			DAB(0xF, 0x07, Vx = vm->delay)
			DAB(0xF, 0x0A, PC -= 2; keyreg = X)
			DAB(0xF, 0x15, vm->delay = Vx)
			DAB(0xF, 0x18, vm->sound = Vx)
			DAB(0xF, 0x1E, I += Vx; VF = (int)Vx + I > 0xFFF)
			DAB(0xF, 0x29, I = Vx * 5)
			DAB(0xF, 0x33, bcd(vm, Vx))
			DAB(0xF, 0x55, regdmp(vm, X))
			DAB(0xF, 0x65, regld(vm, X))
			die("invalid instruction\n");
		}

		if (vm->delay)
			vm->delay--;
		if (vm->sound)
			vm->sound--;

		clock_gettime(CLOCK_MONOTONIC, &end);
		sleeptonexttick(&start, &end);
		refreshscreen(vm);
	}
}

#define USAGE "usage: chip8 [-a ADDR] [-k KEYMAP] [-r SEED] [-s SPEED] ROM\n"
int
main(int argc, char **argv)
{
	CHIP8 vm = {.pc = 512, .inspertick = 11, .keymap = "x123qweasdzc4rfv"};
	int ch = 0;
	uint16_t addr = vm.pc;
	while ((ch = getopt(argc, argv, "ha:k:r:s:")) != -1) switch (ch){
		case 'a':
			addr = vm.pc = atoi(optarg);
			if (addr < 0 || addr >= MEMORY_SIZE)
				die("invalid load address\n");
			break;
		case 'k':
			if (strlen(optarg) != 16)
				die("invalid keymap\n");
			vm.keymap = optarg;
			break;
		case 'r':
			srand(atoi(optarg));
			break;
		case 's':
			vm.inspertick = atoi(optarg);
			if (vm.inspertick <= 0)
				die("invalid instructions per tick\n");
			break;
		default:
			die(USAGE);
			break;
	}
	argc -= optind; argv += optind;

	if (argc != 1)
		die(USAGE);

	initscreen();
	loadfonts(vm.mem, 0);
	loadrom(argv[0], vm.mem, addr);
	run(&vm);

	endwin();
	return EXIT_SUCCESS;
}
