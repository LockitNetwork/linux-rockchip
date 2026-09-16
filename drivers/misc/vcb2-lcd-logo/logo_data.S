/* logo_data.S
 *
 * Links logo.bin into the kernel module as a read-only data section.
 * The assembler runs from the kernel build dir; the Makefile adds this
 * module's source dir to its include path (-Wa,-I$(src)) so logo.bin is found.
 */

	.section .rodata, "a"

	.global logo_data
	.type   logo_data, @object
logo_data:
	.incbin "logo.bin"
logo_data_end:
	.global logo_data_end
	.type   logo_data_end, @object

	.previous
