# Run one command line on a private desktop that is never shown, wait for it and return its exit code.
# The whole process tree (test runner, browser, WebView2 children) inherits that desktop: no window appears on the
# user's screen or taskbar and nothing can take the user's keyboard focus. Meant for the browser GUI tests
# (tests\*.ps1, tab-drag-test, app-window-test) when they are run on a working machine.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-on-hidden-desktop.ps1 -OutputFile out.txt `
#     -CommandLine 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\session-guard-test.ps1 -Exe C:\path\jisu-browser.exe'
#
# -OutputFile collects stdout+stderr of the command (the hidden process has no console) and prints it at the end.
# A command that is still running after -TimeoutSeconds is terminated and the exit code is 124.
param(
  [Parameter(Mandatory)][string]$CommandLine,
  [int]$TimeoutSeconds = 300,
  [string]$OutputFile
)
$ErrorActionPreference = 'Stop'
$compiler = New-Object CodeDom.Compiler.CompilerParameters
$compiler.GenerateInMemory = $true
$compiler.TempFiles = New-Object CodeDom.Compiler.TempFileCollection([IO.Path]::GetTempPath(), $false)
Add-Type -CompilerParameters $compiler -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class JisuHiddenDesktop {
  [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr devmode, int flags, uint access, IntPtr sa);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool CloseDesktop(IntPtr desktop);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct STARTUPINFO {
    public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                           IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] public static extern uint WaitForSingleObject(IntPtr handle, uint ms);
  [DllImport("kernel32.dll")] public static extern bool GetExitCodeProcess(IntPtr handle, out uint code);
  [DllImport("kernel32.dll")] public static extern bool TerminateProcess(IntPtr handle, uint code);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@
if ($OutputFile) {
  $OutputFile = [IO.Path]::GetFullPath($OutputFile)
  Remove-Item -LiteralPath $OutputFile -ErrorAction SilentlyContinue
  $CommandLine = 'cmd.exe /d /c "' + $CommandLine + ' > "' + $OutputFile + '" 2>&1"'
}
$name = 'jisu-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$desktop = [JisuHiddenDesktop]::CreateDesktopW($name, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
if ($desktop -eq [IntPtr]::Zero) { throw "CreateDesktop failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
$code = 1
try {
  $si = New-Object JisuHiddenDesktop+STARTUPINFO
  $si.cb = [Runtime.InteropServices.Marshal]::SizeOf([type][JisuHiddenDesktop+STARTUPINFO])
  $si.lpDesktop = "WinSta0\$name"
  $si.dwFlags = 1      # STARTF_USESHOWWINDOW
  $si.wShowWindow = 0  # SW_HIDE
  $pi = New-Object JisuHiddenDesktop+PROCESS_INFORMATION
  $command = New-Object Text.StringBuilder $CommandLine
  # CREATE_NO_WINDOW (0x08000000): no console window either. [NullString]::Value: PowerShell would pass "" for $null.
  if (-not [JisuHiddenDesktop]::CreateProcessW([NullString]::Value, $command, [IntPtr]::Zero, [IntPtr]::Zero, $false, 0x08000000,
                                                [IntPtr]::Zero, [NullString]::Value, [ref]$si, [ref]$pi)) {
    throw "CreateProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
  }
  if ([JisuHiddenDesktop]::WaitForSingleObject($pi.hProcess, [uint32]($TimeoutSeconds * 1000)) -ne 0) {
    [JisuHiddenDesktop]::TerminateProcess($pi.hProcess, 1) | Out-Null
    $code = 124
    "timeout after $TimeoutSeconds s"
  } else {
    [uint32]$exit = 0
    [JisuHiddenDesktop]::GetExitCodeProcess($pi.hProcess, [ref]$exit) | Out-Null
    $code = [BitConverter]::ToInt32([BitConverter]::GetBytes($exit), 0)  # NTSTATUS-style codes (0xC0000005) do not fit [int] casts
  }
  [JisuHiddenDesktop]::CloseHandle($pi.hThread) | Out-Null
  [JisuHiddenDesktop]::CloseHandle($pi.hProcess) | Out-Null
} finally {
  [JisuHiddenDesktop]::CloseDesktop($desktop) | Out-Null
}
if ($OutputFile -and (Test-Path -LiteralPath $OutputFile)) { Get-Content -LiteralPath $OutputFile }
"exit=$code"
exit $code
