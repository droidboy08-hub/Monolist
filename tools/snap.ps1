param([string]$View = 'downloads', [int]$Wait = 9, [string]$Out = '', [string[]]$Extra = @())
$safe = $View -replace '[^a-zA-Z0-9_-]', '-'
if (-not $Out) { $Out = "$PSScriptRoot\snap-$safe.png" }
# Launches the app on one view, captures its window, and closes it again.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[Win]::SetProcessDPIAware() | Out-Null
$env:QT_FORCE_STDERR_LOGGING = '1'
$log = "$PSScriptRoot\snap-$safe.log"
$p = Start-Process -FilePath 'C:\dev\monolist-build\debug\monolist.exe' -ArgumentList (@('--view', $View) + $Extra) -PassThru -RedirectStandardError $log -WindowStyle Normal
Start-Sleep -Seconds $Wait
$p.Refresh()
$h = $p.MainWindowHandle
if ($p.HasExited -or -not $h -or $h -eq [IntPtr]::Zero) {
  "no window: the app exited or never showed one"
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  "--- log ---"; Get-Content -LiteralPath $log | Select-Object -First 25
  exit 1
}
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700
$r = New-Object Win+RECT; [Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc(); $ok = [Win]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc)   # 2 = PW_RENDERFULLCONTENT (GPU surfaces)
if (-not $ok) { $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size) }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force
"captured ${w}x${hgt} -> $Out"
"--- QML / app warnings ---"
Get-Content -LiteralPath $log | Where-Object { $_ -notmatch '^selftest' } | Select-Object -First 25
