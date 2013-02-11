/*
 * Dynamic function tracing support.
 *
 * Copyright (C) 2008 Abhishek Sagar <sagar.abhishek@gmail.com>
 *
 * For licencing details, see COPYING.
 *
 * Defines low-level handling of mcount calls when the kernel
 * is compiled with the -pg flag. When using dynamic ftrace, the
 * mcount call-sites get patched lazily with NOP till they are
 * enabled. All code mutation routines here take effect atomically.
 */

#include <linux/ftrace.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/ftrace.h>

#define PC_OFFSET      8
#define BL_OPCODE      0xeb000000
#define BL_OFFSET_MASK 0x00ffffff

static unsigned long bl_insn;
/*
 * for some arm toolchain like android toolchain would put a conditional
 * instruction after mcount calling, so we need refresh the CPSR
 * to jump over this inst
 */
static const unsigned long NOP = 0xe328f206; /* msr CPSR_f, #0x60000000 */
static const unsigned long LDMIA = 0xE8BD4000; /* ldmia   sp!, {lr} */

unsigned char *ftrace_nop_replace(void)
{
	char *ret_vaule;
#ifndef KBUILD_NEW_GNU_MCOUNT
	ret_vaule = (char *)&NOP;
#else
	/*
	 * For the arm toolchain change the mount to __gnu_mount_nc since 4.1.1
	 * and it add "push {lr}" behavior before call the __gnu_mount_nc,
	 * so we need to restore the lr, when we try to replace the jmp code
	 * to a normal code that don't impact performance
	 */
	ret_vaule = (char *)&LDMIA;
#endif
	return ret_vaule;
}

/* construct a branch (BL) instruction to addr */
unsigned char *ftrace_call_replace(unsigned long pc, unsigned long addr)
{
	long offset;

	offset = (long)addr - (long)(pc + PC_OFFSET);
	if (unlikely(offset < -33554432 || offset > 33554428)) {
		/* Can't generate branches that far (from ARM ARM). Ftrace
		 * doesn't generate branches outside of kernel text.
		 */
		WARN_ON_ONCE(1);
		return NULL;
	}
	offset = (offset >> 2) & BL_OFFSET_MASK;
	bl_insn = BL_OPCODE | offset;
	return (unsigned char *)&bl_insn;
}

int ftrace_modify_code(unsigned long pc, unsigned char *old_code,
		       unsigned char *new_code)
{
	unsigned long err = 0, replaced = 0, old, new;

	old = *(unsigned long *)old_code;
	new = *(unsigned long *)new_code;

	__asm__ __volatile__ (
		"1:  ldr    %1, [%2]  \n"
		"    cmp    %1, %4    \n"
		"2:  streq  %3, [%2]  \n"
		"    cmpne  %1, %3    \n"
		"    movne  %0, #2    \n"
		"3:\n"

		".pushsection .fixup, \"ax\"\n"
		"4:  mov  %0, #1  \n"
		"    b    3b      \n"
		".popsection\n"

		".pushsection __ex_table, \"a\"\n"
		"    .long 1b, 4b \n"
		"    .long 2b, 4b \n"
		".popsection\n"

		: "=r"(err), "=r"(replaced)
		: "r"(pc), "r"(new), "r"(old), "0"(err), "1"(replaced)
		: "memory");

	if (!err && (replaced == old))
		flush_icache_range(pc, pc + MCOUNT_INSN_SIZE);

	return 0;
}

int ftrace_update_ftrace_func(ftrace_func_t func)
{
	int ret;
	unsigned long pc, old;
	unsigned char *new;

	pc = (unsigned long)&ftrace_call;
	memcpy(&old, &ftrace_call, MCOUNT_INSN_SIZE);
	new = ftrace_call_replace(pc, (unsigned long)func);
	ret = ftrace_modify_code(pc, (unsigned char *)&old, new);
	return ret;
}

int ftrace_make_nop(struct module *mod,
		struct dyn_ftrace *rec, unsigned long addr)
{
	unsigned char *new, *old;
	unsigned long ip = rec->ip;

	old = ftrace_call_replace(ip, addr);
	new = ftrace_nop_replace();

	return ftrace_modify_code(rec->ip, old, new);
}

int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	unsigned char *new, *old;
	unsigned long ip = rec->ip;

	old = ftrace_nop_replace();
	new = ftrace_call_replace(ip, addr);

	return ftrace_modify_code(rec->ip, old, new);
}

/* run from ftrace_init with irqs disabled */
int __init ftrace_dyn_arch_init(void *data)
{
	*(unsigned long *)data = 0;
	return 0;
}
