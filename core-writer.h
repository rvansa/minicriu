#pragma once

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>

#define MC_NOTE_PADDING 4
#define MUST(expr) do { if (expr) { return -1; }} while(0)

typedef struct {
	FILE *file;
	size_t bytes_written;
} core_writer;

int core_writer_open(core_writer *w, const char *path);
int core_writer_close(core_writer *w);
int core_write_elf_header(core_writer *w, size_t phnum);
int core_write_padding(core_writer *w, size_t bytes);
int core_write(core_writer *w, const void *data, size_t bytes);
int core_write_note_prologue(core_writer *w, Elf64_Word type, size_t bytes);
int core_write_note_epilogue(core_writer *w, size_t bytes);
int core_write_note(core_writer *w, Elf64_Word type, const void *data, size_t bytes);

static inline unsigned long align_up(unsigned long v, unsigned p) {
	return (v + p - 1) & ~(p - 1);
}
