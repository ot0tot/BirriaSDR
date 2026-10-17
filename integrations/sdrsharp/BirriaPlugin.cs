// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.Reflection;
using System.Security.Cryptography;
using SDRSharp.Common;
using SDRSharp.Radio;

namespace SDRSharp.BirriaSDR;

// Recent SDR# builds discover ISharpPlugin, but no longer read FrontEnds.xml.
// Keep the necessary private host API isolated here. Never patch the host or
// assume that a different executable has a compatible source/menu layout.
public sealed class BirriaPlugin : ISharpPlugin
{
    internal const string SourceName = "BirriaSDR (MT7961)";
    internal const string HostSha256 = "4FA94D895EAE710C86709122206E0153F2A43D797F0E6F16BBD861AF3E92DBD8";
    const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
    ISharpControl? control;
    public string DisplayName => SourceName;
    public UserControl Gui => null!;

    public void Initialize(ISharpControl sharpControl)
    {
        using var executable = File.OpenRead(Environment.ProcessPath!);
        if (Convert.ToHexString(SHA256.HashData(executable)) != HostSha256)
            throw new NotSupportedException("BirriaSDR source registration supports the inspected x64 .NET 8 SDR# 1921 executable only. Rebuild and validate the registration adapter for this host. No source was registered.");

        var proxy = sharpControl.GetType();
        if (proxy.FullName != "SDRSharp.SharpControlProxy")
            throw new NotSupportedException("Unexpected SDR# control proxy.");
        object owner = proxy.GetField("_owner", PrivateInstance)?.GetValue(sharpControl)
            ?? throw new MissingMemberException("SDR# control owner is unavailable.");
        Type host = owner.GetType();
        if (host.FullName != "SDRSharp.MainForm" || owner is not Form form || form.InvokeRequired)
            throw new NotSupportedException("BirriaSDR registration requires the SDR# main UI thread.");
        if (host.GetField("_sliceOwner", PrivateInstance)?.GetValue(owner) != null) return;

        var types = host.GetField("_frontendTypes", PrivateInstance)?.GetValue(owner) as Dictionary<string, Type>
            ?? throw new MissingMemberException("SDR# receiver type list is unavailable.");
        var menu = host.GetField("sourceMenuItem", PrivateInstance)?.GetValue(owner) as ToolStripMenuItem
            ?? throw new MissingMemberException("SDR# source menu is unavailable.");
        var selected = host.GetField("_selectedIQSource", PrivateInstance)
            ?? throw new MissingMemberException("SDR# selected receiver index is unavailable.");
        var register = host.GetMethod("LoadSourceType", PrivateInstance, null,
            new[] { typeof(string), typeof(Type), typeof(int) }, null)
            ?? throw new MissingMethodException("SDR# source registration is unavailable.");
        if (types.TryGetValue(SourceName, out var existing)) {
            if (existing != typeof(BirriaIO)) throw new InvalidOperationException("Another BirriaSDR source is already registered.");
            return;
        }
        int count = types.Count;
        if (menu.DropDownItems.Count != count + 3 || menu.DropDownItems[count + 2].Tag != null ||
            types.Select((pair, index) => menu.DropDownItems[index + 2].Tag is int tag &&
                tag == index && menu.DropDownItems[index + 2].Text == pair.Key).Any(valid => !valid))
            throw new NotSupportedException("Unexpected SDR# source ordering. No source was registered.");

        // The sound-card entry must stay last: the host indexes menu entries
        // at sourceIndex+2 and identifies sound-card input with types.Count.
        bool soundCardSelected = (int)selected.GetValue(owner)! == count;
        register.Invoke(owner, new object[] { SourceName, typeof(BirriaIO), 10 });
        var item = menu.DropDownItems[menu.DropDownItems.Count - 1];
        menu.DropDownItems.Remove(item);
        menu.DropDownItems.Insert(count + 2, item);
        if (soundCardSelected) selected.SetValue(owner, count + 1);

        control = sharpControl;
        control.PropertyChanged += SourceChanged;
        if (Utils.GetBooleanSetting("birriasdr.selected", false))
            form.BeginInvoke(new Action(() => { if (!form.IsDisposed) item.PerformClick(); }));
    }

    void SourceChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (args.PropertyName == "SourceName") SaveSelection();
    }
    void SaveSelection()
    {
        if (control != null) Utils.SaveSetting("birriasdr.selected", control.Source is BirriaIO);
    }
    public void Close()
    {
        SaveSelection();
        if (control != null) control.PropertyChanged -= SourceChanged;
        control = null;
    }
}
