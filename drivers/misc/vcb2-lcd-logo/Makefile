# SPDX-License-Identifier: GPL-2.0
#
# VCB2 ST7789 boot logo. Sources, logo.bin and the DT overlay are copied from
# power-management/st7789-kernel-logo with `just copy-lcd-logo`.

obj-$(CONFIG_VCB2_LCD_LOGO)	+= st7789_logo.o
st7789_logo-y			:= st7789_logo_main.o logo_data.o

# The assembler runs from the object tree, so .incbin "logo.bin" needs this
# directory on its include path.
AFLAGS_logo_data.o := -Wa,-I$(srctree)/$(src)

# Re-assemble logo_data.o whenever logo.bin changes.
$(obj)/logo_data.o: $(srctree)/$(src)/logo.bin
