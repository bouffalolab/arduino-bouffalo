param(
    [string]$OutputPath,
    [ValidateRange(5, 1200)][int]$Seconds = 180,
    [switch]$HciTransportCounters,
    [switch]$AttPacketSummary,
    [switch]$HciLinkSummary,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
if (($AttPacketSummary -or $HciLinkSummary) -and -not $HciTransportCounters) {
    throw 'Packet summaries require HciTransportCounters schema validation'
}

# Real-time only: no ETL file, device address, activity ID or event payload is
# copied to the output. Default mode reads only EVENT_RECORD headers. The
# explicit HciTransportCounters option also reads a TDH-verified 8-byte
# transport prefix, never the variable HCI packet body.
# AttPacketSummary explicitly opts into validating ACL/L2CAP framing and hashing
# complete ATT PDUs. Only opcode, length, numeric handles and FNV are written.
# HciLinkSummary reads only whitelisted numeric link-event fields, skipping
# Connection Complete address fields and all advertising/command bodies.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class UnoR4BleEtwMetadata {
    [StructLayout(LayoutKind.Explicit, Size=80)]
    struct Header {
        [FieldOffset(12)] public uint ProcessId;
        [FieldOffset(16)] public long Timestamp;
        [FieldOffset(24)] public Guid Provider;
        [FieldOffset(40)] public ushort Id;
        [FieldOffset(42)] public byte Version;
        [FieldOffset(44)] public byte Level;
        [FieldOffset(45)] public byte Opcode;
        [FieldOffset(46)] public ushort Task;
    }
    // EVENT_TRACE_LOGFILEW x64 layout from evntrace.h. The unused fields
    // remain zero; PROCESS_TRACE_MODE_EVENT_RECORD selects this callback ABI.
    [StructLayout(LayoutKind.Explicit, Size=448)]
    struct Logfile {
        [FieldOffset(8)] public IntPtr LoggerName;
        [FieldOffset(28)] public uint Mode;
        [FieldOffset(424)] public IntPtr Callback;
    }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    delegate void EventCallback(IntPtr record);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, EntryPoint="StartTraceW")]
    static extern uint StartTrace(out ulong handle, string name, IntPtr properties);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, EntryPoint="ControlTraceW")]
    static extern uint ControlTrace(ulong handle, string name, IntPtr properties, uint control);
    [DllImport("advapi32.dll")]
    static extern uint EnableTraceEx2(ulong handle, ref Guid provider, uint control,
                                    byte level, ulong any, ulong all, uint timeout, IntPtr parameters);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, EntryPoint="OpenTraceW")]
    static extern ulong OpenTrace(ref Logfile logfile);
    [DllImport("advapi32.dll")]
    static extern uint ProcessTrace(ulong[] handles, uint count, IntPtr start, IntPtr end);
    [DllImport("advapi32.dll")]
    static extern uint CloseTrace(ulong handle);
    [DllImport("kernel32.dll")]
    static extern bool QueryPerformanceFrequency(out long value);
    [DllImport("kernel32.dll")]
    static extern bool QueryPerformanceCounter(out long value);
    [StructLayout(LayoutKind.Sequential)]
    struct Descriptor {
        public ushort Id; public byte Version, Channel, Level, Opcode;
        public ushort Task; public ulong Keyword;
    }
    [DllImport("tdh.dll")]
    static extern uint TdhGetManifestEventInformation(ref Guid provider, ref Descriptor descriptor, IntPtr info, ref uint size);

    static readonly Guid Bthport = new Guid("8a1f9517-3a8c-4a9e-a018-4f17a200f277");

    public static void ValidateTransportCounters() {
        Guid provider=Bthport;
        Descriptor descriptor=new Descriptor {Id=402,Channel=16,Level=4,Keyword=0x8000000000000000UL};
        uint size=0;
        if (TdhGetManifestEventInformation(ref provider,ref descriptor,IntPtr.Zero,ref size)!=122 || size<232 || size>1048576)
            throw new InvalidOperationException("BTHPORT schema unavailable");
        IntPtr info=Marshal.AllocHGlobal((int)size);
        try {
            if (TdhGetManifestEventInformation(ref provider,ref descriptor,info,ref size)!=0 || Marshal.ReadInt32(info,100)!=5)
                throw new InvalidOperationException("BTHPORT schema changed");
            string[] names={"CommandPacketsAllowed","AclWriteCredits","BIP_Type","BIP_DataLen","BIP_Data"};
            short[] types={4,6,4,8,14}; // UINT8, UINT16, UINT8, UINT32, BINARY
            for(int i=0;i<5;i++) {
                int offset=112+i*24, name=Marshal.ReadInt32(info,offset+4);
                if(name<232 || name>=size || Marshal.PtrToStringUni(IntPtr.Add(info,name))!=names[i] ||
                   Marshal.ReadInt16(info,offset+8)!=types[i] || Marshal.ReadInt32(info,offset)!=(i==4?2:0))
                    throw new InvalidOperationException("BTHPORT property layout changed");
            }
            if(Marshal.ReadInt32(info,104)!=5 || Marshal.ReadInt16(info,112+4*24+18)!=3)
                throw new InvalidOperationException("BTHPORT variable body schema changed");
        } finally { Marshal.FreeHGlobal(info); }
        IntPtr record=Marshal.AllocHGlobal(112), prefix=Marshal.AllocHGlobal(8);
        try {
            Marshal.Copy(new byte[112],0,record,112);
            Marshal.Copy(new byte[] {7,0x34,0x12,2,0,0,0,0},0,prefix,8);
            Header header=new Header {Provider=Bthport,Id=402,Timestamp=2000};
            Marshal.StructureToPtr(header,record,false);
            Marshal.WriteInt16(record,86,8);
            Marshal.WriteIntPtr(record,96,prefix);
            if(TransportRow(record,1000,1000)!="1000.000,7,4660,2,0")
                throw new InvalidOperationException("BTHPORT prefix projection failed");
            Marshal.WriteInt16(record,86,7);
            bool rejected=false;
            try { TransportRow(record,1000,1000); } catch(InvalidOperationException) {rejected=true;}
            if(!rejected) throw new InvalidOperationException("Truncated BTHPORT prefix accepted");
            Marshal.WriteInt16(record,86,8);
            Marshal.WriteInt32(prefix,4,1);
            rejected=false;
            try { TransportRow(record,1000,1000); } catch(InvalidOperationException) {rejected=true;}
            if(!rejected) throw new InvalidOperationException("Invalid BTHPORT data length accepted");
            header.Id=403; Marshal.StructureToPtr(header,record,false);
            Marshal.WriteIntPtr(record,96,new IntPtr(1));
            if(TransportRow(record,1000,1000)!=null)
                throw new InvalidOperationException("Unrelated event prefix read");
        } finally {Marshal.FreeHGlobal(prefix);Marshal.FreeHGlobal(record);}
        Console.WriteLine("BLE_ETW_METADATA:TRANSPORT_COUNTERS_VALIDATE_PASS");
    }

    static string TransportRow(IntPtr record,long origin,long frequency) {
        Header h=(Header)Marshal.PtrToStructure(record,typeof(Header));
        if(h.Provider!=Bthport || h.Id!=402) return null;
        ushort size=unchecked((ushort)Marshal.ReadInt16(record,86));
        if(h.Version!=0 || size<8) throw new InvalidOperationException("Unsupported BTHPORT event");
        IntPtr prefix=Marshal.ReadIntPtr(record,96);
        if(prefix==IntPtr.Zero) throw new InvalidOperationException("Missing BTHPORT prefix");
        uint dataLength=unchecked((uint)Marshal.ReadInt32(prefix,4));
        if(dataLength!=(uint)(size-8)) throw new InvalidOperationException("BTHPORT prefix length mismatch");
        // Read only bytes 0..7. BIP_Data starts at byte 8 and is never read.
        return ((h.Timestamp-origin)*1000.0/frequency).ToString("F3",CultureInfo.InvariantCulture)+","+
            Marshal.ReadByte(prefix,0)+","+unchecked((ushort)Marshal.ReadInt16(prefix,1))+","+
            Marshal.ReadByte(prefix,3)+","+dataLength;
    }

    static string AttRow(IntPtr record,long origin,long frequency) {
        Header h=(Header)Marshal.PtrToStructure(record,typeof(Header));
        if(h.Provider!=Bthport || h.Id!=402) return null;
        ushort size=unchecked((ushort)Marshal.ReadInt16(record,86));
        if(h.Version!=0 || size<8) throw new InvalidOperationException("Unsupported BTHPORT event");
        IntPtr prefix=Marshal.ReadIntPtr(record,96);
        if(prefix==IntPtr.Zero) throw new InvalidOperationException("Missing BTHPORT prefix");
        uint dataLength=unchecked((uint)Marshal.ReadInt32(prefix,4));
        if(dataLength!=(uint)(size-8)) throw new InvalidOperationException("BTHPORT prefix length mismatch");
        byte kind=Marshal.ReadByte(prefix,3);
        // These are observed transport types, not an asserted direction map.
        // Do not inspect command/event bodies, which can contain addresses.
        if((kind!=3 && kind!=4) || dataLength<9) return null;
        IntPtr acl=IntPtr.Add(prefix,8);
        ushort handleFlags=unchecked((ushort)Marshal.ReadInt16(acl,0));
        ushort aclLength=unchecked((ushort)Marshal.ReadInt16(acl,2));
        int pb=(handleFlags>>12)&3, bc=(handleFlags>>14)&3;
        if(bc!=0 || (pb!=0 && pb!=2) || aclLength!=dataLength-4) return null;
        ushort pduLength=unchecked((ushort)Marshal.ReadInt16(acl,4));
        ushort cid=unchecked((ushort)Marshal.ReadInt16(acl,6));
        if(cid!=4 || pduLength<1 || pduLength!=dataLength-8) return null;
        // Whole PDUs only; continuation fragments are excluded. Never copy or
        // print ATT bytes, UUIDs, device names, addresses, or attribute values.
        uint fingerprint=2166136261U;
        for(int i=0;i<pduLength;i++) {
            fingerprint=unchecked((fingerprint^Marshal.ReadByte(acl,8+i))*16777619U);
        }
        return ((h.Timestamp-origin)*1000.0/frequency).ToString("F3",CultureInfo.InvariantCulture)+","+
            kind+","+(handleFlags&0xfff)+","+pb+","+Marshal.ReadByte(acl,8)+","+pduLength+","+fingerprint;
    }

    static string LinkRow(IntPtr record,long origin,long frequency) {
        Header h=(Header)Marshal.PtrToStructure(record,typeof(Header));
        if(h.Provider!=Bthport || h.Id!=402) return null;
        ushort size=unchecked((ushort)Marshal.ReadInt16(record,86));
        if(h.Version!=0 || size<8) throw new InvalidOperationException("Unsupported BTHPORT event");
        IntPtr prefix=Marshal.ReadIntPtr(record,96);
        if(prefix==IntPtr.Zero) throw new InvalidOperationException("Missing BTHPORT prefix");
        uint length=unchecked((uint)Marshal.ReadInt32(prefix,4));
        if(length!=(uint)(size-8)) throw new InvalidOperationException("BTHPORT prefix length mismatch");
        // Type 2 is an observed candidate framing, not a TDH direction enum.
        // Read only event header plus whitelisted numeric link fields. Never
        // read the address fields of Connection Complete or advertising data.
        int kind=Marshal.ReadByte(prefix,3);
        if(kind!=2 || length<2) return null;
        IntPtr packet=IntPtr.Add(prefix,8);
        int code=Marshal.ReadByte(packet,0), plen=Marshal.ReadByte(packet,1);
        if(plen+2!=length) return null;
        int sub=-1,status=-1,handle=-1,detail=-1,interval=-1,latency=-1,timeout=-1;
        if(code==5 && plen==4) {
            status=Marshal.ReadByte(packet,2);
            if(status==0) {
                handle=unchecked((ushort)Marshal.ReadInt16(packet,3));
                detail=Marshal.ReadByte(packet,5);
            }
        } else if(code==0x3e && plen>=1) {
            sub=Marshal.ReadByte(packet,2);
            if(!((sub==1 && plen==19) || (sub==10 && plen==31))) return null;
            status=Marshal.ReadByte(packet,3);
            if(status==0) {
                handle=unchecked((ushort)Marshal.ReadInt16(packet,4));
                detail=Marshal.ReadByte(packet,6); // role
                int offset=sub==1?14:26; // skip all address bytes
                interval=unchecked((ushort)Marshal.ReadInt16(packet,offset));
                latency=unchecked((ushort)Marshal.ReadInt16(packet,offset+2));
                timeout=unchecked((ushort)Marshal.ReadInt16(packet,offset+4));
                if(detail>1 || interval<6 || interval>3200 || latency>499 || timeout<10 || timeout>3200) return null;
            }
        } else return null;
        if(handle>4095) return null;
        return ((h.Timestamp-origin)*1000.0/frequency).ToString("F3",CultureInfo.InvariantCulture)+","+
            kind+","+code+","+sub+","+status+","+handle+","+detail+","+interval+","+latency+","+timeout;
    }

    public static void ValidateLinkSummary() {
        ValidateTransportCounters();
        IntPtr record=Marshal.AllocHGlobal(112), payload=Marshal.AllocHGlobal(41);
        try {
            Marshal.Copy(new byte[112],0,record,112);
            Marshal.Copy(new byte[41],0,payload,41);
            Header h=new Header {Provider=Bthport,Id=402,Timestamp=2000};
            Marshal.StructureToPtr(h,record,false);
            Marshal.WriteIntPtr(record,96,payload);
            Marshal.WriteInt16(record,86,29);Marshal.WriteByte(payload,3,2);Marshal.WriteInt32(payload,4,21);
            byte[] connection={0x3e,19,1,0,1,0,1,0, 0xaa,0xbb,0xcc,0xdd,0xee,0xff, 24,0,0,0,244,1,0};
            Marshal.Copy(connection,0,IntPtr.Add(payload,8),21);
            if(LinkRow(record,1000,1000)!="1000.000,2,62,1,0,1,1,24,0,500")
                throw new InvalidOperationException("Link projection failed");
            // Address bytes are irrelevant to the projection.
            for(int i=16;i<22;i++) Marshal.WriteByte(payload,i,0x55);
            if(LinkRow(record,1000,1000)!="1000.000,2,62,1,0,1,1,24,0,500")
                throw new InvalidOperationException("Address affected link projection");
            Marshal.WriteByte(payload,11,0x3e); // failure: other fields undefined
            if(LinkRow(record,1000,1000)!="1000.000,2,62,1,62,-1,-1,-1,-1,-1")
                throw new InvalidOperationException("Undefined failed-link fields retained");
            Marshal.WriteInt16(record,86,41);Marshal.WriteInt32(payload,4,33);
            Marshal.Copy(new byte[33],0,IntPtr.Add(payload,8),33);
            Marshal.WriteByte(payload,8,0x3e);Marshal.WriteByte(payload,9,31);Marshal.WriteByte(payload,10,10);
            Marshal.WriteInt16(payload,12,2);Marshal.WriteByte(payload,14,0);
            for(int i=16;i<34;i++) Marshal.WriteByte(payload,i,0xab);
            Marshal.WriteInt16(payload,34,40);Marshal.WriteInt16(payload,36,2);Marshal.WriteInt16(payload,38,500);
            if(LinkRow(record,1000,1000)!="1000.000,2,62,10,0,2,0,40,2,500")
                throw new InvalidOperationException("Enhanced link projection failed");
            Marshal.WriteByte(payload,10,2); // advertising body must be ignored
            if(LinkRow(record,1000,1000)!=null) throw new InvalidOperationException("Advertising body accepted");
            Marshal.WriteInt16(record,86,14);Marshal.WriteInt32(payload,4,6);
            Marshal.Copy(new byte[] {5,4,0,1,0,19},0,IntPtr.Add(payload,8),6);
            if(LinkRow(record,1000,1000)!="1000.000,2,5,-1,0,1,19,-1,-1,-1")
                throw new InvalidOperationException("Disconnect projection failed");
            Marshal.WriteByte(payload,9,3);
            if(LinkRow(record,1000,1000)!=null) throw new InvalidOperationException("Truncated link event accepted");
            Marshal.WriteByte(payload,3,3);
            if(LinkRow(record,1000,1000)!=null) throw new InvalidOperationException("ACL inspected as event");
            h.Id=403;Marshal.StructureToPtr(h,record,false);Marshal.WriteIntPtr(record,96,new IntPtr(1));
            if(LinkRow(record,1000,1000)!=null) throw new InvalidOperationException("Unrelated event inspected");
        } finally {Marshal.FreeHGlobal(payload);Marshal.FreeHGlobal(record);}
        Console.WriteLine("BLE_ETW_METADATA:LINK_SUMMARY_VALIDATE_PASS");
    }

    public static void ValidateAttSummary() {
        ValidateTransportCounters();
        IntPtr record=Marshal.AllocHGlobal(112), payload=Marshal.AllocHGlobal(19);
        try {
            Marshal.Copy(new byte[112],0,record,112);
            byte[] data={1,4,0,3,11,0,0,0, 1,0x20,7,0, 3,0,4,0, 10,3,0};
            Marshal.Copy(data,0,payload,data.Length);
            Header h=new Header {Provider=Bthport,Id=402,Timestamp=2000};
            Marshal.StructureToPtr(h,record,false);
            Marshal.WriteInt16(record,86,19);Marshal.WriteIntPtr(record,96,payload);
            if(AttRow(record,1000,1000)!="1000.000,3,1,2,10,3,3853939470")
                throw new InvalidOperationException("ATT summary projection failed");
            Marshal.WriteByte(payload,9,0x10); // continuation, not a whole PDU
            if(AttRow(record,1000,1000)!=null) throw new InvalidOperationException("ATT continuation accepted");
            Marshal.WriteByte(payload,9,0x20);
            Marshal.WriteByte(payload,14,5); // other L2CAP channel
            if(AttRow(record,1000,1000)!=null) throw new InvalidOperationException("Non-ATT channel accepted");
            Marshal.WriteByte(payload,14,4);
            Marshal.WriteByte(payload,12,4); // L2CAP length mismatch
            if(AttRow(record,1000,1000)!=null) throw new InvalidOperationException("Malformed L2CAP accepted");
            Marshal.WriteByte(payload,12,3);
            Marshal.WriteByte(payload,3,1); // command body must not be inspected
            if(AttRow(record,1000,1000)!=null) throw new InvalidOperationException("Command body accepted");
            h.Id=403;Marshal.StructureToPtr(h,record,false);
            Marshal.WriteIntPtr(record,96,new IntPtr(1));
            if(AttRow(record,1000,1000)!=null) throw new InvalidOperationException("Unrelated event inspected");
        } finally {Marshal.FreeHGlobal(payload);Marshal.FreeHGlobal(record);}
        Console.WriteLine("BLE_ETW_METADATA:ATT_SUMMARY_VALIDATE_PASS");
    }

    static string Row(IntPtr record, long origin, long frequency) {
        Header h = (Header)Marshal.PtrToStructure(record, typeof(Header));
        // EVENT_RECORD.UserDataLength is fixed metadata at offset 86.
        // Never dereference UserData (offset 96), even for diagnostic events.
        ushort length = unchecked((ushort)Marshal.ReadInt16(record, 86));
        return String.Join(",", new string[] {
            ((h.Timestamp-origin)*1000.0/frequency).ToString("F3", System.Globalization.CultureInfo.InvariantCulture),
            h.Provider.ToString(), h.Id.ToString(), h.Opcode.ToString(),
            h.Task.ToString(), h.Level.ToString(), h.ProcessId.ToString(), length.ToString()
        });
    }

    public static void Validate() {
        if (IntPtr.Size != 8 || Marshal.SizeOf(typeof(Header)) != 80 ||
            Marshal.SizeOf(typeof(Logfile)) != 448)
            throw new InvalidOperationException("ETW collector requires Windows x64 ABI");
        IntPtr record = Marshal.AllocHGlobal(112);
        try {
            Marshal.Copy(new byte[112], 0, record, 112);
            Header h = new Header { ProcessId=7, Timestamp=2000, Id=11, Opcode=3, Task=4, Level=5 };
            Marshal.StructureToPtr(h, record, false);
            Marshal.WriteInt16(record, 86, unchecked((short)65535));
            // Poison pointer makes accidental payload dereference fail rather
            // than silently passing a test with an empty payload.
            Marshal.WriteIntPtr(record, 96, new IntPtr(1));
            string row = Row(record, 1000, 1000);
            if (row != "1000.000,00000000-0000-0000-0000-000000000000,11,3,4,5,7,65535")
                throw new InvalidOperationException("ETW metadata projection failed");
        } finally { Marshal.FreeHGlobal(record); }
        Console.WriteLine("BLE_ETW_METADATA:VALIDATE_PASS");
    }

    public static void Collect(string path, int seconds, bool transportCounters, bool attSummary, bool linkSummary) {
        Validate();
        if(transportCounters) ValidateTransportCounters();
        if(attSummary) ValidateAttSummary();
        if(linkSummary) ValidateLinkSummary();
        string name = "UnoR4BleMetadata-" + Guid.NewGuid().ToString("N");
        IntPtr properties = Marshal.AllocHGlobal(1024);
        IntPtr loggerName = IntPtr.Zero;
        ulong controller = 0, consumer = UInt64.MaxValue;
        Thread worker = null;
        StreamWriter writer = null;
        StreamWriter transportWriter = null;
        StreamWriter attWriter = null;
        StreamWriter linkWriter = null;
        EventCallback callback = null;
        uint processResult = 0, stopResult = 0;
        int callbackErrors = 0, events = 0, eventsLost = 0, buffersLost = 0;
        long frequency, origin, originAfter;
        QueryPerformanceFrequency(out frequency);
        QueryPerformanceCounter(out origin);
        DateTime originUtc = DateTime.UtcNow;
        QueryPerformanceCounter(out originAfter);
        double originBracketMs = (originAfter - origin) * 1000.0 / frequency;
        // A previous run's sidecar must never be accepted for this capture.
        if (File.Exists(path + ".summary.json")) File.Delete(path + ".summary.json");
        if (File.Exists(path + ".transport.csv")) File.Delete(path + ".transport.csv");
        if (File.Exists(path + ".att.csv")) File.Delete(path + ".att.csv");
        if (File.Exists(path + ".links.csv")) File.Delete(path + ".links.csv");
        try {
            Marshal.Copy(new byte[1024], 0, properties, 1024);
            // EVENT_TRACE_PROPERTIES x64: WNODE_HEADER=48, whole struct=120.
            Marshal.WriteInt32(properties, 0, 1024);
            Marshal.WriteInt32(properties, 40, 1);       // QPC clock
            Marshal.WriteInt32(properties, 44, 0x20000); // WNODE_FLAG_TRACED_GUID
            Marshal.WriteInt32(properties, 48, 64);     // 64 KiB per buffer
            Marshal.WriteInt32(properties, 52, 2);
            Marshal.WriteInt32(properties, 56, 8);
            Marshal.WriteInt32(properties, 64, 0x100);   // REAL_TIME, no file
            Marshal.WriteInt32(properties, 116, 120);   // LoggerNameOffset
            byte[] nameBytes = Encoding.Unicode.GetBytes(name + "\0");
            Marshal.Copy(nameBytes, 0, IntPtr.Add(properties, 120), nameBytes.Length);
            uint result = StartTrace(out controller, name, properties);
            if (result != 0) {
                controller = 0;
                throw new InvalidOperationException("ETW StartTrace failed: " + result);
            }
            Guid[] providers = {
                new Guid("0602ECEF-6381-4BC0-AEDA-EB9BB919B276"), // Bluetooth Policy
                new Guid("DB25B328-A6F6-444F-9D97-A50E20217D16"), // Bthmini
                new Guid("33693E1D-246A-471B-83BE-3E75F47A832D"), // BTHUSB
                new Guid("8A1F9517-3A8C-4A9E-A018-4F17A200F277")  // BTHPORT HCI/L2CAP
            };
            foreach (Guid value in providers) {
                Guid provider = value;
                result = EnableTraceEx2(controller, ref provider, 1, 5, UInt64.MaxValue, 0, 0, IntPtr.Zero);
                if (result != 0) throw new InvalidOperationException("ETW EnableTrace failed: " + result);
            }
            writer = new StreamWriter(path, false, new UTF8Encoding(false));
            writer.AutoFlush = true;
            writer.WriteLine("elapsed_ms,provider,event_id,opcode,task,level,process_id,user_data_length");
            if(transportCounters) {
                transportWriter=new StreamWriter(path+".transport.csv",false,new UTF8Encoding(false));
                transportWriter.AutoFlush=true;
                transportWriter.WriteLine("elapsed_ms,command_packets_allowed,acl_write_credits,bip_type,bip_data_length");
            }
            if(attSummary) {
                attWriter=new StreamWriter(path+".att.csv",false,new UTF8Encoding(false));
                attWriter.AutoFlush=true;
                attWriter.WriteLine("elapsed_ms,bip_type,connection_handle,pb_flag,att_opcode,att_length,fnv1a");
            }
            if(linkSummary) {
                linkWriter=new StreamWriter(path+".links.csv",false,new UTF8Encoding(false));
                linkWriter.AutoFlush=true;
                linkWriter.WriteLine("elapsed_ms,bip_type,event_code,subevent,status,connection_handle,detail,interval,latency,supervision_timeout");
            }
            callback = delegate(IntPtr record) {
                try {
                    if(linkWriter!=null) {
                        string link=LinkRow(record,origin,frequency);
                        if(link!=null) linkWriter.WriteLine(link);
                    }
                    if(attWriter!=null) {
                        string att=AttRow(record,origin,frequency);
                        if(att!=null) attWriter.WriteLine(att);
                    }
                    if(transportWriter!=null) {
                        string transport=TransportRow(record,origin,frequency);
                        if(transport!=null) transportWriter.WriteLine(transport);
                    }
                    writer.WriteLine(Row(record, origin, frequency)); ++events;
                }
                catch { Interlocked.Increment(ref callbackErrors); }
            };
            loggerName = Marshal.StringToHGlobalUni(name);
            // RAW_TIMESTAMP preserves the QPC clock selected above.
            Logfile logfile = new Logfile { LoggerName=loggerName, Mode=0x10001100,
                Callback=Marshal.GetFunctionPointerForDelegate(callback) };
            consumer = OpenTrace(ref logfile);
            if (consumer == UInt64.MaxValue)
                throw new InvalidOperationException("ETW OpenTrace failed");
            ulong consumerHandle = consumer;
            worker = new Thread(delegate() {
                processResult = ProcessTrace(new ulong[] {consumerHandle}, 1, IntPtr.Zero, IntPtr.Zero);
            });
            worker.IsBackground = true;
            worker.Start();
            Console.WriteLine("BLE_ETW_METADATA:ORIGIN_UTC=" + originUtc.ToString("o"));
            Console.WriteLine("BLE_ETW_METADATA:ORIGIN_BRACKET_MS=" + originBracketMs.ToString("R", CultureInfo.InvariantCulture));
            Console.WriteLine("BLE_ETW_METADATA:READY_UTC=" + DateTime.UtcNow.ToString("o"));
            for (int i=0; i<seconds; ++i) {
                if (!worker.IsAlive) throw new InvalidOperationException("ETW consumer stopped early: " + processResult);
                Thread.Sleep(1000);
            }
        } finally {
            if (controller != 0) {
                stopResult = ControlTrace(controller, name, properties, 1);
                eventsLost = Marshal.ReadInt32(properties, 88);
                buffersLost = Marshal.ReadInt32(properties, 100);
            }
            if (consumer != UInt64.MaxValue) CloseTrace(consumer);
            if (worker != null && !worker.Join(10000))
                throw new InvalidOperationException("ETW consumer did not stop");
            if (writer != null) writer.Dispose();
            if (transportWriter != null) transportWriter.Dispose();
            if (attWriter != null) attWriter.Dispose();
            if (linkWriter != null) linkWriter.Dispose();
            GC.KeepAlive(callback);
            if (loggerName != IntPtr.Zero) Marshal.FreeHGlobal(loggerName);
            Marshal.FreeHGlobal(properties);
        }
        Console.WriteLine("BLE_ETW_METADATA:COUNTS=" + events + "," + eventsLost + "," + buffersLost);
        bool passed = events > 0 && stopResult == 0 && callbackErrors == 0 && eventsLost == 0 && buffersLost == 0 &&
            (processResult == 0 || processResult == 1223);
        File.WriteAllText(path + ".summary.json", "{\"schema\":1,\"passed\":" + (passed ? "true" : "false") +
            ",\"origin_utc\":\"" + originUtc.ToString("o") + "\",\"origin_bracket_ms\":" + originBracketMs.ToString("R", CultureInfo.InvariantCulture) +
            ",\"events\":" + events + ",\"events_lost\":" + eventsLost + ",\"buffers_lost\":" + buffersLost +
            ",\"callback_errors\":" + callbackErrors + ",\"stop_result\":" + stopResult + ",\"process_result\":" + processResult + "}\n");
        if (!passed)
            throw new InvalidOperationException("ETW collection failed: " + stopResult + "," + processResult + "," + callbackErrors);
        Console.WriteLine("BLE_ETW_METADATA:PASS");
    }
}
'@
if ($ValidateOnly) {
    [UnoR4BleEtwMetadata]::Validate()
    if ($HciTransportCounters) { [UnoR4BleEtwMetadata]::ValidateTransportCounters() }
    if ($AttPacketSummary) { [UnoR4BleEtwMetadata]::ValidateAttSummary() }
    if ($HciLinkSummary) { [UnoR4BleEtwMetadata]::ValidateLinkSummary() }
} else {
    if (-not $OutputPath) { throw 'Provide -OutputPath for metadata CSV' }
    [UnoR4BleEtwMetadata]::Collect($OutputPath, $Seconds, $HciTransportCounters.IsPresent, $AttPacketSummary.IsPresent, $HciLinkSummary.IsPresent)
}
