#
## Makefile for the module touchscreen drivers.
#

# Touch Core Driver Common Header Code..!!
#ccflags-y += -I$(srctree)/drivers/input/touchscreen/lge/

#obj-$(CONFIG_LGE_TOUCH_MODULE_SW49106)	+= touch_module_sw49106.o
#touch_module_sw49106-objs				+= touch_sw49106.o

obj-$(CONFIG_LGE_TOUCH_MODULE_FT3518)	+= touch_module_ft3518.o
touch_module_ft3518-objs				+= touch_ft3518.o

# The S3706 second-screen (Dual Screen) driver is a near-duplicate of the
# built-in synaptics S3707 main-panel driver and shares many global symbol
# names with it. When both are built-in (CONFIG_LGE_TOUCH_MODULE_S3706=y),
# those globals collide at link time. Rename this module's private globals with
# an s3706m_ prefix so it can be linked into vmlinux alongside touch_s3707.
# (Harmless when built as =m too.) Keep in sync with the S3707 driver's globals.
S3706_SYMBOL_RENAMES := \
	-DDefaultTimeout=s3706m_DefaultTimeout \
	-DF54Test=s3706m_F54Test \
	-DRead8BitRegisters=s3706m_Read8BitRegisters \
	-DRxChannelCount=s3706m_RxChannelCount \
	-DSCAN_PDT=s3706m_SCAN_PDT \
	-DTxChannelCount=s3706m_TxChannelCount \
	-DWrite8BitRegisters=s3706m_Write8BitRegisters \
	-Dext_trx_short_limit=s3706m_ext_trx_short_limit \
	-Dfwu_remove_complete=s3706m_fwu_remove_complete \
	-Dfwu_sysfs_mutex=s3706m_fwu_sysfs_mutex \
	-Dhigh_resistance_lower=s3706m_high_resistance_lower \
	-Dhigh_resistance_upper=s3706m_high_resistance_upper \
	-Dhybrid_abs_rx_lower=s3706m_hybrid_abs_rx_lower \
	-Dhybrid_abs_rx_upper=s3706m_hybrid_abs_rx_upper \
	-Dhybrid_abs_tx_lower=s3706m_hybrid_abs_tx_lower \
	-Dhybrid_abs_tx_upper=s3706m_hybrid_abs_tx_upper \
	-Djitter_lower=s3706m_jitter_lower \
	-Djitter_upper=s3706m_jitter_upper \
	-Dlogbuf=s3706m_logbuf \
	-Dprint_sd_log=s3706m_print_sd_log \
	-Drawcap_lower=s3706m_rawcap_lower \
	-Drawcap_upper=s3706m_rawcap_upper \
	-Drmidev_remove_complete=s3706m_rmidev_remove_complete \
	-Dsynaptics_fw_updater=s3706m_synaptics_fw_updater \
	-Dt_interval=s3706m_t_interval \
	-Dtouch_debug_mask=s3706m_touch_debug_mask \
	-Dtrx_short_limit=s3706m_trx_short_limit

CFLAGS_touch_s3706.o			+= $(S3706_SYMBOL_RENAMES)
CFLAGS_touch_s3706_fw_upgrade.o	+= $(S3706_SYMBOL_RENAMES)
CFLAGS_touch_s3706_prd.o		+= $(S3706_SYMBOL_RENAMES)
CFLAGS_touch_s3706_f54_test.o	+= $(S3706_SYMBOL_RENAMES)
CFLAGS_touch_s3706_rmi_dev.o	+= $(S3706_SYMBOL_RENAMES)

obj-$(CONFIG_LGE_TOUCH_MODULE_S3706)	+= touch_module_s3706.o
touch_module_s3706-objs					+= touch_s3706.o
touch_module_s3706-objs					+= touch_s3706_fw_upgrade.o
touch_module_s3706-objs					+= touch_s3706_prd.o
touch_module_s3706-objs					+= touch_s3706_f54_test.o
touch_module_s3706-objs					+= touch_s3706_rmi_dev.o
