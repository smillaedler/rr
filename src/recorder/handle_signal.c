/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "handle_signal.h"

#include <assert.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <sys/mman.h>
#include <sys/user.h>

#include "recorder.h"

#include "../share/dbg.h"
#include "../share/ipc.h"
#include "../share/util.h"
#include "../share/trace.h"
#include "../share/sys.h"
#include "../share/hpc.h"
#include "../share/syscall_buffer.h"

static __inline__ unsigned long long rdtsc(void)
{
	unsigned hi, lo;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long) lo) | (((unsigned long long) hi) << 32);
}

/**
 * Return nonzero if |ctx| was stopped because of a SIGSEGV resulting
 * from a rdtsc and |ctx| was updated appropriately, zero otherwise.
 */
static int try_handle_rdtsc(struct context *ctx)
{
	int retval = 0;
	pid_t tid = ctx->child_tid;
	int sig = signal_pending(ctx->status);
	assert(sig != SIGTRAP);

	if (sig <= 0 || sig != SIGSEGV) {
		return retval;
	}

	int size;
	char *inst = get_inst(ctx, 0, &size);
	if (!inst) {
		/* If the segfault was caused by a jump to a bad $ip,
		 * then we obviously won't be able to read the
		 * instruction. */
		return 0;
	}

	/* if the current instruction is a rdtsc, the segfault was triggered by
	 * by reading the rdtsc instruction */
	if (strncmp(inst, "rdtsc", 5) == 0) {
		long int eax, edx;
		unsigned long long current_time;

		current_time = rdtsc();
		eax = current_time & 0xffffffff;
		edx = current_time >> 32;

		struct user_regs_struct regs;
		read_child_registers(tid, &regs);
		regs.eax = eax;
		regs.edx = edx;
		regs.eip += size;
		write_child_registers(tid, &regs);
		ctx->event = SIG_SEGV_RDTSC;
		retval = 1;

		debug("  trapped for rdtsc: returning %llu", current_time);
	}

	sys_free((void**)&inst);

	return retval;
}

/**
 * Return nonzero if |ctx| was stopped because of a SIGSEGV resulting
 * from access of a shared mmap and |ctx| was updated appropriately,
 * zero otherwise.
 */
static int try_handle_shared_mmap_access(struct context *ctx,
					 const siginfo_t* si)
{
	pid_t tid = ctx->child_tid;
	int sig = signal_pending(ctx->status);
	assert(sig != SIGTRAP);

	if (sig <= 0 || sig != SIGSEGV) {
		return 0;
	}

	// locate the offending address
	void* addr = si->si_addr;

	// check that its indeed in a shared mmaped region we previously protected
	if (!is_protected_map(ctx,addr)){
		return 0;
	}

	// get the type of the instruction
	int size;
	bool is_write = is_write_mem_instruction(tid, 0, &size);

	// since emulate_child_inst also advances the eip,
	// we need to record the event BEFORE the instruction is executed
	ctx->event = is_write ? SIG_SEGV_MMAP_WRITE : SIG_SEGV_MMAP_READ;
	emulate_child_inst(ctx,0);
	/*
	struct user_regs_struct regs;
	read_child_registers(tid, &regs);
	regs.eip += size;
	write_child_registers(tid, &regs);
	*/

	/*
	// unprotect the region and allow the instruction to run
	mprotect_child_region(ctx, addr, PROT_WRITE);
	sys_ptrace_singlestep(tid,0);

	if (!is_write) { // we only need to record on reads, writes are fine
		record_child_data(ctx,SIG_SEGV_MMAP_READ,get_mmaped_region_end(ctx,addr) - addr,addr);
	}

	// protect the region again
	mprotect_child_region(ctx, addr, PROT_NONE);
	 */
	return ctx->event;
}

static int is_desched_event_syscall(struct context* ctx,
				    const struct user_regs_struct* regs)
{
	return (SYS_ioctl == regs->orig_eax
		&& ctx->desched_fd_child == regs->ebx);
}

static int is_arm_desched_event_syscall(struct context* ctx,
					const struct user_regs_struct* regs)
{
	return (is_desched_event_syscall(ctx, regs)
		&& PERF_EVENT_IOC_ENABLE == regs->ecx);
}

static int is_disarm_desched_event_syscall(struct context* ctx,
					   const struct user_regs_struct* regs)
{
	return (is_desched_event_syscall(ctx, regs)
		&& PERF_EVENT_IOC_DISABLE == regs->ecx);
}

static void disarm_desched_event(struct context* ctx)
{
	if (ioctl(ctx->desched_fd, PERF_EVENT_IOC_DISABLE, 0)) {
		fatal("Failed to disarm desched event");
	}
}

static uint64_t
read_desched_counter(struct context* ctx)
{
	uint64_t nr_descheds;
	read(ctx->desched_fd, &nr_descheds, sizeof(nr_descheds));
	return nr_descheds;
}

/**
 * Return nonzero if |ctx| was stopped because of a SIGIO resulting
 * from notification of |ctx| being descheduled, zero otherwise.  The
 * tracee's execution may be advanced, and if so |regs| is updated to
 * the tracee's latest state.
 */
static int try_handle_desched_event(struct context* ctx, const siginfo_t* si,
				    struct user_regs_struct* regs)
{
	pid_t tid = ctx->child_tid;
	int call = ctx->event;
	uint64_t nr_descheds;
	int status;

	assert(SIGIO == si->si_signo);

	if (si->si_code != POLL_IN || si->si_fd != ctx->desched_fd_child) {
		debug("  (SIGIO not for desched: code=%d, fd=%d)",
		      si->si_code, si->si_fd);
		return 0;
	}

	/* TODO: how can signals interrupt us here? */

	/* Clear the pending input. */
	nr_descheds = read_desched_counter(ctx);
	debug("  (desched #%llu during `%s')", nr_descheds, syscallname(call));

	/* Disarm the counter to prevent further desched events while
	 * we're manipulating the tracee and it finishes its
	 * syscall. */
	disarm_desched_event(ctx);

	/* For not-perfectly-understood reasons, we see a second,
	 * redundant SIGIO notificaion (see syscall_buffer.c).  Step
	 * over the redundant one. */
	sys_ptrace_syscall(tid);
	sys_waitpid(tid, &status);
	assert(WIFSTOPPED(status) && SIGIO == WSTOPSIG(status)
	       && read_desched_counter(ctx) == nr_descheds);

	/* The next event is either a trace of entering the blocking
	 * syscall, or if the syscall has already finished, then a
	 * trace of the syscallbuf code entering the
	 * disarm_desched_event() ioctl. */
	sys_ptrace_syscall(tid);
	sys_waitpid(tid, &status);
	/* (syscall trap event) */
	assert(WIFSTOPPED(status) && (0x80 | SIGTRAP) == WSTOPSIG(status));

	assert(SYSCALLBUF_IS_IP_BUFFERED_SYSCALL(regs->eip, ctx));

	read_child_registers(tid, regs);
	if (is_disarm_desched_event_syscall(ctx, regs)) {
		/* Finish the ioctl().  It won't block, and we don't
		 * need to record it or any other data for the replay
		 * to work properly.  The reason we can do this is
		 * that the replaying code doesn't need any trace data
		 * to know how to step over these ioctls.  And the
		 * syscall buffer must not be full (or we wouldn't
		 * have armed the desched event), so it doesn't need
		 * flushing.  The next traced event will flush the
		 * buffer normally. */
		sys_ptrace_syscall(tid);
		sys_waitpid(tid, &status);

		debug("  consumed unneeded desched event during `%s'",
		      syscallname(call));
		return USR_NOOP;
	}

	debug("  resuming (and probably switching out) blocked `%s'",
	      syscallname(call));

	if (SYS_restart_syscall == regs->orig_eax) {
		/* If we'll be resuming this as a "restart syscall",
		 * then note that the last started syscall was the one
		 * interrupted by desched. */
		ctx->last_syscall = call;
	}
	ctx->desched = 1;
	return call;
}

static int is_deterministic_signal(const siginfo_t* si)
{
	switch (si->si_signo) {
		/* These signals may be delivered deterministically;
		 * we'll check for sure below. */
	case SIGILL:
	case SIGTRAP:
	case SIGBUS:
	case SIGFPE:
	case SIGSEGV:
	case SIGSTKFLT:
		/* As bits/siginfo.h documents,
		 *
		 *   Values for `si_code'.  Positive values are
		 *   reserved for kernel-generated signals.
		 *
		 * So if the signal is maybe-synchronous, and the
		 * kernel delivered it, then it must have been
		 * delivered deterministically. */
		return si->si_code > 0;
	default:
		/* All other signals can never be delivered
		 * deterministically (to the approximation required by
		 * rr). */
		return 0;
	}

}

static void record_signal(int sig, struct context* ctx, const siginfo_t* si,
			  uint64_t max_rbc)
{
	ctx->child_sig = sig;

	if (is_deterministic_signal(si)) {
		ctx->event = -(sig | DET_SIGNAL_BIT);
	} else {
		ctx->event = -sig;
	}

	record_event(ctx, STATE_SYSCALL_ENTRY);
	reset_hpc(ctx, max_rbc); // TODO: the hpc gets reset in record event.
	assert(read_insts(ctx->hpc) == 0);
	// enter the sig handler
	sys_ptrace_singlestep_sig(ctx->child_tid, sig);
	// wait for the kernel to finish setting up the handler
	sys_waitpid(ctx->child_tid, &(ctx->status));
	// 0 instructions means we entered a handler
	int insts = read_insts(ctx->hpc);
	// TODO: find out actual struct sigframe size. 128 seems to be too small
	size_t frame_size = (insts == 0) ? 1024 : 0;
	struct user_regs_struct regs;
	read_child_registers(ctx->child_tid, &regs);
	record_child_data(ctx, ctx->event, frame_size, (void*)regs.esp);
}

static int is_trace_trap(const siginfo_t* si)
{
	return SIGTRAP == si->si_signo && TRAP_TRACE == si->si_code;
}

/**
 * Return nonzero if |si| seems to indicate that single-stepping in
 * the syscallbuf lib reached a syscall entry.
 *
 * What we *really* want to do is check to see if the delivered signal
 * was |STOPSIG_SYSCALL|, like when we continue with PTRACE_SYSCALL.
 * But for some reason that's not delivered with PTRACE_SINGLESTEP.
 * What /does/ seem to be delivered is SIGTRAP/code=BRKPT, as opposed
 * to SIGTRAP/code=TRACE for stepping normal instructions.
 */
static int seems_to_be_syscallbuf_syscall_trap(const siginfo_t* si)
{
	return (STOPSIG_SYSCALL == si->si_signo
		|| (SIGTRAP == si->si_signo && TRAP_BRKPT == si->si_code));
}

/**
 * Take |ctx| to a place where it's OK to deliver a signal.  |si| and
 * |regs| must be the current state of |ctx|.  The registers at the
 * happy place will be returned in |regs|.
 */
void go_to_a_happy_place(struct context* ctx,
			 const siginfo_t* si, struct user_regs_struct* regs)
{
	pid_t tid = ctx->child_tid;
	/* If we deliver the signal at the tracee's current execution
	 * point, it will result in a syscall-buffer-flush event being
	 * recorded if there are any buffered syscalls.  The
	 * signal-delivery event will follow.  So the definition of a
	 * "happy place" to deliver a signal is one in which the
	 * syscall buffer flush (i.e., executing all the buffered
	 * syscalls) will be guaranteed to happen before the signal
	 * delivery during replay.
	 *
	 * If the syscallbuf isn't allocated, then there can't be any
	 * buffered syscalls, so there's no chance of a
	 * syscall-buffer-flush event being recorded before the signal
	 * delivery.  So that's a happy place.
	 *
	 * If the $ip isn't in the syscall lib and the syscallbuf is
	 * allocated, then we may have buffered syscalls.  But the $ip
	 * being outside the lib means one of two things: the tracee
	 * is either doing something unrelated to the syscall lib, or
	 * called a wrapper in the syscallbuf lib and the buffer
	 * overflowed (falling back on a traced syscall).  In either
	 * case, the buffer-flush event will do what we want, so it's
	 * a happy place.
	 *
	 * So we continue with the $ip in the syscallbuf lib and the
	 * syscallbuf allocated.  For the purpose of this analysis, we
	 * can abstract the tracee's execution state as being in one
	 * of these intervals
	 *
	 * --- ... ---
	 *
	 *   (Before we allocate the syscallbuf, it's OK to deliver
	 *   the signal, as discussed above.)
	 *
	 * --- allocated syscallbuf ---
	 *
	 *   In this interval, no syscalls can be buffered, so there's
	 *   no possibility of a syscall-buffer-flush event being
	 *   recorded before the signal delivery.
	 *
	 * --- check to see if syscallbuf is locked ---
	 *
	 *   Assume there are buffered syscalls.  If the check says
	 *   "buffer is locked" (meaning we're re-entering the
	 *   syscallbuf code through a signal handler), then we'll
	 *   record a buffer-flush event (or already have).  But, this
	 *   means that during replay, we'll have already finished
	 *   flushing the buffer at this point in execution.  So the
	 *   check will say, "buffer is *un*locked".  That will cause
	 *   replay divergence.  So,
	 *
	 *   ***** NOT SAFE TO DELIVER SIGNAL HERE *****
	 *
	 * --- lock syscallbuf ---
	 *
	 *   The lib locks the buffer just before it allocates a
	 *   record.  Assume that the buffer is almost full.  During
	 *   recording, the lib can allocate a record that overflows
	 *   the buffer.  The tracee will abort the untraced syscall
	 *   at |can_buffer_syscall()|.  But if we drop a buffer-flush
	 *   event here, then during replay the buffer will be clear
	 *   and the overflow check will (most likely) succeed.
	 *   (Technically, if this was the first record allocated,
	 *   it's OK.)  So
	 *
	 *   ***** NOT SAFE TO DELIVER SIGNAL HERE *****
	 *
	 * --- arm-desched ioctl() ---
	 *
	 *   If the buffer isn't empty during recording, and is
	 *   flushed just after this during replay, then the lib will
	 *   allocate a different pointer for the record.  This
	 *   diverges so
	 *
	 *   ***** NOT SAFE TO DELIVER SIGNAL HERE *****
	 *
	 * --- (untraced) syscall ---
	 *
	 *   Still not safe for the reason above.  Note, if the tracee
	 *   falls back on a traced syscall, then its $ip will be
	 *   outside the syscallbuf lib, and that's a happy place (see
	 *   above).
	 *
	 *   ***** NOT SAFE TO DELIVER SIGNAL HERE *****
	 *
	 * --- disarm-desched ioctl() ---
	 *
	 *   Still not safe for the reason above.
	 *
	 *   ***** NOT SAFE TO DELIVER SIGNAL HERE *****
	 *
	 * --- commit syscall record ---
	 *
	 *   We just exited all code related to the last syscall in
	 *   the buffer, so during replay all buffered syscalls must
	 *   have been retired by now.  So it's OK to deliver the
	 *   signal.
	 *
	 * --- unlock syscallbuf ---
	 *
	 *   Still safe for the reasons above.
	 *
	 * --- ... ---
	 *
	 * The reason the analysis is that pedantic is because our
	 * next job is to figure out which interval the tracee is in.
	 * We can observe the following state in the tracee
	 *  - $ip
	 *  - buffer locked-ness
	 *  - buffer record counter
	 *  - enter/exit syscall
	 *
	 * It's not hard to work out what state bits imply which
	 * interval, and how changes in that state signify moving to
	 * another interval, and that's what the code below does. */
	struct syscallbuf_hdr initial_hdr;
	struct syscallbuf_hdr* hdr = ctx->syscallbuf_hdr;
	int status = ctx->status;

	debug("Stepping tracee to happy place to deliver signal ...");

	if (!hdr) {
		/* Witness that we're in the (...,
		 * allocated-syscallbuf) interval. */
		debug("  tracee hasn't allocated syscallbuf yet");
		return;
	}

	if (SYSCALLBUF_IS_IP_IN_LIB(regs->eip, ctx)
	    && is_deterministic_signal(si)) {
		fatal("TODO: support deterministic signals triggered by syscallbuf code");
	}
	/* TODO: when we add support for deterministic signals, we
	 * should sigprocmask-off all tracee signals while we're
	 * stepping.  If we tried that with the current impl, the
	 * syscallbuf code segfaulting would lead to an infinite
	 * single-stepping loop here.. */

	initial_hdr = *hdr;
	while (1) {
		siginfo_t tmp_si;
		int is_syscall;

		if (!SYSCALLBUF_IS_IP_IN_LIB(regs->eip, ctx)) {
			/* The tracee can't possible affect the
			 * syscallbuf here, so a flush is safe.. */
			debug("  tracee outside syscallbuf lib");
			goto happy_place;
		}
		if (initial_hdr.locked && !hdr->locked) {
			/* Witness that the tracee moved into the safe
			 * (unlock-syscallbuf, ...)  interval. */
			debug("  tracee just unlocked syscallbuf");
			goto happy_place;
		}
		/* XXX we /could/ check if the syscall record was just
		 * commited, since that's a safe interval, but the
		 * tracee will also unlock the buffer just after that,
		 * so meh.  Should do this though if it's a perf
		 * win. */

		/* We've now established that the tracee is in the
		 * interval (allocated-syscallbuf, unlock-syscallbuf).
		 * Until we can prove the tracee moved into a safe
		 * interval within that, keep stepping. */
		sys_ptrace_singlestep(tid);
		sys_waitpid(tid, &status);

		assert(WIFSTOPPED(status));
		sys_ptrace_getsiginfo(tid, &tmp_si);
		read_child_registers(tid, regs);
		is_syscall = seems_to_be_syscallbuf_syscall_trap(&tmp_si);

		if (!is_syscall && !is_trace_trap(&tmp_si)) {
			fatal("TODO: support multiple pending signals; received %s (code: %d) at $ip:%p while trying to deliver %s (code: %d)",
			      signalname(tmp_si.si_signo), tmp_si.si_code,
			      (void*)regs->eip,
			      signalname(si->si_signo), si->si_code);
		}
		if (!is_syscall) {
			continue;
		}

		/* TODO more signals can be delivered while we're
		 * stepping here too.  Sigh.  See comment above about
		 * masking signals off.  When we mask off signals, we
		 * won't need to disarm the desched event, but we will
		 * need to handle spurious desched notifications. */
		if (is_desched_event_syscall(ctx, regs)) {
			debug("  stepping over desched-event syscall");
			/* Finish the syscall. */
			sys_ptrace_singlestep(tid);
			sys_waitpid(tid, &status);
			if (is_arm_desched_event_syscall(ctx, regs)) {
				/* Disarm the event: we don't need or
				 * want to hear about descheds while
				 * we're stepping the tracee through
				 * the syscall wrapper. */
				disarm_desched_event(ctx);
			}
			/* We don't care about disarm-desched-event
			 * syscalls; they're irrelevant. */
		} else {
			debug("  running wrapped syscall");
			/* We may have been notified of the signal
			 * just after arming the event, but just
			 * before entering the syscall.  So disarm for
			 * safety. */
			/* XXX we really should warn about this, but
			 * it's just too noisy during unit tests.
			 * Should find a better way to choose mode. */
			/*log_warn("Disabling context-switching for possibly-blocking syscall (%s); deadlock may follow",
			  syscallname(regs->orig_eax));*/
			disarm_desched_event(ctx);
			/* And (hopefully!) finish the syscall. */
			sys_ptrace_singlestep(tid);
			sys_waitpid(tid, &status);
		}
	}

happy_place:
	/* TODO: restore previous tracee signal mask. */
	(void)0;
}

void handle_signal(const struct flags* flags, struct context* ctx)
{
	pid_t tid = ctx->child_tid;
	int sig = signal_pending(ctx->status);
	int event;
	siginfo_t si;
	struct user_regs_struct regs;
	uint64_t max_rbc = flags->max_rbc;

	if (sig <= 0) {
		return;
	}

	debug("%d: handling signal %s (pevent: %d, event: %s)",
	      ctx->child_tid, signalname(sig),
	      GET_PTRACE_EVENT(ctx->status), strevent(ctx->event));

	sys_ptrace_getsiginfo(tid, &si);
	read_child_registers(tid, &regs);

	if (SIGIO == sig
	    && (event = try_handle_desched_event(ctx, &si, &regs))) {
		ctx->event = event;
		ctx->child_sig = 0; 
		return;
	}

	go_to_a_happy_place(ctx, &si, &regs);

	/* See if this signal occurred because of an rr implementation detail,
	 * and fudge ctx appropriately. */
	switch (sig) {
	case SIGSEGV: {
		int mmap_event;
		if (try_handle_rdtsc(ctx)) {
			ctx->event = SIG_SEGV_RDTSC;
			ctx->child_sig = 0;
			return;
		}
		if ((mmap_event = try_handle_shared_mmap_access(ctx, &si))) {
			ctx->event = mmap_event;
			ctx->child_sig = 0;
			return;
		}
		break;
	}
	case SIGIO:
		/* TODO: imprecise counters can probably race with
		 * delivery of a "real" SIGIO */
		if (read_rbc(ctx->hpc) >= max_rbc) {
			/* HPC interrupt due to exceeding time
			 * slice. */
			ctx->event = USR_SCHED;
			ctx->child_sig = 0;
			return;
		}
		break;
	}

	/* This signal was generated by the program or an external
	 * source, record it normally. */
	record_signal(sig, ctx, &si, max_rbc);
}
