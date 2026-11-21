// Exported by the Windows-only `platform_lookup_test` DLL. The engine's
// `lookupSymbol` must find it after `LoadLibrary`, not only in the process
// image, kernel32 or the CRT.
#ifdef _WIN32
#define OSKEL_TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define OSKEL_TEST_EXPORT extern "C"
#endif

OSKEL_TEST_EXPORT int oskelLoadedDllSymbol() { return 4242; }
