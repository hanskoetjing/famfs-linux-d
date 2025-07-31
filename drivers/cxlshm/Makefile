# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_CXL_SHMM_HANDLER) += cxlshm_tcpconnmgr.o
obj-$(CONFIG_CXL_SHMM_HANDLER) += cxlshm_mm.o
cxlshm_mm-y                    := cxlshm_mm_impl.o
cxlshm_mm-y                    += cxlshm_daxhelper.o
cxlshm_mm-y                    += cxlshm_ownership_helper.o
obj-$(CONFIG_CXL_SHMM_HANDLER) += cxlshm_invalidator_mod.o
cxlshm_invalidator_mod-y       := cxlshm_invalidator.o
