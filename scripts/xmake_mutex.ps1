[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory)][string]$ProjectPath,
    [Parameter(Mandatory)][string]$Log,
    [string]$Command = '',
    [string]$ArgsBase64 = '',
    [string]$Executable = 'xmake',
    [string[]]$CommandArgs = @(),
    [Parameter(ValueFromRemainingArguments)][string[]]$RemainingArgs = @()
)
function Format-Win32Argument([string]$Value) {
    if ($null -eq $Value -or $Value.Length -eq 0) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\*)$', '$1$1')
    return '"' + $escaped + '"'
}

$runnerSource = @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

public static class EngraveMutexRunner {
    public static int Run(string fileName, string workingDirectory, string arguments, string logPath) {
        var psi = new ProcessStartInfo {
            FileName = fileName,
            WorkingDirectory = workingDirectory,
            Arguments = arguments,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        var sync = new object();
        using (var writer = new StreamWriter(logPath, false, new UTF8Encoding(false)) { AutoFlush = true })
        using (var process = new Process { StartInfo = psi }) {
            process.OutputDataReceived += (sender, args) => {
                if (args.Data == null) return;
                lock (sync) {
                    Console.Out.WriteLine(args.Data);
                    writer.WriteLine(args.Data);
                }
            };
            process.ErrorDataReceived += (sender, args) => {
                if (args.Data == null) return;
                lock (sync) {
                    Console.Error.WriteLine(args.Data);
                    writer.WriteLine(args.Data);
                }
            };
            if (!process.Start()) {
                throw new InvalidOperationException("Failed to start process: " + fileName);
            }
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            writer.Flush();
            return process.ExitCode;
        }
    }
}
'@

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$logPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Log)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($logPath)) | Out-Null
# A single name also serializes builds of separate projects sharing xmake's cache.
$mutex = [System.Threading.Mutex]::new($false, 'Global\engrave-xmake-build')
$acquired = $false
$exitCode = 1
try {
    try { $acquired = $mutex.WaitOne() }
    catch [System.Threading.AbandonedMutexException] { $acquired = $true }
    Push-Location -LiteralPath $ProjectPath
    try {
        $resolvedExe = (Get-Command -Name $Executable -ErrorAction Stop | Select-Object -First 1).Source
        [string[]]$trailingArgs = @($CommandArgs) + @($RemainingArgs)
        [string]$argLine = $null
        if ($ArgsBase64) {
            if ($trailingArgs.Count -gt 0 -or $Command) {
                throw 'xmake_mutex.ps1: -ArgsBase64 cannot be combined with -Command or trailing command arguments'
            }
            $jsonText = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($ArgsBase64))
            $parsed = ConvertFrom-Json -InputObject $jsonText
            [string[]]$argv = @($parsed | ForEach-Object { if ($null -eq $_) { '' } else { [string]$_ } })
            $argLine = (@($argv | ForEach-Object { Format-Win32Argument $_ }) -join ' ')
        } elseif ($trailingArgs.Count -gt 0) {
            if ($Command) {
                throw 'xmake_mutex.ps1: -Command cannot be combined with trailing command arguments'
            }
            $argLine = (@($trailingArgs | ForEach-Object { Format-Win32Argument $_ }) -join ' ')
        } elseif ($Command) {
            $argLine = $Command
        } else {
            throw 'xmake_mutex.ps1 requires -ArgsBase64, -Command, or trailing command arguments'
        }

        if (-not ('EngraveMutexRunner' -as [type])) {
            Add-Type -TypeDefinition $runnerSource -Language CSharp
        }
        $resolvedDir = (Get-Location -PSProvider FileSystem).ProviderPath
        $exitCode = [EngraveMutexRunner]::Run($resolvedExe, $resolvedDir, $argLine, $logPath)
    } finally { Pop-Location }
} finally {
    if ($acquired) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
exit $exitCode
