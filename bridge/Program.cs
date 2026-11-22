using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Management;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

internal sealed class BrightnessOverlay : Form
{
    private const int WsExTransparent = 0x20;
    private const int WsExToolWindow = 0x80;
    private const int WsExLayered = 0x80000;
    private const uint IoctlSetNativeBrightness = 0x80006024;
    private const int ExpectedPanelMaximum = 2047;
    private readonly System.Windows.Forms.Timer _timer = new System.Windows.Forms.Timer();
    private readonly string _logPath;
    private int? _lastApplied;
    private int? _lastFallback;
    private SafeFileHandle _driverHandle;
    private DateTime _nextDriverRetry = DateTime.MinValue;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr security, uint creation, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle handle, uint code,
        byte[] input, uint inputLength, byte[] output, uint outputLength,
        out uint returned, IntPtr overlapped);

    public BrightnessOverlay()
    {
        string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LillipupBrightnessBridge");
        Directory.CreateDirectory(folder);
        _logPath = Path.Combine(folder, "bridge.log");
        FormBorderStyle = FormBorderStyle.None;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.Manual;
        BackColor = Color.Black;
        TopMost = true;
        Opacity = 0.0;
        Bounds = Screen.PrimaryScreen.Bounds;
        _timer.Interval = 250;
        _timer.Tick += delegate { Poll(); };
        SystemEvents.DisplaySettingsChanged += delegate { Bounds = Screen.PrimaryScreen.Bounds; };
        Shown += delegate { Poll(); _timer.Start(); };
        Log("Brightness bridge started; native eDP AUX control will be used when available; overlay remains as fallback.");
    }

    protected override CreateParams CreateParams
    {
        get
        {
            CreateParams cp = base.CreateParams;
            cp.ExStyle |= WsExTransparent | WsExToolWindow | WsExLayered;
            return cp;
        }
    }

    private void Poll()
    {
        try
        {
            int? value = ReadBrightness();
            if (value.HasValue && value.Value != _lastApplied)
            {
                uint panelLevel;
                string error;
                if (TrySetNativeBrightness(value.Value, out panelLevel, out error))
                {
                    ApplyNative();
                    Log("Applied Windows brightness " + value.Value + "% through eDP AUX; panel readback=" + panelLevel + "/" + ExpectedPanelMaximum + ".");
                    _lastApplied = value.Value;
                    _lastFallback = null;
                }
                else
                {
                    ApplyOverlay(value.Value);
                    if (!_lastFallback.HasValue || _lastFallback.Value != value.Value)
                        Log("Native eDP AUX unavailable (" + error + "); applied overlay fallback at Windows brightness " + value.Value + "% and will retry.");
                    _lastFallback = value.Value;
                    _lastApplied = null;
                }
            }
        }
        catch (Exception ex) { Log("Poll failed: " + ex.Message); }
    }

    private int? ReadBrightness()
    {
        using (ManagementObjectSearcher searcher = new ManagementObjectSearcher("root\\WMI", "SELECT CurrentBrightness FROM WmiMonitorBrightness"))
        using (ManagementObjectCollection results = searcher.Get())
        {
            foreach (ManagementObject item in results)
            {
                object value = item["CurrentBrightness"];
                if (value != null) return Math.Max(0, Math.Min(100, Convert.ToInt32(value)));
            }
        }
        return null;
    }

    private bool TrySetNativeBrightness(int percent, out uint panelLevel, out string error)
    {
        panelLevel = 0;
        error = "unknown error";
        try
        {
            if (_driverHandle == null || _driverHandle.IsInvalid || _driverHandle.IsClosed)
                EnsureDriverReady();
            if (_driverHandle == null || _driverHandle.IsInvalid)
            {
                error = "LillipupAuxProbe device is unavailable";
                return false;
            }

            byte[] input = BitConverter.GetBytes((uint)percent);
            byte[] output = new byte[28];
            uint returned;
            if (!DeviceIoControl(_driverHandle, IoctlSetNativeBrightness, input, (uint)input.Length,
                output, (uint)output.Length, out returned, IntPtr.Zero))
            {
                error = "DeviceIoControl failed with Win32 " + Marshal.GetLastWin32Error();
                _driverHandle.Dispose();
                _driverHandle = null;
                return false;
            }

            uint status = BitConverter.ToUInt32(output, 0);
            uint readbackStatus = BitConverter.ToUInt32(output, 16);
            panelLevel = BitConverter.ToUInt32(output, 20);
            if (status != 0 || readbackStatus != 0)
            {
                error = "driver status=0x" + status.ToString("X8") + ", readback=0x" + readbackStatus.ToString("X8");
                return false;
            }

            uint expected = (uint)((percent * ExpectedPanelMaximum + 50) / 100);
            if (Math.Abs((long)panelLevel - expected) > 1)
            {
                error = "panel readback " + panelLevel + " differs from expected " + expected;
                return false;
            }
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    private void EnsureDriverReady()
    {
        if (_driverHandle != null && !_driverHandle.IsInvalid && !_driverHandle.IsClosed) return;
        if (_driverHandle != null) _driverHandle.Dispose();
        _driverHandle = null;
        if (DateTime.Now >= _nextDriverRetry)
        {
            _nextDriverRetry = DateTime.Now.AddSeconds(5);
            ProcessStartInfo start = new ProcessStartInfo();
            start.FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32\\sc.exe");
            start.Arguments = "start LillipupAuxProbe";
            start.CreateNoWindow = true;
            start.UseShellExecute = false;
            using (Process process = Process.Start(start)) process.WaitForExit(10000);
            Thread.Sleep(500);
        }
        _driverHandle = CreateFile("\\\\.\\LillipupAuxProbe", 0xC0000000, 3,
            IntPtr.Zero, 3, 0, IntPtr.Zero);
    }

    private void ApplyNative()
    {
        Opacity = 0.0;
        if (Visible) Hide();
    }

    private void ApplyOverlay(int percent)
    {
        percent = Math.Max(0, Math.Min(100, percent));
        Opacity = (100 - percent) / 100.0;
        if (Opacity == 0.0) Hide(); else if (!Visible) Show();
        if (Visible) { TopMost = true; Bounds = Screen.PrimaryScreen.Bounds; Invalidate(); }
    }

    private void Log(string message)
    {
        try { File.AppendAllText(_logPath, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + " " + message + Environment.NewLine); }
        catch { }
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        if (_driverHandle != null && !_driverHandle.IsInvalid) _driverHandle.Dispose();
        base.OnFormClosed(e);
    }

    [STAThread]
    private static void Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new BrightnessOverlay());
    }
}

