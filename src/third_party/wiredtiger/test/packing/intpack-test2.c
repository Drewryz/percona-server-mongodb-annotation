/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>

#include "wt_internal.h"			/* For __wt_XXX */

int
main(void)
{
	uint8_t buf[10], *p, *end;
	int64_t i;

	for (i = 1; i < 1LL << 60; i <<= 1) {
		end = buf;
		assert(__wt_vpack_uint(&end, sizeof(buf), (uint64_t)i) == 0);
		printf("%" PRId64 " ", i);
		for (p = buf; p < end; p++)
			printf("%02x", *p);
		printf("\n");

		end = buf;
		assert(__wt_vpack_int(&end, sizeof(buf), -i) == 0);
		printf("%" PRId64 " ", -i);
		for (p = buf; p < end; p++)
			printf("%02x", *p);
		printf("\n");
	}

	return (0);
}
