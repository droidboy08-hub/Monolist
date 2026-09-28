param(
    [string]$View = 'home',
    [int]$Wait = 8,
    # "rclick:x,y;click:x,y;move:x,y;wait:ms;key:ESC" with x,y relative to the window
    [string]$Actions = '',
    [string]$Out = '',
    [string[]]$Extra = @(),
    [switch]$Screen,        # capture the screen area (for popups that are their own windows)
    [int]$Settle = 400,     # pause before capturing; 0 to catch an animation mid-flight
    [int]$AfterClick = 350  # pause after each click
)
# Launches the app on one view, drives the mouse, captures the window, closes it.
$safe = $View -replace '[^a-zA-Z0-9_-]', '-'
if (-not $Out) { $Out = "$PSScriptRoot\click-$safe.png" }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class WinC {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
"@
[WinC]::SetProcessDPIAware() | Out-Null
$env:QT_FORCE_STDERR_LOGGING = '1'
$log = "$PSScriptRoot\click-$safe.log"
$p = Start-Process -FilePath 'C:\dev\monolist-build\debug\monolist.exe' -ArgumentList (@('--view', $View) + $Extra) -PassThru -RedirectStandardError $log -WindowStyle Normal
Start-Sleep -Seconds $Wait
$p.Refresh()
$h = $p.MainWindowHandle
if ($p.HasExited -or -not $h -or $h -eq [IntPtr]::Zero) {
  "no window"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  Get-Content -LiteralPath $log | Select-Object -First 25; exit 1
}
# Windows only lets the foreground process hand focus over, so attach to the
# window's input queue first — otherwise clicks land but keystrokes do not.
function Focus-App {
  $target = [WinC]::GetWindowThreadProcessId($h, [IntPtr]::Zero)
  $mine = [WinC]::GetCurrentThreadId()
  [WinC]::AttachThreadInput($mine, $target, $true) | Out-Null
  [WinC]::BringWindowToTop($h) | Out-Null
  [WinC]::SetForegroundWindow($h) | Out-Null
  [WinC]::AttachThreadInput($mine, $target, $false) | Out-Null
}
Focus-App
Start-Sleep -Milliseconds 500
if ([WinC]::GetForegroundWindow() -ne $h) { "warning: the app is not the foreground window; keystrokes may go elsewhere" }
$r = New-Object WinC+RECT; [WinC]::GetWindowRect($h, [ref]$r) | Out-Null
foreach ($step in ($Actions -split ';' | Where-Object { $_ })) {
  $kind, $arg = $step -split ':', 2
  switch ($kind) {
    'wait'   { Start-Sleep -Milliseconds ([int]$arg) }
    'size'   { $sw, $sh = $arg -split ','
               [WinC]::MoveWindow($h, $r.Left, $r.Top, [int]$sw, [int]$sh, $true) | Out-Null
               Start-Sleep -Milliseconds 800
               [WinC]::GetWindowRect($h, [ref]$r) | Out-Null }
    # scroll:x,y,notches — negative notches scroll down, as a wheel does.
    'scroll' { $sx, $sy, $notches = $arg -split ','
               [WinC]::SetCursorPos($r.Left + [int]$sx, $r.Top + [int]$sy) | Out-Null
               Start-Sleep -Milliseconds 120
               # mouse_event takes the wheel delta as an unsigned 32-bit word,
               # so a scroll down has to be written as its two's complement.
               $delta = [int]$notches * 120
               $word = [uint32]([int64]$delta -band 0xFFFFFFFFL)
               [WinC]::mouse_event(0x0800, 0, 0, $word, [UIntPtr]::Zero)
               Start-Sleep -Milliseconds 500 }
    'key'    { $vk = @{ 'ESC' = 0x1B; 'ENTER' = 0x0D; 'DOWN' = 0x28; 'RIGHT' = 0x27 }[$arg]
               [WinC]::keybd_event([byte]$vk, 0, 0, [UIntPtr]::Zero); [WinC]::keybd_event([byte]$vk, 0, 2, [UIntPtr]::Zero) }
    'type'   { Focus-App
               Add-Type -AssemblyName System.Windows.Forms
               [System.Windows.Forms.SendKeys]::SendWait($arg) }
    default  {
      $x, $y = $arg -split ','
      [WinC]::SetCursorPos($r.Left + [int]$x, $r.Top + [int]$y) | Out-Null
      Start-Sleep -Milliseconds 120
      if ($kind -eq 'click')  { [WinC]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero); [WinC]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero) }
      if ($kind -eq 'rclick') { [WinC]::mouse_event(0x8, 0, 0, 0, [UIntPtr]::Zero); [WinC]::mouse_event(0x10, 0, 0, 0, [UIntPtr]::Zero) }
      Start-Sleep -Milliseconds $AfterClick
    }
  }
}
if ($Settle -gt 0) { Start-Sleep -Milliseconds $Settle }
$w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
if ($Screen) { $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size) }
else { $hdc = $g.GetHdc(); [WinC]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc) }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force
"captured ${w}x${hgt} -> $Out"
Get-Content -LiteralPath $log | Where-Object { $_ -notmatch '^selftest' } | Select-Object -First 25
