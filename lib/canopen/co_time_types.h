/*
 * Copyright (c) 2010-2020 Janez Paternoster
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The typedefs are from CANopenNode v1.3 example/CO_OD.h.
 */

/*
 * The TIME types the v1.3 stack's CO_TIME.h expects from CO_OD.h. CANopenEditor
 * no longer emits them; the stack's own sample adds them by hand. Force-included
 * by lib/canopen/CMakeLists.txt, so a node's od/CO_OD.h stays a plain export.
 */

#ifndef ZELOS_CO_TIME_TYPES_H_
#define ZELOS_CO_TIME_TYPES_H_

typedef union {
	unsigned long long ullValue;
	struct {
		unsigned long ms:28;
		unsigned reserved:4;
		unsigned days:16;
		unsigned reserved2:16;
	};
} timeOfDay_t;
typedef timeOfDay_t TIME_OF_DAY;
typedef timeOfDay_t TIME_DIFFERENCE;

#endif /* ZELOS_CO_TIME_TYPES_H_ */
