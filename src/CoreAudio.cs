// Windows Core Audio COM interop (Vista+).
// Vtable order of every interface below must match the Windows SDK headers exactly.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace VolumeGuard
{
    enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
    enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }

    [StructLayout(LayoutKind.Sequential)]
    struct PropertyKey { public Guid fmtid; public int pid; }

    // PROPVARIANT: 16 bytes on x86, 24 bytes on x64 — this layout matches both.
    [StructLayout(LayoutKind.Sequential)]
    struct PropVariant
    {
        public ushort vt;
        public ushort r1, r2, r3;
        public IntPtr p;
        public IntPtr p2;
    }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator
    {
        IMMDeviceCollection EnumAudioEndpoints(EDataFlow dataFlow, int stateMask);
        IMMDevice GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role);
        IMMDevice GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id);
        [PreserveSig] int RegisterEndpointNotificationCallback(IMMNotificationClient client);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IMMNotificationClient client);
    }

    // Implemented by VolumeGuard (AudioEvents): Windows calls these on its own threads.
    [ComImport, Guid("7991EEC9-7E89-4D85-8390-6C703CEC60C0"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMNotificationClient
    {
        [PreserveSig] int OnDeviceStateChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId, int newState);
        [PreserveSig] int OnDeviceAdded([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        [PreserveSig] int OnDeviceRemoved([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        [PreserveSig] int OnDefaultDeviceChanged(EDataFlow flow, ERole role, [MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        [PreserveSig] int OnPropertyValueChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId, PropertyKey key);
    }

    [ComImport, Guid("641DD20B-4D41-49CC-ABA3-174B9477BB08"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionNotification
    {
        [PreserveSig] int OnSessionCreated(IntPtr newSession);   // not kept: the next scan opens it properly
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection
    {
        int GetCount();
        IMMDevice Item(int index);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice
    {
        [return: MarshalAs(UnmanagedType.IUnknown)]
        object Activate(ref Guid iid, int clsCtx, IntPtr activationParams);
        IPropertyStore OpenPropertyStore(int stgmAccess);
        [return: MarshalAs(UnmanagedType.LPWStr)]
        string GetId();
        int GetState();
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore
    {
        int GetCount();
        [PreserveSig] int GetAt(int index, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
        [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int Commit();
    }

    [ComImport, Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioEndpointVolume
    {
        [PreserveSig] int RegisterControlChangeNotify(IntPtr notify);
        [PreserveSig] int UnregisterControlChangeNotify(IntPtr notify);
        int GetChannelCount();
        void SetMasterVolumeLevel(float levelDb, ref Guid ctx);
        void SetMasterVolumeLevelScalar(float level, ref Guid ctx);
        float GetMasterVolumeLevel();
        float GetMasterVolumeLevelScalar();
        void SetChannelVolumeLevel(int channel, float levelDb, ref Guid ctx);
        void SetChannelVolumeLevelScalar(int channel, float level, ref Guid ctx);
        float GetChannelVolumeLevel(int channel);
        float GetChannelVolumeLevelScalar(int channel);
        void SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid ctx);
        [return: MarshalAs(UnmanagedType.Bool)]
        bool GetMute();
        void GetVolumeStepInfo(out int step, out int stepCount);
        void VolumeStepUp(ref Guid ctx);
        void VolumeStepDown(ref Guid ctx);
        int QueryHardwareSupport();
        void GetVolumeRange(out float minDb, out float maxDb, out float incrementDb);
    }

    [ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioMeterInformation
    {
        float GetPeakValue();
        int GetMeteringChannelCount();
        void GetChannelsPeakValues(int count, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] float[] values);
        int QueryHardwareSupport();
    }

    [ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionManager2
    {
        // IAudioSessionManager
        [PreserveSig] int GetAudioSessionControl(IntPtr sessionGuid, int flags, out IntPtr control);
        [PreserveSig] int GetSimpleAudioVolume(IntPtr sessionGuid, int flags, out IntPtr volume);
        // IAudioSessionManager2
        IAudioSessionEnumerator GetSessionEnumerator();
        [PreserveSig] int RegisterSessionNotification(IAudioSessionNotification notify);
        [PreserveSig] int UnregisterSessionNotification(IAudioSessionNotification notify);
        [PreserveSig] int RegisterDuckNotification(IntPtr sessionId, IntPtr notify);
        [PreserveSig] int UnregisterDuckNotification(IntPtr notify);
    }

    [ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionEnumerator
    {
        int GetCount();
        IAudioSessionControl2 GetSession(int index);
    }

    [ComImport, Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionControl2
    {
        // IAudioSessionControl
        int GetState();
        [return: MarshalAs(UnmanagedType.LPWStr)]
        string GetDisplayName();
        void SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string value, ref Guid ctx);
        [return: MarshalAs(UnmanagedType.LPWStr)]
        string GetIconPath();
        void SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string value, ref Guid ctx);
        Guid GetGroupingParam();
        void SetGroupingParam(ref Guid group, ref Guid ctx);
        [PreserveSig] int RegisterAudioSessionNotification(IntPtr notify);
        [PreserveSig] int UnregisterAudioSessionNotification(IntPtr notify);
        // IAudioSessionControl2
        [return: MarshalAs(UnmanagedType.LPWStr)]
        string GetSessionIdentifier();
        [return: MarshalAs(UnmanagedType.LPWStr)]
        string GetSessionInstanceIdentifier();
        int GetProcessId();
        [PreserveSig] int IsSystemSoundsSession(); // S_OK (0) = yes, S_FALSE (1) = no
        void SetDuckingPreference([MarshalAs(UnmanagedType.Bool)] bool optOut);
    }

    [ComImport, Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface ISimpleAudioVolume
    {
        void SetMasterVolume(float level, ref Guid ctx);
        float GetMasterVolume();
        void SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid ctx);
        [return: MarshalAs(UnmanagedType.Bool)]
        bool GetMute();
    }

    static class Native
    {
        public const int WM_HOTKEY = 0x0312;
        public const uint MOD_ALT = 0x1, MOD_CONTROL = 0x2, MOD_NOREPEAT = 0x4000;

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint modifiers, uint vk);

        [DllImport("user32.dll")]
        public static extern bool UnregisterHotKey(IntPtr hWnd, int id);

        [DllImport("ole32.dll")]
        public static extern int PropVariantClear(ref PropVariant pv);

        [DllImport("user32.dll")]
        public static extern bool SetProcessDPIAware();

        [DllImport("user32.dll")]
        public static extern bool DestroyIcon(IntPtr hIcon);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr OpenProcess(int access, bool inherit, int pid);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool QueryFullProcessImageName(IntPtr hProcess, int flags, StringBuilder name, ref int size);

        [DllImport("kernel32.dll")]
        public static extern bool CloseHandle(IntPtr h);

        public const int PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

        [DllImport("winmm.dll")]
        public static extern int timeBeginPeriod(int periodMs);

        [DllImport("winmm.dll")]
        public static extern int timeEndPeriod(int periodMs);

        [DllImport("kernel32.dll")]
        public static extern IntPtr GetCurrentProcess();

        [DllImport("kernel32.dll")]
        public static extern bool SetProcessWorkingSetSize(IntPtr process, IntPtr minSize, IntPtr maxSize);

        /// Gives unused memory back to Windows (used when the window goes to the tray).
        public static void TrimMemory()
        {
            try
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                SetProcessWorkingSetSize(GetCurrentProcess(), (IntPtr)(-1), (IntPtr)(-1));
            }
            catch { }
        }

        public static string ProcessPath(int pid)
        {
            IntPtr h = IntPtr.Zero;
            try
            {
                h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
                if (h == IntPtr.Zero) return null;
                var sb = new StringBuilder(1024);
                int size = sb.Capacity;
                return QueryFullProcessImageName(h, 0, sb, ref size) ? sb.ToString() : null;
            }
            catch { return null; }
            finally { if (h != IntPtr.Zero) CloseHandle(h); }
        }
    }
}
