// SPDX-License-Identifier: MIT
using SDRSharp.Radio;

namespace SDRSharp.BirriaSDR;

// Conversion and optional application-side DC rejection. The native API and
// recorder retain the original integer samples. State spans USB blocks so a
// block boundary cannot create a periodic offset step or comb of notches.
internal sealed class IqConverter
{
    double meanI, meanQ, alpha;
    internal void Reset(uint rate)
    {
        meanI = meanQ = 0;
        alpha = 1 - Math.Exp(-2 * Math.PI * 10 / rate);
    }
    internal void Convert(ReadOnlySpan<short> input, Span<Complex> output, bool removeDc)
    {
        if (input.Length != output.Length * 2) throw new ArgumentException("Expected complete I,Q pairs.");
        if (!removeDc) {
            for (int n = 0; n < output.Length; ++n) {
                output[n].Real = input[2*n] / 8192f;
                output[n].Imag = input[2*n+1] / 8192f;
            }
            return;
        }
        double i = meanI, q = meanQ, a = alpha;
        for (int n = 0; n < output.Length; ++n) {
            double real = input[2*n] / 8192.0, imag = input[2*n+1] / 8192.0;
            i += a * (real - i); q += a * (imag - q);
            output[n].Real = (float)(real - i);
            output[n].Imag = (float)(imag - q);
        }
        meanI = i; meanQ = q;
    }
}
