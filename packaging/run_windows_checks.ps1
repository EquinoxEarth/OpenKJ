param([string]$BuildDirectory = 'build-windows-dev')
$ErrorActionPreference = 'Stop'

# Children inherit this mode: a missing DLL must fail the test, not wait for a
# desktop error dialog on the hosted runner.
Add-Type @'
using System.Runtime.InteropServices;
public static class OpenKJErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint mode);
}
'@
[void][OpenKJErrorMode]::SetErrorMode(0x8003)
& ctest --test-dir $BuildDirectory --output-on-failure
$result = $LASTEXITCODE
$songEditLog = Join-Path $BuildDirectory 'song-edit-results.txt'
if ($result -ne 0 -and (Test-Path $songEditLog)) {
    Get-Content $songEditLog
}
exit $result
