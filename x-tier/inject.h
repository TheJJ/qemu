#ifndef XTIER_INJECT_H_
#define XTIER_INJECT_H_

#include "external_command.h"
#include "qemu-common.h"

/*
 * STRUCTS
 */
/**
 * Structure to translate function calls.
 */
struct XTIER_x86_call_registers
{
	// By placing the ESP regs on top of the other regs,
	// they will be on the stack first.
	// Thus if an printk requires more function arguments,
	// they will be naturally there where printk will look for them :).
	uint64_t esp0; // First esp
	uint64_t esp1;
	uint64_t esp2;
	uint64_t esp3;
	uint64_t esp4; // Last esp

	uint64_t rdi; // First arg
	uint64_t rsi;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t r8;
	uint64_t r9; // Last arg
};

/**
 * Handle a hypercall from an injected module.
 */
void XTIER_inject_handle_interrupt(CPUState *env,
                                   struct XTIER_external_command_redirect *redirect);

#endif /* XTIER_INJECT_H_ */
