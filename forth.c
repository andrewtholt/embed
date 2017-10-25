/** @file      forth.c
 *  @brief     Forth Virtual Machine
 *  @copyright Richard James Howe (2017)
 *  @license   MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CORE (65536u)  /* core size in bytes */
#define SP0  (8704u)   /* Variable Stack Start: 8192 (end of program area) + 512 (block size) */
#define RP0  (32767u)  /* Return Stack Start: end of CORE in words */
#define DEFAULT ("eforth.blk") /* Default memory file */

typedef uint16_t uw_t;
typedef int16_t  sw_t;
typedef uint32_t ud_t;

typedef struct {
	uw_t pc, t, rp, sp, core[CORE/sizeof(uw_t)]; 
} forth_t;

static FILE *fopen_or_die(const char *file, const char *mode)
{
	FILE *f = NULL;
	errno = 0;
	if(!(f = fopen(file, mode))) {
		fprintf(stderr, "failed to open file '%s' (mode %s): %s\n", file, mode, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return f;
}

static int binary_memory_load(FILE *input, uw_t *p, size_t length)
{
	for(size_t i = 0; i < length; i++) {
		const int r1 = fgetc(input);
		const int r2 = fgetc(input);
		if(r1 < 0 || r2 < 0)
			return -1;
		p[i] = (((unsigned)r1 & 0xffu)) | (((unsigned)r2 & 0xffu) << 8u);
	}
	return 0;
}

static int binary_memory_save(FILE *output, uw_t *p, size_t start, size_t length)
{
	for(size_t i = start; i < length; i++) {
		errno = 0;
		const int r1 = fputc((p[i])       & 0xff, output);
		const int r2 = fputc((p[i] >> 8u) & 0xff, output);
		if(r1 < 0 || r2 < 0) {
			fprintf(stderr, "memory write failed: %s\n", strerror(errno));
			return -1;
		}
	}
	return 0;
}

int load(forth_t *h, const char *name)
{
	assert(h && name);
	FILE *input = fopen_or_die(name, "rb");
	const int r = binary_memory_load(input, h->core, CORE/sizeof(uw_t));
	fclose(input);
	h->pc = 0; h->t = 0; h->rp = RP0; h->sp = SP0;
	return r;
}

int save(forth_t *h, const char *name, size_t start, size_t length)
{
	assert(h);
	if(!name)
		return -1;
	FILE *output = fopen_or_die(name, "wb");
	const int r = binary_memory_save(output, h->core, start, length);
	fclose(output);
	return r;
}

int forth(forth_t *h, FILE *in, FILE *out, const char *block)
{
	static const uw_t delta[] = { 0x0000, 0x0001, 0xFFFE, 0xFFFF };
	register uw_t pc = h->pc, t = h->t, rp = h->rp, sp = h->sp;
	register ud_t d;
	assert(h && in && out);
	uw_t *m = h->core;
	for(;;) {
		uw_t instruction = m[pc];
		assert(!(sp & 0x8000) && !(rp & 0x8000));

		if(0x8000 & instruction) { /* literal */
			m[++sp] = t;
			t       = instruction & 0x7FFF;
			pc++;
		} else if ((0xE000 & instruction) == 0x6000) { /* ALU */
			uw_t n = m[sp], T = t;

			pc = instruction & 0x10 ? m[rp] >> 1 : pc + 1;

			switch((instruction >> 8u) & 0x1f) {
			case  0: /*T = t;*/                                break;
			case  1: T = n;                                    break;
			case  2: T = m[rp];                                break;
			case  3: T = m[t >> 1];                            break;
			case  4: m[t >> 1] = n; T = m[--sp];               break;
			case  5: d = (ud_t)t + (ud_t)n; T = d >> 16; m[sp] = d; n = d; break;
			case  6: d = (ud_t)t * (ud_t)n; T = d >> 16; m[sp] = d; n = d; break;
			case  7: T &= n;                                   break;
			case  8: T |= n;                                   break;
			case  9: T ^= n;                                   break;
			case 10: T = ~t;                                   break;
			case 11: T--;                                      break;
			case 12: T = -(t == 0);                            break;
			case 13: T = -(t == n);                            break;
			case 14: T = -(n < t);                             break;
			case 15: T = -((sw_t)n < (sw_t)t);                 break;
			case 16: T = n >> t;                               break;
			case 17: T = n << t;                               break;
			case 18: T = sp << 1;                              break;
			case 19: T = rp << 1;                              break;
			case 20: sp = t >> 1;                              break;
			case 21: rp = t >> 1; T = n;                       break;
			case 22: T = save(h, block, n >> 1, ((ud_t)T + 1u) >> 1);  break;
			case 23: T = fputc(t, out);                        break;
			case 24: T = fgetc(in);                            break;
			case 25: if(t) { T=n/t; t=n%t; n=t; } else { pc=1; T=10; n=T; t=n; } break;
			case 26: if(t) { T=(sw_t)n/(sw_t)t; t=(sw_t)n%(sw_t)t; n=t; } else { pc=1; T=10; n=T; t=n; } break;
			case 27: goto finished;
			}

			sp += delta[ instruction       & 0x3];
			rp -= delta[(instruction >> 2) & 0x3];
			if(instruction & 0x20)
				T = n;
			if(instruction & 0x40)
				m[rp] = t;
			if(instruction & 0x80)
				m[sp] = t;
			t = T;
		} else if (0x4000 & instruction) { /* call */
			m[--rp] = (pc + 1) << 1;
			pc = instruction & 0x1FFF;
		} else if (0x2000 & instruction) { /* 0branch */
			pc = !t ? instruction & 0x1FFF : pc + 1;
			t = m[sp--];
		} else { /* branch */
			pc = instruction & 0x1FFF;
		}
	}
finished:
	h->pc = pc; h->sp = sp; h->rp = rp; h->t = t;
	return t;
}

int main(int argc, char **argv)
{
	static forth_t h;
	int i, interactive = 0;
	char *in = DEFAULT, *out = DEFAULT;
	memset(h.core, 0, CORE);

	for(i = 1; i < argc && argv[i][0] == '-'; i++)
		switch(argv[i][1]) {
		case '\0': goto done;
		case 'i': case 'o':
			   if(i >= (argc - 1))
				   goto fail;
			   if(argv[i][1] == 'i')
				   in = argv[++i];
			   else
				   out = argv[++i];
			   break;
		case 'I': interactive = 1; break;
		fail: default:
			   fprintf(stderr, "usage: %s -i file.blk -o file.blk file.fth", argv[0]);
			   return -1;
		}
done:
	load(&h, in);
	interactive = interactive || (i == argc);
	for(;i < argc; i++) {
		FILE *in = fopen_or_die(argv[i], "rb");
		int r = forth(&h, in, stdout, out);
		fclose(in);
		if(r != 0) {
			fprintf(stderr, "run failed: %d\n", r);
			return r;
		}
	}
	if(interactive)
		return forth(&h, stdin, stdout, out);
	return 0;
}

