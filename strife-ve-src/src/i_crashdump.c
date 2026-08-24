//
// DESCRIPTION:
//   Diagnostic crash handler. Installs an unhandled-exception filter that
//   writes a minidump (crash.dmp) and a symbolized stack trace
//   (crashlog.txt) next to the executable before the process terminates,
//   using dbghelp.dll (shipped with every Windows install - no extra
//   tooling required to capture a crash for later analysis).
//

#include "config.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "dbghelp.lib")

static void WriteMiniDump(EXCEPTION_POINTERS *ep)
{
    HANDLE dumpFile = CreateFileA("crash.dmp", GENERIC_WRITE, 0, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dumpFile == INVALID_HANDLE_VALUE)
        return;

    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile,
                           MiniDumpWithIndirectlyReferencedMemory, &mei, NULL, NULL);
    }

    CloseHandle(dumpFile);
}

static void WriteStackTrace(EXCEPTION_POINTERS *ep, HANDLE process, HANDLE thread)
{
    FILE *log = fopen("crashlog.txt", "w");
    CONTEXT context;
    STACKFRAME64 frame;
    DWORD machine;
    int i;

    if (log == NULL)
        return;

    fprintf(log, "Exception code: 0x%08lX at address %p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        fprintf(log, "  %s violation at address 0x%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "Write" : "Read",
                (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    fprintf(log, "\nStack trace (faulting thread):\n");

    context = *ep->ContextRecord;
    memset(&frame, 0, sizeof(frame));

#if defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = context.Eip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrStack.Mode   = AddrModeFlat;
#elif defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = context.Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;
#else
    machine = 0;
#endif

    for (i = 0; i < 64; i++)
    {
        BYTE symbolBuffer[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbolBuffer;
        DWORD64 displacement64 = 0;
        DWORD displacement = 0;
        IMAGEHLP_LINE64 line;

        if (!StackWalk64(machine, process, thread, &frame, &context, NULL,
                          SymFunctionTableAccess64, SymGetModuleBase64, NULL))
        {
            break;
        }

        if (frame.AddrPC.Offset == 0)
            break;

        memset(symbolBuffer, 0, sizeof(symbolBuffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen   = 255;

        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement64, symbol))
        {
            memset(&line, 0, sizeof(line));
            line.SizeOfStruct = sizeof(line);
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &displacement, &line))
            {
                fprintf(log, "  %s (%s:%lu)\n", symbol->Name, line.FileName, line.LineNumber);
            }
            else
            {
                fprintf(log, "  %s\n", symbol->Name);
            }
        }
        else
        {
            fprintf(log, "  0x%p (no symbol)\n", (void *)(UINT_PTR)frame.AddrPC.Offset);
        }
    }

    fclose(log);
}

static LONG WINAPI I_CrashHandler(EXCEPTION_POINTERS *ep)
{
    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);

    WriteMiniDump(ep);
    WriteStackTrace(ep, process, thread);

    SymCleanup(process);

    return EXCEPTION_EXECUTE_HANDLER;
}

void I_InstallCrashHandler(void)
{
    SetUnhandledExceptionFilter(I_CrashHandler);
}

#else

void I_InstallCrashHandler(void)
{
}

#endif
