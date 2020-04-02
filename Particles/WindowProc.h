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

#pragma once
//=======================================================================================
// WNDPROC windows procedure for CreateWindow()
//=======================================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <cstdint>

struct InputState
{
    struct Vector2i
    {
        std::int32_t x, y;
        Vector2i() { x = 0; y = 0; }
        void Set(int32_t in_x, int32_t in_y) { x = in_x; y = in_y; }
    };

    bool m_hasFocus;

    struct
    {
        uint32_t forward : 1;
        uint32_t back : 1;
        uint32_t left : 1;
        uint32_t right : 1;
        uint32_t rotxl : 1;
        uint32_t rotxr : 1;
        uint32_t rotyl : 1;
        uint32_t rotyr : 1;
    } m_keyDown;

    uint32_t m_keyPress; // record other keypresses for debugging

    bool m_mouseDown;
    Vector2i m_mouseLeftDelta;
    Vector2i m_mouseRightDelta;
    Vector2i m_mousePos;

    InputState() : m_hasFocus(false), m_keyDown({ 0 }), m_keyPress(0), m_mouseDown(false)
    {
    }
};

//-----------------------------------------------------------------------------
// key presses -> translation to render loop
//-----------------------------------------------------------------------------
class WindowProc
{
public:
    //--------------------------------------------------------------------------------------
    // Called every time the application receives a message
    //--------------------------------------------------------------------------------------
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    static InputState m_inputState;
};
