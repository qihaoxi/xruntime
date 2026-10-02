/*
 * L0 最小栈切换(SysV x86-64):6 个 callee-saved GPR + ret 地址。
 * 形态与 PEL stackman x86_64 asm 相同(6 push + SP 交换 + 6 pop,~15ns);
 * 不自研收益见 02 §0.5.2——此处为 L0 对照轮的测量仪器,非产品代码。
 *
 * 无 CFI:自定义栈无法用 DWARF 表达;ASan/TSan 与自定义栈不兼容,
 * L0 测试仅在 sanitizer=none 门禁运行(见 CMakeLists)。
 */

	.text
	.globl	xr_cpu_switch
	.type	xr_cpu_switch, @function
xr_cpu_switch:
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	movq	%rsp, (%rdi)		/* *save_sp = rsp */
	movq	%rsi, %rsp		/* rsp = new_sp   */
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
	.size	xr_cpu_switch, .-xr_cpu_switch

/*
 * 首次进入任务栈的着陆点:r12 = ctx(伪造帧写入)。call 前 rsp 16 对齐
 * (c->sp 布局 6 寄存器 + ret;pop6+ret 后 rsp = stack_top)。
 */
	.globl	xr_ctx_trampoline
	.type	xr_ctx_trampoline, @function
xr_ctx_trampoline:
	movq	%r12, %rdi
	call	xr_ctx_entry_c
	ud2
	.size	xr_ctx_trampoline, .-xr_ctx_trampoline

	.section .note.GNU-stack,"",@progbits
