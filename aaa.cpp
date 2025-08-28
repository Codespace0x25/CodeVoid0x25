// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Save module handle for use when installing the hook from our thread
        g_hModule = hModule;
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

#include <Windows.h>
#include <queue>
#include <mutex>
#include <atomic>

// Structure to hold key event data
struct KeyEvent {
    WPARAM vkCode;
    bool isKeyDown;
};

std::queue<KeyEvent> keyEventQueue;
std::mutex queueMutex;

// NOTE: the low-level keyboard hook's KBDLLHOOKSTRUCT flags do NOT include a repeat flag.
// The original code had a define for LLKHF_REPEAT; we'll keep the define to preserve
// the original content but we do NOT rely on it for repeat detection.
#define LLKHF_REPEAT 0x40000000

// Forward declarations for hook thread
static DWORD WINAPI HookThreadProc(LPVOID lpParameter);
static HHOOK g_hook = NULL;
static HANDLE g_hookThread = NULL;
static DWORD g_hookThreadId = 0;
static HMODULE g_hModule = NULL; // saved in DllMain

// Keyboard hook procedure
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* pKbdStruct = (KBDLLHOOKSTRUCT*)lParam;
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        // We cannot rely on a "repeat" flag in the low-level hook.
        // Implement per-key state tracking to avoid repeated keydown events.
        static bool keyState[256] = { false };
        BYTE vk = static_cast<BYTE>(pKbdStruct->vkCode & 0xFF);

        if (isKeyDown)
        {
            // only enqueue if we previously believe the key was up
            if (!keyState[vk])
            {
                keyState[vk] = true;
                std::lock_guard<std::mutex> lock(queueMutex);
                keyEventQueue.push({ pKbdStruct->vkCode, true });
            }
        }
        else if (isKeyUp)
        {
            // enqueue key up always (if it was down)
            if (keyState[vk])
            {
                keyState[vk] = false;
                std::lock_guard<std::mutex> lock(queueMutex);
                keyEventQueue.push({ pKbdStruct->vkCode, false });
            }
            else
            {
                // It's also possible to get an up without a tracked down (edge cases).
                // We can still push it to keep callers informed, if desired:
                std::lock_guard<std::mutex> lock(queueMutex);
                keyEventQueue.push({ pKbdStruct->vkCode, false });
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Hook thread: installs the low-level hook and runs a message loop so hook callbacks are delivered.
static DWORD WINAPI HookThreadProc(LPVOID lpParameter)
{
    // Install hook using our DLL's module handle to be correct if needed
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hModule, 0);
    if (g_hook == NULL)
    {
        // Failed to set hook; nothing more we can do on this thread.
        return GetLastError();
    }

    // Message loop — required for WH_KEYBOARD_LL callbacks on this thread
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Unhook on thread exit (in case StopKeyboardHook didn't unhook already)
    if (g_hook)
    {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }

    return 0;
}

extern "C"
{
    __declspec(dllexport) void StartKeyboardHook()
    {
        // If already running, don't create another thread
        if (g_hookThread != NULL)
            return;

        // Create a dedicated thread that sets the hook and runs a message loop.
        g_hookThread = CreateThread(
            NULL,
            0,
            HookThreadProc,
            NULL,
            0,
            &g_hookThreadId
        );

        // Note: CreateThread may fail; callers can't get an error code via this interface.
        // If you want detailed errors, change API to return success/failure.
    }

    __declspec(dllexport) void StopKeyboardHook()
    {
        // Signal the hook thread to exit by posting a WM_QUIT to its message queue.
        if (g_hookThread != NULL && g_hookThreadId != 0)
        {
            PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);

            // Wait for the thread to exit and clean up handles.
            DWORD wait = WaitForSingleObject(g_hookThread, 3000); // 3s wait
            if (wait == WAIT_TIMEOUT)
            {
                // force cleanup: attempt to unhook and close handles
                if (g_hook)
                {
                    UnhookWindowsHookEx(g_hook);
                    g_hook = NULL;
                }
                // Still try to close handle
            }

            CloseHandle(g_hookThread);
            g_hookThread = NULL;
            g_hookThreadId = 0;
        }
        else
        {
            // If the thread handle wasn't set but hook exists (edge case), try to unhook
            if (g_hook)
            {
                UnhookWindowsHookEx(g_hook);
                g_hook = NULL;
            }
        }
    }

    __declspec(dllexport) bool GetNextKeyEvent(WPARAM* outVkCode, bool* outIsKeyDown)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!keyEventQueue.empty())
        {
            KeyEvent event = keyEventQueue.front();
            keyEventQueue.pop();
            *outVkCode = event.vkCode;
            *outIsKeyDown = event.isKeyDown;
            return true;
        }
        return false;
    }
}