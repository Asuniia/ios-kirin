/*
 * What ir3asm needs from the parts of Mesa it does not build (driver state,
 * OS helpers, c11 threads, the printer).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "c11/threads.h"
#include "isa.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"

enum ir3_shader_debug ir3_shader_debug;

/* ir3_shader.c, without the driver-side constant data and params. */
void *
ir3_shader_assemble(struct ir3_shader_variant *v)
{
	struct ir3_info *info = &v->info;
	ir3_collect_info(v);
	info->size = align(info->size, v->compiler->instr_align * sizeof(uint64_t));
	void *bin = isa_assemble(v);
	v->constlen = align(v->constlen, 4);
	info->constlen = v->constlen;
	return bin;
}

void
ir3_print(struct ir3 *ir)
{
	(void)ir;
}

void
ir3_assert_handler(const char *expr, const char *file, int line, const char *func)
{
	fprintf(stderr, "%s:%d %s: assertion %s failed\n", file, line, func, expr);
	abort();
}

int
mtx_init(mtx_t *mtx, int type)
{
	(void)mtx;
	(void)type;
	return thrd_success;
}

void
call_once(once_flag *flag, void (*func)(void))
{
	static int done;
	(void)flag;
	if (!done) {
		done = 1;
		func();
	}
}

const char *
util_get_process_name(void)
{
	return "ir3asm";
}

size_t
u_printf_length(const char *fmt, va_list untouched_args)
{
	va_list args;
	va_copy(args, untouched_args);
	int n = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	return n < 0 ? 0 : (size_t)n;
}

void
os_log_message(const char *message)
{
	fputs(message, stderr);
}

const char *
os_get_option(const char *name)
{
	return getenv(name);
}

const char *
os_get_option_cached(const char *name)
{
	return getenv(name);
}
