#ifndef MEMFILE_H
#define MEMFILE_H

#include <stdio.h>
#include <stdint.h>

/* RAM-backed read-only asset FILEs streamed by a core-1 reader thread.
 * See memfile.c (Fase 74). */
void memfile_init(void);
FILE *memfile_open(const char *path);  /* NULL = not eligible, use native */
int memfile_close(FILE *f);            /* 1 = was ours and is closed */
void memfile_stats(uint32_t *opens, uint32_t *bytes, uint32_t *wait_ms);

size_t mf_fread(void *ptr, size_t size, size_t n, FILE *f);
int mf_fseek(FILE *f, long off, int whence);
long mf_ftell(FILE *f);
int mf_fgetc(FILE *f);
int mf_getc(FILE *f);
int mf_ungetc(int c, FILE *f);
int mf_feof(FILE *f);
int mf_ferror(FILE *f);
int mf_fflush(FILE *f);
int mf_setvbuf(FILE *f, char *buf, int mode, size_t size);
int mf_fgetpos(FILE *f, fpos_t *pos);
int mf_fsetpos(FILE *f, const fpos_t *pos);

#endif
