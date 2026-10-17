// SPDX-License-Identifier: MIT
using SDRSharp.BirriaSDR;
using SDRSharp.Radio;
using System.Diagnostics;
using System.Runtime.InteropServices;

internal static unsafe class SmokeTest
{
    [STAThread]
    static int Main(string[] args)
    {
        if(args.Length>0 && args[0]=="--dsp") return DspTest.Run();
        if(args.Length>0 && args[0]=="--highband") return HighBand();
        // Load the application's old USB runtime first to exercise coexistence.
        if (args.Length > 0 && args[0]!="--panel") NativeLibrary.Load(Path.GetFullPath(args[0]));
        using var receiver = new BirriaIO();
        if(args.Length>0 && args[0]=="--panel") {
            using var form=new Form { ClientSize=new System.Drawing.Size(340,580) };
            var panel=receiver.Gui;panel.Dock=DockStyle.Fill;form.Controls.Add(panel);
            form.ShowInTaskbar=false;
            form.StartPosition=FormStartPosition.Manual;
            form.Location=new System.Drawing.Point(-2000,-2000);
            form.Show();Application.DoEvents();form.PerformLayout();
            var layout=(FlowLayoutPanel)panel.Controls[0];
            var selectors=layout.Controls.OfType<ComboBox>().ToArray();
            selectors[1].SelectedIndex=1;selectors[2].SelectedIndex=1;
            if(receiver.SampleRate!=80000000 || receiver.FilterBandwidth!=80000000 || receiver.UsableSpectrumRatio!=.9f) throw new InvalidOperationException("Wide profile panel selection");
            selectors[1].SelectedIndex=0;
            if(receiver.FilterBandwidth!=40000000 || selectors[2].SelectedIndex!=0) throw new InvalidOperationException("Narrow rate did not reset filter");
            selectors[2].SelectedIndex=1;
            if(receiver.FilterBandwidth!=40000000 || selectors[2].SelectedIndex!=0) throw new InvalidOperationException("Invalid filter was accepted");
            selectors[1].SelectedIndex=1;selectors[2].SelectedIndex=1;
            using var bitmap=new System.Drawing.Bitmap(panel.Width,panel.Height);
            panel.DrawToBitmap(bitmap,new System.Drawing.Rectangle(System.Drawing.Point.Empty,bitmap.Size));
            bitmap.Save(args[1]);
            Console.WriteLine("Panel selection, profile reset and invalid-combination checks passed.");
            return 0;
        }
        using var ready = new ManualResetEventSlim();
        for (int pass = args.Length>2 ? int.Parse(args[2]) : 0; pass < 3; ++pass)
        {
            receiver.SampleRate=pass==0 ? 40000000u : 80000000u;
            receiver.FilterBandwidth=pass==2 ? 80000000u : 40000000u;
            if(receiver.Samplerate!=receiver.SampleRate) throw new InvalidOperationException("Sample rate metadata");
            long total = 0; float peak = 0; bool invalid = false;
            ready.Reset(); receiver.Open(); receiver.Frequency = pass==1 ? 5800000000 : 2437000000;
            SamplesAvailableDelegate callback = (source, samples, count) =>
            {
                for (int i = 0; i < count; ++i)
                {
                    float re = samples[i].Real, im = samples[i].Imag;
                    if (!float.IsFinite(re) || !float.IsFinite(im) || re < -1 || re >= 1 || im < -1 || im >= 1) invalid = true;
                    peak = Math.Max(peak, Math.Max(Math.Abs(re), Math.Abs(im)));
                }
                if (Interlocked.Add(ref total, count) >= 4000000) ready.Set();
            };
            receiver.Start(callback);
            try { receiver.FilterBandwidth=receiver.FilterBandwidth==40000000 ? 80000000u : 40000000u; throw new Exception("Live filter change accepted"); }
            catch(InvalidOperationException) { }
            if (!ready.Wait(TimeSpan.FromSeconds(8))) throw new TimeoutException(receiver.LastError ?? "No SDR# samples");
            receiver.Frequency = pass%2 == 0 ? 5800000000 : 2610000000;
            var wait=Stopwatch.StartNew();int duration=pass>0 && args.Length>1 ? int.Parse(args[1]) : 100;
            while(wait.ElapsedMilliseconds<duration && receiver.LastError==null) Thread.Sleep(10);
            receiver.Stop(); receiver.Close();
            Console.WriteLine($"{{\"pass\":{pass},\"rate\":{receiver.SampleRate},\"bandwidth\":{receiver.FilterBandwidth},\"samples\":{total},\"peak\":{peak},\"invalid\":{invalid.ToString().ToLowerInvariant()},\"firmware_state\":{receiver.LastFirmwareState},\"error\":{System.Text.Json.JsonSerializer.Serialize(receiver.LastError)}}}");
            if (invalid || peak == 0 || receiver.LastError != null) return 1;
        }
        return 0;
    }
    static int HighBand()
    {
        using var receiver=new BirriaIO { SampleRate=80000000,FilterBandwidth=80000000 };
        typeof(BirriaIO).GetField("manual",System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance)!.SetValue(receiver,false);
        receiver.Open();receiver.Frequency=5900000000;
        long total=0,invalid=0;double peak=0;using var ready=new ManualResetEventSlim();
        SamplesAvailableDelegate callback=(source,samples,count)=> {
            for(int n=0;n<count;++n) {
                float re=samples[n].Real,im=samples[n].Imag;
                if(!float.IsFinite(re)||!float.IsFinite(im))invalid++;
                peak=Math.Max(peak,Math.Max(Math.Abs(re),Math.Abs(im)));
            }
            if(Interlocked.Add(ref total,count)>=8000000)ready.Set();
        };
        receiver.Start(callback);
        if(!ready.Wait(8000))throw new TimeoutException(receiver.LastError??"No high-band samples");
        Thread.Sleep(3000);receiver.Frequency=5905000000;Thread.Sleep(7000);
        receiver.Stop();receiver.Close();
        bool passed=invalid==0 && receiver.LastError==null && receiver.LastFirmwareState==3 && total>=700000000;
        Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(new{passed,rate=receiver.SampleRate,bandwidth=receiver.FilterBandwidth,manual_gain=false,dc_correction=receiver.DcCorrection,samples=total,invalid,peak,state=receiver.LastFirmwareState,error=receiver.LastError}));
        return passed?0:1;
    }
}
