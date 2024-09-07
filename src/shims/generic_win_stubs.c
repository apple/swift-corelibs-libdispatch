#include "internal.h"

typedef NTSTATUS (NTAPI *_NtQueryInformationFile_fn_t)(HANDLE FileHandle,
		PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
		FILE_INFORMATION_CLASS FileInformationClass);

DISPATCH_STATIC_GLOBAL(dispatch_once_t _dispatch_ntdll_pred);
DISPATCH_STATIC_GLOBAL(_NtQueryInformationFile_fn_t _dispatch_NtQueryInformationFile_ptr);

bool
_dispatch_handle_is_socket(HANDLE hFile)
{
	// GetFileType() returns FILE_TYPE_PIPE for both pipes and sockets. We can
	// disambiguate by checking if PeekNamedPipe() fails with
	// ERROR_INVALID_FUNCTION.
	if (GetFileType(hFile) == FILE_TYPE_PIPE &&
			!PeekNamedPipe(hFile, NULL, 0, NULL, NULL, NULL)) {
		return GetLastError() == ERROR_INVALID_FUNCTION;
	}
	return false;
}

static void
_dispatch_init_ntdll(void *context DISPATCH_UNUSED)
{
	HMODULE ntdll = LoadLibraryW(L"ntdll.dll");
	if (!ntdll) {
		// ntdll is not required.
		return;
	}
	_dispatch_NtQueryInformationFile_ptr = (_NtQueryInformationFile_fn_t)
			GetProcAddress(ntdll, "NtQueryInformationFile");
}

NTSTATUS _dispatch_NtQueryInformationFile(HANDLE FileHandle,
		PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
		FILE_INFORMATION_CLASS FileInformationClass)
{
	dispatch_once_f(&_dispatch_ntdll_pred, NULL, _dispatch_init_ntdll);
	if (!_dispatch_NtQueryInformationFile_ptr) {
		return STATUS_NOT_SUPPORTED;
	}
	return _dispatch_NtQueryInformationFile_ptr(FileHandle, IoStatusBlock,
			FileInformation, Length, FileInformationClass);
}
