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

/*-----------------------------------------------------------------------------
ArgParser

Parse arguments to a Windows application
On finding a match, calls custom code
Case is ignored while parsing

example:
This creates the parser, then searches for a few values.
The value is expected to follow the token.

runprogram.exe gRaVity 20.27 upIsDown dothing

float m_float = 0;
bool m_flipGravity = false;

ArgParser argParser;
argParser.AddArg(L"gravity", m_float);
argParser.AddArg(L"upisdown", m_flipGravity); // inverts current value
argParser.AddArg(L"dothing", [=](std::wstring) { DoTheThing(); } );
argParser.Parse();

-----------------------------------------------------------------------------*/
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers.
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <shellapi.h>
#include <sstream>

//-----------------------------------------------------------------------------
// parse command line
//-----------------------------------------------------------------------------
class ArgParser
{
public:
    void AddArg(std::wstring s, std::function<void(std::wstring)> f) { m_args.push_back(ArgPair(s, f)); }

    template<typename T> void AddArg(std::wstring s, T& out_value) = delete;

    void Parse();
private:
    class ArgPair
    {
    public:
        ArgPair(std::wstring s, std::function<void(std::wstring)> f) : m_arg(s), m_func(f)
        {
            for (auto& c : m_arg) { c = ::towlower(c); }
        }
        void TestEqual(std::wstring in_arg, const WCHAR* in_value)
        {
            for (auto& c : in_arg) { c = ::towlower(c); }
            if (m_arg == in_arg)
            {
                m_func(in_value);
            }
        }
        const std::wstring& Get() { return m_arg; }
    private:
        std::wstring m_arg;
        std::function<void(std::wstring)> m_func;
    };

    std::vector < ArgPair > m_args;
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void ArgParser::Parse()
{
    int numArgs = 0;
    LPWSTR* cmdLine = CommandLineToArgvW(GetCommandLineW(), &numArgs);

    if ((2 == numArgs) && (std::wstring(L"?") == cmdLine[1]))
    {
        std::wstringstream stream;
        for (auto& arg : m_args)
        {
            stream << arg.Get() << std::endl;
        }
        MessageBox(0, stream.str().c_str(), L"Command Line Args", MB_OK);
    }

    for (int i = 0; i < numArgs; i++)
    {
        for (auto& arg : m_args)
        {
            arg.TestEqual(cmdLine[i], (i < numArgs - 1) ? cmdLine[i + 1] : L"");
        }
    }
}

template<> inline void ArgParser::AddArg(std::wstring arg, long& out_value) { AddArg(arg, [&](std::wstring s) { out_value = std::stol(s); }); }
template<> inline void ArgParser::AddArg(std::wstring arg, UINT& out_value) { AddArg(arg, [&](std::wstring s) { out_value = std::stoul(s); }); }
template<> inline void ArgParser::AddArg(std::wstring arg, int& out_value) { AddArg(arg, [&](std::wstring s) { out_value = std::stoi(s); }); }
template<> inline void ArgParser::AddArg(std::wstring arg, float& out_value) { AddArg(arg, [&](std::wstring s) { out_value = std::stof(s); }); }
template<> inline void ArgParser::AddArg(std::wstring arg, bool& out_value) { AddArg(arg, [&](std::wstring s) { out_value = !out_value; }); }
