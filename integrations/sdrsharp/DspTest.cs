// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Text.Json;
using SDRSharp.BirriaSDR;
using SDRSharp.Radio;

internal static class DspTest
{
    static void Check(bool value,string message) { if(!value) throw new Exception(message); }
    internal static int Run()
    {
        var input=new short[32768];var output=new Complex[16384];var converter=new IqConverter();
        for(int n=0;n<output.Length;++n) {input[2*n]=(short)(n-8192);input[2*n+1]=(short)(8191-n);}
        converter.Reset(80000000);converter.Convert(input,output,false);
        for(int n=0;n<output.Length;++n) Check(output[n].Real==input[2*n]/8192f && output[n].Imag==input[2*n+1]/8192f,"Raw scaling or I/Q order changed");

        var random=new Random(1961);var values=new short[262144];for(int n=0;n<values.Length;++n)values[n]=(short)random.Next(-400,700);
        var whole=new Complex[values.Length/2];var split=new Complex[whole.Length];
        converter.Reset(80000000);converter.Convert(values,whole,true);converter.Reset(80000000);
        for(int n=0;n<whole.Length;) {int count=Math.Min(whole.Length-n,random.Next(1,17000));converter.Convert(values.AsSpan(n*2,count*2),split.AsSpan(n,count),true);n+=count;}
        for(int n=0;n<whole.Length;++n)Check(whole[n].Real==split[n].Real && whole[n].Imag==split[n].Imag,"DC rejection depends on USB block boundaries");

        for(int n=0;n<output.Length;++n) {input[2*n]=2048;input[2*n+1]=-1024;}
        converter.Reset(80000000);
        for(int n=0;n<1000;++n)converter.Convert(input,output,true);
        double residual=Math.Sqrt(output[^1].Real*output[^1].Real+output[^1].Imag*output[^1].Imag);
        Check(residual<1e-6,"DC did not settle below the required level");

        var gains=new List<object>();
        foreach(uint rate in new uint[]{40000000,80000000}) foreach(double hz in new[]{100000.0,1250000.0,3750000.0,-3750000.0}) {
            for(int n=0;n<output.Length;++n) {input[2*n]=(short)Math.Round(1024*Math.Cos(2*Math.PI*hz*n/rate));input[2*n+1]=(short)Math.Round(1024*Math.Sin(2*Math.PI*hz*n/rate));}
            converter.Reset(rate);converter.Convert(input,output,true);double original=0,filtered=0;
            for(int n=0;n<output.Length;++n) {original+=(double)input[2*n]*input[2*n]+(double)input[2*n+1]*input[2*n+1];filtered+=(double)output[n].Real*output[n].Real+(double)output[n].Imag*output[n].Imag;}
            double db=10*Math.Log10(filtered*8192*8192/original);Check(Math.Abs(db)<.001,"DC rejection changed an off-center tone");gains.Add(new{rate,hz,gain_db=db});
        }
        converter.Reset(80000000);for(int n=0;n<100;++n)converter.Convert(input,output,true);
        var watch=Stopwatch.StartNew();for(int n=0;n<5000;++n)converter.Convert(input,output,true);watch.Stop();
        double speed=5000.0*output.Length/watch.Elapsed.TotalSeconds;
        Console.WriteLine(JsonSerializer.Serialize(new{passed=true,raw_scaling_exact=true,chunk_boundaries_exact=true,dc_residual=residual,tones=gains,conversion_samples_per_second=speed}));
        return 0;
    }
}
