

    .globl	_start
	.ent	_start
_start:
	.set	noreorder
	la		gp, _gp					# set the global data pointer, defined in the linker script
	.end	_start
