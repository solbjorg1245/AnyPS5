# Converts APS5_PASS_DUMP raw images (<seq>.raw: one "PASSDUMP1 vkformat=.. format=.. width=.. height=..
# layers=.. texelbytes=.." line, then the texels row after row) to PNG, next to each file or into -Out.
# Float and integer images are scaled by the 99th percentile of their finite values (times -Exposure) and
# gamma-encoded; unorm images are shown as stored; depth is stretched over its finite range. NaN texels
# are magenta, infinities cyan. A one-channel image is gray, a two-channel one red/green.
# usage: raw2png.ps1 -Path <file.raw or dump folder> [-Out <dir>] [-Layer 0] [-Exposure 1]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Out = "",
    [int]$Layer = 0,
    [double]$Exposure = 1.0
)

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class PassDumpRaw {
    static float Half(int bits) {
        int exponent = (bits >> 10) & 31, mantissa = bits & 1023;
        double value;
        if (exponent == 0) value = mantissa * Math.Pow(2, -24);
        else if (exponent == 31) value = mantissa == 0 ? double.PositiveInfinity : double.NaN;
        else value = (1 + mantissa / 1024.0) * Math.Pow(2, exponent - 15);
        return (float)(((bits >> 15) & 1) == 1 ? -value : value);
    }

    // The unsigned 5-bit-exponent floats of B10G11R11.
    static float Small(uint bits, int mantissaBits) {
        uint exponent = bits >> mantissaBits, mantissa = bits & ((1u << mantissaBits) - 1);
        if (exponent == 31) return mantissa == 0 ? float.PositiveInfinity : float.NaN;
        if (exponent == 0) return (float)(mantissa * Math.Pow(2, -14 - mantissaBits));
        return (float)((1 + mantissa / (double)(1u << mantissaBits)) * Math.Pow(2, (int)exponent - 15));
    }

    // Fills `values` (four floats per texel) and returns the channel count, 0 for a format it cannot show.
    static int Decode(string format, byte[] data, int offset, int count, int texelBytes, float[] values) {
        if (format.StartsWith("B10G11R11")) {
            for (int i = 0; i < count; ++i) {
                uint word = BitConverter.ToUInt32(data, offset + i * 4);
                values[i * 4] = Small(word & 0x7ff, 6);
                values[i * 4 + 1] = Small((word >> 11) & 0x7ff, 6);
                values[i * 4 + 2] = Small(word >> 22, 5);
                values[i * 4 + 3] = 1;
            }
            return 3;
        }
        if (format.StartsWith("E5B9G9R9")) {
            for (int i = 0; i < count; ++i) {
                uint word = BitConverter.ToUInt32(data, offset + i * 4);
                double scale = Math.Pow(2, (int)(word >> 27) - 24);
                values[i * 4] = (float)((word & 511) * scale);
                values[i * 4 + 1] = (float)(((word >> 9) & 511) * scale);
                values[i * 4 + 2] = (float)(((word >> 18) & 511) * scale);
                values[i * 4 + 3] = 1;
            }
            return 3;
        }
        if (format.StartsWith("A2B10G10R10") || format.StartsWith("A2R10G10B10")) {
            bool bgr = format.StartsWith("A2R10G10B10");
            bool unorm = format.Contains("UNORM");
            for (int i = 0; i < count; ++i) {
                uint word = BitConverter.ToUInt32(data, offset + i * 4);
                float low = word & 1023, middle = (word >> 10) & 1023, high = (word >> 20) & 1023, alpha = word >> 30;
                float scale = unorm ? 1 / 1023f : 1;
                values[i * 4] = (bgr ? high : low) * scale;
                values[i * 4 + 1] = middle * scale;
                values[i * 4 + 2] = (bgr ? low : high) * scale;
                values[i * 4 + 3] = alpha * (unorm ? 1 / 3f : 1);
            }
            return 4;
        }
        // Uniform channels: <letter><bits>... then _UNORM, _SNORM, _UINT, _SINT, _SFLOAT or _SRGB
        // (A8B8G8R8 is R8G8B8A8 in memory).
        if (format.StartsWith("A8B8G8R8")) format = "R8G8B8A8" + format.Substring(8);
        int underscore = format.IndexOf('_');
        if (underscore < 0) return 0;
        string kind = format.Substring(underscore + 1);
        var letters = new List<char>();
        int bits = 0;
        for (int at = 0; at < underscore; ++at) {
            char c = format[at];
            if (char.IsLetter(c)) { letters.Add(c); continue; }
            int start = at;
            while (at < underscore && char.IsDigit(format[at])) ++at;
            int width = int.Parse(format.Substring(start, at - start));
            if (bits != 0 && bits != width) return 0;
            bits = width;
            --at;
        }
        if (letters.Count == 0 || bits % 8 != 0 || letters.Count * bits / 8 != texelBytes) return 0;
        int bytes = bits / 8;
        for (int i = 0; i < count; ++i) {
            values[i * 4 + 3] = 1;
            for (int channel = 0; channel < letters.Count; ++channel) {
                int at = offset + i * texelBytes + channel * bytes;
                double raw;
                ulong unsigned = bytes == 1 ? data[at] : bytes == 2 ? BitConverter.ToUInt16(data, at) : BitConverter.ToUInt32(data, at);
                long signed = bytes == 1 ? (sbyte)data[at] : bytes == 2 ? BitConverter.ToInt16(data, at) : BitConverter.ToInt32(data, at);
                double range = Math.Pow(2, bits) - 1, signedRange = Math.Pow(2, bits - 1) - 1;
                if (kind == "SFLOAT") raw = bytes == 2 ? Half((int)unsigned) : bytes == 4 ? BitConverter.ToSingle(data, at) : double.NaN;
                else if (kind == "SNORM") raw = Math.Max(-1.0, signed / signedRange);
                else if (kind == "SINT") raw = signed;
                else if (kind == "UINT") raw = unsigned;
                else raw = unsigned / range;
                char letter = letters[channel];
                int slot = letter == 'G' ? 1 : letter == 'B' ? 2 : letter == 'A' ? 3 : 0;
                values[i * 4 + slot] = (float)raw;
            }
        }
        return Math.Min(letters.Count, 4);
    }

    public static string Convert(string path, string output, int layer, double exposure) {
        byte[] data = File.ReadAllBytes(path);
        int newline = Array.IndexOf(data, (byte)10);
        if (newline < 0) return path + ": no header line";
        string header = Encoding.ASCII.GetString(data, 0, newline);
        if (!header.StartsWith("PASSDUMP1")) return path + ": not a pass dump image";
        var fields = new Dictionary<string, string>();
        foreach (string token in header.Split(' ')) {
            int equals = token.IndexOf('=');
            if (equals > 0) fields[token.Substring(0, equals)] = token.Substring(equals + 1);
        }
        string format = fields["format"];
        int width = int.Parse(fields["width"]), height = int.Parse(fields["height"]);
        int layers = int.Parse(fields["layers"]), texelBytes = int.Parse(fields["texelbytes"]);
        layer = Math.Max(0, Math.Min(layer, layers - 1));
        int count = width * height;
        long offset = newline + 1 + (long)layer * count * texelBytes;
        if (count == 0 || offset + (long)count * texelBytes > data.Length) return path + ": shorter than its header says";
        float[] values = new float[count * 4];
        int channels = Decode(format, data, (int)offset, count, texelBytes, values);
        if (channels == 0) return path + ": format " + format + " is not shown";

        bool depth = format.StartsWith("D");
        bool display = !depth && (format.Contains("UNORM") || format.Contains("SRGB") || format.Contains("SNORM"));
        int shown = Math.Min(channels, 3);
        double low = 0, scale = 1;
        var sample = new List<double>();
        int step = Math.Max(1, count / 65536);
        double minimum = double.MaxValue, maximum = double.MinValue;
        for (int i = 0; i < count; i += step) {
            for (int channel = 0; channel < shown; ++channel) {
                double value = values[i * 4 + channel];
                if (double.IsNaN(value) || double.IsInfinity(value)) continue;
                sample.Add(Math.Abs(value));
                minimum = Math.Min(minimum, value);
                maximum = Math.Max(maximum, value);
            }
        }
        if (depth && maximum > minimum) { low = minimum; scale = maximum - minimum; }
        else if (!display && sample.Count > 0) {
            sample.Sort();
            scale = sample[(int)((sample.Count - 1) * 0.99)];
            if (scale <= 0) scale = sample[sample.Count - 1] > 0 ? sample[sample.Count - 1] : 1;
        }
        scale /= exposure;

        long nan = 0, infinite = 0;
        using (var bitmap = new Bitmap(width, height, PixelFormat.Format24bppRgb)) {
            BitmapData locked = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            byte[] row = new byte[locked.Stride];
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    int texel = (y * width + x) * 4;
                    bool isNan = false, isInfinite = false;
                    for (int channel = 0; channel < channels; ++channel) {
                        isNan |= float.IsNaN(values[texel + channel]);
                        isInfinite |= float.IsInfinity(values[texel + channel]);
                    }
                    byte[] rgb = new byte[3];
                    if (isNan) { rgb[0] = 255; rgb[2] = 255; ++nan; }
                    else if (isInfinite) { rgb[1] = 255; rgb[2] = 255; ++infinite; }
                    else {
                        for (int channel = 0; channel < 3; ++channel) {
                            int source = shown == 1 ? 0 : channel;
                            if (source >= shown) continue;
                            double value = (values[texel + source] - low) / scale;
                            value = Math.Max(0.0, Math.Min(1.0, value));
                            if (!display && !depth) value = Math.Pow(value, 1 / 2.2);
                            rgb[channel] = (byte)Math.Round(value * 255);
                        }
                    }
                    row[x * 3] = rgb[2];
                    row[x * 3 + 1] = rgb[1];
                    row[x * 3 + 2] = rgb[0];
                }
                Marshal.Copy(row, 0, new IntPtr(locked.Scan0.ToInt64() + (long)y * locked.Stride), locked.Stride);
            }
            bitmap.UnlockBits(locked);
            bitmap.Save(output, ImageFormat.Png);
        }
        return String.Format("{0}: {1} {2}x{3} layer {4}/{5}, scale {6:g4}, {7} NaN, {8} infinite -> {9}", Path.GetFileName(path), format, width, height, layer, layers, scale, nan, infinite, output);
    }
}
"@

$files = if (Test-Path $Path -PathType Container) {
    Get-ChildItem $Path -Filter *.raw | Sort-Object { [long]$_.BaseName }
} else {
    Get-Item $Path
}
if ($Out -ne "") { New-Item -ItemType Directory -Force $Out | Out-Null }
foreach ($file in $files) {
    $target = if ($Out -ne "") { Join-Path $Out ($file.BaseName + ".png") } else { [IO.Path]::ChangeExtension($file.FullName, ".png") }
    try {
        [PassDumpRaw]::Convert($file.FullName, $target, $Layer, $Exposure)
    } catch {
        Write-Warning "$($file.Name): $_"
    }
}
