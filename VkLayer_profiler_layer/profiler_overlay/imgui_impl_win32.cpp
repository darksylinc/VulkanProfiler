// Copyright (c) 2019-2021 Lukasz Stalmirski
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "imgui_impl_win32.h"
#include <backends/imgui_impl_win32.h>

#include "profiler/profiler_helpers.h"

// Use implementation provided by the ImGui
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND, UINT, WPARAM, LPARAM );

static ConcurrentMap<HWND, ImGui_ImplWin32_Context*> g_pWin32Contexts;
static HHOOK g_GetMessageHook;

/***********************************************************************************\

Function:
    ImGui_ImpWin32_Context

Description:
    Constructor.

\***********************************************************************************/
ImGui_ImplWin32_Context::ImGui_ImplWin32_Context( HWND hWnd )
    : m_AppWindow( hWnd )
{
    // Context is a kind of lock for processing - WindowProc will invoke ImGui implementation as long
    // as this context resides in the map
    g_pWin32Contexts.insert( m_AppWindow, this );

    if( !ImGui_ImplWin32_Init( m_AppWindow ) )
    {
        InitError();
    }

    if( !g_GetMessageHook )
    {
        HINSTANCE hProfilerDllInstance =
            static_cast<HINSTANCE>( Profiler::ProfilerPlatformFunctions::GetLibraryInstanceHandle() );

        // Register a global window hook on GetMessage/PeekMessage function.
        g_GetMessageHook = SetWindowsHookEx(
            WH_GETMESSAGE,
            ImGui_ImplWin32_Context::GetMessageHook,
            hProfilerDllInstance,
            0 /*dwThreadId*/ );

        if( !g_GetMessageHook )
        {
            // Failed to register hook on GetMessage.
            InitError();
        }
    }
}

/***********************************************************************************\

Function:
    ~ImGui_ImpWin32_Context

Description:
    Destructor.

\***********************************************************************************/
ImGui_ImplWin32_Context::~ImGui_ImplWin32_Context()
{
    // Erase context from map
    g_pWin32Contexts.remove( m_AppWindow );

    ImGui_ImplWin32_Shutdown();
}

/***********************************************************************************\

Function:
    GetName

Description:

\***********************************************************************************/
const char* ImGui_ImplWin32_Context::GetName() const
{
    return "Win32";
}

/***********************************************************************************\

Function:
    NewFrame

Description:

\***********************************************************************************/
void ImGui_ImplWin32_Context::NewFrame()
{
    ImGui_ImplWin32_NewFrame();
}

/***********************************************************************************\

Function:
    GetDPIScale

Description:

\***********************************************************************************/
float ImGui_ImplWin32_Context::GetDPIScale() const
{
    return ImGui_ImplWin32_GetDpiScaleForHwnd( m_AppWindow );
}

/***********************************************************************************\

Function:
    InitError

Description:
    Cleanup from partially initialized state

\***********************************************************************************/
void ImGui_ImplWin32_Context::InitError()
{
    ImGui_ImplWin32_Context::~ImGui_ImplWin32_Context();
    throw;
}

/***********************************************************************************\

Function:
    GetMessageHook

Description:

\***********************************************************************************/
LRESULT CALLBACK ImGui_ImplWin32_Context::GetMessageHook( int nCode, WPARAM wParam, LPARAM lParam )
{
    bool filterMessage = false;

    // MSDN: GetMsgHook procedure must process messages when (nCode == HC_ACTION)
    // https://docs.microsoft.com/en-us/previous-versions/windows/desktop/legacy/ms644981(v=vs.85)
    if( (nCode == HC_ACTION) || (nCode > 0) )
    {
        // Make local copy of MSG structure which will be passed to the application
        MSG msg = *reinterpret_cast<const MSG*>(lParam);

        if( msg.hwnd )
        {
            // Process message in ImGui
            ImGui_ImplWin32_Context* context = nullptr;

            if( g_pWin32Contexts.find( msg.hwnd, &context ) )
            {
                ImGuiIO& io = ImGui::GetIO();

                // Translate the message so that character input is handled correctly
                MSG translatedMsg = msg;
                TranslateMessage( &translatedMsg );

                // Capture mouse and keyboard events
                if( (IsMouseMessage( translatedMsg )) ||
                    (IsKeyboardMessage( translatedMsg )) )
                {
                    ImGui_ImplWin32_WndProcHandler( translatedMsg.hwnd, translatedMsg.message, translatedMsg.wParam, translatedMsg.lParam );

                    // Don't pass captured events to the application
                    filterMessage = io.WantCaptureMouse || io.WantCaptureKeyboard;
                }

                // Resize window
                if( msg.message == WM_SIZE )
                {
                    ImGui::GetIO().DisplaySize.x = LOWORD( msg.lParam );
                    ImGui::GetIO().DisplaySize.y = HIWORD( msg.lParam );
                }
            }
        }
    }

    // Invoke next hook in chain
    // Call this before modifying lParam (MSG) so that all hooks receive the same message
    const LRESULT result = CallNextHookEx( nullptr, nCode, wParam, lParam );

    if( filterMessage )
    {
        // Change message type to WM_NULL to ignore it in window procedure
        reinterpret_cast<MSG*>(lParam)->message = WM_NULL;
    }

    return result;
}

/***********************************************************************************\

Function:
    IsMouseMessage

Description:
    Checks if MSG describes mouse message.

\***********************************************************************************/
bool ImGui_ImplWin32_Context::IsMouseMessage( const MSG& msg )
{
    return (msg.message >= WM_MOUSEFIRST) && (msg.message <= WM_MOUSELAST);
}

/***********************************************************************************\

Function:
    IsKeyboardMessage

Description:
    Checks if MSG describes keyboard message.

\***********************************************************************************/
bool ImGui_ImplWin32_Context::IsKeyboardMessage( const MSG& msg )
{
    return (msg.message >= WM_KEYFIRST) && (msg.message <= WM_KEYLAST);
}
