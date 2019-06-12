#include "internal.h"

typedef void (WINAPI *_precise_time_fn_t)(PULONGLONG);

DISPATCH_STATIC_GLOBAL(dispatch_once_t _dispatch_precise_time_pred);
DISPATCH_STATIC_GLOBAL(_precise_time_fn_t _dispatch_QueryInterruptTimePrecise_ptr);
DISPATCH_STATIC_GLOBAL(_precise_time_fn_t _dispatch_QueryUnbiasedInterruptTimePrecise_ptr);

static void
_dispatch_init_precise_time(void *context DISPATCH_UNUSED)
{
	HMODULE kernelbase = LoadLibraryW(L"KernelBase.dll");
	if (!kernelbase) {
		DISPATCH_INTERNAL_CRASH(0, "failed to load KernelBase.dll");
	}
	_dispatch_QueryInterruptTimePrecise_ptr = (_precise_time_fn_t)
			GetProcAddress(kernelbase, "QueryInterruptTimePrecise");
	_dispatch_QueryUnbiasedInterruptTimePrecise_ptr = (_precise_time_fn_t)
			GetProcAddress(kernelbase, "QueryUnbiasedInterruptTimePrecise");
	if (!_dispatch_QueryInterruptTimePrecise_ptr) {
		DISPATCH_INTERNAL_CRASH(0, "could not locate QueryInterruptTimePrecise");
	}
	if (!_dispatch_QueryUnbiasedInterruptTimePrecise_ptr) {
		DISPATCH_INTERNAL_CRASH(0, "could not locate QueryUnbiasedInterruptTimePrecise");
	}
}

void
_dispatch_QueryInterruptTimePrecise(PULONGLONG lpInterruptTimePrecise)
{
	dispatch_once_f(&_dispatch_precise_time_pred, NULL, _dispatch_init_precise_time);
	return _dispatch_QueryInterruptTimePrecise_ptr(lpInterruptTimePrecise);
}

void
_dispatch_QueryUnbiasedInterruptTimePrecise(PULONGLONG lpUnbiasedInterruptTimePrecise)
{
	dispatch_once_f(&_dispatch_precise_time_pred, NULL, _dispatch_init_precise_time);
	return _dispatch_QueryUnbiasedInterruptTimePrecise_ptr(lpUnbiasedInterruptTimePrecise);
}
