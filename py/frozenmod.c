/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Paul Sokolovsky
 * Copyright (c) 2016 Damien P. George
 * Copyright (c) 2021 Jim Mussared
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>
#include <stdint.h>

#include "py/lexer.h"
#include "py/frozenmod.h"

#if MICROPY_MODULE_FROZEN

// Null-separated frozen file names. All string-type entries are listed first,
// followed by mpy-type entries. Use mp_frozen_str_sizes to determine how
// many string entries.
extern const char mp_frozen_names[];

#if MICROPY_MODULE_FROZEN_STR

#ifndef MICROPY_MODULE_FROZEN_LEXER
#define MICROPY_MODULE_FROZEN_LEXER mp_lexer_new_from_str_len
#else
mp_lexer_t *MICROPY_MODULE_FROZEN_LEXER(qstr src_name, const char *str, mp_uint_t len, mp_uint_t free_len);
#endif

// Size in bytes of each string entry, followed by a zero (terminator).
extern const uint32_t mp_frozen_str_sizes[];
// Null-separated string content.
extern const char mp_frozen_str_content[];
#endif // MICROPY_MODULE_FROZEN_STR

#if MICROPY_MODULE_FROZEN_MPY

#include "py/emitglue.h"

extern const mp_frozen_module_t *const mp_frozen_mpy_content[];

#if MICROPY_MODULE_FROZEN_MPY_COMPRESS_NAMES
extern const uint8_t mp_frozen_mpy_names[];
extern const uint16_t mp_frozen_mpy_name_block_offsets[];
extern const uint16_t mp_frozen_mpy_name_count;
extern const uint16_t mp_frozen_mpy_name_block_count;

#define MP_FROZEN_MPY_NAME_BLOCK_SIZE (16)

static const uint8_t *mp_frozen_mpy_name_decode(const uint8_t *encoded, char *name) {
    size_t prefix_len = *encoded++;
    size_t suffix_len = *encoded++;
    memcpy(name + prefix_len, encoded, suffix_len);
    name[prefix_len + suffix_len] = '\0';
    return encoded + suffix_len;
}
#endif

#endif // MICROPY_MODULE_FROZEN_MPY

// Search for "str" as a frozen entry, returning the stat result
// (no-exist/file/dir), as well as the type (none/str/mpy) and data.
// frozen_type can be NULL if its value isn't needed (and then data is assumed to be NULL).
mp_import_stat_t mp_find_frozen_module(const char *str, int *frozen_type, void **data) {
    size_t len = strlen(str);
    const char *name = mp_frozen_names;

    if (frozen_type != NULL) {
        *frozen_type = MP_FROZEN_NONE;
    }

    // Count the number of str lengths we have to find how many str entries.
    size_t num_str = 0;
    #if MICROPY_MODULE_FROZEN_STR
    for (const uint32_t *s = mp_frozen_str_sizes; *s != 0; ++s) {
        ++num_str;
    }
    #endif

    #if MICROPY_MODULE_FROZEN_MPY_COMPRESS_NAMES
    for (size_t i = 0; i < num_str; i++) {
    #else
    for (size_t i = 0; *name != 0; i++) {
    #endif
        size_t entry_len = strlen(name);
        if (entry_len >= len && memcmp(str, name, len) == 0) {
            // Query is a prefix of the current entry.
            if (entry_len == len) {
                // Exact match --> file.

                if (frozen_type != NULL) {
                    #if MICROPY_MODULE_FROZEN_STR
                    if (i < num_str) {
                        *frozen_type = MP_FROZEN_STR;
                        // Use the size table to figure out where this index starts.
                        size_t offset = 0;
                        for (size_t j = 0; j < i; ++j) {
                            offset += mp_frozen_str_sizes[j] + 1;
                        }
                        size_t content_len = mp_frozen_str_sizes[i];
                        const char *content = &mp_frozen_str_content[offset];

                        // Note: str & len have been updated by find_frozen_entry to strip
                        // the ".frozen/" prefix (to avoid this being a distinct qstr to
                        // the original path QSTR in frozen_content.c).
                        qstr source = qstr_from_strn(str, len);
                        mp_lexer_t *lex = MICROPY_MODULE_FROZEN_LEXER(source, content, content_len, 0);
                        *data = lex;
                    }
                    #endif

                    #if MICROPY_MODULE_FROZEN_MPY && !MICROPY_MODULE_FROZEN_MPY_COMPRESS_NAMES
                    if (i >= num_str) {
                        *frozen_type = MP_FROZEN_MPY;
                        // Load the corresponding index as a raw_code, taking
                        // into account any string entries to offset by.
                        *data = (void *)mp_frozen_mpy_content[i - num_str];
                    }
                    #endif
                }

                return MP_IMPORT_STAT_FILE;
            } else if (name[len] == '/') {
                // Matches up to directory separator, this is a valid
                // directory path.
                return MP_IMPORT_STAT_DIR;
            }
        }
        // Skip null separator.
        name += entry_len + 1;
    }

    #if MICROPY_MODULE_FROZEN_MPY && MICROPY_MODULE_FROZEN_MPY_COMPRESS_NAMES
    char decoded_name[MICROPY_ALLOC_PATH_MAX];
    size_t lower_block = 0;
    size_t upper_block = mp_frozen_mpy_name_block_count;

    while (lower_block < upper_block) {
        size_t mid = (lower_block + upper_block) / 2;
        const uint8_t *encoded = mp_frozen_mpy_names + mp_frozen_mpy_name_block_offsets[mid];
        mp_frozen_mpy_name_decode(encoded, decoded_name);
        if (strcmp(decoded_name, str) <= 0) {
            lower_block = mid + 1;
        } else {
            upper_block = mid;
        }
    }

    size_t first_block = lower_block == 0 ? 0 : lower_block - 1;
    for (size_t block = first_block; block < mp_frozen_mpy_name_block_count && block <= first_block + 1; block++) {
        const uint8_t *encoded = mp_frozen_mpy_names + mp_frozen_mpy_name_block_offsets[block];
        size_t first_index = block * MP_FROZEN_MPY_NAME_BLOCK_SIZE;
        size_t last_index = first_index + MP_FROZEN_MPY_NAME_BLOCK_SIZE;
        if (last_index > mp_frozen_mpy_name_count) {
            last_index = mp_frozen_mpy_name_count;
        }

        decoded_name[0] = '\0';
        for (size_t index = first_index; index < last_index; index++) {
            encoded = mp_frozen_mpy_name_decode(encoded, decoded_name);
            int comparison = strcmp(decoded_name, str);
            if (comparison < 0) {
                continue;
            }
            if (comparison == 0) {
                if (frozen_type != NULL) {
                    *frozen_type = MP_FROZEN_MPY;
                    *data = (void *)mp_frozen_mpy_content[index];
                }
                return MP_IMPORT_STAT_FILE;
            }
            if (decoded_name[len] == '/') {
                return MP_IMPORT_STAT_DIR;
            }
            return MP_IMPORT_STAT_NO_EXIST;
        }
    }
    #endif

    return MP_IMPORT_STAT_NO_EXIST;
}

#if MICROPY_MODULE_FROZEN_MPY_COMPRESS_NAMES
void mp_frozen_module_names_iterate(mp_frozen_module_name_callback_t callback, void *context) {
    const char *name = mp_frozen_names;
    while (*name != 0) {
        size_t len = strlen(name);
        callback(name, len, context);
        name += len + 1;
    }

    #if MICROPY_MODULE_FROZEN_MPY
    char decoded_name[MICROPY_ALLOC_PATH_MAX];
    const uint8_t *encoded = mp_frozen_mpy_names;
    decoded_name[0] = '\0';
    for (size_t index = 0; index < mp_frozen_mpy_name_count; index++) {
        if (index % MP_FROZEN_MPY_NAME_BLOCK_SIZE == 0) {
            decoded_name[0] = '\0';
        }
        encoded = mp_frozen_mpy_name_decode(encoded, decoded_name);
        callback(decoded_name, strlen(decoded_name), context);
    }
    #endif
}
#endif

#endif // MICROPY_MODULE_FROZEN
