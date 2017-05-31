/* Copyright (c) 2017, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#ifndef FILE_ENUMR_H
#define FILE_ENUMR_H

#define FILE_ENUMR_API_NAME "file-enumr"
#define FILE_ENUMR_API_VERSION 1
#define FILE_ENUMR_ENV_VAR "FILE_ENUMR_PATH"

int file_enumr_register(const char *path);

#endif
