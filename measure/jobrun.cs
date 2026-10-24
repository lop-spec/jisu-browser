// jobrun：把一个程序及其全部子孙进程放进同一个 Job Object 运行，按需读出整棵树累计的 CPU 时间
// （已经退出的子进程也算在内）。标准输入逐行接收命令：
//   mark <标签>  → 标准输出打印一行 JSON：{"label":..,"ms":距启动毫秒,"userMs":..,"kernelMs":..,"active":..,"total":..,"idleMs":..}
//   quit         → 结束整棵进程树并退出
// 用法：jobrun.exe <程序> [参数...]      （含空格的参数会重新加引号）
//       jobrun.exe --screen              打印虚拟屏幕左上角与当前键鼠空闲毫秒后退出
// 编译（.NET Framework 自带 csc）：csc /nologo /target:exe /platform:x64 /out:jobrun.exe jobrun.cs
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

internal static class JobRun
{
    [StructLayout(LayoutKind.Sequential)]
    private struct STARTUPINFO
    {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_ACCOUNTING_INFORMATION
    {
        public long TotalUserTime, TotalKernelTime, ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime;
        public uint TotalPageFaultCount, TotalProcesses, ActiveProcesses, TotalTerminatedProcesses;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS { public ulong a, b, c, d, e, f; }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation; public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int length);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool QueryInformationJobObject(IntPtr job, int infoClass, out JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info, int length, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CreateProcess(string app, StringBuilder commandLine, IntPtr pa, IntPtr ta, bool inherit,
                                             uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")]
    private static extern uint ResumeThread(IntPtr thread);
    [DllImport("user32.dll")]
    private static extern bool GetLastInputInfo(ref LASTINPUTINFO info);
    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);

    private const uint CREATE_SUSPENDED = 0x4;
    private const int STARTF_USESHOWWINDOW = 0x1;
    private const short SW_SHOWMINNOACTIVE = 7;

    private static long IdleMs()
    {
        var info = new LASTINPUTINFO();
        info.cbSize = (uint)Marshal.SizeOf(info);
        GetLastInputInfo(ref info);
        return (long)(uint)(Environment.TickCount - (int)info.dwTime);
    }

    // 含空格或引号的参数重新加引号（参数解析时引号已被去掉）
    private static string Quote(string arg)
    {
        if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return arg;
        return "\"" + arg.Replace("\"", "\\\"") + "\"";
    }

    private static int Main(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("usage: jobrun <exe> [args...] | jobrun --screen"); return 64; }
        if (args[0] == "--screen")
        {
            Console.WriteLine("{\"left\":" + GetSystemMetrics(76) + ",\"top\":" + GetSystemMetrics(77) + ",\"idleMs\":" + IdleMs() + "}");
            return 0;
        }
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        limits.BasicLimitInformation.LimitFlags = 0x2000;  // KILL_ON_JOB_CLOSE：jobrun 退出时整棵树一起结束
        if (!SetInformationJobObject(job, 9, ref limits, Marshal.SizeOf(limits))) throw new Win32Exception();

        var commandLine = new StringBuilder(Quote(args[0]));
        for (int i = 1; i < args.Length; i++) commandLine.Append(' ').Append(Quote(args[i]));
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMINNOACTIVE;  // 不抢焦点；程序自己可以再定位置
        PROCESS_INFORMATION pi;
        if (!CreateProcess(null, commandLine, IntPtr.Zero, IntPtr.Zero, false, CREATE_SUSPENDED, IntPtr.Zero, null, ref si, out pi))
            throw new Win32Exception();
        if (!AssignProcessToJobObject(job, pi.hProcess)) throw new Win32Exception();
        var clock = Stopwatch.StartNew();
        ResumeThread(pi.hThread);
        Console.WriteLine("{\"label\":\"started\",\"pid\":" + pi.dwProcessId + "}");
        Console.Out.Flush();

        string line;
        while ((line = Console.ReadLine()) != null)
        {
            line = line.Trim();
            if (line == "quit") break;
            if (!line.StartsWith("mark")) continue;
            string label = line.Length > 5 ? line.Substring(5).Replace("\\", "/").Replace("\"", "'") : "";
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
            QueryInformationJobObject(job, 1, out info, Marshal.SizeOf(typeof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION)), IntPtr.Zero);
            Console.WriteLine("{\"label\":\"" + label + "\",\"ms\":" + clock.ElapsedMilliseconds +
                              ",\"userMs\":" + info.TotalUserTime / 10000.0 + ",\"kernelMs\":" + info.TotalKernelTime / 10000.0 +
                              ",\"active\":" + info.ActiveProcesses + ",\"total\":" + info.TotalProcesses +
                              ",\"idleMs\":" + IdleMs() + "}");
            Console.Out.Flush();
        }
        TerminateJobObject(job, 0);
        return 0;
    }
}
