// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using SDRSharp.Common;
using SDRSharp.Radio;

namespace SDRSharp.BirriaSDR;

internal static unsafe class Native
{
    const string Dll = "birriasdr";
    static Native()
    {
        string directory=Path.Combine(Path.GetDirectoryName(typeof(Native).Assembly.Location)!,"birriasdr");
        string usb=Path.Combine(directory,"birriasdr-usb-1.0.dll");
        // Load the uniquely named dependency from its absolute path. SDR# may
        // already have an older libusb-1.0.dll loaded for another receiver.
        if (File.Exists(usb)) NativeLibrary.Load(usb);
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, assembly, path) =>
            name == Dll ? NativeLibrary.Load(Path.Combine(directory,"birriasdr.dll")) : IntPtr.Zero);
    }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void ReadCallback(short* iq, uint count, ulong first, IntPtr context);
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct Gain { internal byte Lna, Lpf, Digital; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct TuningRange { internal ulong Minimum, Maximum; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
    internal struct DeviceInfo {
        internal ushort Vid,Pid;
        internal byte Bus,Address;
        internal int Chipset;
        internal uint Capabilities;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=96)] internal string Name;
    }
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_get_device_count")]
    internal static extern int DeviceCount();
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_get_device_info")]
    internal static extern int Device(uint index,out DeviceInfo info);
    [StructLayout(LayoutKind.Sequential)]
    internal struct Stats { internal ulong Samples,Packets,UsbBytes;internal uint FirmwareState,DmaErrors,Stage;internal int ResetError; }
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_get_stream_stats")]
    internal static extern int GetStats(IntPtr device,out Stats stats);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_open_ex")]
    internal static extern int Open(out IntPtr device, uint index, uint flags);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_close")]
    internal static extern int Close(IntPtr device);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_set_center_freq")]
    internal static extern int Frequency(IntPtr device, ulong frequency);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_set_sample_rate")]
    internal static extern int Rate(IntPtr device, uint rate);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_set_tuner_bandwidth")]
    internal static extern int Bandwidth(IntPtr device,uint bandwidth);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_get_tuning_range")]
    internal static extern int Range(IntPtr device, out ulong minimum, out ulong maximum);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_get_tuning_ranges")]
    internal static extern int Ranges(IntPtr device, [Out] TuningRange[] ranges, uint capacity);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_set_tuner_gain_mode")]
    internal static extern int GainMode(IntPtr device, int manual);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_set_gain_codes")]
    internal static extern int GainCodes(IntPtr device, ref Gain gain);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_read_async")]
    internal static extern int Read(IntPtr device, ReadCallback callback, IntPtr context, uint transfers, uint samples);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_cancel_async")]
    internal static extern int Cancel(IntPtr device);
    [DllImport(Dll, CallingConvention=CallingConvention.Cdecl, EntryPoint="birriasdr_strerror")]
    static extern IntPtr Error(int code);
    internal static void Check(int code)
    {
        if (code < 0) throw new IOException($"BirriaSDR: {Marshal.PtrToStringAnsi(Error(code))} ({code}).");
    }
}

// SDR# calls the source on its UI/control thread. Only the bounded queue is
// shared with the native reader and SDR# sample consumer. No UI calls or DSP
// run inside the native callback. Overflow stops the stream, never drops IQ.
public sealed unsafe class BirriaIO : IFrontendController, IIQStreamController,
    ITunableSource, IConfigurationPanelProvider, IControlAwareObject, ISpectrumProvider, IDisposable
{
    const int Block = 16384, Depth = 128;
    readonly object lifecycle = new(), queue = new();
    readonly short[][] buffers = Enumerable.Range(0, Depth).Select(_ => new short[Block*2]).ToArray();
    readonly int[] lengths = new int[Depth];
    readonly Native.ReadCallback nativeCallback;
    readonly IqConverter converter = new();
    IntPtr device;
    Thread? reader, consumer;
    SamplesAvailableDelegate? callback;
    volatile bool stopping, finished;
    int head, tail, available;
    long frequency = 2437000000, minimum = 1800000000, maximum = 8000000000;
    Native.TuningRange[] ranges = { new() { Minimum=1800000000,Maximum=3100000000 }, new() { Minimum=4000000000,Maximum=8000000000 } };
    uint index;
    bool manual = true;
    bool dcCorrection = true;
    bool wide = true;
    uint sampleRate=40000000;
    uint filterBandwidth=40000000;
    Native.Gain gain = new() { Lna = 1, Lpf = 4, Digital = 0 };
    UserControl? panel;
    Label? status;
    ISharpControl? control;
    SynchronizationContext? uiContext;
    public string? LastError { get; private set; }
    public uint LastFirmwareState { get; private set; }
    public BirriaIO() { nativeCallback = Receive; }
    public bool CanTune => true;
    public double Samplerate => sampleRate;
    public bool DcCorrection
    {
        get => dcCorrection;
        set {
            lock(lifecycle) {
                if(reader!=null || control?.IsPlaying==true) throw new InvalidOperationException("Stop reception before changing DC correction.");
                dcCorrection=value;
            }
        }
    }
    public uint SampleRate
    {
        get => sampleRate;
        set
        {
            if(value!=40000000 && value!=80000000) throw new ArgumentOutOfRangeException(nameof(value));
            lock(lifecycle)
            {
                if(value==sampleRate) return;
                if(reader!=null || control?.IsPlaying==true) throw new InvalidOperationException("Stop reception before changing the sample rate.");
                Close(); sampleRate=value;
                if(value==40000000) filterBandwidth=40000000;
                UpdateStatus();
            }
        }
    }
    public uint FilterBandwidth
    {
        get => filterBandwidth;
        set {
            if(value!=40000000 && value!=80000000) throw new ArgumentOutOfRangeException(nameof(value));
            lock(lifecycle) {
                if(value==filterBandwidth) return;
                if(reader!=null || control?.IsPlaying==true) throw new InvalidOperationException("Stop reception before changing the filter.");
                if(value==80000000 && sampleRate!=80000000) throw new InvalidOperationException("Select 80 MSps before the wider filter.");
                Close(); filterBandwidth=value;
                UpdateStatus();
            }
        }
    }
    public float UsableSpectrumRatio => filterBandwidth==80000000 ? 0.9f : sampleRate==80000000 ? 0.45f : 0.9f;
    public long MinimumTunableFrequency => minimum;
    public long MaximumTunableFrequency => maximum;
    public long Frequency
    {
        get { lock (lifecycle) return frequency; }
        set
        {
            lock (lifecycle)
            {
                if (value < 0 || !ranges.Any(range => (ulong)value>=range.Minimum && (ulong)value<=range.Maximum)) throw new ArgumentOutOfRangeException(nameof(value), "Center is outside the enabled tuning windows.");
                if (device != IntPtr.Zero) Native.Check(Native.Frequency(device, checked((ulong)value)));
                frequency = value;
            }
        }
    }
    public void SetControl(object value) { control = value as ISharpControl; uiContext = SynchronizationContext.Current; }
    public void Open()
    {
        lock (lifecycle)
        {
            if (device != IntPtr.Zero) return;
            Native.Check(Native.Open(out device, index, 1u | (wide ? 2u : 0u) | (sampleRate==80000000 ? 4u : 0u)));
            try
            {
                Native.Check(Native.Range(device, out ulong min, out ulong max));
                minimum = checked((long)min); maximum = checked((long)max);
                var windows=new Native.TuningRange[2];
                int count=Native.Ranges(device,windows,2);Native.Check(count);ranges=windows.Take(count).ToArray();
                frequency = Math.Clamp(frequency, minimum, maximum);
                Native.Check(Native.Frequency(device, checked((ulong)frequency)));
                Native.Check(Native.Rate(device,sampleRate));
                Native.Check(Native.Bandwidth(device,filterBandwidth));
                ApplyGain();
            }
            catch { Native.Close(device); device = IntPtr.Zero; throw; }
        }
    }
    void ApplyGain()
    {
        if (device == IntPtr.Zero) return;
        Native.Check(Native.GainMode(device, manual ? 1 : 0));
        if (manual) Native.Check(Native.GainCodes(device, ref gain));
    }
    public void Start(SamplesAvailableDelegate samplesAvailable)
    {
        ArgumentNullException.ThrowIfNull(samplesAvailable);
        lock (lifecycle)
        {
            if (reader != null) throw new InvalidOperationException("BirriaSDR is already started.");
            Open();
            callback = samplesAvailable; LastError = null; stopping = false; finished = false;
            converter.Reset(sampleRate);
            lock (queue) { head = tail = available = 0; }
            consumer = new Thread(Consume) { IsBackground = true, Name = "BirriaSDR SDR# IQ" };
            reader = new Thread(() =>
            {
                try {
                    int result = Native.Read(device, nativeCallback, IntPtr.Zero, 0, Block);
                    if(Native.GetStats(device,out var stats)==0) LastFirmwareState=stats.FirmwareState;
                    if (result < 0) Native.Check(result);
                }
                catch (Exception error) { Fail(error); }
                finally { lock (queue) { finished = true; Monitor.PulseAll(queue); } }
            }) { IsBackground = true, Name = "BirriaSDR USB" };
            consumer.Start(); reader.Start();
        }
    }
    void Receive(short* iq, uint count, ulong first, IntPtr context)
    {
        try
        {
            if (stopping) return;
            lock (queue)
            {
                if (count > Block || available == Depth) throw new IOException($"SDR# cannot keep up with {sampleRate/1000000} MSps. The stream stopped without dropping samples.");
                new ReadOnlySpan<short>(iq, checked((int)count*2)).CopyTo(buffers[tail]);
                lengths[tail] = checked((int)count); tail = (tail+1)%Depth; ++available;
                Monitor.PulseAll(queue);
            }
        }
        catch (Exception error) { Fail(error); stopping = true; Native.Cancel(device); }
    }
    void Consume()
    {
        var converted = new Complex[Block];
        try
        {
            while (true)
            {
                int slot, count;
                lock (queue)
                {
                    while (available == 0 && !finished && !stopping) Monitor.Wait(queue);
                    if (stopping || (available == 0 && finished)) return;
                    slot = head; count = lengths[slot];
                }
                // The head remains occupied until conversion finishes.
                short[] iq = buffers[slot];
                converter.Convert(iq.AsSpan(0,count*2),converted.AsSpan(0,count),dcCorrection);
                lock (queue) { head = (head+1)%Depth; --available; }
                fixed (Complex* data = converted) callback!(this, data, count);
            }
        }
        catch (Exception error) { Fail(error); stopping = true; Native.Cancel(device); }
    }
    void Fail(Exception error)
    {
        LastError ??= error.Message;
        Trace.TraceError(LastError);
        uiContext?.Post(_ => { if (status != null) status.Text = LastError; if (control?.IsPlaying == true) control.StopRadio(); }, null);
    }
    public void Stop()
    {
        Thread? activeReader, activeConsumer;
        lock (lifecycle)
        {
            activeReader = reader; activeConsumer = consumer;
            if (activeReader == null) return;
            if (Thread.CurrentThread == activeReader || Thread.CurrentThread == activeConsumer)
                throw new InvalidOperationException("Stop BirriaSDR from the SDR# control thread.");
            stopping = true;
        }
        lock (queue) Monitor.PulseAll(queue);
        // Retry cancellation across the short thread-start race. Always join
        // before closing the handle or allowing another acquisition.
        while (activeReader.IsAlive) { Native.Cancel(device); activeReader.Join(20); }
        activeConsumer?.Join();
        lock (lifecycle) { reader = consumer = null; callback = null; }
    }
    public void Close()
    {
        Stop();
        lock (lifecycle)
        {
            if (device == IntPtr.Zero) return;
            int result = Native.Close(device); device = IntPtr.Zero; Native.Check(result);
        }
    }
    public void Dispose() { Close(); panel?.Dispose(); GC.SuppressFinalize(this); }
    void UpdateStatus() {
        if(status!=null) status.Text=$"Ready. {sampleRate/1000000} MSps, nominal {filterBandwidth/1000000} MHz filter.\nGain codes and extended tuning are uncalibrated.";
    }
    public UserControl Gui => panel ??= CreatePanel();
    UserControl CreatePanel()
    {
        var result = new UserControl { AutoSize = true, MinimumSize = new Size(300, 350) };
        var layout = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, Padding=new Padding(6) };
        result.Controls.Add(layout);
        layout.Controls.Add(new Label { Text = "BirriaSDR • MT7961", AutoSize = true });
        var devices=new ComboBox { DropDownStyle=ComboBoxStyle.DropDownList,Width=275 };
        var refresh=new Button { Text="Refresh",AutoSize=true };
        void RefreshDevices() {
            if(device!=IntPtr.Zero) { if(status!=null) status.Text="Close the source before refreshing devices."; return; }
            try {
                devices.Items.Clear();
                int count=Native.DeviceCount();Native.Check(count);
                for(uint i=0;i<count;++i) {
                    Native.Check(Native.Device(i,out var info));
                    string name=info.Vid==0x0e8d && info.Pid==0x7961 ? "MT7961 USB" : info.Name;
                    devices.Items.Add($"{i}: {name} ({info.Vid:x4}:{info.Pid:x4})");
                }
                if(count>0) devices.SelectedIndex=(int)Math.Min(index,(uint)count-1);
                else if(status!=null) status.Text="No receiver found. Connect the adapter to USB 3 and install libusbK for WiFi_If (Interface 3).";
            } catch(Exception error) { if(status!=null) status.Text=error.Message; }
        }
        devices.SelectedIndexChanged+=(_,_)=> {
            if(device==IntPtr.Zero && devices.SelectedIndex>=0) index=(uint)devices.SelectedIndex;
            else if(devices.SelectedIndex!=(int)index) devices.SelectedIndex=(int)index;
        };
        refresh.Click+=(_,_)=>RefreshDevices();
        layout.Controls.Add(new Label { Text="Receiver (select before opening)",AutoSize=true });
        layout.Controls.Add(devices);layout.Controls.Add(refresh);
        var rates=new ComboBox { DropDownStyle=ComboBoxStyle.DropDownList,Width=275 };
        rates.Items.AddRange(new object[]{"40 MSps","80 MSps (experimental)"});rates.SelectedIndex=sampleRate==80000000 ? 1 : 0;
        rates.SelectedIndexChanged+=(_,_)=> {
            try { SampleRate=rates.SelectedIndex==1 ? 80000000u : 40000000u; }
            catch(Exception error) { if(status!=null) status.Text=error.Message;rates.SelectedIndex=sampleRate==80000000 ? 1 : 0; }
        };
        layout.Controls.Add(new Label { Text="Sample rate (while stopped)",AutoSize=true });layout.Controls.Add(rates);
        var filter=new ComboBox { DropDownStyle=ComboBoxStyle.DropDownList,Width=275 };
        filter.Items.AddRange(new object[]{"40 MHz filter","80 MHz filter (experimental)"});filter.SelectedIndex=filterBandwidth==80000000 ? 1 : 0;
        filter.SelectedIndexChanged+=(_,_)=> {
            try { FilterBandwidth=filter.SelectedIndex==1 ? 80000000u : 40000000u; }
            catch(Exception error) { if(status!=null) status.Text=error.Message;filter.SelectedIndex=filterBandwidth==80000000 ? 1 : 0; }
        };
        rates.SelectedIndexChanged+=(_,_)=>filter.SelectedIndex=filterBandwidth==80000000 ? 1 : 0;
        layout.Controls.Add(new Label { Text="Receive filter (while stopped)",AutoSize=true });layout.Controls.Add(filter);
        var dc=new CheckBox { Text="Remove DC offset (10 Hz notch)",Checked=dcCorrection,AutoSize=true };
        dc.CheckedChanged+=(_,_)=> {
            try { DcCorrection=dc.Checked; }
            catch(Exception error) { if(status!=null) status.Text=error.Message;dc.Checked=dcCorrection; }
        };
        layout.Controls.Add(dc);
        var tuning = new CheckBox { Text = "Extended low/high tuning (experimental)", Checked = wide, AutoSize = true };
        tuning.CheckedChanged += (_,_) => {
            if (device==IntPtr.Zero) wide=tuning.Checked;
            else { tuning.Checked=wide; if(status!=null) status.Text="Stop and reselect the source to change tuning mode."; }
        };
        layout.Controls.Add(tuning);
        var mode = new CheckBox { Text = "Manual gain (raw codes)", Checked = manual, AutoSize = true };
        mode.CheckedChanged += (_,_) => UpdateGain(() => manual = mode.Checked);
        layout.Controls.Add(mode);
        void Code(string label, byte initial, byte max, Action<byte> assign)
        {
            var row = new FlowLayoutPanel { AutoSize = true };
            row.Controls.Add(new Label { Text = label, Width = 90 });
            var value = new NumericUpDown { Minimum = 0, Maximum = max, Value = initial, Width = 65 };
            value.ValueChanged += (_,_) => UpdateGain(() => assign((byte)value.Value));
            row.Controls.Add(value); layout.Controls.Add(row);
        }
        Code("LNA code", gain.Lna, 3, value => gain.Lna = value);
        Code("Filter gain", gain.Lpf, 8, value => gain.Lpf = value);
        Code("Digital code", gain.Digital, 12, value => gain.Digital = value);
        status = new Label { Text = "Receive only. The wider filter needs 80 MSps.\nExtended tuning and gain codes are uncalibrated.", AutoSize = true, MaximumSize = new Size(280,0) };
        layout.Controls.Add(status);
        UpdateStatus();
        RefreshDevices();
        return result;
    }
    void UpdateGain(Action assign)
    {
        lock (lifecycle)
        {
            var previous = gain; bool previousMode = manual;
            try { assign(); ApplyGain(); }
            catch (Exception error) { gain = previous; manual = previousMode; if (status != null) status.Text = error.Message; }
        }
    }
}
