/*	$OpenBSD: strlcpy.c,v/strlcat.c,v 1.16 2019/01/25 00:19:25 millert Exp $	*/

/*
 * Copyright (c) 1998, 2015 Todd C. Miller <millert@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <ctype.h>
#include <string.h>
#include <sys/types.h>

size_t strlcat(char *dst, const char *src, size_t size)
{
	size_t n = size, dlen;
	const char *odst = dst;
	const char *osrc = src;

	while (n-- != 0 && *dst != '\0')
		dst++;
	dlen = dst - odst;
	n = size - dlen;

	if (n-- == 0)
		return dlen + strlen(src);
	while (*src != '\0') {
		if (n != 0) {
			*dst++ = *src;
			n--;
		}
		src++;
	}
	*dst = '\0';

	return dlen + (src - osrc);
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t n = size;
	const char *osrc = src;

	if (n != 0)
		while (--n != 0)
			if ((*dst++ = *src++) == '\0')
				break;
	if (n == 0) {
		if (size != 0)
			*dst = '\0';
		while (*src++);
	}
	return src - osrc - 1;
}

int strqetok(char **src, char *dst, size_t size)
{
	size_t n = 0;
    int q, sq = 0;
    char *s, *head, *tail;

    while (**src && (isspace(**src) || **src == '='))
		(*src)++;

    if ((q = **src == '"' || (sq = **src == '\''))) {
        head = *src + 1;
        if (!(tail = strchr(head, sq ? '\'' : '"')))
			return 0;
		if (!sq)
			while (*(tail - 1) == '\\')
				tail = strchr(tail + 1, '"');
    } else {
        head = *src;
        tail = strpbrk(*src, " =\t\n");
    }

    s = head;
    while (n + 1 < size && tail ? s < tail : *s)
        if (q && !sq && *s == '\\' && *(s + 1) == '"')
            s++;
        else {
            n++;
            *dst++ = *s++;
        }
    *dst = '\0';
	*src = tail ? ++tail : '\0';

    return n || q;
}
