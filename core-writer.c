#include <string.h>

#include "core-writer.h"

int core_writer_open(core_writer *w, const char *path) {
	w->bytes_written = 0;
	w->file = fopen(path, "w+");
	if (w->file == NULL) {
		fprintf(stderr, "Could not create file %s: %m", path);
		return -1;
	}
	return 0;
}

int core_writer_close(core_writer *w) {
	if (w->file != NULL) {
		fclose(w->file);
		w->file = NULL;
	}
}

int core_write_elf_header(core_writer *w, size_t phnum) {
	Elf64_Ehdr ehdr;
	// Create Elf header
	memset(&ehdr, 0, sizeof(ehdr));
	memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
	ehdr.e_ident[EI_CLASS] = ELFCLASS64;
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
	#else
	ehdr.e_ident[EI_DATA] = ELFDATA2MSB;
	#endif
	ehdr.e_ident[EI_VERSION] = EV_CURRENT;
	ehdr.e_ident[EI_OSABI] = ELFOSABI_SYSV;
	ehdr.e_type = ET_CORE;
	ehdr.e_machine = EM_X86_64;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_phoff = sizeof(Elf64_Ehdr);
	ehdr.e_ehsize = sizeof(Elf64_Ehdr);
	ehdr.e_phentsize = sizeof(Elf64_Phdr);
	ehdr.e_phnum = phnum;

	MUST(core_write(w, &ehdr, sizeof(Elf64_Ehdr)));
	return 0;
}

int core_write_padding(core_writer *w, size_t bytes) {
	char paddingData[0x1000];
	memset(paddingData, 0, sizeof(paddingData));
	while (bytes > 0) {
		size_t max = bytes > sizeof(paddingData) ? sizeof(paddingData) : bytes;
		int written = fwrite(paddingData, 1, max, w->file);
		if (written == 0) {
			fprintf(stderr, "Cannot write padding: %m\n");
			return -1;
		}
		w->bytes_written += written;
		bytes -= written;
	}
	return 0;
}

int core_write(core_writer *w, const void *data, size_t bytes) {
	size_t written = fwrite(data, 1, bytes, w->file);
	if (written != bytes) {
		fprintf(stderr, "Written too few bytes (%ld/%ld): %m", written, bytes);
		return -1;
	}
	w->bytes_written += bytes;
	return 0;
}

int core_write_note_prologue(core_writer *w, Elf64_Word type, size_t bytes) {
	char owner[] = "CORE"; // "CORE" gives more information while reading using readelf and eu-readelf tools

	Elf64_Nhdr nhdr;
	nhdr.n_type = type;
	nhdr.n_namesz = sizeof(owner);
	nhdr.n_descsz = bytes;
	MUST(core_write(w, &nhdr, sizeof(Elf64_Nhdr)));
	MUST(core_write(w, owner, sizeof(owner)));

	if (nhdr.n_namesz % MC_NOTE_PADDING != 0) {
		int padding = align_up(w->bytes_written, MC_NOTE_PADDING) - w->bytes_written;
		MUST(core_write_padding(w, padding));
	}
	return 0;
}

int core_write_note_epilogue(core_writer *w, size_t bytes) {
	if (bytes % MC_NOTE_PADDING != 0) {
		int padding = align_up(w->bytes_written, MC_NOTE_PADDING) - w->bytes_written;
		MUST(core_write_padding(w, padding));
	}
	return 0;
}

int core_write_note(core_writer *w, Elf64_Word type, const void *data, size_t bytes) {
	MUST(core_write_note_prologue(w, type, bytes));
	MUST(core_write(w, data, bytes));
	MUST(core_write_note_epilogue(w, bytes));
	return 0;
}
