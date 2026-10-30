using System;
using System.Runtime.InteropServices;
using System.Security;

// A temporary WLAN profile is passed in memory, never installed or exported.
public static class BridgePrivateSta {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Parameters {
        public int Mode;
        [MarshalAs(UnmanagedType.LPWStr)] public string Profile;
        public IntPtr Ssid, Bssids;
        public int BssType;
        public uint Flags;
    }
    [DllImport("wlanapi.dll")] static extern uint WlanOpenHandle(uint v, IntPtr r, out uint n, out IntPtr h);
    [DllImport("wlanapi.dll")] static extern uint WlanCloseHandle(IntPtr h, IntPtr r);
    [DllImport("wlanapi.dll")] static extern uint WlanConnect(IntPtr h, ref Guid g, ref Parameters p, IntPtr r);
    [DllImport("wlanapi.dll")] static extern uint WlanQueryInterface(IntPtr h, ref Guid g, uint op, IntPtr r, out uint size, out IntPtr data, out uint type);
    [DllImport("wlanapi.dll")] static extern void WlanFreeMemory(IntPtr p);
    public static string CurrentProfile(Guid id) {
        uint n; IntPtr h;
        if (WlanOpenHandle(2,IntPtr.Zero,out n,out h)!=0) return null;
        IntPtr data=IntPtr.Zero;
        try {
            uint size,type;
            if(WlanQueryInterface(h,ref id,7,IntPtr.Zero,out size,out data,out type)!=0 || data==IntPtr.Zero || size<520 || Marshal.ReadInt32(data)!=1) return null;
            return Marshal.PtrToStringUni(IntPtr.Add(data,8),256).TrimEnd('\0');
        } finally { if(data!=IntPtr.Zero)WlanFreeMemory(data);WlanCloseHandle(h,IntPtr.Zero); }
    }
    public static uint Connect(Guid id,string ssid,string password) {
        string security=String.IsNullOrEmpty(password)?
          "<authEncryption><authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX></authEncryption>":
          "<authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>"+SecurityElement.Escape(password)+"</keyMaterial></sharedKey>";
        string xml="<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>BridgePrivateStaSession</name><SSIDConfig><SSID><name>"+SecurityElement.Escape(ssid)+"</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>manual</connectionMode><MSM><security>"+security+"</security></MSM></WLANProfile>";
        uint n;IntPtr h;uint result=WlanOpenHandle(2,IntPtr.Zero,out n,out h);
        if(result!=0)return result;
        try {
            var p=new Parameters { Mode=1,Profile=xml,Ssid=IntPtr.Zero,Bssids=IntPtr.Zero,BssType=1,Flags=0 };
            return WlanConnect(h,ref id,ref p,IntPtr.Zero);
        } finally { WlanCloseHandle(h,IntPtr.Zero); }
    }
}
