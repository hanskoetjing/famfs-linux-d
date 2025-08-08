/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CXL_ALLOC_H
#define CXL_ALLOC_H

extern unsigned long __cxl_alloc(char *dax_device_path, unsigned long len);
extern unsigned long __cxl_alloc_dsm(char *dax_device_path, unsigned long len);

#endif