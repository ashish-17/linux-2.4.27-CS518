/*
 *  linux/arch/ppc64/kernel/signal.c
 *
 *  PowerPC version 
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 *  Derived from "arch/i386/kernel/signal.c"
 *    Copyright (C) 1991, 1992 Linus Torvalds
 *    1997-11-28  Modified for POSIX.1b signals by Richard Henderson
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/stddef.h>
#include <linux/elf.h>
#include <asm/ppc32.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/ppcdebug.h>
#include <asm/unistd.h>

#define DEBUG_SIG 0

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

#define GP_REGS_SIZE	MIN(sizeof(elf_gregset_t), sizeof(struct pt_regs))
#define FP_REGS_SIZE	sizeof(elf_fpregset_t)

#define TRAMP_TRACEBACK	3
#define TRAMP_SIZE	6

/*
 * When we have signals to deliver, we set up on the user stack,
 * going down from the original stack pointer:
 *	1) a sigframe/rt_sigframe struct which contains the sigcontext/ucontext	
 *	2) a gap of __SIGNAL_FRAMESIZE bytes which acts as a dummy caller
 *	   frame for the signal handler.
 */

struct sigframe {
	/* sys_sigreturn requires the sigcontext be the first field */
	struct sigcontext sc;
	unsigned int tramp[TRAMP_SIZE];
	/* 64 bit ABI allows for 288 bytes below sp before decrementing it. */
	char abigap[288];
};

struct rt_sigframe {
	/* sys_rt_sigreturn requires the ucontext be the first field */
	struct ucontext uc;
	unsigned long _unused[2];
	unsigned int tramp[TRAMP_SIZE];
	struct siginfo *pinfo;
	void *puc;
	struct siginfo info;
	/* 64 bit ABI allows for 288 bytes below sp before decrementing it. */
	char abigap[288];
};

extern long sys_wait4(pid_t pid, unsigned int *stat_addr,
		     int options, /*unsigned long*/ struct rusage *ru);

int
copy_siginfo_to_user(siginfo_t *to, siginfo_t *from)
{
	if (!access_ok (VERIFY_WRITE, to, sizeof(siginfo_t)))
		return -EFAULT;
	if (from->si_code < 0)
		return __copy_to_user(to, from, sizeof(siginfo_t));
	else {
		int err;

		/* If you change siginfo_t structure, please be sure
		   this code is fixed accordingly.
		   It should never copy any pad contained in the structure
		   to avoid security leaks, but must copy the generic
		   3 ints plus the relevant union member.  */
		err = __put_user(from->si_signo, &to->si_signo);
		err |= __put_user(from->si_errno, &to->si_errno);
		err |= __put_user((short)from->si_code, &to->si_code);
		/* First 32bits of unions are always present.  */
		err |= __put_user(from->si_pid, &to->si_pid);
		switch (from->si_code >> 16) {
		case __SI_FAULT >> 16:
			err |= __put_user(from->si_addr, &to->si_addr);
			break;
		case __SI_CHLD >> 16:
			err |= __put_user(from->si_utime, &to->si_utime);
			err |= __put_user(from->si_stime, &to->si_stime);
			err |= __put_user(from->si_status, &to->si_status);
		default:
			err |= __put_user(from->si_uid, &to->si_uid);
			break;
		/* case __SI_RT: This is not generated by the kernel as of now.  */
		}
		return err;
	}
}

int do_signal(sigset_t *oldset, struct pt_regs *regs);

/*
 * Atomically swap in the new signal mask, and wait for a signal.
 */
asmlinkage long
sys_sigsuspend(old_sigset_t mask, int p2, int p3, int p4, int p6, int p7,
	       struct pt_regs *regs)
{
	sigset_t saveset;

	mask &= _BLOCKABLE;
	spin_lock_irq(&current->sigmask_lock);
	saveset = current->blocked;
	siginitset(&current->blocked, mask);
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	regs->result = -EINTR;
	regs->gpr[3] = EINTR;
	regs->ccr |= 0x10000000;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(&saveset, regs))
			/*
			 * If a signal handler needs to be called,
			 * do_signal() has set R3 to the signal number (the
			 * first argument of the signal handler), so don't
			 * overwrite that with EINTR !
			 * In the other cases, do_signal() doesn't touch 
			 * R3, so it's still set to -EINTR (see above).
			 */
			return regs->gpr[3];
	}
}

asmlinkage long
sys_rt_sigsuspend(sigset_t *unewset, size_t sigsetsize, int p3, int p4, int p6,
		  int p7, struct pt_regs *regs)
{
	sigset_t saveset, newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&newset, unewset, sizeof(newset)))
		return -EFAULT;
	sigdelsetmask(&newset, ~_BLOCKABLE);

	spin_lock_irq(&current->sigmask_lock);
	saveset = current->blocked;
	current->blocked = newset;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	regs->result = -EINTR;
	regs->gpr[3] = EINTR;
	regs->ccr |= 0x10000000;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(&saveset, regs))
			return regs->gpr[3];
	}
}

asmlinkage long
sys_sigaltstack(const stack_t *uss, stack_t *uoss, unsigned long r5,
		unsigned long r6, unsigned long r7, unsigned long r8,
		struct pt_regs *regs)
{
        return do_sigaltstack(uss, uoss, regs->gpr[1]);
}

asmlinkage long
sys_sigaction(int sig, const struct old_sigaction *act,
	      struct old_sigaction *oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	if (act) {
		old_sigset_t mask;

		if (verify_area(VERIFY_READ, act, sizeof(*act)) ||
		    __get_user(new_ka.sa.sa_handler, &act->sa_handler) ||
		    __get_user(new_ka.sa.sa_restorer, &act->sa_restorer))
			return -EFAULT;
		__get_user(new_ka.sa.sa_flags, &act->sa_flags);
		__get_user(mask, &act->sa_mask);
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);
	if (!ret && oact) {
		if (verify_area(VERIFY_WRITE, oact, sizeof(*oact)) ||
		    __put_user(old_ka.sa.sa_handler, &oact->sa_handler) ||
		    __put_user(old_ka.sa.sa_restorer, &oact->sa_restorer))
			return -EFAULT;
		__put_user(old_ka.sa.sa_flags, &oact->sa_flags);
		__put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask);
	}

	return ret;
}

/*
 * Set up the sigcontext for the signal frame.
 */

static int
setup_sigcontext(struct sigcontext *sc, struct pt_regs *regs,
		 int signr, sigset_t *set, unsigned long handler)
{
	int err = 0;

	if (regs->msr & MSR_FP)
		giveup_fpu(current);

	current->thread.saved_msr = regs->msr & ~(MSR_FP | MSR_FE0 | MSR_FE1);
	regs->msr = current->thread.saved_msr | current->thread.fpexc_mode;
	current->thread.saved_softe = regs->softe;

	err |= __put_user(&sc->gp_regs, &sc->regs);
	err |= __copy_to_user(&sc->gp_regs, regs, GP_REGS_SIZE);
	err |= __copy_to_user(&sc->fp_regs, &current->thread.fpr, FP_REGS_SIZE);
	err |= __put_user(signr, &sc->signal);
	err |= __put_user(handler, &sc->handler);
	if (set != NULL)
		err |=  __put_user(set->sig[0], &sc->oldmask);

	regs->msr &= ~(MSR_FP | MSR_FE0 | MSR_FE1);
	current->thread.fpscr = 0;

	return err;
}

/*
 * Restore the sigcontext from the signal frame.
 */

static int
restore_sigcontext(struct pt_regs *regs, sigset_t *set, struct sigcontext *sc)
{
	unsigned int err = 0;

	if (regs->msr & MSR_FP)
		giveup_fpu(current);

	err |= __copy_from_user(regs, &sc->gp_regs, GP_REGS_SIZE);
	err |= __copy_from_user(&current->thread.fpr, &sc->fp_regs, FP_REGS_SIZE);
	current->thread.fpexc_mode = regs->msr & (MSR_FE0 | MSR_FE1);
	if (set != NULL)
		err |=  __get_user(set->sig[0], &sc->oldmask);

	/* Don't allow the signal handler to change these modulo FE{0,1} */
	regs->msr = current->thread.saved_msr & ~(MSR_FP | MSR_FE0 | MSR_FE1);
	regs->softe = current->thread.saved_softe;

	return err;
}

/*
 * Allocate space for the signal frame
 */
static inline void *
get_sigframe(struct k_sigaction *ka, struct pt_regs *regs, size_t frame_size)
{
        unsigned long newsp;

        /* Default to using normal stack */
        newsp = regs->gpr[1];

	if (ka->sa.sa_flags & SA_ONSTACK) {
		if (! on_sig_stack(regs->gpr[1]))
			newsp = (current->sas_ss_sp + current->sas_ss_size);
	}

	/* The ABI requires quadword alignment for the stack. */
	return (void *)((newsp - frame_size) & -16ul);

}

static int
setup_trampoline(unsigned int syscall, unsigned int *tramp)
{
	int i, err = 0;

	/* addi r1, r1, __SIGNAL_FRAMESIZE  # Pop the dummy stackframe */
	err |= __put_user(0x38210000UL | (__SIGNAL_FRAMESIZE & 0xffff), &tramp[0]);
	/* li r0, __NR_[rt_]sigreturn| */
	err |= __put_user(0x38000000UL | (syscall & 0xffff), &tramp[1]);
	/* sc */
	err |= __put_user(0x44000002UL, &tramp[2]);

	/* Minimal traceback info */
	for (i=TRAMP_TRACEBACK; i < TRAMP_SIZE ;i++)
		err |= __put_user(0, &tramp[i]);

	if (!err)
		flush_icache_range((unsigned long) &tramp[0],
			   (unsigned long) &tramp[TRAMP_SIZE]);

	return err;
}


asmlinkage int
sys_rt_sigreturn(unsigned long r3, unsigned long r4, unsigned long r5,
		 unsigned long r6, unsigned long r7, unsigned long r8,
		 struct pt_regs *regs)
{
	struct ucontext *uc = (struct ucontext *)regs->gpr[1];
	sigset_t set;
	stack_t st;

	if (verify_area(VERIFY_READ, uc, sizeof(*uc)))
		goto badframe;

	if (__copy_from_user(&set, &uc->uc_sigmask, sizeof(set)))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	if (restore_sigcontext(regs, NULL, &uc->uc_mcontext))
		goto badframe;

	if (__copy_from_user(&st, &uc->uc_stack, sizeof(st)))
		goto badframe;

	/* This function sets back the stack flags into
	   the current task structure.  */
	sys_sigaltstack(&st, NULL, 0, 0, 0, 0, regs);

	return regs->result;

badframe:
	do_exit(SIGSEGV);
}

static void
setup_rt_frame(int signr, struct k_sigaction *ka, siginfo_t *info,
		sigset_t *set, struct pt_regs *regs)
{
	/* Handler is *really* a pointer to the function descriptor for
	 * the signal routine.  The first entry in the function
	 * descriptor is the entry address of signal and the second
	 * entry is the TOC value we need to use.
	 */
	func_descr_t *funct_desc_ptr;
	struct rt_sigframe *frame;
	unsigned long newsp;
	int err = 0;

	frame = get_sigframe(ka, regs, sizeof(*frame));

	if (verify_area(VERIFY_WRITE, frame, sizeof(*frame)))
		goto give_sigsegv;

	err |= __put_user(&frame->info, &frame->pinfo);
	err |= __put_user(&frame->uc, &frame->puc);
	err |= copy_siginfo_to_user(&frame->info, info);
	if (err)
		goto give_sigsegv;

	/* Create the ucontext.  */
	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(0, &frame->uc.uc_link);
	err |= __put_user(current->sas_ss_sp, &frame->uc.uc_stack.ss_sp);
	err |= __put_user(sas_ss_flags(regs->gpr[1]),
			  &frame->uc.uc_stack.ss_flags);
	err |= __put_user(current->sas_ss_size, &frame->uc.uc_stack.ss_size);
	err |= setup_sigcontext(&frame->uc.uc_mcontext, regs, signr, NULL,
				(unsigned long)ka->sa.sa_handler);
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	if (err)
		goto give_sigsegv;

	/* Set up to return from userspace. */
	err |= setup_trampoline(__NR_rt_sigreturn, &frame->tramp[0]);
	if (err)
		goto give_sigsegv;

	funct_desc_ptr = (func_descr_t *) ka->sa.sa_handler;

	/* Allocate a dummy caller frame for the signal handler. */
	newsp = (unsigned long)frame - __SIGNAL_FRAMESIZE;
	err |= put_user(0, (unsigned long *)newsp);

	/* Set up "regs" so we "return" to the signal handler. */
	err |= get_user(regs->nip, &funct_desc_ptr->entry);
	regs->link = (unsigned long) &frame->tramp[0];
	regs->gpr[1] = newsp;
	err |= get_user(regs->gpr[2], &funct_desc_ptr->toc);
	regs->gpr[3] = signr;
	err |= get_user(regs->gpr[4], (unsigned long *)&frame->pinfo);
	err |= get_user(regs->gpr[5], (unsigned long *)&frame->puc);
	regs->gpr[6] = (unsigned long) frame;
	if (err)
		goto give_sigsegv;

	return;

give_sigsegv:
#if DEBUG_SIG
	printk("badframe in setup_rt_frame, regs=%p frame=%p, newsp=0x%lx\n",
		regs, frame, newsp);
#endif
	do_exit(SIGSEGV);
}

/*
 * Do a signal return; undo the signal stack.
 */
asmlinkage long
sys_sigreturn(unsigned long r3, unsigned long r4, unsigned long r5,
	      unsigned long r6, unsigned long r7, unsigned long r8,
	      struct pt_regs *regs)
{
	struct sigcontext *sc = (struct sigcontext *)regs->gpr[1];
	sigset_t set;

	if (verify_area(VERIFY_READ, sc, sizeof(*sc)))
		goto badframe;

	if (restore_sigcontext(regs, &set, sc))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	return regs->result;

badframe:
	do_exit(SIGSEGV);
}	


static void
setup_frame(int signr, struct k_sigaction *ka, sigset_t *set,
	    struct pt_regs *regs)
{
	/* Handler is *really* a pointer to the function descriptor for
	 * the signal routine.  The first entry in the function
	 * descriptor is the entry address of signal and the second
	 * entry is the TOC value we need to use.
	 */
	func_descr_t *funct_desc_ptr;
	struct sigframe *frame;
	unsigned long newsp;
	int err = 0;

	frame = get_sigframe(ka, regs, sizeof(*frame));

	if (verify_area(VERIFY_WRITE, frame, sizeof(*frame)))
		goto badframe;

	err |= setup_sigcontext(&frame->sc, regs, signr, set,
				(unsigned long)ka->sa.sa_handler);

	/* Set up to return from userspace. */
	err |= setup_trampoline(__NR_sigreturn, &frame->tramp[0]);
	if (err)
		goto badframe;

	funct_desc_ptr = (func_descr_t *) ka->sa.sa_handler;

	/* Allocate a dummy caller frame for the signal handler. */
	newsp = (unsigned long)frame - __SIGNAL_FRAMESIZE;
	err |= put_user(0, (unsigned long *)newsp);

	/* Set up "regs" so we "return" to the signal handler. */
	err |= get_user(regs->nip, &funct_desc_ptr->entry);
	regs->link = (unsigned long) &frame->tramp[0];
	regs->gpr[1] = newsp;
	err |= get_user(regs->gpr[2], &funct_desc_ptr->toc);
	regs->gpr[3] = signr;
	regs->gpr[4] = (unsigned long) &frame->sc;
	if (err)
		goto badframe;

	return;

badframe:
#if DEBUG_SIG
	printk("badframe in setup_frame, regs=%p frame=%p newsp=%lx\n",
	       regs, frame, newsp);
#endif
	do_exit(SIGSEGV);
}

/*
 * OK, we're invoking a handler
 */
static void
handle_signal(unsigned long sig, struct k_sigaction *ka,
	      siginfo_t *info, sigset_t *oldset, struct pt_regs *regs)
{
	/* Set up Signal Frame */
	if (ka->sa.sa_flags & SA_SIGINFO)
		setup_rt_frame(sig, ka, info, oldset, regs);
	else
		setup_frame(sig, ka, oldset, regs);

	if (ka->sa.sa_flags & SA_ONESHOT)
		ka->sa.sa_handler = SIG_DFL;

	if (!(ka->sa.sa_flags & SA_NODEFER)) {
		spin_lock_irq(&current->sigmask_lock);
		sigorsets(&current->blocked,&current->blocked,&ka->sa.sa_mask);
		sigaddset(&current->blocked,sig);
		recalc_sigpending(current);
		spin_unlock_irq(&current->sigmask_lock);
	}
}

static inline void
syscall_restart(struct pt_regs *regs, struct k_sigaction *ka)
{
	switch ((int)regs->result) {
		case -ERESTARTNOHAND:
			/* ERESTARTNOHAND means that the syscall should only
			   be restarted if there was no handler for the signal,
			   and since we only get here if there is a handler,
			   we dont restart */
			regs->result = -EINTR;
			break;

		case -ERESTARTSYS:
			/* ERESTARTSYS means to restart the syscall if there is
			   no handler or the handler was registered with SA_RESTART */
			if (!(ka->sa.sa_flags & SA_RESTART)) {
				regs->result = -EINTR;
				break;
			}
		/* fallthrough */
		case -ERESTARTNOINTR:
			/* ERESTARTNOINTR means that the syscall should be
			   called again after the signal handler returns */
			regs->gpr[3] = regs->orig_gpr3;
			regs->nip -= 4;
			regs->result = 0;
	}
}

static int
get_signal_to_deliver(siginfo_t *info, struct pt_regs *regs)
{
	for (;;) {
		unsigned long signr;
		struct k_sigaction *ka;

		spin_lock_irq(&current->sigmask_lock);
		signr = dequeue_signal(&current->blocked, info);
		spin_unlock_irq(&current->sigmask_lock);

		if (!signr)
			break;

		if ((current->ptrace & PT_PTRACED) && signr != SIGKILL) {
			/* Let the debugger run.  */
			current->exit_code = signr;
			current->state = TASK_STOPPED;
			notify_parent(current, SIGCHLD);
			schedule();

			/* We're back.  Did the debugger cancel the sig?  */
			signr = current->exit_code;
			if (signr == 0)
				continue;
			current->exit_code = 0;

			/* The debugger continued.  Ignore SIGSTOP.  */
			if (signr == SIGSTOP)
				continue;

			/* Update the siginfo structure.  Is this good?  */
			if (signr != info->si_signo) {
				info->si_signo = signr;
				info->si_errno = 0;
				info->si_code = SI_USER;
				info->si_pid = current->p_pptr->pid;
				info->si_uid = current->p_pptr->uid;
			}

			/* If the (new) signal is now blocked, requeue it.  */
			if (sigismember(&current->blocked, signr)) {
				send_sig_info(signr, info, current);
				continue;
			}
		}

		ka = &current->sig->action[signr-1];

		if (ka->sa.sa_handler == SIG_IGN) {
			if (signr != SIGCHLD)
				continue;
			/* Check for SIGCHLD: it's special.  */
			while (sys_wait4(-1, NULL, WNOHANG, NULL) > 0)
				/* nothing */;
			continue;
		}

		if (ka->sa.sa_handler == SIG_DFL) {
			int exit_code = signr;

			/* Init gets no signals it doesn't want.  */
			if (current->pid == 1)
				continue;

			switch (signr) {
			case SIGCONT: case SIGCHLD: case SIGWINCH: case SIGURG:
				continue;

			case SIGTSTP: case SIGTTIN: case SIGTTOU:
				if (is_orphaned_pgrp(current->pgrp))
					continue;
				/* FALLTHRU */

			case SIGSTOP: {
				struct signal_struct *sig;
				current->state = TASK_STOPPED;
				current->exit_code = signr;
				sig = current->p_pptr->sig;
				if (sig && !(sig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDSTOP))
					notify_parent(current, SIGCHLD);
				schedule();
				continue;
			}

			case SIGQUIT: case SIGILL: case SIGTRAP:
			case SIGABRT: case SIGFPE: case SIGSEGV:
			case SIGBUS: case SIGSYS: case SIGXCPU: case SIGXFSZ:
				if (do_coredump(signr, regs))
					exit_code |= 0x80;
				/* FALLTHRU */

			default:
				sig_exit(signr, exit_code, info);
				/* NOTREACHED */
			}
		}
		return signr;
	}
	return 0;
}

/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 */
extern int do_signal32(sigset_t *oldset, struct pt_regs *regs);

int
do_signal(sigset_t *oldset, struct pt_regs *regs)
{
	siginfo_t info;
	int signr;

	/*
	 * If the current thread is 32 bit - invoke the
	 * 32 bit signal handling code
	 */
	if (current->thread.flags & PPC_FLAG_32BIT)
		return do_signal32(oldset, regs);

        if (!oldset)
		oldset = &current->blocked;

	signr = get_signal_to_deliver(&info, regs);
	if (signr > 0) {
		struct k_sigaction *ka = &current->sig->action[signr-1];

		/* Whee!  Actually deliver the signal.  */
		if (regs->trap == 0x0C00)
			syscall_restart(regs, ka);
		handle_signal(signr, ka, &info, oldset, regs);
		return 1;
	}

	if (regs->trap == 0x0C00 /* System Call! */ &&
	    ((int)regs->result == -ERESTARTNOHAND ||
	     (int)regs->result == -ERESTARTSYS ||
	     (int)regs->result == -ERESTARTNOINTR)) {
		regs->gpr[3] = regs->orig_gpr3;
		regs->nip -= 4;		/* Back up & retry system call */
		regs->result = 0;
	}

	return 0;
}



