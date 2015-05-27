/*
 * XTIER_inject.c
 *
 *  Created on: Apr 3, 2012
 *      Author: Sebastian Vogl <vogls@sec.in.tum.de>
 */


#include "X-TIER_inject.h"
#include "X-TIER_event_handler.h"
#include "X-TIER_debug.h"
#include "X-TIER_qemu.h"

#include "../include/sysemu/kvm.h"
#include "../include/exec/cpu-all.h"

#include "../X-TIER-base/X-TIER_hypercall.h"


/*
 * Try to convert the given value to a HV address. If this does not succeed,
 * this may be a not a pointer, so use the original value.
 *
 * Warning: dirty hack. seriously.
 */
static hwaddr XTIER_inject_transform_call_register(CPUState *state, hwaddr in)
{
	hwaddr ret = 0;

	// try to use the input as memory address, convert it to HV
	ret = gva_to_hva(state, in);

	// mapping could not be found, use the address as number.
	if (ret == -1) {
		ret = in;
	}

	return ret;
}

/*
 * Convert all GV addresses within the registers to HV addresses.
 */
static void XTIER_inject_transform_call_registers64(CPUState *state, struct kvm_regs *vm_regs, struct XTIER_x86_call_registers *call_regs)
{
	//get register values, try to convert possible GVA addresses to HVA
	call_regs->rdi = XTIER_inject_transform_call_register(state, vm_regs->rdi);
	call_regs->rsi = XTIER_inject_transform_call_register(state, vm_regs->rsi);
	call_regs->rdx = XTIER_inject_transform_call_register(state, vm_regs->rdx);
	call_regs->rcx = XTIER_inject_transform_call_register(state, vm_regs->rcx);
	call_regs->r8  = XTIER_inject_transform_call_register(state, vm_regs->r8);
	call_regs->r9  = XTIER_inject_transform_call_register(state, vm_regs->r9);

	// try to get esp for transfering the stack
	hwaddr esp = XTIER_inject_transform_call_register(state, vm_regs->rsp);

	if (esp == vm_regs->rsp) {
		printf("Warning: Could not convert ESP!\n");
	}
	else {
		call_regs->esp0 = XTIER_inject_transform_call_register(state, esp + 8);
		call_regs->esp1 = XTIER_inject_transform_call_register(state, esp + 16);
		call_regs->esp2 = XTIER_inject_transform_call_register(state, esp + 24);
		call_regs->esp3 = XTIER_inject_transform_call_register(state, esp + 32);
		call_regs->esp4 = XTIER_inject_transform_call_register(state, esp + 40);
		//more stack members are ignored for now.
	}
}

/*
 * Convert all GV addresses within the registers to HV addresses.
 */
static void XTIER_inject_transform_call_registers32(CPUState *state, struct kvm_regs *vm_regs, struct XTIER_x86_call_registers *call_regs)
{
	// Try to convert ebp
	hwaddr esp = XTIER_inject_transform_call_register(state, vm_regs->rsp);

	if(esp == vm_regs->rsp)
		printf("Warning: Could not convert ESP!\n");

	//printf("RSP: 0x%llx\n", esp);
	//printf("RSP+4: 0x%llx\n", *(u64 *)(esp + 4));
	//printf("RSP+8: 0x%llx\n", *(u64 *)(esp + 8));

	call_regs->rdi = XTIER_inject_transform_call_register(state, esp + 4);
	call_regs->rsi = XTIER_inject_transform_call_register(state, esp + 8);
	call_regs->rdx = XTIER_inject_transform_call_register(state, esp + 12);
	call_regs->rcx = XTIER_inject_transform_call_register(state, esp + 16);
	call_regs->r8  = XTIER_inject_transform_call_register(state, esp + 20);
	call_regs->r9  = XTIER_inject_transform_call_register(state, esp + 24);

	call_regs->esp0 = XTIER_inject_transform_call_register(state, esp + 28);
	call_regs->esp1 = XTIER_inject_transform_call_register(state, esp + 32);
	call_regs->esp2 = XTIER_inject_transform_call_register(state, esp + 36);
	call_regs->esp3 = XTIER_inject_transform_call_register(state, esp + 40);
	call_regs->esp4 = XTIER_inject_transform_call_register(state, esp + 44);
}

/**
 * redirect guest kernel printk to a printf call.
 */
static void XTIER_inject_handle_printk(CPUState *state, struct kvm_regs *regs)
{
	//the first members of this struct store the the stack elements
	//more printf/k arguments will use these as the additional arguments
	struct XTIER_x86_call_registers call_regs;

	int (*printf_ptr)(char const *str, ...);

	// Transform the GVA addresses into host addresses
	if (_XTIER.os == XTIER_OS_LINUX_64) {
		XTIER_inject_transform_call_registers64(state, regs, &call_regs);

		// Is this a normal printf or something that should be forwarded to a script?
		// This is currently only supported for 64-bit systems.
		if (XTIER_event_handler_print_dispatch(state, regs, &call_regs)) {
			return;
		}
	}
	else {
		XTIER_inject_transform_call_registers32(state, regs, &call_regs);
	}

	printf_ptr = &printf;
	//printf("[kernel printf]\n");

	__asm__ volatile(
		"movq %0, %%rdi;"
		"movq %1, %%rsi;"
		"movq %2, %%rdx;"
		"movq %3, %%rcx;"
		"movq %4, %%r8;"
		"movq %5, %%r9;"
		"movq %6, %%rax;"
		"call *%%rax;"
		:
		:
		"r" (call_regs.rdi),
		"r" (call_regs.rsi),
		"r" (call_regs.rdx),
		"r" (call_regs.rcx),
		"r" (call_regs.r8),
		"r" (call_regs.r9),
		"r" (printf_ptr)
		:
		"%rdi",
		"%rsi",
		"%rdx",
		"%rcx",
		"%r8",
		"%r9",
		"%rax");
}

static void XTIER_inject_handle_data_transfer(CPUState *state,
                                              char *data, long size,
                                              struct XTIER_external_command_redirect *redirect)
{
	int i = 0;

	// Convert pointer
	PRINT_DEBUG("Converting %p ...\n", data);
	char *data_on_host = (char *)gva_to_hva(state, (long)data);

	// Check pointer
	if ((long)data_on_host == -1)
	{
		PRINT_ERROR("Could not convert %p to a host address!\n", data);
		return;
	}

	// DEBUG
	PRINT_DEBUG("Received %ld bytes of data from %p (orig was %p)\n", size, data_on_host, data);

	if (redirect)
	{
		// Is this a pipe redirection?
		if (redirect->type == PIPE)
		{
			// Check if we must open the file
			if (!redirect->stream)
			{
				redirect->stream = fopen(redirect->filename, "w");

				if (redirect->stream <= 0)
				{
					PRINT_ERROR("Could not open file '%s'\n", redirect->filename);
					return;
				}

				// We just opened the file, so we send the data begin header
				fprintf(redirect->stream, "" XTIER_EXTERNAL_OUTPUT_BEGIN);
			}

			// Print data to file if the type is PIPE
			for (i = 0; i < size; i++)
			{
				fprintf(redirect->stream, "%c", *(data_on_host + i));
			}
		}

		// We currently do not handle any other cases
	}
}

/*
 * Handle an interrupt from an injected program.
 */
void XTIER_inject_handle_interrupt(CPUState *state,
                                   struct XTIER_external_command_redirect *redirect)
{
	struct kvm_regs regs;

	PRINT_DEBUG("Handling interrupt...\n");

	// Get the registers
	XTIER_ioctl(KVM_GET_REGS, &regs);

	// Handle the command
	// CONVENTIONS:
	// -> COMMAND: RAX
	// -> FIRST PARAM: RBX
	// -> SECOND PARAM: RCX
	//
	// -> RETURN VALUE: RAX
	switch (regs.rax) {
		case XTIER_HYPERCALL_RESERVE_MEMORY:
			XTIER_ioctl(XTIER_IOCTL_INJECT_RESERVE_MEMORY, (void *)regs.rbx);
			break;
		case XTIER_HYPERCALL_PRINT:
			XTIER_inject_handle_printk(state, &regs);
			break;
		case XTIER_HYPERCALL_DATA_TRANSFER:
			XTIER_inject_handle_data_transfer(state, (char *)regs.rbx, regs.rcx, redirect);
			break;
		default:
			PRINT_ERROR("unknown hypercall %d command '%lld'!\n", XTIER_HYPERCALL_INTERRUPT, regs.rax);
			break;
	}
}
