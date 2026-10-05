// net/cefsub/main.cpp — CEF subprocess exe.
//
// CEF launches this (browser_subprocess_path) for renderer/gpu/zygote
// processes. It must run CefExecuteProcess with a null app and exit with the
// returned code. Keeping it separate means reLCS.exe and the launcher never
// act as subprocesses and never need CEF at build time.
#include "include/cef_app.h"
#include "include/cef_client.h"

#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
	CefMainArgs args(hInstance);
	return CefExecuteProcess(args, nullptr, nullptr);
}
