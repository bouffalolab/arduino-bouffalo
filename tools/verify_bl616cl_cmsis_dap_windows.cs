using System;
using System.Collections.Generic;
using System.IO.Ports;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

public static class CmsisDapWindowsProbe
{
    private const uint DIGCF_PRESENT = 0x00000002;
    private const uint DIGCF_DEVICEINTERFACE = 0x00000010;
    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x00000001;
    private const uint FILE_SHARE_WRITE = 0x00000002;
    private const uint OPEN_EXISTING = 3;
    private const uint FILE_FLAG_OVERLAPPED = 0x40000000;
    private const uint ERROR_IO_PENDING = 997;
    private const uint WAIT_OBJECT_0 = 0;
    private const uint WAIT_TIMEOUT = 258;
    private const int CrSuccess = 0;

    private const byte DapInfo = 0x00;
    private const byte DapConnect = 0x02;
    private const byte DapDisconnect = 0x03;
    private const byte DapTransferConfigure = 0x04;
    private const byte DapTransfer = 0x05;
    private const byte DapTransferBlock = 0x06;
    private const byte DapWriteAbort = 0x08;
    private const byte DapSwjClock = 0x11;
    private const byte DapSwjSequence = 0x12;
    private const byte DapSwdConfigure = 0x13;
    private const byte DapSwdSequence = 0x1D;
    private const byte DapPortSwd = 0x01;
    private const byte DapTransferOk = 0x01;
    private const byte DapTransferDpReadIdcode = 0x02;
    private const byte DapTransferDpWriteCtrlStat = 0x04;
    private const byte DapTransferDpReadCtrlStat = 0x06;
    private const byte DapTransferDpWriteSelect = 0x08;
    private const byte DapTransferDpReadRdbuff = 0x0E;
    private const byte DapTransferApReadCsw = 0x03;
    private const byte DapTransferApWriteCsw = 0x01;
    private const byte DapTransferApReadTar = 0x07;
    private const byte DapTransferApWriteTar = 0x05;
    private const byte DapTransferApReadDrw = 0x0F;
    private const byte DapSwjPinSwclk = 0x01;
    private const byte DapSwjPinSwdio = 0x02;
    private const byte DapSwjPinReset = 0x80;
    private const byte DapSwdSequenceDin = 0x80;
    private const int PacketSize = 64;
    private const int WindowsReportSize = PacketSize + 1;
    private const byte DiagnosticFeatureVersion = 35;
    private const uint DpAbortStickyClear = 0x0000001E;
    private const uint DpCtrlStatPowerUpRequest = 0x50000000;
    private const uint DpCtrlStatPowerUpAcknowledge = 0xA0000000;
    private const uint DpCtrlStatPowerUpMask = 0xF0000000;
    private const uint AhbApCswWordIncrement = 0x23000052;
    private const uint ScsCpuidAddress = 0xE000ED00;
    private const uint CoreDebugDhcsrAddress = 0xE000EDF0;
    private const uint DefaultFlashVectorAddress = 0x00000000;
    private const uint DefaultSramAddress = 0x20000000;
    private const uint DhcsrDebugKey = 0xA05F0000;
    private const uint DhcsrCDebugen = 0x00000001;
    private const uint DhcsrCHalt = 0x00000002;
    private const uint DhcsrSHalt = 0x00020000;

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDeviceInterfaceData
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDevInfoData
    {
        public int cbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public IntPtr Reserved;
    }

    [DllImport("hid.dll")]
    private static extern void HidD_GetHidGuid(out Guid hidGuid);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_GetFeature(
        SafeFileHandle handle,
        byte[] reportBuffer,
        int reportBufferLength);

    [DllImport("hid.dll", SetLastError = true)]
    private static extern bool HidD_SetFeature(
        SafeFileHandle handle,
        byte[] reportBuffer,
        int reportBufferLength);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(
        ref Guid classGuid,
        IntPtr enumerator,
        IntPtr hwndParent,
        uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr deviceInfoSet,
        IntPtr deviceInfoData,
        ref Guid interfaceClassGuid,
        uint memberIndex,
        ref SpDeviceInterfaceData deviceInterfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(
        IntPtr deviceInfoSet,
        ref SpDeviceInterfaceData deviceInterfaceData,
        IntPtr deviceInterfaceDetailData,
        uint deviceInterfaceDetailDataSize,
        out uint requiredSize,
        IntPtr deviceInfoData);

    [DllImport(
        "setupapi.dll",
        EntryPoint = "SetupDiGetDeviceInterfaceDetail",
        CharSet = CharSet.Auto,
        SetLastError = true)]
    private static extern bool SetupDiGetDeviceInterfaceDetailWithDeviceInfo(
        IntPtr deviceInfoSet,
        ref SpDeviceInterfaceData deviceInterfaceData,
        IntPtr deviceInterfaceDetailData,
        uint deviceInterfaceDetailDataSize,
        out uint requiredSize,
        ref SpDevInfoData deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("cfgmgr32.dll", SetLastError = true)]
    private static extern int CM_Get_Parent(
        out uint parentDevInst,
        uint devInst,
        uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern int CM_Get_Device_ID(
        uint devInst,
        StringBuilder buffer,
        int bufferLength,
        uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [StructLayout(LayoutKind.Sequential)]
    private struct Overlapped
    {
        public IntPtr Internal;
        public IntPtr InternalHigh;
        public uint Offset;
        public uint OffsetHigh;
        public IntPtr Event;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateEvent(
        IntPtr eventAttributes,
        bool manualReset,
        bool initialState,
        string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(
        SafeFileHandle file,
        byte[] buffer,
        uint numberOfBytesToWrite,
        out uint numberOfBytesWritten,
        ref Overlapped overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadFile(
        SafeFileHandle file,
        byte[] buffer,
        uint numberOfBytesToRead,
        out uint numberOfBytesRead,
        ref Overlapped overlapped);

    [DllImport("kernel32.dll", EntryPoint = "WriteFile", SetLastError = true)]
    private static extern bool WriteFileSynchronous(
        SafeFileHandle file,
        byte[] buffer,
        uint numberOfBytesToWrite,
        out uint numberOfBytesWritten,
        IntPtr overlapped);

    [DllImport("kernel32.dll", EntryPoint = "ReadFile", SetLastError = true)]
    private static extern bool ReadFileSynchronous(
        SafeFileHandle file,
        byte[] buffer,
        uint numberOfBytesToRead,
        out uint numberOfBytesRead,
        IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetOverlappedResult(
        SafeFileHandle file,
        ref Overlapped overlapped,
        out uint numberOfBytesTransferred,
        bool wait);

    private static string GetDeviceInstanceId(uint devInst)
    {
        StringBuilder buffer = new StringBuilder(512);
        if (CM_Get_Device_ID(devInst, buffer, buffer.Capacity, 0) != CrSuccess)
        {
            return string.Empty;
        }
        return buffer.ToString();
    }

    private static string CmsisDapTargetSerial()
    {
        string requested = Environment.GetEnvironmentVariable(
            "CMSIS_DAP_TARGET_SERIAL");
        return String.IsNullOrWhiteSpace(requested)
            ? "BL616CL-PROVISIONAL"
            : requested.Trim();
    }

    private static string CmsisDapTargetInterface()
    {
        string requested = Environment.GetEnvironmentVariable(
            "CMSIS_DAP_TARGET_INTERFACE");
        if (String.IsNullOrWhiteSpace(requested))
        {
            return "02";
        }
        requested = requested.Trim();
        if (requested.Length != 2 ||
            requested[0] < '0' || requested[0] > '9' ||
            requested[1] < '0' || requested[1] > '9')
        {
            throw new InvalidOperationException(
                "CMSIS_DAP_TARGET_INTERFACE must contain exactly two decimal digits");
        }
        return requested;
    }

    private static bool SkipSwjPinReadback()
    {
        return string.Equals(
            Environment.GetEnvironmentVariable("CMSIS_DAP_SKIP_SWJ_PIN_READBACK"),
            "1",
            StringComparison.Ordinal);
    }

    private static bool IsCmsisDapTargetDevice(uint devInst, string serial)
    {
        string expectedInstanceSuffix =
            "USB\\VID_2341&PID_1002\\" + serial;
        for (int level = 0; level != 8; ++level)
        {
            string instanceId = GetDeviceInstanceId(devInst);
            if (instanceId.IndexOf(
                expectedInstanceSuffix,
                StringComparison.OrdinalIgnoreCase) >= 0)
            {
                return true;
            }

            uint parent;
            if (CM_Get_Parent(out parent, devInst, 0) != CrSuccess)
            {
                break;
            }
            devInst = parent;
        }
        return false;
    }

    private static string FindCmsisDapPath()
    {
        string targetSerial = CmsisDapTargetSerial();
        string targetInterface = CmsisDapTargetInterface();
        string expectedHidPathToken =
            "vid_2341&pid_1002&mi_" + targetInterface;
        bool allowUnserializedTarget = string.Equals(
            Environment.GetEnvironmentVariable(
                "BL616CL_CMSIS_DAP_ALLOW_UNSERIALIZED_TARGET"),
            "1",
            StringComparison.Ordinal);
        List<string> unserializedCandidates = new List<string>();

        Guid hidGuid;
        HidD_GetHidGuid(out hidGuid);
        IntPtr deviceSet = SetupDiGetClassDevs(
            ref hidGuid,
            IntPtr.Zero,
            IntPtr.Zero,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (deviceSet == new IntPtr(-1))
        {
            throw new InvalidOperationException("SetupDiGetClassDevs failed");
        }

        try
        {
            for (uint index = 0; ; ++index)
            {
                SpDeviceInterfaceData interfaceData = new SpDeviceInterfaceData();
                interfaceData.cbSize = Marshal.SizeOf(typeof(SpDeviceInterfaceData));
                if (!SetupDiEnumDeviceInterfaces(
                    deviceSet,
                    IntPtr.Zero,
                    ref hidGuid,
                    index,
                    ref interfaceData))
                {
                    break;
                }

                uint requiredSize;
                SetupDiGetDeviceInterfaceDetail(
                    deviceSet,
                    ref interfaceData,
                    IntPtr.Zero,
                    0,
                    out requiredSize,
                    IntPtr.Zero);
                if (requiredSize == 0)
                {
                    continue;
                }

                IntPtr detailData = Marshal.AllocHGlobal((int)requiredSize);
                try
                {
                    Marshal.WriteInt32(detailData, IntPtr.Size == 8 ? 8 : 6);
                    uint ignored;
                    SpDevInfoData deviceInfo = new SpDevInfoData();
                    deviceInfo.cbSize = Marshal.SizeOf(typeof(SpDevInfoData));
                    if (!SetupDiGetDeviceInterfaceDetailWithDeviceInfo(
                        deviceSet,
                        ref interfaceData,
                        detailData,
                        requiredSize,
                        out ignored,
                        ref deviceInfo))
                    {
                        continue;
                    }

                    string path = Marshal.PtrToStringAuto(
                        IntPtr.Add(detailData, 4));
                    if (path != null &&
                        path.IndexOf(
                            expectedHidPathToken,
                            StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        if (IsCmsisDapTargetDevice(deviceInfo.DevInst, targetSerial))
                        {
                            return path;
                        }

                        if (allowUnserializedTarget)
                        {
                            unserializedCandidates.Add(path);
                        }
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(detailData);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(deviceSet);
        }

        if (allowUnserializedTarget && unserializedCandidates.Count == 1)
        {
            Console.WriteLine(
                "WARNING: using the only VID_2341:PID_1002 MI_02 HID target " +
                "because BL616CL_CMSIS_DAP_ALLOW_UNSERIALIZED_TARGET=1");
            return unserializedCandidates[0];
        }

        if (allowUnserializedTarget && unserializedCandidates.Count > 1)
        {
            throw new InvalidOperationException(
                "multiple unserialized VID_2341:PID_1002 MI_02 HID interfaces " +
                "are present; refusing to select a CMSIS-DAP target");
        }

        throw new InvalidOperationException(
            "CMSIS-DAP HID interface for serial " + targetSerial +
            " (VID_2341 PID_1002 MI_" + targetInterface + ") was not found");
    }

    private static void CompleteIo(
        SafeFileHandle handle,
        byte[] buffer,
        bool write,
        out uint transferred)
    {
        bool completed = write
            ? WriteFileSynchronous(
                handle,
                buffer,
                (uint)buffer.Length,
                out transferred,
                IntPtr.Zero)
            : ReadFileSynchronous(
                handle,
                buffer,
                (uint)buffer.Length,
                out transferred,
                IntPtr.Zero);
        if (!completed)
        {
            throw new InvalidOperationException(
                (write ? "WriteFile" : "ReadFile") +
                " failed, win32=" + Marshal.GetLastWin32Error());
        }
    }

    private static byte[] Transact(SafeFileHandle handle, params byte[] request)
    {
        if (request.Length > PacketSize)
        {
            throw new ArgumentException("CMSIS-DAP request exceeds 64 bytes");
        }

        byte[] packet = new byte[WindowsReportSize];
        packet[0] = 0;
        Array.Copy(request, 0, packet, 1, request.Length);
        uint written;
        CompleteIo(handle, packet, true, out written);
        if (written != packet.Length)
        {
            throw new InvalidOperationException("short CMSIS-DAP write");
        }

        byte[] response = new byte[WindowsReportSize];
        uint read;
        CompleteIo(handle, response, false, out read);
        if (read != response.Length)
        {
            throw new InvalidOperationException("short CMSIS-DAP response");
        }
        if (response[1] != request[0])
        {
            throw new InvalidOperationException(
                "unexpected CMSIS-DAP response command 0x" + response[1].ToString("X2"));
        }
        byte[] payload = new byte[PacketSize];
        Array.Copy(response, 1, payload, 0, PacketSize);
        return payload;
    }

    private static void RequireOk(byte[] response, string name)
    {
        if (response[1] != 0)
        {
            throw new InvalidOperationException(
                name + " failed with status 0x" + response[1].ToString("X2"));
        }
    }

    private static byte[] DapInfoRequest(SafeFileHandle handle, byte id)
    {
        byte[] response = Transact(handle, DapInfo, id);
        int length = response[1];
        if (length > PacketSize - 2)
        {
            throw new InvalidOperationException("DAP_Info returned an invalid length");
        }
        byte[] value = new byte[length];
        Array.Copy(response, 2, value, 0, length);
        return value;
    }

    private static void SwjSequence(SafeFileHandle handle, int bitCount, byte[] data)
    {
        byte[] request = new byte[data.Length + 2];
        request[0] = DapSwjSequence;
        request[1] = (byte)bitCount;
        Array.Copy(data, 0, request, 2, data.Length);
        RequireOk(Transact(handle, request), "DAP_SWJ_Sequence");
    }

    private static byte SwjPins(
        SafeFileHandle handle,
        byte value,
        byte select,
        uint waitMilliseconds)
    {
        byte[] request = new byte[7];
        request[0] = 0x10; // DAP_SWJ_Pins
        request[1] = value;
        request[2] = select;
        request[3] = (byte)(waitMilliseconds & 0xff);
        request[4] = (byte)((waitMilliseconds >> 8) & 0xff);
        request[5] = (byte)((waitMilliseconds >> 16) & 0xff);
        request[6] = (byte)((waitMilliseconds >> 24) & 0xff);
        byte[] response = Transact(handle, request);
        return response[1];
    }

    private static void SetSwjClock(SafeFileHandle handle, uint hertz)
    {
        RequireOk(
            Transact(
                handle,
                DapSwjClock,
                (byte)(hertz & 0xff),
                (byte)((hertz >> 8) & 0xff),
                (byte)((hertz >> 16) & 0xff),
                (byte)((hertz >> 24) & 0xff)),
            "DAP_SWJ_Clock");
    }

    private static void WriteU32Le(byte[] bytes, int offset, uint value)
    {
        bytes[offset] = (byte)(value & 0xff);
        bytes[offset + 1] = (byte)((value >> 8) & 0xff);
        bytes[offset + 2] = (byte)((value >> 16) & 0xff);
        bytes[offset + 3] = (byte)((value >> 24) & 0xff);
    }

    private static uint TransferRead(
        SafeFileHandle handle,
        byte request,
        string name)
    {
        byte[] response = Transact(
            handle,
            DapTransfer,
            0x00,
            0x01,
            request);
        if (response[1] != 1 || response[2] != DapTransferOk)
        {
            throw new InvalidOperationException(
                name + " failed: count=" + response[1] +
                " ack=0x" + response[2].ToString("X2"));
        }
        return ReadU32Le(response, 3);
    }

    private static void TransferWrite(
        SafeFileHandle handle,
        byte request,
        uint value,
        string name)
    {
        byte[] transaction = new byte[8];
        transaction[0] = DapTransfer;
        transaction[1] = 0x00;
        transaction[2] = 0x01;
        transaction[3] = request;
        WriteU32Le(transaction, 4, value);
        byte[] response = Transact(handle, transaction);
        if (response[1] != 1 || response[2] != DapTransferOk)
        {
            throw new InvalidOperationException(
                name + " failed: count=" + response[1] +
                " ack=0x" + response[2].ToString("X2"));
        }
    }

    private static void ClearDpStickyErrors(SafeFileHandle handle)
    {
        byte[] request = new byte[6];
        request[0] = DapWriteAbort;
        request[1] = 0x00;
        WriteU32Le(request, 2, DpAbortStickyClear);
        RequireOk(Transact(handle, request), "DAP_WriteABORT");
    }

    private static uint ReadAp(
        SafeFileHandle handle,
        byte bank,
        byte address,
        string name)
    {
        TransferWrite(
            handle,
            DapTransferDpWriteSelect,
            (uint)(bank << 4),
            name + " SELECT");
        return TransferRead(
            handle,
            (byte)(0x01 | 0x02 | (address & 0x0c)),
            name);
    }

    private static void WriteAp(
        SafeFileHandle handle,
        byte bank,
        byte address,
        uint value,
        string name)
    {
        TransferWrite(
            handle,
            DapTransferDpWriteSelect,
            (uint)(bank << 4),
            name + " SELECT");
        TransferWrite(
            handle,
            (byte)(0x01 | (address & 0x0c)),
            value,
            name);
    }

    private static uint ReadAhbWord(
        SafeFileHandle handle,
        uint address,
        string name)
    {
        WriteAp(handle, 0x00, 0x00, AhbApCswWordIncrement, name + " CSW");
        WriteAp(handle, 0x00, 0x04, address, name + " TAR");
        return ReadAp(handle, 0x00, 0x0c, name + " DRW");
    }

    private static void WriteAhbWord(
        SafeFileHandle handle,
        uint address,
        uint value,
        string name)
    {
        WriteAp(handle, 0x00, 0x00, AhbApCswWordIncrement, name + " CSW");
        WriteAp(handle, 0x00, 0x04, address, name + " TAR");
        WriteAp(handle, 0x00, 0x0c, value, name + " DRW");
    }

    private static uint[] ReadAhbBlock(
        SafeFileHandle handle,
        uint address,
        ushort wordCount,
        string name)
    {
        if (wordCount == 0 || wordCount > 15)
        {
            throw new ArgumentOutOfRangeException(
                "wordCount",
                "CMSIS-DAP 64-byte reports support one to fifteen 32-bit words");
        }

        WriteAp(handle, 0x00, 0x00, AhbApCswWordIncrement, name + " CSW");
        WriteAp(handle, 0x00, 0x04, address, name + " TAR");
        byte[] response = Transact(
            handle,
            DapTransferBlock,
            0x00,
            (byte)(wordCount & 0xff),
            (byte)((wordCount >> 8) & 0xff),
            DapTransferApReadDrw);
        ushort completed = (ushort)(response[1] | (response[2] << 8));
        if (completed != wordCount || response[3] != DapTransferOk)
        {
            throw new InvalidOperationException(
                name + " failed: count=" + completed +
                " ack=0x" + response[3].ToString("X2"));
        }

        uint[] values = new uint[wordCount];
        for (int index = 0; index < values.Length; ++index)
        {
            values[index] = ReadU32Le(response, 4 + index * 4);
        }
        return values;
    }

    private static bool EqualWords(uint[] left, uint[] right)
    {
        if (left.Length != right.Length)
        {
            return false;
        }
        for (int index = 0; index < left.Length; ++index)
        {
            if (left[index] != right[index])
            {
                return false;
            }
        }
        return true;
    }

    private static string FormatWords(uint[] values)
    {
        string[] text = new string[values.Length];
        for (int index = 0; index < values.Length; ++index)
        {
            text[index] = "0x" + values[index].ToString("X8");
        }
        return string.Join(",", text);
    }

    private static void RunD1ToD3ReadOnly(
        SafeFileHandle handle,
        uint hertz)
    {
        ClearDpStickyErrors(handle);
        TransferWrite(
            handle,
            DapTransferDpWriteCtrlStat,
            DpCtrlStatPowerUpRequest,
            "DP CTRL/STAT power-up request");

        uint ctrlStat = 0;
        for (int attempt = 0; attempt != 10; ++attempt)
        {
            ctrlStat = TransferRead(
                handle,
                DapTransferDpReadCtrlStat,
                "DP CTRL/STAT read");
            if ((ctrlStat & DpCtrlStatPowerUpAcknowledge) ==
                DpCtrlStatPowerUpAcknowledge)
            {
                break;
            }
            Thread.Sleep(2);
        }
        if ((ctrlStat & DpCtrlStatPowerUpAcknowledge) !=
            DpCtrlStatPowerUpAcknowledge)
        {
            throw new InvalidOperationException(
                "DP CTRL/STAT power-up acknowledge is absent: 0x" +
                ctrlStat.ToString("X8"));
        }

        TransferWrite(
            handle,
            DapTransferDpWriteSelect,
            0x00000000,
            "DP SELECT bank 0");
        uint rdbuff = TransferRead(handle, DapTransferDpReadRdbuff, "DP RDBUFF read");
        uint apIdr = ReadAp(handle, 0x0f, 0x0c, "AHB-AP IDR");
        uint apBase = ReadAp(handle, 0x0f, 0x08, "AHB-AP BASE");
        uint csw = ReadAp(handle, 0x00, 0x00, "AHB-AP CSW");
        uint tar = ReadAp(handle, 0x00, 0x04, "AHB-AP TAR");
        if (apIdr == 0 || apIdr == 0xffffffff)
        {
            throw new InvalidOperationException(
                "AHB-AP IDR is invalid: 0x" + apIdr.ToString("X8"));
        }
        if ((apBase & 0x00000001) == 0)
        {
            throw new InvalidOperationException(
                "AHB-AP BASE does not report a debug ROM table: 0x" +
                apBase.ToString("X8"));
        }

        uint cpuid = ReadAhbWord(handle, ScsCpuidAddress, "CPUID");
        uint flashVector = ReadAhbWord(
            handle,
            DefaultFlashVectorAddress,
            "Flash vector");
        uint sramWord = ReadAhbWord(handle, DefaultSramAddress, "SRAM word");
        if (cpuid == 0 || cpuid == 0xffffffff || ((cpuid >> 4) & 0x0fff) != 0x0c24)
        {
            throw new InvalidOperationException(
                "unexpected Cortex-M4 CPUID: 0x" + cpuid.ToString("X8"));
        }

        uint romTableAddress = apBase & 0xfffff000;
        uint[] romTable = ReadAhbBlock(
            handle,
            romTableAddress,
            8,
            "ROM table block");
        uint[] blockRetry = ReadAhbBlock(
            handle,
            romTableAddress,
            8,
            "ROM table block retry");
        if (!EqualWords(romTable, blockRetry))
        {
            throw new InvalidOperationException(
                "DAP_TransferBlock ROM table data changed between repeated reads");
        }

        Console.WriteLine(
            "D1 DP: CTRL/STAT=0x" + ctrlStat.ToString("X8") +
            " RDBUFF=0x" + rdbuff.ToString("X8"));
        Console.WriteLine(
            "D2 AHB-AP: IDR=0x" + apIdr.ToString("X8") +
            " BASE=0x" + apBase.ToString("X8") +
            " CSW=0x" + csw.ToString("X8") +
            " TAR=0x" + tar.ToString("X8"));
        Console.WriteLine(
            "D2 memory: CPUID=0x" + cpuid.ToString("X8") +
            " flash_vector=0x" + flashVector.ToString("X8") +
            " sram_20000000=0x" + sramWord.ToString("X8"));
        Console.WriteLine(
            "D3 " + hertz + " Hz ROM table @0x" +
            romTableAddress.ToString("X8") + ": " + FormatWords(romTable));
    }

    private static void SwitchJtagToSwd(SafeFileHandle handle)
    {
        SwjSequence(handle, 56, new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
        SwjSequence(handle, 16, new byte[] { 0x9E, 0xE7 });
        SwjSequence(handle, 56, new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
    }

    private static byte ReadReleasedSwdioBits(
        SafeFileHandle handle,
        byte driveValue,
        byte resetValue)
    {
        const byte SelectPins = DapSwjPinSwclk | DapSwjPinSwdio | DapSwjPinReset;
        byte requestedPins = (byte)(DapSwjPinSwclk | resetValue | driveValue);
        byte drivenPins = SwjPins(handle, requestedPins, SelectPins, 1);
        byte expectedSwdio = driveValue == 0 ? (byte)0 : DapSwjPinSwdio;

        if ((drivenPins & DapSwjPinSwdio) != expectedSwdio)
        {
            throw new InvalidOperationException(
                "SWDIO drive readback mismatch: requested=" +
                (driveValue == 0 ? "LOW" : "HIGH") +
                " pins=0x" + drivenPins.ToString("X2"));
        }

        byte[] response = Transact(
            handle,
            DapSwdSequence,
            0x01,
            (byte)(DapSwdSequenceDin | 0x08));
        RequireOk(response, "DAP_SWD_Sequence");
        return response[2];
    }

    private static string FormatSwdioBits(byte bits)
    {
        char[] text = new char[8];
        for (int index = 0; index < text.Length; ++index)
        {
            text[index] = (bits & (1 << index)) != 0 ? '1' : '0';
        }
        return new string(text);
    }

    private static void PrintReleasedSwdioDiagnostic(
        SafeFileHandle handle,
        bool resetAsserted)
    {
        byte resetValue = resetAsserted ? (byte)0 : DapSwjPinReset;
        byte highAfterRelease = ReadReleasedSwdioBits(handle, DapSwjPinSwdio, resetValue);
        byte lowAfterRelease = ReadReleasedSwdioBits(handle, 0, resetValue);
        Console.WriteLine(
            "SWDIO released sample (" +
            (resetAsserted ? "nRESET asserted" : "nRESET released") +
            "): after-drive-high=" + FormatSwdioBits(highAfterRelease) +
            " (0x" + highAfterRelease.ToString("X2") + ")" +
            " after-drive-low=" + FormatSwdioBits(lowAfterRelease) +
            " (0x" + lowAfterRelease.ToString("X2") + ")");
    }

    private static bool TryReadDpIdcode(
        SafeFileHandle handle,
        uint hertz,
        bool switchWhileResetAsserted,
        ushort resetPulseMilliseconds,
        out uint idcode,
        out byte count,
        out byte ack)
    {
        const byte ResetSelect = DapSwjPinReset;
        idcode = 0;
        count = 0;
        ack = 0;

        SetSwjClock(handle, hertz);
        if (switchWhileResetAsserted)
        {
            byte asserted = SwjPins(handle, 0x00, ResetSelect, resetPulseMilliseconds);
            Console.WriteLine("  nRESET asserted, SWJ pins=0x" + asserted.ToString("X2"));
            SwitchJtagToSwd(handle);
            byte released = SwjPins(handle, ResetSelect, ResetSelect, 20);
            Console.WriteLine("  nRESET released, SWJ pins=0x" + released.ToString("X2"));
            Thread.Sleep(20);
        }
        else
        {
            byte asserted = SwjPins(handle, 0x00, ResetSelect, resetPulseMilliseconds);
            Console.WriteLine("  nRESET asserted, SWJ pins=0x" + asserted.ToString("X2"));
            byte released = SwjPins(handle, ResetSelect, ResetSelect, 20);
            Console.WriteLine("  nRESET released, SWJ pins=0x" + released.ToString("X2"));
            Thread.Sleep(20);
            SwitchJtagToSwd(handle);
        }

        if (!SkipSwjPinReadback())
        {
            PrintReleasedSwdioDiagnostic(handle, false);
            PrintSwjPinState(handle);
        }
        byte[] response = Transact(
            handle,
            DapTransfer,
            0x00,
            0x01,
            DapTransferDpReadIdcode);
        count = response[1];
        ack = response[2];
        if (count != 1 || ack != DapTransferOk)
        {
            return false;
        }

        idcode = BitConverter.ToUInt32(response, 3);
        if (idcode == 0 || idcode == 0xFFFFFFFF)
        {
            return false;
        }
        RunD1ToD3ReadOnly(handle, hertz);
        return true;
    }

    private static bool TryReadDpIdcodeOnly(
        SafeFileHandle handle,
        uint hertz,
        out uint idcode,
        out byte count,
        out byte ack)
    {
        const byte ResetSelect = DapSwjPinReset;
        idcode = 0;
        count = 0;
        ack = 0;

        SetSwjClock(handle, hertz);
        byte asserted = SwjPins(handle, 0x00, ResetSelect, 100);
        Console.WriteLine(
            "  minimal nRESET asserted, SWJ pins=0x" + asserted.ToString("X2"));
        byte released = SwjPins(handle, ResetSelect, ResetSelect, 20);
        Console.WriteLine(
            "  minimal nRESET released, SWJ pins=0x" + released.ToString("X2"));
        Thread.Sleep(20);
        SwitchJtagToSwd(handle);

        byte[] response = Transact(
            handle,
            DapTransfer,
            0x00,
            0x01,
            DapTransferDpReadIdcode);
        count = response[1];
        ack = response[2];
        if (count != 1 || ack != DapTransferOk)
        {
            return false;
        }

        idcode = BitConverter.ToUInt32(response, 3);
        return idcode != 0 && idcode != 0xFFFFFFFF;
    }

    private static void PrintSwjPinState(SafeFileHandle handle)
    {
        const byte SelectAll =
            DapSwjPinSwclk |
            DapSwjPinSwdio |
            DapSwjPinReset;
        byte state = SwjPins(handle, SelectAll, SelectAll, 0);
        Console.WriteLine("SWJ pin state: 0x" + state.ToString("X2"));
    }

    private static uint WaitForDhcsr(
        SafeFileHandle handle,
        bool halted,
        string name)
    {
        for (int attempt = 0; attempt != 25; ++attempt)
        {
            uint dhcsr = ReadAhbWord(handle, CoreDebugDhcsrAddress, name);
            if (((dhcsr & DhcsrSHalt) != 0) == halted)
            {
                return dhcsr;
            }
            Thread.Sleep(10);
        }
        throw new InvalidOperationException(
            name + " did not observe S_HALT=" + (halted ? "1" : "0"));
    }

    private static uint ReadU32Le(byte[] bytes, int offset)
    {
        return (uint)(
            bytes[offset] |
            (bytes[offset + 1] << 8) |
            (bytes[offset + 2] << 16) |
            (bytes[offset + 3] << 24));
    }

    private static ushort ReadU16Le(byte[] bytes, int offset)
    {
        return (ushort)(bytes[offset] | (bytes[offset + 1] << 8));
    }

    private static string Gpio35ModeName(byte mode)
    {
        if (mode == 0)
        {
            return "uart0-rx";
        }
        if (mode == 1)
        {
            return "high-impedance-input";
        }
        if (mode == 2)
        {
            return "uart0-rx-no-pullup";
        }
        if (mode == 3)
        {
            return "pad-disabled";
        }
        return "unknown(" + mode + ")";
    }

    private static void PrintUserUartRxFeature(SafeFileHandle handle)
    {
        byte[] report = new byte[WindowsReportSize];
        report[0] = 0;
        if (!HidD_GetFeature(handle, report, report.Length))
        {
            throw new InvalidOperationException(
                "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
        }
        if (report[1] != (byte)'U' ||
            report[2] != (byte)'3' ||
            report[3] != (byte)'5' ||
            report[4] != (byte)'D' ||
            report[5] != DiagnosticFeatureVersion ||
            report[6] != 0)
        {
            throw new InvalidOperationException("unexpected user UART RX diagnostic feature report");
        }

        byte flags = report[7];
        byte gpio = report[10];
        Console.WriteLine(
            "user UART RX GPIO" + gpio + ": mode=" + Gpio35ModeName(report[6]) +
            " ie=" + ((flags & 0x01) != 0 ? "1" : "0") +
            " oe=" + ((flags & 0x02) != 0 ? "1" : "0") +
            " expected=" + ((flags & 0x04) != 0 ? "1" : "0") +
            " pu=" + ((flags & 0x08) != 0 ? "1" : "0") +
            " function=" + report[8] +
            " uart_signal=" + report[9]);
        Console.WriteLine(
            "user UART RX GPIO" + gpio + ": pad=0x" + ReadU32Le(report, 13).ToString("X8") +
            " uart_cfg2=0x" + ReadU32Le(report, 17).ToString("X8") +
            " samples=" + ReadU32Le(report, 21) +
            " anomalies=" + ReadU32Le(report, 25));
        Console.WriteLine(
            "user UART transport: cdc_packets=" + ReadU32Le(report, 29) +
            " cdc_bytes=" + ReadU32Le(report, 33) +
            " queue_drops=" + ReadU32Le(report, 37) +
            " uart_tx_bytes=" + ReadU32Le(report, 41) +
            " uart_tx_failures=" + ReadU32Le(report, 45));
        Console.WriteLine(
            "user UART RX: received_bytes=" + ReadU32Le(report, 49) +
            " ring_overflows=" + ReadU32Le(report, 53) +
            " buffered_bytes=" + (report[57] | (report[58] << 8)) +
            " program_1200=" + report[59] +
            " reset_2400=" + report[60] +
            " normal=" + report[61] +
            " line_coding_drops=" + report[62]);
    }

    private static void SelectDiagnosticFeaturePage(SafeFileHandle handle, byte page)
    {
        byte[] report = new byte[WindowsReportSize];
        report[0] = 0;
        report[1] = 0xD5;
        report[2] = page;
        if (!HidD_SetFeature(handle, report, report.Length))
        {
            throw new InvalidOperationException(
                "HidD_SetFeature diagnostic page selection failed, win32=" +
                Marshal.GetLastWin32Error());
        }
        Thread.Sleep(20);
    }

    private static string DescribeFeatureReport(byte[] report)
    {
        return BitConverter.ToString(report);
    }

    private static int ReadI32Le(byte[] value, int offset)
    {
        return unchecked((int)ReadU32Le(value, offset));
    }

    private static void PrintCdcInTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 1);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 1)
            {
                throw new InvalidOperationException(
                    "unexpected CDC IN telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "CDC IN: configured=" + report[7] +
                " state=" + report[8] +
                " pending=" + (report[11] | (report[12] << 8)) +
                " submits=" + ReadU32Le(report, 13) +
                " submit_failures=" + ReadU32Le(report, 17) +
                " completions=" + ReadU32Le(report, 21) +
                " completion_bytes=" + ReadU32Le(report, 25));
            Console.WriteLine(
                "CDC IN recovery: recoveries=" + ReadU32Le(report, 29) +
                " zlp_submits=" + ReadU32Le(report, 33) +
                " zlp_failures=" + ReadU32Le(report, 37) +
                " submitted_bytes=" + ReadU32Le(report, 57));
            Console.WriteLine(
                "USB events: configured_generation=" + ReadU32Le(report, 41) +
                " reset=" + ReadU32Le(report, 45) +
                " disconnected=" + ReadU32Le(report, 49) +
                " errors=" + ReadU32Le(report, 53) +
                " sequence=" + ReadU32Le(report, 61));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintHidDataPathTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 7);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 7)
            {
                throw new InvalidOperationException(
                    "unexpected HID data-path telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "HID data path: configured=" + report[7] +
                " tx_busy=" + report[8] +
                " rx_armed=" + report[9] +
                " last_out_length=" + report[10] +
                " last_request=0x" + report[11].ToString("X2") +
                " last_response=0x" + report[12].ToString("X2") +
                " last_in_submit=" + unchecked((sbyte)report[13]) +
                " last_read_rearm=" + unchecked((sbyte)report[14]));
            Console.WriteLine(
                "HID data path counts: out_callbacks=" + ReadU32Le(report, 17) +
                " invalid_out=" + ReadU32Le(report, 21) +
                " busy_drops=" + ReadU32Le(report, 25) +
                " dap_requests=" + ReadU32Le(report, 29) +
                " in_submits=" + ReadU32Le(report, 33) +
                " in_submit_failures=" + ReadU32Le(report, 37) +
                " in_completions=" + ReadU32Le(report, 41) +
                " read_rearms=" + ReadU32Le(report, 45) +
                " read_rearm_failures=" + ReadU32Le(report, 49) +
                " usb_event_sequence=" + ReadU32Le(report, 53));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintTcpRuntimeTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 22);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 22)
            {
                throw new InvalidOperationException(
                    "unexpected TCP runtime telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "TCP listener: flags=0x" + ReadU32Le(report, 7).ToString("X8") +
                " generation=" + ReadU32Le(report, 11) +
                " handle=" + ReadI32Le(report, 15) +
                " port=" + (report[19] | (report[20] << 8)) +
                " listener_errno=" + ReadI32Le(report, 21) +
                " creates=" + ReadU32Le(report, 25));
            Console.WriteLine(
                "TCP accept: attempts=" + ReadU32Le(report, 29) +
                " success=" + ReadU32Le(report, 33) +
                " would_block=" + ReadU32Le(report, 37) +
                " failures=" + ReadU32Le(report, 41) +
                " last_errno=" + ReadI32Le(report, 45));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintTcpDataPlaneTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 23);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 23)
            {
                throw new InvalidOperationException(
                    "unexpected TCP data-plane telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "TCP accepted socket: flags=0x" + ReadU32Le(report, 7).ToString("X8") +
                " handle=" + ReadI32Le(report, 11) +
                " peer_errno=" + ReadI32Le(report, 15) +
                " getsockopt_errno=" + ReadI32Le(report, 19) +
                " so_error=" + ReadI32Le(report, 23));
            Console.WriteLine(
                "TCP available probes: calls=" + ReadU32Le(report, 27) +
                " ready=" + ReadU32Le(report, 31) +
                " eof=" + ReadU32Le(report, 35) +
                " would_block=" + ReadU32Le(report, 39) +
                " failures=" + ReadU32Le(report, 43) +
                " last_errno=" + ReadI32Le(report, 47));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintApPacketInputTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 27);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 27)
            {
                throw new InvalidOperationException(
                    "unexpected AP input telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP input: packets=" + ReadU32Le(report, 7) +
                " sta=" + ReadU32Le(report, 11) +
                " ap=" + ReadU32Le(report, 15) +
                " ap_ipv4=" + ReadU32Le(report, 19) +
                " ap_tcp=" + ReadU32Le(report, 23) +
                " ap_tcp_4243=" + ReadU32Le(report, 27) +
                " malformed=" + ReadU32Le(report, 31));
            Console.WriteLine(
                "AP input last: sequence=" + ReadU32Le(report, 35) +
                " src_port=" + (report[39] | (report[40] << 8)) +
                " dst_port=" + (report[41] | (report[42] << 8)) +
                " flags=0x" + report[43].ToString("X2") +
                " is_sta=" + report[44] +
                " ipv4=" + report[45] +
                " tcp=" + report[46]);
            Console.WriteLine(
                "AP listener output: packets=" + ReadU32Le(report, 47) +
                " syn_ack=" + ReadU32Le(report, 51) +
                " reset=" + ReadU32Le(report, 55) +
                " src_port=" + (report[59] | (report[60] << 8)) +
                " dst_port=" + (report[61] | (report[62] << 8)) +
                " flags=0x" + report[63].ToString("X2") +
                " is_sta=" + report[64]);
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintTcpListenerLwipTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 28);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 28)
            {
                throw new InvalidOperationException(
                    "unexpected TCP listener lwIP telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "TCP listener lwIP: total=" + report[7] +
                " matching_port=" + report[8] +
                " configured_addr=0x" + ReadU32Le(report, 9).ToString("X8") +
                " observed_addr=0x" + ReadU32Le(report, 13).ToString("X8") +
                " observed_port=" + (report[17] | (report[18] << 8)) +
                " getsockname_errno=" + ReadI32Le(report, 19) +
                " last_syn_src=0x" + ReadU32Le(report, 23).ToString("X8") +
                " last_syn_dst=0x" + ReadU32Le(report, 27).ToString("X8"));
            for (int index = 0; index < 3; ++index)
            {
                int offset = 31 + index * 8;
                ushort port = (ushort)(report[offset] | (report[offset + 1] << 8));
                uint address = ReadU32Le(report, offset + 2);
                byte netifIndex = report[offset + 6];
                byte state = report[offset + 7];
                if (port == 0 && address == 0 && netifIndex == 0 && state == 0)
                {
                    continue;
                }
                Console.WriteLine(
                    "TCP listener lwIP pcb[" + index + "]: port=" + port +
                    " addr=0x" + address.ToString("X8") +
                    " netif=" + netifIndex +
                    " state=" + state);
            }
            Console.WriteLine(
                "TCP listener lwIP socket: flags=0x" +
                report[55].ToString("X2") +
                " netconn_state=" + report[56] +
                " netconn_type=" + report[57] +
                " pcb_state=" + report[58] +
                " pcb_port=" + (report[59] | (report[60] << 8)) +
                " pcb_addr=0x" + ReadU32Le(report, 61).ToString("X8"));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static string DescribeTcpCloseOrigin(byte origin)
    {
        switch (origin)
        {
            case 0: return "none";
            case 1: return "tcp_client_slot";
            case 2: return "tcp_server_client_slot";
            case 3: return "tcp_server_end";
            case 4: return "softresetwifi";
            case 5: return "listener_setup_failure";
            default: return "unknown";
        }
    }

    private static void PrintTcpListenerCloseTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 31);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 31)
            {
                throw new InvalidOperationException(
                    "unexpected TCP listener close telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            byte origin = report[19];
            Console.WriteLine(
                "TCP listener close: requests=" + ReadU32Le(report, 7) +
                " listener_requests=" + ReadU32Le(report, 11) +
                " last_handle=" + ReadI32Le(report, 15) +
                " origin=" + origin +
                " origin_name=" + DescribeTcpCloseOrigin(origin) +
                " result=" + ReadI32Le(report, 21) +
                " errno=" + ReadI32Le(report, 25) +
                " close_generation=" + ReadU32Le(report, 29) +
                " active_generation=" + ReadU32Le(report, 33) +
                " listener_handle=" + ReadI32Le(report, 37));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintApUdpPacketInputTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 29);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 29)
            {
                throw new InvalidOperationException(
                    "unexpected AP UDP input telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP UDP input: packets=" + ReadU32Le(report, 7) +
                " ap_udp_4244=" + ReadU32Le(report, 11) +
                " ap_udp_4244_output=" + ReadU32Le(report, 15));
            Console.WriteLine(
                "AP UDP input last: src_port=" + (report[19] | (report[20] << 8)) +
                " dst_port=" + (report[21] | (report[22] << 8)) +
                " src=0x" + ReadU32Le(report, 23).ToString("X8") +
                " dst=0x" + ReadU32Le(report, 27).ToString("X8") +
                " is_sta=" + report[31]);
            Console.WriteLine(
                "AP UDP output last: src_port=" + (report[32] | (report[33] << 8)) +
                " dst_port=" + (report[34] | (report[35] << 8)) +
                " is_sta=" + report[36]);
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static bool GetDeferredFeature(SafeFileHandle handle, byte[] report, byte page)
    {
        DateTime deadline = DateTime.UtcNow.AddSeconds(1);
        do
        {
            if (!HidD_GetFeature(handle, report, report.Length)) return false;
            if (report[6] == page) return true;
            Thread.Sleep(10);
        } while (DateTime.UtcNow < deadline);
        return false;
    }

    private static void PrintUdpSocketTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 30);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!GetDeferredFeature(handle, report, 30))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 30)
            {
                throw new InvalidOperationException(
                    "unexpected UDP socket telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "UDP socket: pcbs=" + report[7] +
                " port_4244=" + report[8] +
                " socket_pcb=" + report[9] +
                " netif=" + report[10] +
                " flags=0x" + report[11].ToString("X2") +
                " netconn_state=" + report[12] +
                " netconn_type=" + report[13] +
                " socket_errno=" + unchecked((sbyte)report[14]) +
                " local_port=" + (report[15] | (report[16] << 8)) +
                " remote_port=" + (report[17] | (report[18] << 8)) +
                " local_addr=0x" + ReadU32Le(report, 19).ToString("X8"));
            Console.WriteLine(
                "UDP socket stats: recv=" + (report[23] | (report[24] << 8)) +
                " drop=" + (report[25] | (report[26] << 8)) +
                " checksum=" + (report[27] | (report[28] << 8)) +
                " length=" + (report[29] | (report[30] << 8)) +
                " protocol=" + (report[31] | (report[32] << 8)) +
                " route=" + (report[33] | (report[34] << 8)) +
                " xmit=" + (report[35] | (report[36] << 8)));
            Console.WriteLine(
                "UDP backend receive: calls=" + (report[37] | (report[38] << 8)) +
                " would_block=" + (report[39] | (report[40] << 8)) +
                " success=" + (report[41] | (report[42] << 8)) +
                " failure=" + (report[43] | (report[44] << 8)) +
                " restore_failure=" + (report[45] | (report[46] << 8)) +
                " bytes=" + (report[47] | (report[48] << 8)) +
                " last_result=" + unchecked((short)(report[49] | (report[50] << 8))) +
                " last_errno=" + unchecked((sbyte)report[51]));
            Console.WriteLine(
                "UDP parse: attempts=" + (report[52] | (report[53] << 8)) +
                " reject_mask=0x" + report[54].ToString("X2") +
                " mode=" + report[55] +
                " arguments=" + report[56] +
                " slot=" + report[57] +
                " slot_open=" + report[58] +
                " backend_present=" + report[59]);
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    public static int RunUserUartGpio35Diagnostic(string requestedMode)
    {
        byte mode;
        if (requestedMode == "read")
        {
            mode = 0xff;
        }
        else if (requestedMode == "uart0-rx")
        {
            mode = 0;
        }
        else if (requestedMode == "high-impedance-input")
        {
            mode = 1;
        }
        else if (requestedMode == "uart0-rx-no-pullup")
        {
            mode = 2;
        }
        else if (requestedMode == "pad-disabled")
        {
            mode = 3;
        }
        else
        {
            throw new ArgumentException(
                "user UART RX diagnostic mode must be read, uart0-rx, high-impedance-input, uart0-rx-no-pullup, or pad-disabled");
        }

        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            if (mode != 0xff)
            {
                byte[] report = new byte[WindowsReportSize];
                report[0] = 0;
                report[1] = 0xD3;
                report[2] = mode;
                if (!HidD_SetFeature(handle, report, report.Length))
                {
                    throw new InvalidOperationException(
                        "HidD_SetFeature failed, win32=" + Marshal.GetLastWin32Error());
                }
                Thread.Sleep(20);
            }

            PrintUserUartRxFeature(handle);
        }
        return 0;
    }

    public static int RunCdcInTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintCdcInTelemetryFeature(handle);
        }
        Console.WriteLine("CDC_IN_TELEMETRY_PASS");
        return 0;
    }

    public static int RunHidDataPathTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintHidDataPathTelemetryFeature(handle);
        }
        Console.WriteLine("HID_DATA_PATH_TELEMETRY_PASS");
        return 0;
    }

    public static int RunTcpRuntimeTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }
            PrintTcpRuntimeTelemetryFeature(handle);
        }
        Console.WriteLine("TCP_RUNTIME_TELEMETRY_PASS");
        return 0;
    }

    public static int RunTcpDataPlaneTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }
            PrintTcpDataPlaneTelemetryFeature(handle);
        }
        Console.WriteLine("TCP_DATA_PLANE_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApPacketInputTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }
            PrintApPacketInputTelemetryFeature(handle);
        }
        Console.WriteLine("AP_PACKET_INPUT_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApUdpPacketInputTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }
            PrintApUdpPacketInputTelemetryFeature(handle);
        }
        Console.WriteLine("AP_UDP_PACKET_INPUT_TELEMETRY_PASS");
        return 0;
    }

    public static int RunUdpSocketTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed, win32=" + Marshal.GetLastWin32Error());
            }
            PrintUdpSocketTelemetryFeature(handle);
        }
        return 0;
    }

    public static int RunTcpListenerLwipTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }
            PrintTcpListenerLwipTelemetryFeature(handle);
        }
        Console.WriteLine("TCP_LISTENER_LWIP_TELEMETRY_PASS");
        return 0;
    }

    public static int RunTcpListenerCloseTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }
            PrintTcpListenerCloseTelemetryFeature(handle);
        }
        Console.WriteLine("TCP_LISTENER_CLOSE_TELEMETRY_PASS");
        return 0;
    }

    public static int RunSynchronousDapInfoProbe()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for synchronous CMSIS-DAP HID path");
            }

            string vendor = Encoding.ASCII.GetString(DapInfoRequest(handle, 0x01));
            string product = Encoding.ASCII.GetString(DapInfoRequest(handle, 0x02));
            byte[] packetSize = DapInfoRequest(handle, 0xFF);
            byte[] capabilities = DapInfoRequest(handle, 0xF0);
            Console.WriteLine(
                "SYNCHRONOUS_DAP_INFO: vendor='" + vendor +
                "' product='" + product +
                "' packet_size=" + BitConverter.ToUInt16(packetSize, 0) +
                " capabilities=0x" +
                (capabilities.Length == 0 ? "00" : capabilities[0].ToString("X2")));
        }
        Console.WriteLine("SYNCHRONOUS_DAP_INFO_PASS");
        return 0;
    }

    public static int RunSynchronousDapConnectProbe()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for synchronous CMSIS-DAP HID path");
            }

            byte[] response = Transact(handle, DapConnect, DapPortSwd);
            if (response[1] != DapPortSwd)
            {
                throw new InvalidOperationException(
                    "DAP_Connect did not select SWD");
            }
            RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
        }
        Console.WriteLine("SYNCHRONOUS_DAP_CONNECT_PASS");
        return 0;
    }

    private static void PrintApStartTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 2);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 2)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start diagnostic feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP start: stage=" + report[7] +
                " native_result=" + unchecked((sbyte)report[8]) +
                " sta_vif=" + report[9] +
                " ap_vif=" + report[10] +
                " sta_connected=" + report[11] +
                " ap_started=" + report[12]);
            Console.WriteLine(
                "AP start channel: requested=" + report[13] +
                " sta_query_result=" + unchecked((sbyte)report[14]) +
                " sta_channel=" + report[15] +
                " fhost_stage=" + report[16] +
                " (" + DescribeApStartStage(report[16]) + ")");
            Console.WriteLine(
                "AP start timing: attempts=" + ReadU32Le(report, 17) +
                " elapsed_ms=" + ReadU32Le(report, 21) +
                " started_at_ms=" + ReadU32Le(report, 25) +
                " completed_at_ms=" + ReadU32Le(report, 29));
            Console.WriteLine(
                "AP start channel pointers: fhost_global=0x" +
                ReadU32Le(report, 33).ToString("X8") +
                " handler=0x" + ReadU32Le(report, 37).ToString("X8") +
                " response_source=0x" + ReadU32Le(report, 41).ToString("X8") +
                " response_copy=0x" + ReadU32Le(report, 45).ToString("X8") +
                " response_received=0x" + ReadU32Le(report, 49).ToString("X8") +
                " wpa_local=0x" + ReadU32Le(report, 53).ToString("X8"));
            Console.WriteLine(
                "AP start channel counts: chan2g4=" + ReadU32Le(report, 57) +
                " chan5g=" + ReadU32Le(report, 61));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintApStartWatchdogTelemetryFeature(
        SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 8);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" +
                    Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 8)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start watchdog diagnostic feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP start watchdog: enabled=" + report[7] +
                " state=" + report[8] +
                " reset_reason=" + report[9] +
                " persisted=" + report[10] +
                " timeout_ms=" + (report[11] | (report[12] << 8)) +
                " arm_count=" + ReadU32Le(report, 13));
            Console.WriteLine(
                "AP start retained liveness: phase=" + report[17] +
                " event_count=" + report[18] +
                " fhost_stage=" + report[19] +
                " (" + DescribeApStartStage(report[19]) + ")" +
                " bridge_stage=" + report[20] +
                " fhost_result=" + unchecked((sbyte)report[21]) +
                " native_result=" + unchecked((sbyte)report[22]) +
                " diagnostic_persisted=" + report[23]);
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static string DescribeApStartStage(byte stage)
    {
        switch (stage)
        {
            case 153: return "add_network_before_sync";
            case 154: return "add_network_event_enqueued";
            case 155: return "add_network_wait_begin";
            case 156: return "add_network_eloop_selected";
            case 157: return "add_network_eloop_handler_dispatch";
            case 158: return "add_network_ctrl_handler_body";
            case 159: return "add_network_ctrl_process_begin";
            case 160: return "add_network_ctrl_process_return";
            case 161: return "add_network_ctrl_reply_stored";
            case 162: return "add_network_eloop_handler_return";
            case 163: return "add_network_eloop_remove_request";
            case 164: return "add_network_eloop_request_removed";
            case 165: return "add_network_completion_signal";
            case 166: return "add_network_sync_return";
            case 167: return "add_network_global_process_enter";
            case 168: return "add_network_ifname_redirect";
            case 169: return "add_network_interface_matched";
            case 170: return "add_network_interface_process_enter";
            case 171: return "add_network_reply_buffer_allocated";
            case 172: return "add_network_command_dispatch";
            case 173: return "add_network_command_return";
            case 174: return "add_network_helper_enter";
            case 175: return "add_network_supplicant_enter";
            case 176: return "add_network_supplicant_return";
            case 177: return "add_network_config_enter";
            case 178: return "add_network_config_allocate";
            case 179: return "add_network_config_return";
            case 180: return "add_network_notify_enter";
            case 181: return "add_network_notify_return";
            case 182: return "add_network_defaults_enter";
            case 183: return "add_network_defaults_return";
            case 185: return "add_network_reply_format";
            case 186: return "add_network_helper_return";
            case 187: return "add_network_config_list_walk";
            case 188: return "add_network_config_list_walk_complete";
            case 189: return "add_network_config_ssid_allocate";
            case 190: return "add_network_config_ssid_allocated";
            case 191: return "add_network_config_ssid_initialized";
            case 192: return "add_network_config_ssid_linked";
            case 193: return "add_network_config_priority_update";
            case 194: return "add_network_config_priority_update_return";
            case 197: return "add_network_priority_list_enter";
            case 198: return "add_network_priority_list_old_free";
            case 199: return "add_network_priority_list_reset";
            case 200: return "add_network_priority_list_item";
            case 201: return "add_network_priority_list_append";
            case 202: return "add_network_priority_list_append_return";
            case 203: return "add_network_priority_list_complete";
            case 204: return "set_sae_pwe_before_request_allocate";
            case 205: return "set_sae_pwe_request_allocated";
            case 206: return "set_sae_pwe_before_sync";
            case 207: return "set_sae_pwe_sync_return";
            case 208: return "set_sae_pwe_event_allocated";
            case 209: return "set_sae_pwe_event_enqueued";
            case 210: return "set_sae_pwe_wait_begin";
            case 211: return "set_sae_pwe_wait_return";
            case 212: return "set_sae_pwe_eloop_selected";
            case 213: return "set_sae_pwe_eloop_handler_dispatch";
            case 214: return "set_sae_pwe_eloop_handler_return";
            case 215: return "set_sae_pwe_eloop_remove_request";
            case 216: return "set_sae_pwe_eloop_request_removed";
            case 217: return "set_sae_pwe_completion_signal";
            case 219: return "enable_network_event_enqueued";
            case 220: return "enable_network_sync_return";
            case 221: return "enable_network_eloop_selected";
            case 222: return "enable_network_eloop_handler_dispatch";
            case 223: return "enable_network_eloop_handler_return";
            case 224: return "enable_network_eloop_remove_request";
            case 225: return "enable_network_completion_signal";
            case 226: return "enable_network_ctrl_process_begin";
            case 227: return "enable_network_ctrl_process_return";
            case 228: return "enable_network_reply_stored";
            case 229: return "enable_network_request_removed";
            case 230: return "enable_network_before_mutex_unlock";
            case 231: return "enable_network_before_completion_callback";
            case 232: return "enable_network_completion_callback_return";
            case 233: return "enable_network_before_sync_wait";
            case 234: return "enable_network_before_signal";
            case 235: return "enable_network_signal_return";
            case 236: return "enable_network_completion_boundary_return";
            case 237: return "enable_network_sync_wait_return";
            case 240: return "eapol_context_before_allocate";
            case 241: return "eapol_context_allocated";
            case 242: return "eapol_state_machine_before_init";
            case 243: return "eapol_state_machine_initialized";
            case 244: return "eapol_state_before_allocate";
            case 245: return "eapol_state_allocated";
            case 246: return "eap_peer_before_init";
            case 247: return "eap_peer_initialized";
            case 248: return "eap_peer_before_zalloc";
            case 249: return "eap_peer_zalloc_return";
            case 250: return "eap_peer_tls_before_init";
            case 251: return "eap_peer_tls_initialized";
            case 252: return "eapol_state_machine_first_step";
            case 253: return "eapol_state_machine_first_step_return";
            case 254: return "eapol_state_machine_second_step_return";
            case 255: return "eapol_timer_registered";
            case 119: return "eap_peer_before_malloc";
            case 120: return "eap_peer_malloc_return";
            case 121: return "eap_peer_before_zero";
            case 122: return "eap_peer_zero_return";
            default: return "other";
        }
    }

    private static void PrintApStartHeapTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 3);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 3)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start heap diagnostic feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP start heap: fhost_stage=" + report[7] +
                " native_result=" + unchecked((sbyte)report[8]) +
                " probe_stage=" + report[9] +
                " persisted=" + report[10] +
                " malloc_failed=" + report[11] +
                " allocator_stage=" + report[12] +
                " eap_peer_alloc_stage=" + report[53]);
            Console.WriteLine(
                "AP start heap before: request=" + ReadU32Le(report, 13) +
                " total=" + ReadU32Le(report, 17) +
                " ocram=" + ReadU32Le(report, 21) +
                " wram=" + ReadU32Le(report, 25));
            Console.WriteLine(
                "AP start heap after: address=0x" +
                ReadU32Le(report, 29).ToString("X8") +
                " total=" + ReadU32Le(report, 33) +
                " ocram=" + ReadU32Le(report, 37) +
                " wram=" + ReadU32Le(report, 41));
            Console.WriteLine(
                "AP start heap timing: attempts=" + ReadU32Le(report, 45) +
                " elapsed_ms=" + ReadU32Le(report, 49));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintApStartCommandTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 4);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 4)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start command telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP start command: command=" + report[7] +
                " (" + DescribeApStartCommand(report[7]) + ")" +
                " phase=" + report[8] +
                " (" + DescribeApStartCommandPhase(report[8]) + ")" +
                " vif=" + unchecked((sbyte)report[9]) +
                " wpa_state=" + report[10] +
                " network_id=" + unchecked((sbyte)report[11]) +
                " sequence=" + report[12]);
            Console.WriteLine(
                "AP start command context: fhost_stage=" + report[13] +
                " (" + DescribeApStartStage(report[13]) + ")" +
                " fhost_result=" + unchecked((sbyte)report[14]) +
                " bridge_stage=" + report[15] +
                " persisted=" + report[16]);
            Console.WriteLine(
                "AP start command timing: attempts=" + ReadU32Le(report, 17) +
                " elapsed_ms=" + ReadU32Le(report, 21) +
                " started_at_ms=" + ReadU32Le(report, 25) +
                " completed_at_ms=" + ReadU32Le(report, 29));
            Console.WriteLine(
                "AP start semaphore: phase=" + report[33] +
                " (" + DescribeApStartSemaphorePhase(report[33]) + ")" +
                " wait_result=" + unchecked((sbyte)report[34]) +
                " signal_result=" + unchecked((sbyte)report[35]) +
                " wait_count_before=" + report[36] +
                " callback_count_before=" + report[37] +
                " callback_count_after=" + report[38] +
                " wait_count_after=" + report[39]);
            Console.WriteLine(
                "AP start semaphore handles: wait=0x" +
                ReadU32Le(report, 41).ToString("X8") +
                " callback=0x" + ReadU32Le(report, 45).ToString("X8") +
                " updates=" + ReadU32Le(report, 49));
            Console.WriteLine(
                "AP start task: handle=0x" +
                ReadU32Le(report, 53).ToString("X8") +
                " state=" + report[57] +
                " (" + DescribeFreeRtosTaskState(report[57]) + ")" +
                " stack_high_water_words=" + ReadU32Le(report, 58));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static string DescribeApStartCommand(byte command)
    {
        switch (command)
        {
            case 0: return "none";
            case 1: return "interface_add";
            case 2: return "interface_remove";
            case 3: return "add_network";
            case 4: return "set_sae_pwe";
            case 5: return "set_country";
            case 6: return "set_non_pref_chan";
            case 7: return "set_network_key_mgmt";
            case 8: return "set_network_mode";
            case 9: return "set_network_ssid";
            case 10: return "set_network_psk";
            case 11: return "set_network_generic";
            case 12: return "ap_scan";
            case 13: return "enable_network";
            case 14: return "other_ap_command";
            default: return "unknown";
        }
    }

    private static string DescribeApStartCommandPhase(byte phase)
    {
        switch (phase)
        {
            case 0: return "idle";
            case 1: return "entered";
            case 2: return "before_sync_wait";
            case 3: return "sync_returned";
            case 4: return "leaving";
            default: return "unknown";
        }
    }

    private static string DescribeApStartSemaphorePhase(byte phase)
    {
        switch (phase)
        {
            case 0: return "idle";
            case 1: return "wait_before_take";
            case 2: return "callback_before_give";
            case 3: return "callback_give_return";
            case 4: return "callback_after_give_count";
            case 5: return "wait_take_return";
            case 6: return "notification_before";
            case 7: return "notification_return";
            case 8: return "notification_wait_return";
            default: return "unknown";
        }
    }

    private static string DescribeFreeRtosTaskState(byte state)
    {
        switch (state)
        {
            case 0: return "running";
            case 1: return "ready";
            case 2: return "blocked";
            case 3: return "suspended";
            case 4: return "deleted";
            case 5: return "invalid";
            default: return "unknown";
        }
    }

    private static string DescribeApStartTraceEvent(byte traceEvent)
    {
        switch (traceEvent)
        {
            case 1: return "wait_before_take";
            case 2: return "callback_before_give";
            case 3: return "callback_give_return";
            case 4: return "wait_take_return";
            case 5: return "notification_before";
            case 6: return "notification_return";
            case 7: return "notification_wait_before";
            case 8: return "notification_wait_return";
            default: return "unknown";
        }
    }

    private static uint ReadApStartTraceTelemetryFeature(
        SafeFileHandle handle,
        bool print)
    {
        SelectDiagnosticFeaturePage(handle, 5);
        try
        {
            return ReadSelectedApStartTraceTelemetryFeature(handle, print);
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static uint ReadSelectedApStartTraceTelemetryFeature(
        SafeFileHandle handle,
        bool print)
    {
        byte[] report = new byte[WindowsReportSize];
        report[0] = 0;
        if (!HidD_GetFeature(handle, report, report.Length))
        {
            throw new InvalidOperationException(
                "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
        }
        if (report[1] != (byte)'U' ||
            report[2] != (byte)'3' ||
            report[3] != (byte)'5' ||
            report[4] != (byte)'D' ||
            report[5] != DiagnosticFeatureVersion ||
            report[6] != 5)
        {
            throw new InvalidOperationException(
                "unexpected AP-start trace telemetry feature report: " +
                DescribeFeatureReport(report));
        }

        byte count = report[8];
        if (count > 3)
        {
            throw new InvalidOperationException(
                "AP-start trace record count exceeds the report capacity");
        }
        uint nextSequence = ReadU32Le(report, 9);
        if (print)
        {
            Console.WriteLine(
                "AP start trace: persisted=" + report[7] +
                " records=" + count +
                " next_sequence=" + nextSequence);
            for (int index = 0; index < count; ++index)
            {
                int offset = 13 + index * 14;
                Console.WriteLine(
                    "AP start trace[" + index + "]: sequence=" +
                    ReadU32Le(report, offset) +
                    " event=" + report[offset + 4] +
                    " (" + DescribeApStartTraceEvent(report[offset + 4]) + ")" +
                    " result=" + unchecked((sbyte)report[offset + 5]) +
                    " semaphore=0x" + ReadU32Le(report, offset + 6).ToString("X8") +
                    " count=" + ReadU32Le(report, offset + 10));
            }
        }
        return nextSequence;
    }

    private static void PrintApStartTraceTelemetryFeature(SafeFileHandle handle)
    {
        ReadApStartTraceTelemetryFeature(handle, true);
    }

    private static void PrintApStartNotificationTelemetryFeature(
        SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 15);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 15)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start notification telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            byte eventMask = report[8];
            Console.WriteLine(
                "AP start notification: persisted=" + report[7] +
                " events=0x" + eventMask.ToString("X2") +
                " callback_before=" + ((eventMask & 0x01) != 0 ? 1 : 0) +
                " callback_after=" + ((eventMask & 0x02) != 0 ? 1 : 0) +
                " wait_before=" + ((eventMask & 0x04) != 0 ? 1 : 0) +
                " wait_return=" + ((eventMask & 0x08) != 0 ? 1 : 0) +
                " last_event=" + report[9] +
                " (" + DescribeApStartTraceEvent(report[9]) + ")" +
                " notification_result=" + unchecked((sbyte)report[10]) +
                " wait_result=" + unchecked((sbyte)report[11]) +
                " attempts=" + ReadU32Le(report, 13) +
                " last_sequence=" + ReadU32Le(report, 17) +
                " waiting_task=0x" + ReadU32Le(report, 21).ToString("X8"));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintApStartWpaEventPathTelemetryFeature(
        SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 19);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 19)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start WPA event-path feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP start WPA event: persisted=" + report[7] +
                " phase_mask=0x" + ReadU32Le(report, 8).ToString("X8") +
                " last_phase=" + report[12] +
                " vif=" + unchecked((sbyte)report[13]) +
                " event=" + report[14] +
                " fhost_wpa_state=" + report[15] +
                " operstate=" + unchecked((sbyte)report[16]) +
                " old_wpa_state=" + unchecked((sbyte)report[17]) +
                " new_wpa_state=" + unchecked((sbyte)report[18]) +
                " result=" + unchecked((sbyte)report[19]) +
                " callback_slot=" + report[20] +
                " updates=" + ReadU32Le(report, 21) +
                " callback_slot_mask=0x" + ReadU32Le(report, 25).ToString("X8"));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintApStartCreateApTelemetryFeature(
        SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 20);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 20)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start create-AP feature report: " +
                    DescribeFeatureReport(report));
            }

            Console.WriteLine(
                "AP start create AP: persisted=" + report[7] +
                " phase_mask=0x" + (report[8] | (report[9] << 8)).ToString("X4") +
                " last_phase=" + report[10] +
                " result=" + unchecked((sbyte)report[11]) +
                " wpa_state=" + report[12] +
                " detail=" + ReadU32Le(report, 17) +
                " updates=" + ReadU32Le(report, 21));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static string DescribeApStartHostapdLifecyclePhase(byte phase)
    {
        switch (phase)
        {
            case 1: return "hostapd_setup_interface_entry";
            case 2: return "hostapd_setup_interface_return";
            case 3: return "setup_interface_entry";
            case 4: return "validate_bssid";
            case 5: return "start_ctrl_iface";
            case 6: return "get_country";
            case 7: return "set_country";
            case 8: return "wait_channel_update_scheduled";
            case 9: return "setup_interface_direct_return";
            case 10: return "channel_update_timeout";
            case 11: return "channel_list_updated";
            case 12: return "setup_interface2_entry";
            case 13: return "get_hw_features";
            case 14: return "fixed_channel_to_frequency";
            case 15: return "select_hw_mode";
            case 16: return "acs_deferred";
            case 17: return "check_ht_capability";
            case 18: return "ht_deferred";
            case 19: return "setup_interface_complete_call";
            case 20: return "setup_complete_sync_entry";
            case 21: return "set_frequency";
            case 22: return "setup_bss";
            case 23: return "driver_commit";
            case 24: return "interface_enabled";
            case 25: return "setup_complete_callback";
            case 26: return "setup_complete_sync_return";
            case 27: return "setup_complete_entry";
            case 28: return "sync_start_wait";
            case 29: return "setup_complete_return";
            case 30: return "setup_failure";
            case 31: return "failure_callback";
            case 32: return "ap_configured_callback_entry";
            case 33: return "ap_configured_callback_disabled_deinit";
            case 34: return "wpa_supplicant_ap_deinit_entry";
            case 35: return "ap_deinit_before_hostapd_deinit";
            case 36: return "hostapd_interface_deinit_entry";
            case 37: return "hostapd_disable_iface_entry";
            case 38: return "wpa_supplicant_mark_disassoc_with_ap";
            case 39: return "wpa_supplicant_deauthenticate_entry";
            case 40: return "fhost_wpa_disable_network_entry";
            case 41: return "wifi_mgmr_ap_stop_entry";
            case 42: return "setup_bss_entry";
            case 43: return "setup_bss_started";
            case 44: return "setup_bss_station_flush";
            case 45: return "setup_bss_get_ssid";
            case 46: return "setup_bss_wpa_psk";
            case 47: return "setup_bss_set_ssid";
            case 48: return "setup_bss_wpa";
            case 49: return "setup_bss_beacon";
            case 50: return "setup_bss_wpa_keys";
            case 51: return "setup_bss_complete";
            default: return "unknown";
        }
    }

    private static void PrintApStartHostapdLifecycleTelemetryFeature(
        SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 24);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 24)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start hostapd lifecycle feature report: " +
                    DescribeFeatureReport(report));
            }

            byte lastPhase = report[12];
            Console.WriteLine(
                "AP start hostapd lifecycle: persisted=" + report[7] +
                " phase_mask=0x" + ReadU32Le(report, 8).ToString("X8") +
                " last_phase=" + lastPhase +
                " (" + DescribeApStartHostapdLifecyclePhase(lastPhase) + ")" +
                " result=" + unchecked((sbyte)report[13]) +
                " iface_state=" + report[14] +
                " wait_channel_update=" + report[15] +
                " detail0=0x" + ReadU32Le(report, 17).ToString("X8") +
                " detail1=0x" + ReadU32Le(report, 21).ToString("X8") +
                " disable_network_caller=0x" +
                ReadU32Le(report, 25).ToString("X8") +
                " disable_network_id=" + unchecked((short)ReadU16Le(report, 29)) +
                " deauthenticate_reason=" + ReadU16Le(report, 31) +
                " bridge_stop_softap_caller=0x" +
                ReadU32Le(report, 58).ToString("X8"));
            byte recordCount = report[33];
            if (recordCount > 3)
            {
                throw new InvalidOperationException(
                    "invalid AP-start hostapd lifecycle record count");
            }
            for (int index = 0; index < recordCount; index++)
            {
                int offset = 34 + index * 8;
                byte phase = report[offset];
                Console.WriteLine(
                    "AP start hostapd lifecycle[" + index + "]: phase=" +
                    phase + " (" +
                    DescribeApStartHostapdLifecyclePhase(phase) + ")" +
                    " result=" + unchecked((sbyte)report[offset + 1]) +
                    " iface_state=" + report[offset + 2] +
                    " wait_channel_update=" + report[offset + 3] +
                    " detail0=0x" +
                    ReadU32Le(report, offset + 4).ToString("X8"));
            }
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static string DescribeHostapdIfaceState(byte state)
    {
        switch (state)
        {
            case 0: return "uninitialized";
            case 1: return "disabled";
            case 2: return "country_update";
            case 3: return "acs";
            case 4: return "ht_scan";
            case 5: return "dfs";
            case 6: return "enabled";
            default: return "unknown";
        }
    }

    private static void PrintApStartHostapdStateUpdateTelemetryFeature(
        SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 26);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 26)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start hostapd state-update feature report: " +
                    DescribeFeatureReport(report));
            }

            byte recordCount = report[8];
            if (recordCount > 3)
            {
                throw new InvalidOperationException(
                    "invalid AP-start hostapd state-update record count");
            }
            Console.WriteLine(
                "AP start hostapd state updates: persisted=" + report[7] +
                " record_count=" + recordCount +
                " next_sequence=" + ReadU32Le(report, 9));
            for (int index = 0; index < recordCount; index++)
            {
                int offset = 13 + index * 12;
                byte state = report[offset + 4];
                Console.WriteLine(
                    "AP start hostapd state update[" + index + "]: sequence=" +
                    ReadU32Le(report, offset) +
                    " hostapd_state=" + state +
                    " (" + DescribeHostapdIfaceState(state) + ")" +
                    " enabled=" + report[offset + 5] +
                    " raw_before=" + report[offset + 6] +
                    " vif_before=" + report[offset + 7] +
                    " raw_after=" + report[offset + 8] +
                    " vif_after=" + report[offset + 9]);
            }
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static string DescribeApStartStatusCodePhase(byte phase)
    {
        switch (phase)
        {
            case 1: return "wpa_callback_enter";
            case 2: return "wpa_state_message_recognized";
            case 3: return "wpa_message_allocated";
            case 4: return "wpa_message_queued";
            case 5: return "fhost_handler_enter";
            case 6: return "fhost_state_prefix_recognized";
            case 7: return "fhost_state_suffix_parsed";
            case 8: return "fhost_vif_state_updated";
            default: return "unknown";
        }
    }

    private static string DescribeApStartStatusCodeKind(byte kind)
    {
        switch (kind)
        {
            case 1: return "State";
            case 2: return "other_filtered_control_message";
            default: return "unknown";
        }
    }

    private static void PrintApStartStatusCodeTelemetryFeature(
        SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 25);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 25)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start status-code feature report: " +
                    DescribeFeatureReport(report));
            }

            byte lastPhase = report[10];
            byte messageKind = report[12];
            Console.WriteLine(
                "AP start status code: persisted=" + report[7] +
                " phase_mask=0x" + ReadU16Le(report, 8).ToString("X4") +
                " last_phase=" + lastPhase +
                " (" + DescribeApStartStatusCodePhase(lastPhase) + ")" +
                " vif=" + unchecked((sbyte)report[11]) +
                " message_kind=" + messageKind +
                " (" + DescribeApStartStatusCodeKind(messageKind) + ")" +
                " message_length=" + ReadU16Le(report, 13) +
                " raw_state=" + report[15] +
                " vif_state=" + report[16] +
                " result=" + unchecked((sbyte)report[17]) +
                " updates=" + ReadU32Le(report, 21));
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static void PrintApStartFrameTelemetryFeature(SafeFileHandle handle)
    {
        SelectDiagnosticFeaturePage(handle, 6);
        try
        {
            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            if (!HidD_GetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_GetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
            if (report[1] != (byte)'U' ||
                report[2] != (byte)'3' ||
                report[3] != (byte)'5' ||
                report[4] != (byte)'D' ||
                report[5] != DiagnosticFeatureVersion ||
                report[6] != 6)
            {
                throw new InvalidOperationException(
                    "unexpected AP-start frame telemetry feature report: " +
                    DescribeFeatureReport(report));
            }

            byte count = report[8];
            if (count > 3)
            {
                throw new InvalidOperationException(
                    "allocator trace record count exceeds the report capacity");
            }
            Console.WriteLine(
                "AP start allocator trace: fhost_stage=" + report[7] +
                " records=" + count +
                " next_sequence=" + ReadU32Le(report, 9));
            for (int index = 0; index < count; ++index)
            {
                int offset = 13 + index * 17;
                Console.WriteLine(
                    "AP start allocator[" + index + "]: sequence=" +
                    ReadU32Le(report, offset) +
                    " stage=" + report[offset + 4] +
                    " (" + DescribeApStartAllocatorStage(report[offset + 4]) + ")" +
                    " value0=0x" + ReadU32Le(report, offset + 5).ToString("X8") +
                    " value1=0x" + ReadU32Le(report, offset + 9).ToString("X8") +
                    " value2=0x" + ReadU32Le(report, offset + 13).ToString("X8"));
            }
        }
        finally
        {
            SelectDiagnosticFeaturePage(handle, 0);
        }
    }

    private static string DescribeApStartAllocatorStage(byte stage)
    {
        switch (stage)
        {
            case 1: return "os_zalloc_enter";
            case 2: return "os_malloc_enter";
            case 3: return "os_malloc_before_rtos_malloc";
            case 4: return "os_malloc_return";
            case 5: return "rtos_malloc_enter";
            case 6: return "rtos_malloc_before_kmalloc";
            case 7: return "rtos_malloc_return";
            case 8: return "kmalloc_enter";
            case 9: return "kmalloc_lock_acquired";
            case 10: return "kmalloc_before_allocator";
            case 11: return "kmalloc_allocator_return";
            case 12: return "kmalloc_lock_released";
            case 13: return "tlsf_allocator_enter";
            case 14: return "tlsf_before_allocate";
            case 15: return "tlsf_allocate_return";
            case 16: return "tlsf_zero_complete";
            case 17: return "os_zalloc_before_zero";
            case 18: return "os_zalloc_return";
            default: return "unknown";
        }
    }

    private static string ReadAtTerminal(
        SerialPort serial,
        int timeoutMilliseconds,
        string commandName)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMilliseconds);
        StringBuilder response = new StringBuilder();
        while (DateTime.UtcNow < deadline)
        {
            string available = serial.ReadExisting();
            if (available.Length != 0)
            {
                response.Append(available);
                if (response.Length > 512)
                {
                    throw new InvalidOperationException(
                        commandName + " response exceeded 512 bytes");
                }
                if (response.ToString().EndsWith("OK\r\n", StringComparison.Ordinal))
                {
                    return response.ToString();
                }
            }
            Thread.Sleep(5);
        }
        throw new TimeoutException(
            commandName + " timed out with " + response.Length + " byte(s)");
    }

    private static void RequireAtResponse(
        SerialPort serial,
        string request,
        string expected,
        string commandName)
    {
        serial.DiscardInBuffer();
        serial.Write(request);
        string response = ReadAtTerminal(serial, 2000, commandName);
        if (!String.Equals(response, expected, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                commandName + " did not match the expected AT framing");
        }
    }

    /*
     * The regular trace page is useful after a warm restart only when RAM is
     * retained. This probe instead keeps CDC and HID open from Windows at the
     * same time, so trace records are read before the SoftAP failure tears
     * down native USB.
     */
    public static int RunApStartLiveTraceTelemetry(string serialPortName)
    {
        if (String.IsNullOrWhiteSpace(serialPortName))
        {
            throw new ArgumentException("serial port name is required");
        }

        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        Console.WriteLine("BL616CL CDC port: " + serialPortName);

        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        using (SerialPort serial = new SerialPort(
            serialPortName,
            115200,
            Parity.None,
            8,
            StopBits.One))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            serial.DtrEnable = false;
            serial.RtsEnable = false;
            serial.ReadTimeout = 50;
            serial.WriteTimeout = 1000;
            serial.Open();

            RequireAtResponse(serial, "AT\r\n", "OK\r\n", "AT");
            RequireAtResponse(
                serial,
                "AT+FWVERSION?\r\n",
                "+FWVERSION: 0.6.0\r\nOK\r\n",
                "FWVERSION");
            RequireAtResponse(
                serial,
                "AT+WIFIMODE=2\r\n",
                "+WIFIMODE: \r\nOK\r\n",
                "WIFIMODE=2");
            RequireAtResponse(
                serial,
                "AT+SOFTAPCONFIG=192.168.42.1,192.168.42.1,255.255.255.0\r\n",
                "+SOFTAPCONFIG: \r\nOK\r\n",
                "SOFTAPCONFIG");
            Console.WriteLine("AP_START_LIVE_TRACE_PREFLIGHT_PASS");
            SelectDiagnosticFeaturePage(handle, 5);
            try
            {
                uint initialSequence =
                    ReadSelectedApStartTraceTelemetryFeature(handle, false);
                Console.WriteLine(
                    "AP_START_LIVE_TRACE_INITIAL_SEQUENCE=" + initialSequence);

                string suffix = Guid.NewGuid().ToString("N").Substring(0, 8);
                string beginRequest =
                    "AT+BEGINSOFTAP=\"uno-r4-" + suffix + "\",\"trace-" +
                    Guid.NewGuid().ToString("N") + "\",1,0\r\n";
                serial.DiscardInBuffer();
                serial.Write(beginRequest);

                DateTime deadline = DateTime.UtcNow.AddMilliseconds(2500);
                uint lastSequence = initialSequence;
                bool capturedTrace = false;
                bool beginResponseComplete = false;
                StringBuilder beginResponse = new StringBuilder();
                while (DateTime.UtcNow < deadline)
                {
                    try
                    {
                        uint nextSequence =
                            ReadSelectedApStartTraceTelemetryFeature(handle, false);
                        if (nextSequence != lastSequence)
                        {
                            capturedTrace |= nextSequence != initialSequence;
                            ReadSelectedApStartTraceTelemetryFeature(handle, true);
                            lastSequence = nextSequence;
                        }
                    }
                    catch (Exception error)
                    {
                        Console.WriteLine(
                            "AP_START_LIVE_TRACE_HID_STOPPED=" +
                            error.GetType().Name + ":" + error.Message);
                        break;
                    }

                    try
                    {
                        string available = serial.ReadExisting();
                        if (available.Length != 0)
                        {
                            beginResponse.Append(available);
                            if (beginResponse.Length > 512)
                            {
                                throw new InvalidOperationException(
                                    "BEGINSOFTAP response exceeded 512 bytes");
                            }
                            if (beginResponse.ToString().EndsWith(
                                "OK\r\n",
                                StringComparison.Ordinal))
                            {
                                beginResponseComplete = true;
                                Console.WriteLine(
                                    "AP_START_LIVE_TRACE_BEGIN_RESPONSE_BYTES=" +
                                    beginResponse.Length);
                            }
                        }
                    }
                    catch (Exception error)
                    {
                        Console.WriteLine(
                            "AP_START_LIVE_TRACE_CDC_STOPPED=" +
                            error.GetType().Name + ":" + error.Message);
                        break;
                    }
                    Thread.Sleep(1);
                }

                if (!beginResponseComplete)
                {
                    throw new InvalidOperationException(
                        "BEGINSOFTAP did not return a terminal response");
                }

                if (String.Equals(
                    beginResponse.ToString(),
                    "+BEGINSOFTAP: 1\r\nOK\r\n",
                    StringComparison.Ordinal))
                {
                    RequireAtResponse(
                        serial,
                        "AT+DISCONNECTSOFTAP\r\n",
                        "OK\r\n",
                        "DISCONNECTSOFTAP cleanup");
                    RequireAtResponse(
                        serial,
                        "AT+WIFIMODE=1\r\n",
                        "+WIFIMODE: \r\nOK\r\n",
                        "WIFIMODE=1 cleanup");
                    Console.WriteLine("AP_START_LIVE_TRACE_CLEANUP_PASS");
                }

                if (!capturedTrace)
                {
                    throw new InvalidOperationException(
                        "no AP-start trace record was observed before USB stopped");
                }
            }
            finally
            {
                try
                {
                    SelectDiagnosticFeaturePage(handle, 0);
                }
                catch (Exception error)
                {
                    Console.WriteLine(
                        "AP_START_LIVE_TRACE_PAGE_RESTORE_FAILED=" +
                        error.GetType().Name + ":" + error.Message);
                }
            }
        }

        Console.WriteLine("AP_START_LIVE_TRACE_CAPTURE_PASS");
        return 0;
    }

    public static int RunApStartTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartWatchdogTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartWatchdogTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_WATCHDOG_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartHeapTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartHeapTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_HEAP_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartCommandTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartCommandTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_COMMAND_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartTraceTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartTraceTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_TRACE_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartNotificationTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartNotificationTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_NOTIFICATION_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartWpaEventPathTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartWpaEventPathTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_WPA_EVENT_PATH_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartCreateApTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartCreateApTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_CREATE_AP_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartHostapdLifecycleTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartHostapdLifecycleTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_HOSTAPD_LIFECYCLE_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartHostapdStateUpdateTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartHostapdStateUpdateTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_HOSTAPD_STATE_UPDATE_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartStatusCodeTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }
            PrintApStartStatusCodeTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_STATUS_CODE_TELEMETRY_PASS");
        return 0;
    }

    public static int RunApStartFrameTelemetry()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }
            PrintApStartFrameTelemetryFeature(handle);
        }
        Console.WriteLine("AP_START_FRAME_TELEMETRY_PASS");
        return 0;
    }

    public static int RunRa4ApplicationReset()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            report[1] = 0xD4;
            if (!HidD_SetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_SetFeature failed, win32=" + Marshal.GetLastWin32Error());
            }
        }
        Console.WriteLine("RA4 application reset feature accepted");
        return 0;
    }

    public static int RunBl616clRecovery()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }

            byte[] report = new byte[WindowsReportSize];
            report[0] = 0;
            report[1] = 0xAA;
            if (!HidD_SetFeature(handle, report, report.Length))
            {
                throw new InvalidOperationException(
                    "HidD_SetFeature recovery failed, win32=" +
                    Marshal.GetLastWin32Error());
            }
        }
        Console.WriteLine("BL616CL recovery feature accepted");
        return 0;
    }

    public static int RunRawNresetPulse()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "RAW_NRESET_PULSE asserted_pins=0x" + asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2") +
                    " low_ms=100");
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
        Console.WriteLine("RAW_NRESET_PULSE_PASS");
        return 0;
    }

    public static int RunD4AttachReadOnlyNoFinalReset()
    {
        const uint D4ClockHertz = 100000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;
                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 attach/read-only probe failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }
                Console.WriteLine(
                    "D4 attach/read-only: DP_IDCODE=0x" +
                    idcode.ToString("X8"));
                Console.WriteLine("RA4_SWD_D4_ATTACH_READONLY_NO_FINAL_RESET_PASS");
                return 0;
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4AttachReadOnlyFinalResetNoReattach()
    {
        const uint D4ClockHertz = 100000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;
                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 attach/read-only reset probe failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "D4 attach/read-only final nRESET: DP_IDCODE=0x" +
                    idcode.ToString("X8") +
                    " asserted_pins=0x" + asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2"));
                Console.WriteLine(
                    "RA4_SWD_D4_ATTACH_READONLY_FINAL_RESET_NO_REATTACH_PASS");
                return 0;
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4ReadOnlyReleasePowerFinalReset()
    {
        const uint D4ClockHertz = 100000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;
                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 power-release probe failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                uint before = TransferRead(
                    handle,
                    DapTransferDpReadCtrlStat,
                    "DP CTRL/STAT before power release");
                TransferWrite(
                    handle,
                    DapTransferDpWriteCtrlStat,
                    0x00000000,
                    "DP CTRL/STAT power-down request");
                uint after = TransferRead(
                    handle,
                    DapTransferDpReadCtrlStat,
                    "DP CTRL/STAT after power release");
                if ((after & DpCtrlStatPowerUpMask) != 0)
                {
                    throw new InvalidOperationException(
                        "DP CTRL/STAT power request/acknowledge bits remain set: 0x" +
                        after.ToString("X8"));
                }

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "D4 release-power final nRESET: DP_IDCODE=0x" +
                    idcode.ToString("X8") +
                    " CTRL_STAT_BEFORE=0x" + before.ToString("X8") +
                    " CTRL_STAT_AFTER=0x" + after.ToString("X8") +
                    " asserted_pins=0x" + asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2"));
                Console.WriteLine(
                    "RA4_SWD_D4_RELEASE_POWER_FINAL_RESET_NO_REATTACH_PASS");
                return 0;
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4MinimalIdcodeFinalResetNoReattach()
    {
        const uint D4ClockHertz = 100000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;
                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcodeOnly(
                    handle,
                    D4ClockHertz,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 minimal IDCODE probe failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "D4 minimal IDCODE final nRESET: DP_IDCODE=0x" +
                    idcode.ToString("X8") +
                    " asserted_pins=0x" + asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2"));
                Console.WriteLine("RA4_SWD_D4_MINIMAL_IDCODE_FINAL_RESET_NO_REATTACH_PASS");
                return 0;
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4ReadOnlyDisconnectDelayReconnectReset()
    {
        const uint D4ClockHertz = 100000;
        const int DisconnectSettleMilliseconds = 1000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;
                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 delayed-reset probe failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect before settle");
                connected = false;
                Thread.Sleep(DisconnectSettleMilliseconds);

                response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException(
                        "DAP reconnect after settle did not select SWD");
                }
                connected = true;
                RequireOk(
                    Transact(handle, DapSwdConfigure, 0x00),
                    "DAP_SWD_Configure after settle");
                RequireOk(
                    Transact(
                        handle,
                        DapTransferConfigure,
                        0x00,
                        100,
                        0x00,
                        100,
                        0x00),
                    "DAP_TransferConfigure after settle");

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "D4 disconnect-delay-reconnect reset: DP_IDCODE=0x" +
                    idcode.ToString("X8") +
                    " settle_ms=" + DisconnectSettleMilliseconds +
                    " asserted_pins=0x" + asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2"));
                Console.WriteLine(
                    "RA4_SWD_D4_DISCONNECT_DELAY_RECONNECT_RESET_PASS");
                return 0;
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4HaltRunDisconnectDelayReconnectReset()
    {
        const uint D4ClockHertz = 100000;
        const int DisconnectSettleMilliseconds = 1000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for BL616CL CMSIS-DAP HID path");
            }

            bool connected = false;
            bool targetHalted = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;
                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 delayed halt/run initial attach failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen | DhcsrCHalt,
                    "DHCSR halt request");
                uint haltedDhcsr = WaitForDhcsr(handle, true, "DHCSR halt confirmation");
                targetHalted = true;
                uint[] haltedRomTable = ReadAhbBlock(
                    handle,
                    0xE00FF000,
                    8,
                    "halted ROM table block");
                uint[] haltedRomTableRetry = ReadAhbBlock(
                    handle,
                    0xE00FF000,
                    8,
                    "halted ROM table block retry");
                if (!EqualWords(haltedRomTable, haltedRomTableRetry))
                {
                    throw new InvalidOperationException(
                        "halted target ROM table changed between repeated reads");
                }

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen,
                    "DHCSR run request");
                uint runningDhcsr = WaitForDhcsr(
                    handle,
                    false,
                    "DHCSR run confirmation");
                targetHalted = false;
                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey,
                    "DHCSR debug-disable request");
                uint debugReleasedDhcsr = ReadAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    "DHCSR debug-disable confirmation");
                if ((debugReleasedDhcsr & (DhcsrCDebugen | DhcsrSHalt)) != 0)
                {
                    throw new InvalidOperationException(
                        "DHCSR did not leave debug/run state: 0x" +
                        debugReleasedDhcsr.ToString("X8"));
                }

                RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect before settle");
                connected = false;
                Thread.Sleep(DisconnectSettleMilliseconds);

                response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException(
                        "DAP reconnect after settle did not select SWD");
                }
                connected = true;
                RequireOk(
                    Transact(handle, DapSwdConfigure, 0x00),
                    "DAP_SWD_Configure after settle");
                RequireOk(
                    Transact(
                        handle,
                        DapTransferConfigure,
                        0x00,
                        100,
                        0x00,
                        100,
                        0x00),
                    "DAP_TransferConfigure after settle");

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "D4 halt/run disconnect-delay-reconnect reset: halt=0x" +
                    haltedDhcsr.ToString("X8") +
                    " run=0x" + runningDhcsr.ToString("X8") +
                    " debug_release=0x" + debugReleasedDhcsr.ToString("X8") +
                    " settle_ms=" + DisconnectSettleMilliseconds +
                    " asserted_pins=0x" + asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2"));
                Console.WriteLine(
                    "RA4_SWD_D4_HALT_RUN_DISCONNECT_DELAY_RESET_PASS");
                return 0;
            }
            finally
            {
                if (targetHalted)
                {
                    try
                    {
                        WriteAhbWord(
                            handle,
                            CoreDebugDhcsrAddress,
                            DhcsrDebugKey | DhcsrCDebugen,
                            "DHCSR emergency run request");
                    }
                    catch
                    {
                    }
                }
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD3FrequencySweep()
    {
        const int AttemptsPerResetMode = 3;
        uint[] clockRates = new uint[]
        {
            100000,
            200000,
            400000,
            800000,
            1000000,
            1500000,
            2000000,
            2500000,
            3000000,
            4000000,
            6000000,
            7700000,
            8000000,
        };

        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;

                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint highestStableRate = 0;
                foreach (uint clockRate in clockRates)
                {
                    int passes = 0;
                    int total = 0;
                    foreach (bool switchWhileResetAsserted in new bool[] { false, true })
                    {
                        string resetMode = switchWhileResetAsserted
                            ? "switch-while-reset-asserted"
                            : "switch-after-reset-release";
                        for (int attempt = 1; attempt <= AttemptsPerResetMode; ++attempt)
                        {
                            total++;
                            uint idcode = 0;
                            byte count = 0;
                            byte ack = 0;
                            bool passed = false;
                            try
                            {
                passed = TryReadDpIdcode(
                    handle,
                    clockRate,
                    switchWhileResetAsserted,
                    20,
                    out idcode,
                    out count,
                    out ack);
                            }
                            catch (Exception error)
                            {
                                Console.WriteLine(
                                    "D3 sweep failure: requested_hz=" + clockRate +
                                    " reset_mode=" + resetMode +
                                    " attempt=" + attempt +
                                    " error=" + error.Message);
                            }

                            if (passed)
                            {
                                passes++;
                                Console.WriteLine(
                                    "D3 sweep pass: requested_hz=" + clockRate +
                                    " reset_mode=" + resetMode +
                                    " attempt=" + attempt);
                            }
                            else
                            {
                                Console.WriteLine(
                                    "D3 sweep miss: requested_hz=" + clockRate +
                                    " reset_mode=" + resetMode +
                                    " attempt=" + attempt +
                                    " count=" + count +
                                    " ack=0x" + ack.ToString("X2"));
                            }
                        }
                    }

                    bool stable = passes == total;
                    Console.WriteLine(
                        "D3_SWEEP_RATE requested_hz=" + clockRate +
                        " passes=" + passes +
                        " total=" + total +
                        " stable=" + (stable ? "1" : "0"));
                    if (stable)
                    {
                        highestStableRate = clockRate;
                    }
                }

                if (highestStableRate == 0)
                {
                    throw new InvalidOperationException(
                        "D3 frequency sweep found no stable requested SWD clock");
                }
                Console.WriteLine(
                    "RA4_SWD_D3_FREQUENCY_SWEEP_PASS highest_requested_hz=" +
                    highestStableRate);
                return 0;
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4HaltRunResetRecovery()
    {
        const uint D4ClockHertz = 100000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            bool connected = false;
            bool targetHalted = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;

                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 initial attach failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen | DhcsrCHalt,
                    "DHCSR halt request");
                uint haltedDhcsr = WaitForDhcsr(handle, true, "DHCSR halt confirmation");
                targetHalted = true;

                uint[] haltedRomTable = ReadAhbBlock(
                    handle,
                    0xE00FF000,
                    8,
                    "halted ROM table block");
                uint[] haltedRomTableRetry = ReadAhbBlock(
                    handle,
                    0xE00FF000,
                    8,
                    "halted ROM table block retry");
                if (!EqualWords(haltedRomTable, haltedRomTableRetry))
                {
                    throw new InvalidOperationException(
                        "halted target ROM table changed between repeated reads");
                }
                uint haltedCpuid = ReadAhbWord(
                    handle,
                    ScsCpuidAddress,
                    "halted CPUID");

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen,
                    "DHCSR run request");
                uint runningDhcsr = WaitForDhcsr(
                    handle,
                    false,
                    "DHCSR run confirmation");
                targetHalted = false;

                /*
                 * A CMSIS-DAP client normally leaves C_DEBUGEN clear when it
                 * returns the target to its standalone application. Explicitly
                 * clear it before the raw nRESET phase so the recovery gate
                 * validates application startup rather than a residual debug
                 * session.
                 */
                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey,
                    "DHCSR debug-disable request");
                uint debugReleasedDhcsr = ReadAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    "DHCSR debug-disable confirmation");
                if ((debugReleasedDhcsr & (DhcsrCDebugen | DhcsrSHalt)) != 0)
                {
                    throw new InvalidOperationException(
                        "DHCSR did not leave debug/run state: 0x" +
                        debugReleasedDhcsr.ToString("X8"));
                }

                uint resetIdcode;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out resetIdcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 nRESET recovery failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }
                uint resetDhcsr = ReadAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    "DHCSR after nRESET recovery");

                Console.WriteLine(
                    "D4 halt: DHCSR=0x" + haltedDhcsr.ToString("X8") +
                    " CPUID=0x" + haltedCpuid.ToString("X8") +
                    " ROM=" + FormatWords(haltedRomTable));
                Console.WriteLine(
                    "D4 run: DHCSR=0x" + runningDhcsr.ToString("X8"));
                Console.WriteLine(
                    "D4 debug release: DHCSR=0x" +
                    debugReleasedDhcsr.ToString("X8"));
                Console.WriteLine(
                    "D4 nRESET recovery: DP_IDCODE=0x" +
                    resetIdcode.ToString("X8") +
                    " DHCSR=0x" + resetDhcsr.ToString("X8"));
                Console.WriteLine("RA4_SWD_D4_HALT_RUN_RESET_PASS");
                return 0;
            }
            finally
            {
                if (targetHalted)
                {
                    try
                    {
                        WriteAhbWord(
                            handle,
                            CoreDebugDhcsrAddress,
                            DhcsrDebugKey | DhcsrCDebugen,
                            "DHCSR emergency run request");
                    }
                    catch
                    {
                    }
                }
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4HaltRunFinalResetNoReattach()
    {
        const uint D4ClockHertz = 100000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            bool connected = false;
            bool targetHalted = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;

                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 no-reattach initial attach failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen | DhcsrCHalt,
                    "DHCSR halt request");
                uint haltedDhcsr = WaitForDhcsr(handle, true, "DHCSR halt confirmation");
                targetHalted = true;

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen,
                    "DHCSR run request");
                uint runningDhcsr = WaitForDhcsr(
                    handle,
                    false,
                    "DHCSR run confirmation");
                targetHalted = false;

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey,
                    "DHCSR debug-disable request");
                uint debugReleasedDhcsr = ReadAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    "DHCSR debug-disable confirmation");
                if ((debugReleasedDhcsr & (DhcsrCDebugen | DhcsrSHalt)) != 0)
                {
                    throw new InvalidOperationException(
                        "DHCSR did not leave debug/run state: 0x" +
                        debugReleasedDhcsr.ToString("X8"));
                }

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "D4 final nRESET without reattach: asserted_pins=0x" +
                    asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2"));
                Console.WriteLine(
                    "D4 no-reattach: halt=0x" + haltedDhcsr.ToString("X8") +
                    " run=0x" + runningDhcsr.ToString("X8") +
                    " debug_release=0x" + debugReleasedDhcsr.ToString("X8"));
                Console.WriteLine("RA4_SWD_D4_FINAL_RESET_NO_REATTACH_PASS");
                return 0;
            }
            finally
            {
                if (targetHalted)
                {
                    try
                    {
                        WriteAhbWord(
                            handle,
                            CoreDebugDhcsrAddress,
                            DhcsrDebugKey | DhcsrCDebugen,
                            "DHCSR emergency run request");
                    }
                    catch
                    {
                    }
                }
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int RunD4HaltRunDisconnectReconnectReset()
    {
        const uint D4ClockHertz = 100000;
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);
        using (SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero))
        {
            if (handle.IsInvalid)
            {
                throw new InvalidOperationException(
                    "CreateFile failed for CMSIS-DAP HID path");
            }

            bool connected = false;
            bool targetHalted = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;
                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                uint idcode;
                byte count;
                byte ack;
                if (!TryReadDpIdcode(
                    handle,
                    D4ClockHertz,
                    false,
                    100,
                    out idcode,
                    out count,
                    out ack))
                {
                    throw new InvalidOperationException(
                        "D4 reconnect-reset initial attach failed: count=" + count +
                        " ack=0x" + ack.ToString("X2"));
                }

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen | DhcsrCHalt,
                    "DHCSR halt request");
                uint haltedDhcsr = WaitForDhcsr(handle, true, "DHCSR halt confirmation");
                targetHalted = true;

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey | DhcsrCDebugen,
                    "DHCSR run request");
                uint runningDhcsr = WaitForDhcsr(
                    handle,
                    false,
                    "DHCSR run confirmation");
                targetHalted = false;

                WriteAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    DhcsrDebugKey,
                    "DHCSR debug-disable request");
                uint debugReleasedDhcsr = ReadAhbWord(
                    handle,
                    CoreDebugDhcsrAddress,
                    "DHCSR debug-disable confirmation");
                if ((debugReleasedDhcsr & (DhcsrCDebugen | DhcsrSHalt)) != 0)
                {
                    throw new InvalidOperationException(
                        "DHCSR did not leave debug/run state: 0x" +
                        debugReleasedDhcsr.ToString("X8"));
                }

                RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect before reset");
                connected = false;
                response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException(
                        "DAP reconnect did not select SWD");
                }
                connected = true;
                RequireOk(
                    Transact(handle, DapSwdConfigure, 0x00),
                    "DAP_SWD_Configure after reconnect");
                RequireOk(
                    Transact(
                        handle,
                        DapTransferConfigure,
                        0x00,
                        100,
                        0x00,
                        100,
                        0x00),
                    "DAP_TransferConfigure after reconnect");

                byte asserted = SwjPins(handle, 0x00, DapSwjPinReset, 100);
                byte released = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "D4 disconnect/reconnect reset: asserted_pins=0x" +
                    asserted.ToString("X2") +
                    " released_pins=0x" + released.ToString("X2"));
                Console.WriteLine(
                    "D4 disconnect/reconnect: halt=0x" + haltedDhcsr.ToString("X8") +
                    " run=0x" + runningDhcsr.ToString("X8") +
                    " debug_release=0x" + debugReleasedDhcsr.ToString("X8"));
                Console.WriteLine("RA4_SWD_D4_DISCONNECT_RECONNECT_RESET_PASS");
                return 0;
            }
            finally
            {
                if (targetHalted)
                {
                    try
                    {
                        WriteAhbWord(
                            handle,
                            CoreDebugDhcsrAddress,
                            DhcsrDebugKey | DhcsrCDebugen,
                            "DHCSR emergency run request");
                    }
                    catch
                    {
                    }
                }
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }
    }

    public static int Main()
    {
        string path = FindCmsisDapPath();
        Console.WriteLine("CMSIS-DAP HID path: " + path);

        SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0U,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            throw new InvalidOperationException(
                "CreateFile failed for CMSIS-DAP HID path");
        }

        using (handle)
        {
            string vendor = System.Text.Encoding.ASCII.GetString(DapInfoRequest(handle, 0x01));
            string product = System.Text.Encoding.ASCII.GetString(DapInfoRequest(handle, 0x02));
            byte[] packetSize = DapInfoRequest(handle, 0xFF);
            byte[] capabilities = DapInfoRequest(handle, 0xF0);
            Console.WriteLine(
                "CMSIS-DAP: vendor='" + vendor + "' product='" + product +
                "' packet_size=" + BitConverter.ToUInt16(packetSize, 0) +
                " capabilities=0x" +
                (capabilities.Length == 0 ? "00" : capabilities[0].ToString("X2")));

            bool connected = false;
            try
            {
                byte[] response = Transact(handle, DapConnect, DapPortSwd);
                if (response[1] != DapPortSwd)
                {
                    throw new InvalidOperationException("DAP_Connect did not select SWD");
                }
                connected = true;

                RequireOk(Transact(handle, DapSwdConfigure, 0x00), "DAP_SWD_Configure");
                RequireOk(
                    Transact(handle, DapTransferConfigure, 0x00, 100, 0x00, 100, 0x00),
                    "DAP_TransferConfigure");

                byte resetAsserted = SwjPins(handle, 0x00, DapSwjPinReset, 20);
                Console.WriteLine(
                    "SWDIO release diagnostic setup, nRESET asserted, SWJ pins=0x" +
                    resetAsserted.ToString("X2"));
                PrintReleasedSwdioDiagnostic(handle, true);
                byte resetReleased = SwjPins(
                    handle,
                    DapSwjPinReset,
                    DapSwjPinReset,
                    20);
                Console.WriteLine(
                    "SWDIO release diagnostic setup, nRESET released, SWJ pins=0x" +
                    resetReleased.ToString("X2"));
                PrintReleasedSwdioDiagnostic(handle, false);

                uint[] clockRates = new uint[] { 10000, 50000, 100000 };
                int successfulAttempts = 0;
                foreach (bool switchWhileResetAsserted in new bool[] { false, true })
                {
                    string resetMode = switchWhileResetAsserted
                        ? "switch-while-reset-asserted"
                        : "switch-after-reset-release";
                    foreach (uint clockRate in clockRates)
                    {
                        Console.WriteLine(
                            "SWD attempt: " + resetMode + ", " + clockRate + " Hz");
                        uint idcode;
                        byte count;
                        byte ack;
                        if (TryReadDpIdcode(
                            handle,
                            clockRate,
                            switchWhileResetAsserted,
                            20,
                            out idcode,
                            out count,
                            out ack))
                        {
                            Console.WriteLine(
                                "RA4M1 SWD DP IDCODE: 0x" + idcode.ToString("X8"));
                            successfulAttempts++;
                            continue;
                        }
                        Console.WriteLine(
                            "  DP IDCODE read failed: count=" + count +
                            " ack=0x" + ack.ToString("X2"));
                    }
                }
                if (successfulAttempts == 0)
                {
                    throw new InvalidOperationException(
                        "DP IDCODE read did not ACK across all read-only timing attempts");
                }
                Console.WriteLine(
                    "CMSIS-DAP SWD read-only probe: PASS (" +
                    successfulAttempts + "/6 timing attempts)");
                return 0;
            }
            finally
            {
                if (connected)
                {
                    RequireOk(Transact(handle, DapDisconnect), "DAP_Disconnect");
                }
            }
        }

    }
}
