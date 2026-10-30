using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Text;

// Explicit host discovery for an AP created after its WLAN profile was added.
public static class Bl616clWlanScan {
    [StructLayout(LayoutKind.Sequential)]
    private struct Notification {
        public uint Source;
        public uint Code;
        public Guid InterfaceId;
        public uint DataSize;
        public IntPtr Data;
    }
    private delegate void Callback(ref Notification data, IntPtr context);
    [StructLayout(LayoutKind.Sequential)]
    private struct Dot11Ssid {
        public uint Length;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] Bytes;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct AvailableNetwork {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string ProfileName;
        public Dot11Ssid Ssid;
        public uint BssType, BssidCount, Connectable, NotConnectableReason, PhyCount;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
        public uint[] PhyTypes;
        public uint MorePhyTypes, SignalQuality, SecurityEnabled, Auth, Cipher, Flags, Reserved;
    }
    [DllImport("wlanapi.dll")]
    private static extern uint WlanOpenHandle(uint version, IntPtr reserved,
        out uint negotiated, out IntPtr handle);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanCloseHandle(IntPtr handle, IntPtr reserved);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanRegisterNotification(IntPtr handle, uint source,
        [MarshalAs(UnmanagedType.Bool)] bool ignoreDuplicates, Callback callback,
        IntPtr context, IntPtr reserved, out uint previous);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanScan(IntPtr handle, ref Guid interfaceId,
        IntPtr ssid, IntPtr informationElements, IntPtr reserved);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanGetAvailableNetworkList(IntPtr handle,
        ref Guid interfaceId, uint flags, IntPtr reserved, out IntPtr networks);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanQueryInterface(IntPtr handle, ref Guid interfaceId,
        uint opcode, IntPtr reserved, out uint size, out IntPtr data, out uint valueType);
    [DllImport("wlanapi.dll")]
    private static extern void WlanFreeMemory(IntPtr data);

    // Read only the WLAN service cache. Never scan/connect here or return names.
    // Counts distinguish service visibility from localized netsh text parsing.
    public static uint AvailableNetworks(Guid interfaceId, string target,
        out uint count, out uint matches) {
        count = matches = 0;
        byte[] expected = Encoding.UTF8.GetBytes(target);
        if (expected.Length == 0 || expected.Length > 32) return 87;
        int stride = Marshal.SizeOf(typeof(AvailableNetwork));
        if (stride != 628 || Marshal.SizeOf(typeof(Dot11Ssid)) != 36) return 13;
        uint negotiated;
        IntPtr handle;
        uint result = WlanOpenHandle(2, IntPtr.Zero, out negotiated, out handle);
        if (result != 0) return result;
        IntPtr data = IntPtr.Zero;
        try {
            result = WlanGetAvailableNetworkList(handle, ref interfaceId, 0,
                IntPtr.Zero, out data);
            if (result != 0) return result;
            if (data == IntPtr.Zero) return 13;
            uint entries = unchecked((uint)Marshal.ReadInt32(data));
            if (entries > 4096) return 13;
            for (uint i = 0; i < entries; ++i) {
                var network = (AvailableNetwork)Marshal.PtrToStructure(
                    IntPtr.Add(data, 8 + checked((int)i * stride)), typeof(AvailableNetwork));
                if (network.Ssid.Length > 32) return 13;
                if (network.Ssid.Length != expected.Length) continue;
                bool equal = true;
                for (int j = 0; j < expected.Length; ++j)
                    if (network.Ssid.Bytes[j] != expected[j]) { equal = false; break; }
                if (equal) ++matches;
            }
            count = entries;
            return 0;
        } finally {
            if (data != IntPtr.Zero) WlanFreeMemory(data);
            WlanCloseHandle(handle, IntPtr.Zero);
        }
    }

    // WLAN_CONNECTION_ATTRIBUTES starts with state, mode and WCHAR profile[256].
    // NLA's display name may differ from this actual connected WLAN profile.
    public static string ConnectedProfile(Guid interfaceId) {
        uint negotiated;
        IntPtr handle;
        if (WlanOpenHandle(2, IntPtr.Zero, out negotiated, out handle) != 0) return null;
        IntPtr data = IntPtr.Zero;
        try {
            uint size, valueType;
            if (WlanQueryInterface(handle, ref interfaceId, 7, IntPtr.Zero,
                out size, out data, out valueType) != 0 || size < 520 || data == IntPtr.Zero)
                return null;
            if (Marshal.ReadInt32(data) != 1) return null; // wlan_interface_state_connected
            return Marshal.PtrToStringUni(IntPtr.Add(data, 8), 256).TrimEnd('\0');
        } finally {
            if (data != IntPtr.Zero) WlanFreeMemory(data);
            WlanCloseHandle(handle, IntPtr.Zero);
        }
    }

    public static uint Scan(Guid interfaceId, int timeoutMs, out uint notification) {
        return ScanCore(interfaceId, IntPtr.Zero, timeoutMs, out notification);
    }

    // One directed request for a newly created test AP; no retry or connect.
    public static uint ScanTarget(Guid interfaceId, string target, int timeoutMs,
        out uint notification) {
        notification = 0;
        byte[] bytes = Encoding.UTF8.GetBytes(target);
        if (bytes.Length == 0 || bytes.Length > 32) return 87;
        IntPtr ssid = Marshal.AllocHGlobal(36);
        try {
            byte[] buffer = new byte[36];
            Array.Copy(BitConverter.GetBytes((uint)bytes.Length), buffer, 4);
            Array.Copy(bytes, 0, buffer, 4, bytes.Length);
            Marshal.Copy(buffer, 0, ssid, buffer.Length);
            return ScanCore(interfaceId, ssid, timeoutMs, out notification);
        } finally { Marshal.FreeHGlobal(ssid); }
    }

    private static uint ScanCore(Guid interfaceId, IntPtr ssid, int timeoutMs,
        out uint notification) {
        notification = 0;
        uint negotiated;
        IntPtr handle;
        uint result = WlanOpenHandle(2, IntPtr.Zero, out negotiated, out handle);
        if (result != 0) return result;
        using (var finished = new ManualResetEvent(false)) {
            uint lastNotification = 0;
            Callback callback = delegate(ref Notification data, IntPtr context) {
                if (data.Source == 8 && data.InterfaceId == interfaceId &&
                    (data.Code == 7 || data.Code == 8)) {
                    lastNotification = data.Code;
                    finished.Set();
                }
            };
            uint previous;
            try {
                result = WlanRegisterNotification(handle, 8, false, callback,
                    IntPtr.Zero, IntPtr.Zero, out previous);
                if (result != 0) return result;
                result = WlanScan(handle, ref interfaceId, ssid,
                    IntPtr.Zero, IntPtr.Zero);
                if (result != 0) return result;
                if (!finished.WaitOne(timeoutMs)) return 258;
                notification = lastNotification;
                return notification == 7 ? 0U : 31U;
            } finally {
                WlanRegisterNotification(handle, 0, false, null, IntPtr.Zero,
                    IntPtr.Zero, out previous);
                WlanCloseHandle(handle, IntPtr.Zero);
                GC.KeepAlive(callback);
            }
        }
    }
}
