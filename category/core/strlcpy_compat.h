// Copyright (C) 2025 Category Labs, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <stddef.h>
#include <string.h>

static inline size_t monad_strlcpy(char *dst, char const *src, size_t size)
{
    size_t const src_len = strlen(src);
    if (size != 0) {
        size_t const copy_len = src_len < (size - 1) ? src_len : (size - 1);
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }
    return src_len;
}
