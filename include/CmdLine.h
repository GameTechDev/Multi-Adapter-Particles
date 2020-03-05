/* ************************************************************************* *\
INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or
nondisclosure agreement with Intel Corporation and may not be copied
or disclosed except in accordance with the terms of that agreement.
Copyright (C) 2012 Intel Corporation. All Rights Reserved.
\* ************************************************************************* */

/*
The supported argument types are (these types are for ease-of-use):

    ArgInt
    ArgString
    ArgBool

to add more types, add another ArgTraits<> specialization
*/

#ifndef CMDLINE_H
#define CMDLINE_H

#include <vector>
#include <exception>
#include <string>
#include <iostream>

namespace gca
{
    // Argument flags.
    enum ARG_FLAG
    {
        AF_HIDDEN = 1 << 0,		// Don't show argument in help.
        AF_GREEDY = 1 << 1,		// Argument is greedy (parsed last, with other greedy args).
        AF_REQUIRED = 1 << 2,	// Argument is required to be parsed.
    };

    class CmdLine;

    class arg_exception : public std::exception
    {
    protected:
        const char * _what;

    public:
        arg_exception(const char * what) : _what(what) { }

        const char * what() const throw()
        {
            return _what;
        }
    };

    typedef std::vector < const char * > arg_list;

    //-------------------------------------------------------------------------
    // Command line argument base.
    class Argument
    {
    protected:
        const char * _desc;
        int _flags;

        Argument(CmdLine & cmdline, const char* typeString, const char * desc, int flags = 0);
    public:

        // Parse this argument.
        virtual void Parse(arg_list & args) { }

        // Notification that parsing is complete and successful.
        virtual void OnParsed() { }

        // Print arg usage.
        virtual void PrintUsage() = 0;
        // Print arg help.
        void PrintDesc() { std::cout << _desc; }

        // Get flags for this argument.
        int Flags() const { return _flags; }
        // Check hidden flag.
        bool IsHidden() const { return (_flags & AF_HIDDEN) != 0; }
        // Parse will eat any unswitched argument.
        bool IsGreedy() const { return (_flags & AF_GREEDY) != 0; }
        // Check required flag.
        bool IsRequired() const { return (_flags & AF_REQUIRED) != 0; }
    };

    // only used/tested with char and const char *
    template <typename T>
    inline std::string ConcatArgDash(T c) { std::string t = "-"; t += c; return t; }

    //-------------------------------------------------------------------------
    // Simple bool switch. Default is false, if the switch is present, value = true.
    class SwitchArg : public Argument
    {
    protected:
        std::string _sw;

    public:
        bool on;

        operator bool() const { return on; }
        template <typename T> operator const T & () const { return on; }

        SwitchArg(CmdLine & cmdline, char sw, const char * desc) : Argument(cmdline, "", desc), _sw(ConcatArgDash(sw)), on(false) { }
        SwitchArg(CmdLine & cmdline, char sw, bool def, const char * desc) : Argument(cmdline, "", desc), _sw(ConcatArgDash(sw)), on(def) { }

        SwitchArg(CmdLine & cmdline, const char* sw, const char * desc) : Argument(cmdline, "", desc), _sw(ConcatArgDash(sw)), on(false) { }
        SwitchArg(CmdLine & cmdline, const char* sw, bool def, const char * desc) : Argument(cmdline, "", desc), _sw(ConcatArgDash(sw)), on(def) { }

        // For switch args, if any of the arguments is -X* where X is _sw, the switch is flipped (true->false or false->true)
        void Parse(arg_list & args)
        {
            for (arg_list::iterator i = args.begin(); i != args.end();)
            {
                const char * arg = *i;
                if (_sw == arg)
                {
                    on = !on;
                    i = args.erase(i);
                }
                else
                {
                    ++i;
                }
            }
        }

        void PrintUsage()
        {
            std::cout << "[" << _sw << "]";
        }
    };

    //-------------------------------------------------------------------------
    // Command line parser.
    class CmdLine
    {
    protected:
        const char * _path;
        const char * _desc;
        const char * _version;
        SwitchArg* _help;
        std::vector < Argument * > _args;

    public:
        CmdLine(const char * desc, const char * version = "") : _desc(desc), _version(version)
        {
            // can't have SwitchArg as a member of the class using : _help(...) , because
            // apparently sometimes the std::vector isn't quite ready in time to call the constructor
            // work around: create _help with new()
            _help = new SwitchArg(*this, '?', "Show the usage for this application.");
        }
        ~CmdLine() { delete _help; }

        // Add argument to the command line options.
        void AddArgument(Argument * arg) { _args.push_back(arg); }

        // Parse command line. If parsing fails, display help and return error code.
        void Parse(int argc, const char *argv[])
        {
            // note: the arg list always has at least one member, "-?"
            try
            {
                _path = argv[0];

                arg_list args;
                for (int i = 1; i < argc; ++i)
                    args.push_back(argv[i]);

                // Parse non-greedy.
                for (std::vector < Argument * >::iterator i = _args.begin(); i != _args.end(); ++i)
                    if (!(*i)->IsGreedy())
                        (*i)->Parse(args);

                // Parse greedy.
                for (std::vector < Argument * >::iterator i = _args.begin(); i != _args.end(); ++i)
                    if ((*i)->IsGreedy())
                        (*i)->Parse(args);

                // Notify parsing is complete.
                for (std::vector < Argument * >::iterator i = _args.begin(); i != _args.end(); ++i)
                    (*i)->OnParsed();

                if (!args.empty())
                {
                    std::cout << "Warning: Unparsed arguments:\n";
                    for (arg_list::iterator i = args.begin(); i != args.end(); ++i)
                    {
                        std::cout << "\t" << *i << std::endl;
                    }
                }
            }
            catch (...)
            {
                Usage();
                throw;
            }
            if (_help->on)
            {
                Usage();
                exit(0);
            }
        }

        // Print usage for the command line.
        void Usage()
        {
            printf("%s %s\n\n", _desc, _version);

            printf("Usage: %s ", _path);
            for (std::vector < Argument * >::iterator i = _args.begin(); i != _args.end(); ++i)
            {
                if ((*i)->IsHidden())
                    continue;

                (*i)->PrintUsage();
                printf(" ");
            }

            printf("\n\n");

            for (std::vector < Argument * >::iterator i = _args.begin(); i != _args.end(); ++i)
            {
                if ((*i)->IsHidden())
                    continue;

                printf("\t");
                (*i)->PrintUsage();
                printf(": ");
                (*i)->PrintDesc();
                printf("\n");
            }
            printf("\n");
        }
    };

    //-------------------------------------------------------------------------
    // Arg type traits. Needs 3 functions:
    // - T Parse(const char *): Return value of type T given a string. Throws exception if invalid string.
    // - void Print(T): Print an instance of T.
    // - DefaultTypeString: return printable string of type, e.g. "int"
    template < typename T >
    struct ArgTraits : public Argument
    {
    protected:
        ArgTraits(CmdLine & cmdline, const char * desc, int flags = 0) :
            Argument(cmdline, DefaultTypeString(), desc, flags) {}

        T Parse(const char * arg);
        void Print(T t) { std::cout << t; }
        const char * DefaultTypeString();
    };

    // specializations
    template <> inline const char * ArgTraits<const char *>::Parse(const char * arg) { return arg; }
    template <> inline const char * ArgTraits<const char *>::DefaultTypeString() { return "string"; }
    template <> inline void ArgTraits<const char *>::Print(const char * t) { if (t) std::cout << t; }

    template <> inline int ArgTraits<int>::Parse(const char * arg) { return atoi(arg); } // TODO: Validation...
    template <> inline const char * ArgTraits<int>::DefaultTypeString() { return "int"; }

    //-------------------------------------------------------------------------
    // An argument with a value of type T.
    template < typename T >
    class ValueArg : public ArgTraits<T>
    {
    protected:
        std::string _sw;

        T _def;

    public:
        T value;

        operator const T & () const { return value; }

        ValueArg(CmdLine & cmdline, char sw, const char * desc, int flags = 0) : ArgTraits(cmdline, desc, flags | AF_REQUIRED), _sw(ConcatArgDash(sw)) { }
        ValueArg(CmdLine & cmdline, char sw, T def, const char * desc, int flags = 0) : ArgTraits(cmdline, desc, flags), _sw(ConcatArgDash(sw)), _def(def), value(def) { }

        ValueArg(CmdLine & cmdline, const char* sw, const char * desc, int flags = 0) : ArgTraits(cmdline, desc, flags | AF_REQUIRED), _sw(ConcatArgDash(sw)) { }
        ValueArg(CmdLine & cmdline, const char* sw, T def, const char * desc, int flags = 0) : ArgTraits(cmdline, desc, flags), _sw(ConcatArgDash(sw)), _def(def), value(def) { }

        void Parse(arg_list & args)
        {
            for (arg_list::iterator i = args.begin(); i != args.end();)
            {
                const char * arg = *i;

                if (_sw == arg)
                {
                    i = args.erase(i);
                    if (i != args.end())
                    {
                        const char * v = *i;
                        i = args.erase(i);
                        value = ArgTraits::Parse(v);
                        return;
                    }
                    else
                    {
                        throw arg_exception("Expected argument.");
                    }
                }
                else
                {
                    ++i;
                }
            }
            if (IsRequired())
                throw arg_exception("Missing required argument.");
        }

        void PrintUsage()
        {
            if (IsRequired())
            {
                printf("%s <%s>", _sw.c_str(), DefaultTypeString());
            }
            else
            {
                // Show default value if not required.
                printf("[%s <%s>=", _sw.c_str(), DefaultTypeString());
                Print(_def);
                printf("]");
            }
        }
    };

    //-------------------------------------------------------------------------
    template < typename T >
    class UnnamedArgs : public ArgTraits<T>
    {
    protected:
        int _min;

    public:
        std::vector < T > values;

        int size() const { return values.size(); }
        const T & operator [] (size_t i) const { return values[i]; }
        typename std::vector < T >::const_iterator begin() const { return values.begin(); }
        typename std::vector < T >::const_iterator end() const { return values.end(); }

        UnnamedArgs(CmdLine & cmdline, const char * desc, int min = 0, int flags = 0) : ArgTraits(cmdline, desc, flags | AF_GREEDY), _min(min) { }

        void Parse(arg_list & args)
        {
            // Parse all the arguments without a '-'.
            values.clear();
            for (arg_list::iterator i = args.begin(); i != args.end();)
            {
                const char * arg = *i;
                if (arg[0] != '-')
                {
                    values.push_back(Tr::Parse(arg));
                    i = args.erase(i);
                }
                else
                {
                    ++i;
                }
            }
            if ((int)values.size() < _min)
                throw arg_exception("Not enough arguments for command.");
        }

        void PrintUsage()
        {
            for (int i = 0; i < _min; ++i)
                printf("<%s>%i ", DefaultTypeString(), i + 1);

            printf("[<%s>%i ...]", DefaultTypeString(), _min + 1);
        }
    };

    //-------------------------------------------------------------------------
    template < typename T >
    class UnnamedArg : public ArgTraits<T>
    {
    protected:
        T _def;

    public:
        T value;

        operator const T & () const { return value; }

        UnnamedArg(CmdLine & cmdline, const char * desc, int flags = 0) : ArgTraits(cmdline, desc, flags | AF_REQUIRED | AF_GREEDY) { }
        UnnamedArg(CmdLine & cmdline, const T & def, const char * desc, int flags = 0) : ArgTraits(cmdline, desc, flags | AF_GREEDY), _def(def), value(def) { }

        void Parse(arg_list & args)
        {
            for (arg_list::iterator i = args.begin(); i != args.end();)
            {
                const char * arg = *i;

                if (arg[0] != '-')
                {
                    i = args.erase(i);
                    value = ArgTraits::Parse(arg);
                    return;
                }
                else
                {
                    ++i;
                }
            }
            if (IsRequired())
                throw arg_exception("Missing required unnamed argument.");
        }

        void PrintUsage()
        {
            if (IsRequired())
            {
                printf("<%s>", DefaultTypeString());
            }
            else
            {
                // Show default value if not required.
                printf("<%s>=", DefaultTypeString());
                Tr::Print(_def);
            }
        }
    };

    //-------------------------------------------------------------------------
    inline Argument::Argument(CmdLine & cmdline, const char* typeString, const char * desc, int flags) :
        _desc(desc), _flags(flags)
    {
        cmdline.AddArgument(this);
    }

    typedef ValueArg<int> ArgInt;
    typedef ValueArg<const char*> ArgString;
    typedef SwitchArg ArgBool;

    //const std::string& ArgString::operator () { return (const char*)value; }
}
#endif
