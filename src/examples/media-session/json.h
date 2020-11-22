/* Simple Plugin API
 *
 * Copyright © 2020 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef SPA_UTILS_JSON_H
#define SPA_UTILS_JSON_H

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>


struct spa_json {
	const char *cur;
	const char *end;
	struct spa_json *parent;
	uint32_t state;
	uint32_t depth;
};

#define SPA_JSON_INIT(data,size) (struct spa_json) { (data), (data)+(size), }

static inline void spa_json_init(struct spa_json * iter, const char *data, size_t size)
{
	*iter =  SPA_JSON_INIT(data, size);
}
#define SPA_JSON_ENTER(iter) (struct spa_json) { (iter)->cur, (iter)->end, (iter), }

static inline void spa_json_enter(struct spa_json * iter, struct spa_json * sub)
{
	*sub = SPA_JSON_ENTER(iter);
}

static inline int spa_json_next(struct spa_json * iter, const char **value)
{
	int utf8_remain = 0;
	enum { __NONE, __STRUCT, __BARE, __STRING, __UTF8, __ESC };

	for (; iter->cur < iter->end; iter->cur++) {
		unsigned char cur = (unsigned char)*iter->cur;
 again:
		switch (iter->state) {
		case __NONE:
			iter->state = __STRUCT;
			iter->depth = 0;
			goto again;
		case __STRUCT:
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n': case ':': case ',':
				continue;
			case '"':
				*value = iter->cur;
				iter->state = __STRING;
				continue;
			case '[': case '{':
				*value = iter->cur;
				if (++iter->depth > 1)
					continue;
				iter->cur++;
				return 1;
			case '}': case ']':
				if (iter->depth == 0) {
					if (iter->parent)
						iter->parent->cur = iter->cur;
					return 0;
				}
				--iter->depth;
				continue;
			case '-': case 'a' ... 'z': case 'A' ... 'Z': case '0' ... '9':
				*value = iter->cur;
				iter->state = __BARE;
				continue;
			}
			return -1;
		case __BARE:
			switch (cur) {
			case '\t': case ' ': case '\r': case '\n':
			case ':': case ',': case ']': case '}':
				iter->state = __STRUCT;
				if (iter->depth > 0)
					goto again;
				return iter->cur - *value;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
			}
			return -1;
		case __STRING:
			switch (cur) {
			case '\\':
				iter->state = __ESC;
				continue;
			case '"':
				iter->state = __STRUCT;
				if (iter->depth > 0)
					continue;
				iter->cur++;
				return iter->cur - *value;
			case 240 ... 247:
				utf8_remain++;
				SPA_FALLTHROUGH;
			case 224 ... 239:
				utf8_remain++;
				SPA_FALLTHROUGH;
			case 192 ... 223:
				utf8_remain++;
				iter->state = __UTF8;
				continue;
			default:
				if (cur >= 32 && cur <= 126)
					continue;
			}
			return -1;
		case __UTF8:
			switch (cur) {
			case 128 ... 191:
				if (--utf8_remain == 0)
					iter->state = __STRING;
				continue;
			}
			return -1;
		case __ESC:
			switch (cur) {
			case '"': case '\\': case '/': case 'b': case 'f':
			case 'n': case 'r': case 't': case 'u':
				iter->state = __STRING;
				continue;
			}
			return -1;
		}
	}
	return iter->depth == 0 ? 0 : -1;
}

static inline int spa_json_enter_container(struct spa_json *iter, struct spa_json *sub, char type)
{
	const char *value;
	if (spa_json_next(iter, &value) < 0 || *value != type)
		return -1;
	spa_json_enter(iter, sub);
	return 1;
}

static inline int spa_json_enter_object(struct spa_json *iter, struct spa_json *sub)
{
	return spa_json_enter_container(iter, sub, '{');
}

static inline int spa_json_enter_array(struct spa_json *iter, struct spa_json *sub)
{
	return spa_json_enter_container(iter, sub, '[');
}

static inline int spa_json_is_object(const char *val, int len)
{
	return len > 0 && *val == '{';
}

static inline bool spa_json_is_array(const char *val, int len)
{
	return len > 0 && *val == '[';
}

static inline bool spa_json_is_float(const char *val, int len)
{
	char *end;
	strtof(val, &end);
	return end == val + len;
}

static inline bool spa_json_is_string(const char *val, int len)
{
	return len > 1 && *val == '"';
}

static inline bool spa_json_is_null(const char *val, int len)
{
	return len == 4 && strcmp(val, "null") == 0;
}

static inline bool spa_json_is_true(const char *val, int len)
{
	return len == 4 && strcmp(val, "true") == 0;
}

static inline bool spa_json_is_false(const char *val, int len)
{
	return len == 5 && strcmp(val, "false") == 0;
}

static inline bool spa_json_is_bool(const char *val, int len)
{
	return spa_json_is_true(val, len) || spa_json_is_false(val, len);
}

static inline int spa_json_parse_float(const char *val, int len, float *result)
{
	char *end;
	*result = strtof(val, &end);
	return end == val + len;
}
static inline int spa_json_parse_bool(const char *val, int len, bool *result)
{
	if ((*result = spa_json_is_true(val, len)))
		return 1;
	if (!(*result = !spa_json_is_false(val, len)))
		return 1;
	return -1;
}

static inline int spa_json_parse_string(const char *val, int len, char *result)
{
	const char *p;
	if (!spa_json_is_string(val, len))
		return -1;
	for (p = val+1; p < val + len-1; p++) {
		if (*p == '\\') {
			p++;
			if (*p == 'n')
				*result++ = '\n';
			else if (*p == 'r')
				*result++ = '\r';
			else if (*p == 'b')
				*result++ = '\b';
			else if (*p == 't')
				*result++ = '\t';
			else
				*result++ = *p;
		} else
			*result++ = *p;
	}
	*result++ = '\0';
	return 1;
}

static inline int spa_json_get_float(struct spa_json *iter, float *res)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0)
		return -1;
	return spa_json_parse_float(value, len, res);
}

static inline int spa_json_get_bool(struct spa_json *iter, bool *res)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0)
		return -1;
	return spa_json_parse_bool(value, len, res);
}

static inline int spa_json_get_string(struct spa_json *iter, char *res, int maxlen)
{
	const char *value;
	int len;
	if ((len = spa_json_next(iter, &value)) <= 0 || maxlen < len)
		return -1;
	return spa_json_parse_string(value, len, res);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_UTILS_JSON_H */
