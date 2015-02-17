#
# Makefile for MMC/SD card drivers
#
CFLAGS_KERNEL := -fno-aggressive-loop-optimizations

obj-$(CONFIG_MMC_BLOCK)		+= mmc_block.o
mmc_block-objs			:= block.o queue.o
obj-$(CONFIG_MMC_TEST)		+= mmc_test.o

obj-$(CONFIG_SDIO_UART)		+= sdio_uart.o

obj-$(CONFIG_MMC_BLOCK_TEST)		+= mmc_block_test.o

obj-$(CONFIG_MMC_YL_PARAMS)	+= yl_params.o
obj-$(CONFIG_MMC_YL_PARAMS)	+= yl_params_if.o
