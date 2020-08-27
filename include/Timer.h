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
    Timer();
    void   Start();
	double Stop() const { return GetTime(); }
	double GetTime() const;

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
    ::QueryPerformanceFrequency(&m_performanceFrequency);
    m_oneOverTicksPerSecond = 1. / (double)m_performanceFrequency.QuadPart;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void Timer::Start()
{
    ::QueryPerformanceCounter(&m_startTime);
}

//-----------------------------------------------------------------------------
// not intended to be executed within inner loop
//-----------------------------------------------------------------------------
inline double Timer::GetTime() const
{
    LARGE_INTEGER endTime;
    ::QueryPerformanceCounter(&endTime);

    const LONGLONG s = m_startTime.QuadPart;
    const LONGLONG e = endTime.QuadPart;
    return double(e-s) * m_oneOverTicksPerSecond;
}

/*======================================================
Usage:
Either: call once every loop with Update(), e.g. for average frametime
Or: call in pairs Start()...Update() to average a region
======================================================*/
class TimerAverageOver
{
public:
    TimerAverageOver(UINT in_numFrames = 30, UINT in_everyN = 1) :
        m_averageIndex(0), m_sum(0), m_skipEvery(in_everyN)
    {
        m_values.resize(in_numFrames, 0);
        m_timer.Start();
        m_previousTime = 0;
    }

    void Start()
    {
        // assert no skipping. doesn't make sense for this usage
        m_skipEvery = 1;
        m_previousTime = (float)m_timer.GetTime();
    }

    void Update()
    {
        m_skipCount++;
        if (m_skipEvery == m_skipCount)
        {
            m_skipCount = 0;

            float t = (float)m_timer.GetTime();
            float delta = t - m_previousTime;
            m_previousTime = t;

            m_averageIndex = (m_averageIndex + 1) % m_values.size();
            m_sum -= m_values[m_averageIndex];
            m_sum += delta;
            m_values[m_averageIndex] = delta;
        }
    }

    float Get()
    {
        return (m_sum / (float(m_values.size()) * m_skipEvery));
    }
private:
    TimerAverageOver(const TimerAverageOver&) = delete;
    TimerAverageOver(TimerAverageOver&&) = delete;
    TimerAverageOver& operator=(const TimerAverageOver&) = delete;
    TimerAverageOver& operator=(TimerAverageOver&&) = delete;

    Timer m_timer;
    UINT m_averageIndex;
    float m_sum;
    std::vector<float> m_values;

    UINT m_skipEvery;
    UINT m_skipCount;

    float m_previousTime;
};
