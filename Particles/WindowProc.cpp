//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************

#include "WindowProc.h"


InputState WindowProc::m_inputState;

//--------------------------------------------------------------------------------------
// Called every time the application receives a message
//--------------------------------------------------------------------------------------
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WindowProc::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
    {
        return true;
    }

    switch (message)
    {
    case WM_SETFOCUS:
        m_inputState.m_hasFocus = true;
        break;
    case WM_KILLFOCUS:
        m_inputState.m_hasFocus = false;
        break;

    case WM_KEYDOWN:
        if (m_inputState.m_hasFocus)
        {
            const uint8_t keyDown = static_cast<uint8_t>(wParam);
            bool* pDrawEnabled = (bool*)::GetWindowLongPtr(hWnd, GWLP_USERDATA);

            // if previous state was key up
            if (0 == (lParam & 1 << 30))
            {
                m_inputState.m_keyPress = keyDown;
            }


            if (keyDown == VK_ESCAPE)
            {
                ::PostQuitMessage(0);
            }
            else
            {
                switch (keyDown)
                {
                case VK_UP:
                    m_inputState.m_keyDown.forward = 1;
                    break;
                case VK_DOWN:
                    m_inputState.m_keyDown.back = 1;
                    break;
                case VK_LEFT:
                    m_inputState.m_keyDown.left = 1;
                    break;
                case VK_RIGHT:
                    m_inputState.m_keyDown.right = 1;
                    break;
                case VK_SPACE:
                    *pDrawEnabled = !*pDrawEnabled;
                    break;
                }

            } // end if not quit
        }
        break;

    case WM_KEYUP:
        if (m_inputState.m_hasFocus)
        {
            const uint8_t keyDown = static_cast<uint8_t>(wParam);
            m_inputState.m_keyPress = 0;

            if (VK_UP == keyDown) m_inputState.m_keyDown.forward = 0;
            if (VK_DOWN == keyDown) m_inputState.m_keyDown.back = 0;
            if (VK_LEFT == keyDown) m_inputState.m_keyDown.left = 0;
            if (VK_RIGHT == keyDown) m_inputState.m_keyDown.right = 0;
        }
        break;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        break;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (m_inputState.m_hasFocus)
        {
            const int32_t x = int32_t(lParam & 0x0000ffff);
            const int32_t y = int32_t(lParam) >> 16;
            m_inputState.m_mousePos.Set(x, y);
        }
        break;

    case WM_MOUSEMOVE:
    {
        if (m_inputState.m_hasFocus)
        {
            const int32_t x = int32_t(lParam & 0x0000ffff);
            const int32_t y = int32_t(lParam) >> 16;
            InputState::Vector2i pos;
            pos.Set(x, y);

            if (wParam & MK_LBUTTON)
            {
                m_inputState.m_mouseLeftDelta.x += pos.x - m_inputState.m_mousePos.x;
                m_inputState.m_mouseLeftDelta.y += pos.y - m_inputState.m_mousePos.y;
            }
            if (wParam & MK_RBUTTON)
            {
                m_inputState.m_mouseRightDelta.x += pos.x - m_inputState.m_mousePos.x;
                m_inputState.m_mouseRightDelta.y += pos.y - m_inputState.m_mousePos.y;
            }
            m_inputState.m_mousePos = pos;
        }
    }
    break;

    default:
        // Handle any other messages
        return ::DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}
