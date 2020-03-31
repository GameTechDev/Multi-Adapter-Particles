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

#include <Windows.h>

//=============================================================================
// return time in seconds
//=============================================================================
class Timer
{
public:
    void   Start();
	double Stop() const { return GetTime(); }
	double GetTime() const;

    Timer();

private:
    LARGE_INTEGER m_startTime;
    LARGE_INTEGER m_performanceFrequency;
    double m_oneOverTicksPerSecond;
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline Timer::Timer()
{
    m_startTime.QuadPart = 0;
    QueryPerformanceFrequency(&m_performanceFrequency);
    m_oneOverTicksPerSecond = 1. / (double)m_performanceFrequency.QuadPart;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void Timer::Start()
{
    QueryPerformanceCounter(&m_startTime);
}

//-----------------------------------------------------------------------------
// not intended to be executed within inner loop
//-----------------------------------------------------------------------------
inline double Timer::GetTime() const
{
    LARGE_INTEGER endTime;
    QueryPerformanceCounter(&endTime);

    LONGLONG s = m_startTime.QuadPart;
    LONGLONG e = endTime.QuadPart;
    return double(e-s) * m_oneOverTicksPerSecond;
}
