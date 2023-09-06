/*
 * Copyright 2017-2022 Azul Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

#include <asm/prctl.h>
#include <assert.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/procfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <unistd.h>

#include "minicriu-client.h"
#include "core-writer.h"
#include "list.h"

// Signal sent to all threads but the checkpointing one
#define MC_CHECKPOINT_THREAD SIGSYS
// Registers are checkpointed on all threads
#define MC_PERSIST_REGISTERS SIGUSR1
#define MC_MAX_MAPS 512
#define MC_MAX_PHDRS 512
#define MC_MAX_THREADS 64
#define MC_OWNER_SIZE 5

#define CORE_NOTE_HEADER_SIZE (sizeof(Elf64_Nhdr) + align_up(MC_OWNER_SIZE, MC_NOTE_PADDING))

// Enable or disable debug logging
#define DEBUG 0

#if DEBUG
	#define debug_log(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
	#define debug_log
#endif

DECLARE_LIST(Elf64_Phdr);
typedef struct elf_prstatus prstatus_t;
DECLARE_LIST(prstatus_t);
prstatus_t_list mc_prstatus;

static pthread_barrier_t mc_thread_barrier;

static volatile uint32_t mc_futex_checkpoint;
static volatile uint32_t mc_futex_restore;
static volatile uint32_t mc_restored_threads;
static volatile uint32_t mc_barrier_initialization;

static void mc_checkpoint_thread(int sig, siginfo_t *info, void *ctx);
static int mc_getmap();
static int mc_cleanup();

struct savedctx {
	unsigned long fsbase, gsbase;
};

typedef struct {
	void *start;
	void *end;
} mc_map;
DECLARE_LIST(mc_map);
mc_map_list mc_maps;

#define SAVE_CTX(ctx) do { \
	asm volatile("rdfsbase %0" : "=r" (ctx.fsbase) : : "memory"); \
	asm volatile("rdgsbase %0" : "=r" (ctx.gsbase) : : "memory"); \
} while(0)

#define RESTORE_CTX(ctx) do { \
	asm volatile("wrfsbase %0" : : "r" (ctx.fsbase) : "memory"); \
	asm volatile("wrgsbase %0" : : "r" (ctx.gsbase) : "memory"); \
} while(0)

static pid_t* gettid_ptr(pthread_t thr) {
	const size_t header_size =
#if defined(__x86_64__)
		0x2c0;
#else
#error "Unimplemented arch"
#endif
	return (pid_t*) ((char*)thr + header_size + 2 * sizeof(void*));
}

static ssize_t readfile(const char *file, char *buf, size_t len) {
	int fd = open(file, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %m", file);
		return -1;
	}
	size_t bytes = 0;
	while (len > 0) {
		ssize_t r = read(fd, buf, len);
		if (r < 0) {
			fprintf(stderr, "Cannot read %s: %m", file);
			close(fd);
			return -1;
		} else if (r == 0) {
			close(fd);
			return bytes;
		}
		bytes += r;
		buf += r;
		len -= r;
	}
}

static ssize_t writefile(const char *file, const char *buf, size_t len) {
	int fd = open(file, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Cannot open (for write) %s: %m", file);
		return -1;
	}
	size_t bytes = 0;
	while (len > 0) {
		ssize_t w = write(fd, buf, len);
		if (w < 0) {
			fprintf(stderr, "Cannot write %s: %m", file);
			close(fd);
			return -1;
		}
		bytes += w;
		buf += w;
		len -= w;
	}
	close(fd);
	return bytes;
}

static int mc_signal_thread(int signum, int tid, void *arg) {
	siginfo_t info;
	info.si_signo = signum;
	info.si_code = SI_QUEUE;
	info.si_value.sival_ptr = arg;
	if (syscall(SYS_rt_tgsigqueueinfo, syscall(SYS_getpid), tid, signum, &info)) {
		fprintf(stderr, "Cannot send signal %d (value %p) to thread %d: %s\n",
			signum, arg, tid, strerror(errno));
	}
}

static void mc_prepare_prpsinfo(struct elf_prpsinfo *info) {
	memset(info, 0, sizeof(struct elf_prpsinfo));
	info->pr_sname = 'R';
	char exe[PATH_MAX];
	ssize_t exelen = readlink("/proc/self/exe", exe, sizeof(exe));
	if (exelen > 0) {
		const char *last_slash = strrchr(exe, '/');
		if (last_slash != NULL) {
			// we are in signal handler, don't want to use snprintf. Missing terminating null is fine.
			strncpy(info->pr_fname, last_slash + 1, sizeof(info->pr_fname));
		}
	}
	int cmdlen = readfile("/proc/self/cmdline", info->pr_psargs, sizeof(info->pr_psargs));
	if (cmdlen > 0) {
		for (int i = 0; i < cmdlen; ++i) {
			if (info->pr_psargs[i] == '\0') info->pr_psargs[i] = ' ';
		}
	}
}

static int mc_save_core_file() {
	pid_t pid = syscall(SYS_getpid);
	Elf64_Phdr_list phdr __attribute__((cleanup (Elf64_Phdr_list_destroy)));
	INIT_LIST(Elf64_Phdr, &phdr);

	FILE *proc_maps = fopen("/proc/self/maps", "r");
	if (proc_maps == NULL) {
		perror("Could not open maps file. Failed to create checkpoint.");
		return -1;
	}

	struct nt_note {
		long count;
		long page_size;
		long descsz;
		struct filemap
		{
			long start;
			long end;
			long fileofs;
		} filemaps[MC_MAX_PHDRS];
		char filepath[MC_MAX_PHDRS][512];
	} nt_file;

	// Initialize NT_FILE
	nt_file.descsz = 0;
	nt_file.descsz += sizeof(nt_file.count) + sizeof(nt_file.page_size);
	nt_file.page_size = 0x1000;

	// Create PT_LOAD and NT_FILE phdrs
	char buffer[256];
	while (fgets(buffer, sizeof(buffer), proc_maps)) {
		void *addr_start, *addr_end;
		char perms[8];
		long ofs;
		int name_start = 0;
		int name_end = 0;

		int res = sscanf(buffer, "%p-%p %7s %lx %*x:%*x %*x %n%*[^\n]%n", &addr_start,
			&addr_end, perms, &ofs, &name_start, &name_end);

		if (res < 4) {
			perror("sscanf. Failed to create checkpoint.");
			fclose(proc_maps);
			return -1;
		}

		// [vsyscall] is mapped to the same address in each process
		if (!strncmp(buffer + name_start, "[vsyscall]", sizeof("[vsyscall]") - 1)) {
			continue;
		}

		// Save mapped files
		if (name_end > name_start && *(buffer + name_start) != '[') {
			int count = nt_file.count;
			nt_file.filemaps[count].start = (long int)addr_start;
			nt_file.filemaps[count].end = (long int)addr_end;
			nt_file.filemaps[count].fileofs = ofs / nt_file.page_size;
			memcpy(nt_file.filepath[count], buffer + name_start, name_end - name_start);
			nt_file.filepath[count][name_end - name_start] = '\0';
			nt_file.descsz += sizeof(struct filemap) + name_end - name_start + 1;
			nt_file.count++;
		}

		Elf64_Phdr *load = APPEND_LIST(Elf64_Phdr, &phdr);
		load->p_type = PT_LOAD;
		load->p_flags = 0;
		load->p_flags |= perms[0] == 'r' ? PF_R : 0;
		load->p_flags |= perms[1] == 'w' ? PF_W : 0;
		load->p_flags |= perms[2] == 'x' ? PF_X : 0;
		load->p_offset = 0;
		load->p_vaddr = (long unsigned int)addr_start;
		load->p_paddr = 0;
		load->p_memsz = addr_end - addr_start;
		// TODO: We should check if the mapped memory equals the file contents
		// and in that case make filesz 0.
		// Even if the mapping is non-readable we should check if it's all-zeroes
		// and exclude contents only if that's so: application might have temporarily
		// non-accessible parts of memory whose protection will eventually change.
		load->p_filesz = load->p_flags != 0 ? addr_end - addr_start : 0;
		load->p_align = 0x1000;

	}

	fclose(proc_maps);

	char auxv[1024];
	int auxvlen = readfile("/proc/self/auxv", auxv, sizeof(auxv));
	if (auxvlen < 0) {
		fprintf(stderr, "read auxv: %s\n", strerror(auxvlen));
	}

	Elf64_Phdr *note = PREPEND_LIST(Elf64_Phdr, &phdr);
	memset(note, 0, sizeof(note));
	note->p_type = PT_NOTE;

	int prpsinfo_sz = CORE_NOTE_HEADER_SIZE + sizeof(struct elf_prpsinfo);
	int auxv_sz = CORE_NOTE_HEADER_SIZE + align_up(auxvlen, MC_NOTE_PADDING);
	int prstatus_sz = mc_prstatus.size * (CORE_NOTE_HEADER_SIZE + sizeof(struct elf_prstatus));
	int ntfile_sz = CORE_NOTE_HEADER_SIZE + align_up(nt_file.descsz, MC_NOTE_PADDING);
	note->p_filesz = prpsinfo_sz + auxv_sz + prstatus_sz + ntfile_sz;
	note->p_offset = sizeof(Elf64_Ehdr) + phdr.size * sizeof(Elf64_Phdr);
	FOREACH(Elf64_Phdr, &phdr) {
		if (prev != NULL) {
			item->value.p_offset = align_up(prev->value.p_offset + prev->value.p_filesz, item->value.p_align);
		}
	}

	char filename[32];
	sprintf(filename, "minicriu-core.%d", pid);
	core_writer w __attribute__((cleanup (core_writer_close)));
	MUST(core_writer_open(&w, filename));
	MUST(core_write_elf_header(&w, phdr.size));
	FOREACH(Elf64_Phdr, &phdr) {
		MUST(core_write(&w, &item->value, sizeof(item->value)));
	}

	struct elf_prpsinfo prpsinfo;
	mc_prepare_prpsinfo(&prpsinfo);
	MUST(core_write_note(&w, NT_PRPSINFO, &prpsinfo, sizeof(prpsinfo)));

	MUST(core_write_note(&w, NT_AUXV, &auxv, auxvlen));

	int thread_counter = mc_prstatus.size;
	// Write PRSTATUS data for every process thread
	FOREACH(prstatus_t, &mc_prstatus) {
		MUST(core_write_note(&w, NT_PRSTATUS, &item->value, sizeof(struct elf_prstatus)));
	}
	DESTROY_LIST(prstatus_t, &mc_prstatus);

	// Write NT_FILE
	MUST(core_write_note_prologue(&w, NT_FILE, nt_file.descsz));
	MUST(core_write(&w, &nt_file, sizeof(nt_file.count) + sizeof(nt_file.page_size)));
	MUST(core_write(&w, &nt_file.filemaps, sizeof(struct filemap) * nt_file.count));
	for (int i = 0; i < nt_file.count; i++) {
		if (fputs(nt_file.filepath[i], w.file) == EOF) {
			perror("fputs");
			return -1;
		}
		if (fputc('\0', w.file) == EOF) {
			perror("putc");
			return -1;
		}
		w.bytes_written += strlen(nt_file.filepath[i]) + 1;
	}
	MUST(core_write_note_epilogue(&w, nt_file.descsz));

	// Write PT_LOAD
	Elf64_Phdr_list_item *prev = NULL;
	FOREACH(Elf64_Phdr, &phdr) {
		if (prev == NULL) {
			continue;
		}
		Elf64_Phdr *load = &item->value;
		if (load->p_filesz != 0) {
			int padding = load->p_offset - (prev->value.p_offset + prev->value.p_filesz);
			MUST(core_write_padding(&w, padding));

			int written = fwrite((void *)load->p_vaddr, 1, load->p_filesz, w.file);
			w.bytes_written += written;

			if (written != load->p_filesz) {
				// This happens when the mapping is larger than the mapped file (rounded up to page size)
				// - errno is EFAULT. Accessing that memory directly would result in SIGBUS.
				if (errno != EFAULT) {
					perror("Failed write map content");
					return 1;
				}

				// We fill the unwritten data with zeros
				MUST(core_write_padding(&w, load->p_filesz - written));
			}
		}
	}

	return 0;
}

static void mc_persist_registers(int sig, siginfo_t *info, void *ctx) {
	ucontext_t *uc = (ucontext_t *)ctx;
	greg_t *gregs = uc->uc_mcontext.gregs;
	prstatus_t *thread_prstatus = (prstatus_t *) info->si_value.sival_ptr;

	struct user_regs_struct *uregs = (void *)thread_prstatus->pr_reg;
	uregs->r15 = gregs[REG_R15];
	uregs->r14 = gregs[REG_R14];
	uregs->r13 = gregs[REG_R13];
	uregs->r12 = gregs[REG_R12];
	uregs->rbp = gregs[REG_RBP];
	uregs->rbx = gregs[REG_RBX];
	uregs->r11 = gregs[REG_R11];
	uregs->r10 = gregs[REG_R10];
	uregs->r9 = gregs[REG_R9];
	uregs->r8 = gregs[REG_R8];
	uregs->rax = gregs[REG_RAX];
	uregs->rcx = gregs[REG_RCX];
	uregs->rdx = gregs[REG_RDX];
	uregs->rsi = gregs[REG_RSI];
	uregs->rdi = gregs[REG_RDI];
	uregs->rip = gregs[REG_RIP];
	uregs->eflags = gregs[REG_EFL];
	uregs->rsp = gregs[REG_RSP];
	syscall(SYS_arch_prctl, ARCH_GET_FS, &(uregs->fs_base));
	syscall(SYS_arch_prctl, ARCH_GET_GS, &(uregs->gs_base));

	thread_prstatus->pr_pid = syscall(SYS_gettid);

	// Wait until all threads save their registers
	pthread_barrier_wait(&mc_thread_barrier);


	// It doesn't matter which thread writes the core file
	if (thread_prstatus == &mc_prstatus.first->value) {
		mc_save_core_file();
	}

	// Wait for all data to be saved. Otherwise the stack data will probably be corrupted.
	pthread_barrier_wait(&mc_thread_barrier);
}

static inline bool mc_is_internal_signal(int signum) {
	// GLIBC uses signals 32 and 33 internally and manipulation causes EINVAL
	return signum == SIGKILL || signum == SIGSTOP || (signum > SIGSYS && signum < SIGRTMIN);
}

// It is not possible to change signal mask for another thread, so in the unlikely
// case that the thread blocks MC_CHECKPOINT_THREAD we must give up on checkpoint.
// In the past this was unblocked by minicriu_register_new_thread but there's no
// guarantee that the thread wouldn't block the signal at any later point: therefore
// we'll just make it a requirement from the application side.
static bool mc_check_signal_blocked(const char *taskid) {
	char buf[256];
	snprintf(buf, sizeof(buf), "/proc/self/task/%s", taskid);
	FILE *status = fopen(buf, "r");
	char line[256];
	while (fgets(line, sizeof(line), status)) {
		if (!strncmp(line, "SigBlk:", 7)) {
			unsigned long long bits = strtoull(line + 7, NULL, 16);
			if (bits & (1 << (MC_CHECKPOINT_THREAD - 1))) {
				fprintf(stderr, "Thread LWP %s is blocking signal %d, cannot perform checkpoint.\n", taskid, MC_CHECKPOINT_THREAD);
				fclose(status);
				return true;
			}
			break; // ignore rest
		}
	}
	fclose(status);
	return false;
}

static void mc_find_args(void **args_start, void **args_end) {
	FILE *stat = fopen("/proc/self/stat", "r");
	if (stat == NULL) {
		perror("Cannot open /proc/self/stat");
	}
	/* See man proc:            1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18  19  20  21  22  23  24  25  26  27  28  29  30  31  32  33  34  35  36  37  38  39  40  41  42  43  44  45  46  47  48  49  50  51  52 */
	int items = fscanf(stat, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %*d %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*d %*d %*u %*u %*u %*u %*d %*u %*u %*u %lu %lu %*u %*u %*d",
		(unsigned long *) args_start, (unsigned long *) args_end);
	if (items != 2) {
		fprintf(stderr, "Failed to parse /proc/self/stat: read %d items\n", items);
		volatile int x = 1; while(x);
	}
	fclose(stat);
}

int minicriu_dump(void) {

	pid_t mytid = syscall(SYS_gettid);
	pid_t mypid = getpid();

	debug_log("minicriu thread %d\n", mytid);

	char comm[1024];
	ssize_t commlen = readfile("/proc/self/comm", comm, sizeof(comm));

	char exe[PATH_MAX];
	ssize_t exelen = readlink("/proc/self/exe", exe, sizeof(exe));

	void *args_start = NULL, *args_end = NULL;
	mc_find_args(&args_start, &args_end);

	struct savedctx ctx;
	SAVE_CTX(ctx);

	struct sigaction checkpoint_thread = {
		.sa_sigaction = mc_checkpoint_thread,
		.sa_flags = SA_SIGINFO
	};
	struct sigaction persist_registers = {
		.sa_sigaction = mc_persist_registers,
		.sa_flags = SA_SIGINFO
	};

	struct sigaction sigactions[SIGRTMAX];
	for (int i = 1; i < SIGRTMAX; ++i) {
		if (mc_is_internal_signal(i)) continue;
		if (sigaction(i, NULL, &sigactions[i])) {
			perror("Cannot save signal handler");
			return 1;
		}
	}

	if (sigaction(MC_CHECKPOINT_THREAD, &checkpoint_thread, NULL)) {
		perror("sigaction");
		return 1;
	}

	if (sigaction(MC_PERSIST_REGISTERS, &persist_registers, NULL)) {
		perror("sigaction");
		return 1;
	}

	sigset_t sigset, oldset;
	if (sigemptyset(&sigset) || sigaddset(&sigset, MC_PERSIST_REGISTERS)) {
		perror("Cannot set signal mask");
		return 1;
	}
	if (pthread_sigmask(SIG_UNBLOCK, &sigset, &oldset)) {
		perror("Cannot unblock signals");
		return 1;
	}

	int thread_counter = 0;
	prstatus_t *my_prstatus = NULL;
	DIR *tasksdir = opendir("/proc/self/task/");
	struct dirent *taskdent;
	while ((taskdent = readdir(tasksdir))) {
		if (taskdent->d_name[0] == '.') {
			continue;
		}
		int tid = atoi(taskdent->d_name);
		debug_log("minicriu %d me %d\n", tid, mytid == tid);
		++thread_counter;
		if (tid == mytid) {
			my_prstatus = APPEND_LIST(prstatus_t, &mc_prstatus);
			continue;
		}
		if (mc_check_signal_blocked(taskdent->d_name)) {
			closedir(tasksdir);
			// TODO: revert all signals etc
			return 1;
		}
		int r = mc_signal_thread(MC_CHECKPOINT_THREAD, tid, APPEND_LIST(prstatus_t, &mc_prstatus));
		__atomic_fetch_sub(&mc_futex_checkpoint, 1, __ATOMIC_SEQ_CST);
	}
	closedir(tasksdir);

	assert(my_prstatus != NULL);
	debug_log("thread_counter = %d\n", thread_counter);

	uint32_t current_count;
	while ((current_count = mc_futex_checkpoint) != 0) {
		syscall(SYS_futex, &mc_futex_checkpoint, FUTEX_WAIT, current_count);
	}

	// Initialize barrier
	pthread_barrier_init(&mc_thread_barrier, NULL, thread_counter);

	// Say to other threads that barrier is initialized
	__atomic_fetch_add(&mc_barrier_initialization, 1, __ATOMIC_SEQ_CST);
	syscall(SYS_futex, &mc_barrier_initialization, FUTEX_WAKE, INT_MAX);
	if (mc_getmap())
		printf("failed to get maps from /proc/self/maps\n");

	pid_t pid = syscall(SYS_getpid);

	// Save registers
	mc_signal_thread(MC_PERSIST_REGISTERS, syscall(SYS_gettid), my_prstatus);

	RESTORE_CTX(ctx);

	int newtid = syscall(SYS_gettid);
	*gettid_ptr(pthread_self()) = newtid;

	for (int i = 1; i < SIGRTMAX; ++i) {
		if (mc_is_internal_signal(i)) continue;
		if (sigaction(i, &sigactions[i], NULL)) {
			perror("Cannot restore signal handler");
			return 1;
		}
	}

	if (pthread_sigmask(SIG_SETMASK, &oldset, NULL)) {
		perror("sigprocmask UNBLOCK");
		return 1;
	}

	mc_futex_restore = 1;
	syscall(SYS_futex, &mc_futex_restore, FUTEX_WAKE, INT_MAX);

	/*
	*	Here we synchronize the threads so that we do not
	*	munmap segments before the threads are restored
	*/

	while ((current_count = mc_restored_threads) != thread_counter - 1) {
		syscall(SYS_futex, &mc_restored_threads, FUTEX_WAIT, current_count);
	}

	if (mc_cleanup())
		printf("failed to clean up maps\n");

	volatile int thread_loop = 0;
	while (thread_loop);

	cap_t capabilities = cap_get_proc();
	cap_flag_value_t has_resource_cap = CAP_CLEAR;
	if (CAP_IS_SUPPORTED(CAP_SYS_RESOURCE) && cap_get_flag(capabilities, CAP_SYS_RESOURCE, CAP_EFFECTIVE, &has_resource_cap)) {
		perror("Failed to check for CAP_SYS_RESOURCE capability");
	}
	cap_free(capabilities);

	if (has_resource_cap && args_start != NULL && args_end != NULL) {
		// We cannot update args' start-end atomically but kernel checks that the range
		// is correctly ordered at any point.
		void *old_start = NULL, *old_end = NULL;
		mc_find_args(&old_start, &old_end);
		if (args_start >= old_end && prctl(PR_SET_MM, PR_SET_MM_ARG_END, args_end, 0, 0)) {
			fprintf(stderr, "Cannot reset arguments to %p-%p(<--): %m\n", args_start, args_end);
		}
		if (prctl(PR_SET_MM, PR_SET_MM_ARG_START, args_start, 0, 0)) {
			fprintf(stderr, "Cannot reset arguments to (-->)%p-%p: %m\n", args_start, args_end);
		}
		if (args_start < old_end && prctl(PR_SET_MM, PR_SET_MM_ARG_END, args_end, 0, 0)) {
			fprintf(stderr, "Cannot reset arguments to %p-%p(<--): %m\n", args_start, args_end);
		}
	}
	if (has_resource_cap && exelen > 0) {
		char buf[PATH_MAX] = { '\0' };
		if (readlink("/proc/self/exe", buf, sizeof(buf)) < 0) {
			perror("Cannot read current exe");
		}
		// Do not change exe in the checkpointed process
		if (strcmp(buf, exe)) {
			int exefd = open(exe, O_RDONLY);
			if (exefd < 0) {
				fprintf(stderr, "Cannot open original exe file %s: %m", exe);
			} else {
				if (prctl(PR_SET_MM, PR_SET_MM_EXE_FILE, exefd, 0, 0)) {
					fprintf(stderr, "Cannot restore exe %s (FD %d): %m\n", exe, exefd);
				}
				close(exefd);
			}
		}
	} else if (commlen > 0) {
		writefile("/proc/self/comm", comm, commlen);
	}

	return 0;
}


static void mc_checkpoint_thread(int sig, siginfo_t *info, void *ctx_unused) {
	void *thread_prstatus = info->si_value.sival_ptr;
	__atomic_fetch_add(&mc_futex_checkpoint, 1, __ATOMIC_SEQ_CST);
	syscall(SYS_futex, &mc_futex_checkpoint, FUTEX_WAKE, 1);

	struct savedctx ctx;
	SAVE_CTX(ctx);
	int tid = syscall(SYS_gettid);
	debug_log("(%d) fsbase %lx gsbase %lx\n", tid, ctx.fsbase, ctx.gsbase);

	pthread_t self = pthread_self();
	pid_t *tidptr = gettid_ptr(self);
	pthread_kill(self, 0); // noop, just error checking

	debug_log("%s: self %ld tidptr %p *tidptr %d\n",
		__func__, self, tidptr, *tidptr);

	assert(*gettid_ptr(pthread_self()) == tid);

	// Make sure that barrier was initialized
	uint32_t current_count;
	while ((current_count = mc_barrier_initialization) == 0) {
		syscall(SYS_futex, &mc_barrier_initialization, FUTEX_WAIT, current_count);
	}

	// Note: if signal MC_CHECKPOINT_THREAD is blocked, we won't get here, and we don't
	// have chance to perform the checkpoint.
	sigset_t sigmask, old_sigmask;
	if (sigemptyset(&sigmask) || sigaddset(&sigmask, MC_PERSIST_REGISTERS)) {
		perror("Cannot construct thread sigmask");
	}
	if (pthread_sigmask(SIG_UNBLOCK, &sigmask, &old_sigmask)) {
		perror("Cannot get thread sigmask");
	}

	// Save registers
	mc_signal_thread(MC_PERSIST_REGISTERS, syscall(SYS_gettid), thread_prstatus);

	while (!mc_futex_restore) {
		// syscall sets thread-local errno while thread-local
		// storage is not yet initialized.
		// syscall(SYS_futex, &mc_futex_restore, FUTEX_WAIT, 0);
		unsigned long ret;
		asm volatile (
			"syscall\n\t"
			: "=a"(ret)
			: "a"(SYS_futex),
			  "D"(&mc_futex_restore),
			  "S"(FUTEX_WAIT),
			  "d"(0),
			  "b"(tid)
			: "memory");
	}

	__atomic_fetch_add(&mc_restored_threads, 1, __ATOMIC_SEQ_CST);
	syscall(SYS_futex, &mc_restored_threads, FUTEX_WAKE, 1);

	RESTORE_CTX(ctx);

	int newtid = syscall(SYS_gettid);
	*gettid_ptr(pthread_self()) = newtid;

	if (pthread_sigmask(SIG_SETMASK, &old_sigmask, NULL)) {
		perror("Cannot restore thread sigmask");
	}

	volatile int thread_loop = 0;
	while (thread_loop);
}

static int mc_getmap() {
	char line[512];
	INIT_LIST(mc_map, &mc_maps);
	FILE *proc_maps;
	proc_maps = fopen("/proc/self/maps", "r");

	if (!proc_maps) {
		perror("open maps");
		return 1;
	}

	while (fgets(line, sizeof(line), proc_maps)) {
		void *addr_start, *addr_end;
		char mapname[256];
		if (sscanf(line, "%p-%p %*s %*x %*d:%*d %*d %s",
					&addr_start, &addr_end, mapname) < 2) {
			fclose(proc_maps);
			perror("maps sscanf");
			return 1;
		}

		/*
		* there is no need to save [vsyscall] as it always
		* maps to the same address in the kernel space
		*/
		if (!strncmp(mapname, "[vsyscall]", 10)) continue;

		mc_map *mapping = APPEND_LIST(mc_map, &mc_maps);
		mapping->start = addr_start;
		mapping->end = addr_end;
	}
	fclose(proc_maps);

	return 0;
}

static int mc_cleanup() {
	char line[512];
	FILE *proc_maps = fopen("/proc/self/maps", "r");
	void *last_map_start;
	void *last_map_end;

	// find last segment mapped in user space
	while (fgets(line, sizeof(line), proc_maps)) {
		void *addr_start, *addr_end;
		char mapname[256];
		if (sscanf(line, "%p-%p %*s %*x %*d:%*d %*d %s",
					&addr_start, &addr_end, mapname) < 2) {
			fclose(proc_maps);
			perror("maps sscanf");
			return 1;
		}

		// location of [vsyscall] page is fixed in the kernel ABI
		if (!strncmp(mapname, "[vsyscall]", 10)) continue;
		last_map_start = addr_start;
		last_map_end = addr_end;
	}
	fclose(proc_maps);

	void *from = NULL;
	FOREACH(mc_map, &mc_maps) {
		size_t size = item->value.start - from;
		munmap(from, size);
		from = item->value.end;
	}

	if(mc_maps.last->value.start < last_map_start) {
		munmap(mc_maps.last->value.end, last_map_end - mc_maps.last->value.end);
	}
	DESTROY_LIST(mc_map, &mc_maps);
	return 0;
}
