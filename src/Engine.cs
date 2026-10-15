// Models, settings, limiter math and the audio engine.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace VolumeGuard
{
    static class Db
    {
        public const float MinDb = -60f; // left edge of the limit slider / meters

        public static float ToLin(float db) { return (float)Math.Pow(10.0, db / 20.0); }
        public static float FromLin(float lin) { return lin <= 1e-6f ? -120f : (float)(20.0 * Math.Log10(lin)); }
        public static float Clamp(float v) { return v < 0f ? 0f : (v > 1f ? 1f : v); }
        public static float LevelToPos(float lin) { return Clamp((FromLin(lin) - MinDb) / -MinDb); }
        public static float DbToPos(float db) { return Clamp((db - MinDb) / -MinDb); }
        public static float PosToDb(float pos) { return (float)Math.Round(MinDb + Clamp(pos) * -MinDb); }
    }

    /// One "channel" the limiter works on: either an application or the whole device.
    class AppModel
    {
        // persisted
        public string Key = "";
        public string Title = "";
        public float UserVol = 1f;      // volume the user chose, 0..1 (same scale as the Windows mixer)
        public bool LimitOn;
        public float LimitDb = -12f;    // max output peak, dBFS
        public bool Muted;
        public bool Pending;            // we lowered its volume and Windows may have remembered that

        // runtime
        public Icon Icon;
        public bool Adopted;
        public float Gain = 1f;         // volume actually applied (<= UserVol)
        public float Env;               // peak envelope of the input signal
        public float OutLevel;          // estimated output peak, linear
        public float DisplayPeak;       // highest OutLevel since the window last drew it
        public bool Limiting;
        public bool Active;
        public int UiVersion;
        public List<SessionWrap> Sessions = new List<SessionWrap>();
        public bool Present { get { return Sessions.Count > 0; } }
    }

    class MasterModel
    {
        public float UserVol = 0.5f;    // Windows master volume scalar the user chose
        public bool LimitOn;
        public float LimitDb = -12f;
        public bool Pending;
        public float R = 1f;            // our reduction factor, linear (1 = none)
        public float Env;
        public float OutLevel;
        public float DisplayPeak;
        public bool Limiting;
        public int UiVersion;
    }

    class SessionWrap
    {
        public string Id;
        public IAudioSessionControl2 Ctl;
        public ISimpleAudioVolume Vol;
        public IAudioMeterInformation Meter;
        public float LastSet = -1f;
        public bool Seen, Dead, Active;
    }

    class Settings
    {
        public bool Enabled = true;
        public string DeviceId = "";
        public bool Tray = true;
        public bool TrayHintShown;
        // Whether peak meters report the level after the volume we set (see README).
        public bool SessionMeterPost = false;
        public bool MasterMeterPost = false;
        public MasterModel Master = new MasterModel();
        public Dictionary<string, AppModel> Apps = new Dictionary<string, AppModel>(StringComparer.OrdinalIgnoreCase);
        public bool Dirty;

        static readonly CultureInfo IC = CultureInfo.InvariantCulture;

        public static string Dir
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "VolumeGuard"); }
        }
        public static string FilePath { get { return Path.Combine(Dir, "settings.txt"); } }

        public AppModel GetApp(string key, string title)
        {
            AppModel a;
            if (!Apps.TryGetValue(key, out a))
            {
                a = new AppModel { Key = key, Title = title };
                Apps[key] = a;
                Dirty = true;
            }
            return a;
        }

        static string B(bool b) { return b ? "1" : "0"; }
        static string F(float f) { return f.ToString("0.####", IC); }
        static float PF(string s, float def) { float f; return float.TryParse(s, NumberStyles.Float, IC, out f) ? f : def; }
        static string Clean(string s) { return (s ?? "").Replace("|", "/").Replace("\r", " ").Replace("\n", " "); }

        public static Settings Load()
        {
            var s = new Settings();
            try
            {
                string path = FilePath;
                if (!File.Exists(path))
                {
                    // first run after the rename: pick up settings saved under the old name
                    string old = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "SoundLockPlus", "settings.txt");
                    if (!File.Exists(old)) return s;
                    path = old;
                    s.Dirty = true;
                }
                foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
                {
                    var p = line.Split(new[] { '|' });
                    if (p.Length < 2) continue;
                    switch (p[0])
                    {
                        case "opt":
                            if (p.Length < 3) break;
                            if (p[1] == "enabled") s.Enabled = p[2] == "1";
                            else if (p[1] == "device") s.DeviceId = p[2];
                            else if (p[1] == "tray") s.Tray = p[2] == "1";
                            else if (p[1] == "trayhint") s.TrayHintShown = p[2] == "1";
                            else if (p[1] == "sessionMeterAfterVolume") s.SessionMeterPost = p[2] == "1";
                            else if (p[1] == "masterMeterPost") s.MasterMeterPost = p[2] == "1";
                            break;
                        case "master":
                            if (p.Length < 5) break;
                            s.Master.UserVol = Db.Clamp(PF(p[1], 0.5f));
                            s.Master.LimitOn = p[2] == "1";
                            s.Master.LimitDb = PF(p[3], -12f);
                            s.Master.Pending = p[4] == "1";
                            break;
                        case "app":
                            if (p.Length < 8) break;
                            var a = new AppModel
                            {
                                Key = p[1], Title = p[2], UserVol = Db.Clamp(PF(p[3], 1f)), LimitOn = p[4] == "1",
                                LimitDb = PF(p[5], -12f), Muted = p[6] == "1", Pending = p[7] == "1"
                            };
                            if (p.Length > 8) a.Env = Math.Max(0f, Math.Min(1.5f, PF(p[8], 0f)));
                            a.Gain = a.UserVol;
                            s.Apps[a.Key] = a;
                            break;
                    }
                }
            }
            catch { }
            return s;
        }

        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Dir);
                var sb = new StringBuilder();
                sb.AppendLine("# VolumeGuard settings");
                sb.AppendLine("opt|enabled|" + B(Enabled));
                sb.AppendLine("opt|device|" + Clean(DeviceId));
                sb.AppendLine("opt|tray|" + B(Tray));
                sb.AppendLine("opt|trayhint|" + B(TrayHintShown));
                sb.AppendLine("opt|sessionMeterAfterVolume|" + B(SessionMeterPost));
                sb.AppendLine("opt|masterMeterPost|" + B(MasterMeterPost));
                sb.AppendLine(string.Join("|", new[] { "master", F(Master.UserVol), B(Master.LimitOn), F(Master.LimitDb), B(Master.Pending) }));
                foreach (var a in Apps.Values)
                    sb.AppendLine(string.Join("|", new[] { "app", Clean(a.Key), Clean(a.Title), F(a.UserVol), B(a.LimitOn), F(a.LimitDb), B(a.Muted), B(a.Pending), F(a.Env) }));
                string tmp = FilePath + ".tmp";
                File.WriteAllText(tmp, sb.ToString(), Encoding.UTF8);
                File.Copy(tmp, FilePath, true);
                File.Delete(tmp);
                Dirty = false;
            }
            catch { }
        }
    }

    static class Limiter
    {
        /// Below this level (−50 dBFS) the input counts as silence.
        public static readonly float Gate = Db.ToLin(-50f);

        /// Level assumed for an app we have not heard yet (−3 dBFS): its volume starts already limited,
        /// so the first sound after start-up or after switching the limit on is not too loud.
        public const float StartEnv = 0.7f;

        /// One step of a feed-forward peak limiter that works by moving a volume knob.
        /// inPeak — level of the signal before our gain; instant attack, smooth release.
        /// The envelope only decays while there is sound: during silence the reduced volume is held,
        /// so a loud sound that starts after a pause does not hit at full volume.
        public static void Step(ref float gain, ref float env, float inPeak, float user, bool on, float limitDb,
                                float floor, float decay, float rel)
        {
            env = inPeak >= Gate ? Math.Max(inPeak, env * decay) : Math.Max(inPeak, env);
            float target = user;
            if (on && env > 1e-4f) target = Math.Min(target, Db.ToLin(limitDb) / env);
            if (target < floor) target = floor;
            if (target < gain) gain = target;
            else
            {
                gain += (target - gain) * rel;
                if (target - gain < 0.002f) gain = target;
            }
            if (gain > user) gain = user;
        }
    }

    interface IEngine
    {
        string Status { get; }
        bool IsOpen { get; }
        string DeviceName { get; }
        List<KeyValuePair<string, string>> ListDevices();
        bool Open();
        void Close();
        void Tick(float dt);
        bool RefreshSessions();
        void SetAppVolume(AppModel a, float v);
        void SetAppMute(AppModel a, bool mute);
        void SetMasterVolume(float v);
        void OnEnabledChanged();
    }

    class AudioEngine : IEngine
    {
        const int CLSCTX_ALL = 0x17;
        const int DEVICE_STATE_ACTIVE = 1;

        readonly Settings s;
        IMMDeviceEnumerator en;
        IMMDevice dev;
        string devId;
        IAudioEndpointVolume epv;
        IAudioMeterInformation epMeter;
        IAudioSessionManager2 mgr;
        readonly Dictionary<string, SessionWrap> sessions = new Dictionary<string, SessionWrap>();
        readonly Dictionary<int, string[]> procCache = new Dictionary<int, string[]>();
        readonly Dictionary<string, Icon> iconCache = new Dictionary<string, Icon>(StringComparer.OrdinalIgnoreCase);
        Guid ctx = Guid.NewGuid();
        int postMismatch;
        float userDb, minDb = -65f, maxDb = 0f, lastSetDb = float.NaN, lastScalar = -1f;

        public string Status { get; private set; }
        public bool IsOpen { get { return epv != null; } }
        public string DeviceName { get; private set; }

        public AudioEngine(Settings s) { this.s = s; }

        IMMDeviceEnumerator En
        {
            get
            {
                if (en == null)
                    en = (IMMDeviceEnumerator)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")));
                return en;
            }
        }

        public List<KeyValuePair<string, string>> ListDevices()
        {
            var list = new List<KeyValuePair<string, string>>();
            try
            {
                var col = En.EnumAudioEndpoints(EDataFlow.eRender, DEVICE_STATE_ACTIVE);
                int n = col.GetCount();
                for (int i = 0; i < n; i++)
                {
                    var d = col.Item(i);
                    list.Add(new KeyValuePair<string, string>(d.GetId(), FriendlyName(d)));
                }
            }
            catch { }
            return list;
        }

        static string FriendlyName(IMMDevice d)
        {
            try
            {
                var ps = d.OpenPropertyStore(0);
                var key = new PropertyKey { fmtid = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), pid = 14 };
                PropVariant pv;
                if (ps.GetValue(ref key, out pv) != 0) return "Устройство";
                string name = pv.vt == 31 ? Marshal.PtrToStringUni(pv.p) : null;
                Native.PropVariantClear(ref pv);
                return string.IsNullOrEmpty(name) ? "Устройство" : name;
            }
            catch { return "Устройство"; }
        }

        public bool Open()
        {
            try
            {
                dev = null;
                if (!string.IsNullOrEmpty(s.DeviceId))
                {
                    try
                    {
                        dev = En.GetDevice(s.DeviceId);
                        if (dev.GetState() != DEVICE_STATE_ACTIVE) dev = null;
                    }
                    catch { dev = null; }
                }
                if (dev == null) dev = En.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eMultimedia);
                devId = dev.GetId();
                DeviceName = FriendlyName(dev);

                Guid iid = typeof(IAudioEndpointVolume).GUID;
                epv = (IAudioEndpointVolume)dev.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero);
                iid = typeof(IAudioMeterInformation).GUID;
                epMeter = (IAudioMeterInformation)dev.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero);
                iid = typeof(IAudioSessionManager2).GUID;
                mgr = (IAudioSessionManager2)dev.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero);

                float inc;
                epv.GetVolumeRange(out minDb, out maxDb, out inc);

                var m = s.Master;
                if (m.Pending)
                {
                    // last run ended (or crashed) while the master volume was lowered — put it back
                    epv.SetMasterVolumeLevelScalar(m.UserVol, ref ctx);
                    m.Pending = false;
                    s.Dirty = true;
                }
                else m.UserVol = epv.GetMasterVolumeLevelScalar();
                userDb = epv.GetMasterVolumeLevel();
                lastScalar = epv.GetMasterVolumeLevelScalar();
                lastSetDb = userDb;
                m.R = 1f; m.Env = Limiter.StartEnv * Db.ToLin(userDb); m.UiVersion++;

                Status = null;
                RefreshSessions();
                return true;
            }
            catch
            {
                Status = "Нет доступного устройства вывода звука";
                Drop();
                return false;
            }
        }

        void Drop()
        {
            foreach (var a in s.Apps.Values) { a.Sessions.Clear(); a.Adopted = false; a.Active = false; }
            sessions.Clear();
            epv = null; epMeter = null; mgr = null; dev = null; devId = null;
        }

        public void Close()
        {
            try { RestoreAll(); } catch { }
            Drop();
        }

        void RestoreAll()
        {
            foreach (var a in s.Apps.Values)
            {
                if (!a.Present) continue;
                a.Gain = a.UserVol;
                foreach (var w in a.Sessions)
                {
                    try { w.Vol.SetMasterVolume(a.UserVol, ref ctx); w.LastSet = a.UserVol; } catch { w.Dead = true; }
                }
                if (a.Pending) { a.Pending = false; s.Dirty = true; }
            }
            var m = s.Master;
            if (epv != null && m.R < 0.999f)
            {
                try
                {
                    epv.SetMasterVolumeLevel(Math.Max(minDb, Math.Min(maxDb, userDb)), ref ctx);
                    lastSetDb = userDb;
                    lastScalar = epv.GetMasterVolumeLevelScalar();
                }
                catch { }
            }
            m.R = 1f;
            if (m.Pending) { m.Pending = false; s.Dirty = true; }
        }

        public void OnEnabledChanged()
        {
            if (!s.Enabled) RestoreAll();
            s.Dirty = true;
        }

        public bool RefreshSessions()
        {
            if (mgr == null) return false;

            // follow the Windows default device when "По умолчанию" is selected
            if (string.IsNullOrEmpty(s.DeviceId))
            {
                try
                {
                    var d = En.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eMultimedia);
                    bool other = d.GetId() != devId;
                    Release(d, dev);
                    if (other) { Close(); Open(); return true; }
                }
                catch { }
            }

            bool changed = false;
            foreach (var w in sessions.Values) w.Seen = false;
            IAudioSessionEnumerator se = null;
            try
            {
                se = mgr.GetSessionEnumerator();
                int n = se.GetCount();
                for (int i = 0; i < n; i++)
                {
                    IAudioSessionControl2 c;
                    int state;
                    string id;
                    try
                    {
                        c = se.GetSession(i);
                        state = c.GetState();
                        id = c.GetSessionInstanceIdentifier();
                    }
                    catch { continue; }
                    if (state == 2 || string.IsNullOrEmpty(id)) continue; // expired

                    SessionWrap w;
                    if (sessions.TryGetValue(id, out w))
                    {
                        if (!w.Dead) { w.Seen = true; w.Active = state == 1; }
                        continue;
                    }
                    try
                    {
                        w = new SessionWrap { Id = id, Ctl = c, Vol = (ISimpleAudioVolume)c, Meter = (IAudioMeterInformation)c, Seen = true, Active = state == 1 };
                        string key, title;
                        Icon icon;
                        Identify(c, out key, out title, out icon);
                        var app = s.GetApp(key, title);
                        app.Title = title;
                        if (app.Icon == null) app.Icon = icon;
                        Attach(app, w);
                        sessions[id] = w;
                        changed = true;
                    }
                    catch { }
                }
            }
            catch { }
            finally
            {
                // a new enumerator every second: free it now instead of waiting for the garbage collector
                Release(se, null);
            }

            foreach (var w in sessions.Values.Where(x => !x.Seen || x.Dead).ToList())
            {
                Release(w.Ctl, null);
                sessions.Remove(w.Id);
                foreach (var a in s.Apps.Values) a.Sessions.Remove(w);
                changed = true;
            }

            foreach (var a in s.Apps.Values)
            {
                if (!a.Present)
                {
                    a.Adopted = false; a.Active = false; a.OutLevel = 0f; a.Env = 0f;
                    continue;
                }
                a.Active = a.Sessions.Any(x => x.Active);
                try
                {
                    bool mute = a.Sessions[0].Vol.GetMute();
                    if (mute != a.Muted) { a.Muted = mute; a.UiVersion++; s.Dirty = true; }
                }
                catch { }
            }
            if (procCache.Count > 256) procCache.Clear();
            return changed;
        }

        /// Frees a COM object we no longer need. Never frees one that is still in use (keep).
        static void Release(object com, object keep)
        {
            if (com == null || ReferenceEquals(com, keep)) return;
            try { if (Marshal.IsComObject(com)) Marshal.FinalReleaseComObject(com); } catch { }
        }

        void Attach(AppModel app, SessionWrap w)
        {
            float cur = w.Vol.GetMasterVolume();
            bool mute = w.Vol.GetMute();
            if (!app.Adopted)
            {
                app.Adopted = true;
                if (app.Pending)
                {
                    // we left this app quieter than the user wanted (closed while limiting / crash)
                    app.Gain = app.UserVol;
                    app.Pending = false;
                }
                else
                {
                    app.UserVol = cur;
                    app.Gain = cur;
                    app.Muted = mute;
                }
                // Start already limited: use the level remembered from last time, or a cautious guess,
                // so the first sound of a freshly opened app is not played at full volume.
                if (app.Env < Limiter.Gate) app.Env = Limiter.StartEnv;
                if (s.Enabled && app.LimitOn)
                    app.Gain = Math.Min(app.UserVol, Math.Max(Math.Min(app.UserVol, 0.02f), Db.ToLin(app.LimitDb) / app.Env));
                app.UiVersion++;
                s.Dirty = true;
            }
            if (Math.Abs(cur - app.Gain) > 0.002f) w.Vol.SetMasterVolume(app.Gain, ref ctx);
            if (mute != app.Muted) w.Vol.SetMute(app.Muted, ref ctx);
            w.LastSet = app.Gain;
            app.Sessions.Add(w);
        }

        void Identify(IAudioSessionControl2 c, out string key, out string title, out Icon icon)
        {
            icon = null;
            if (c.IsSystemSoundsSession() == 0)
            {
                key = "#system"; title = "Системные звуки";
                return;
            }
            int pid = 0;
            try { pid = c.GetProcessId(); } catch { }
            string[] info;
            if (!procCache.TryGetValue(pid, out info))
            {
                info = ProcInfo(pid);
                procCache[pid] = info;
            }
            if (info == null)
            {
                string dn = null;
                try { dn = c.GetDisplayName(); } catch { }
                key = "#pid" + pid;
                title = string.IsNullOrEmpty(dn) || dn.StartsWith("@") ? "Неизвестное приложение" : dn;
                return;
            }
            key = info[0]; title = info[1];
            string path = info[2];
            if (path != null && !iconCache.TryGetValue(path, out icon))
            {
                try { icon = Icon.ExtractAssociatedIcon(path); } catch { icon = null; }
                iconCache[path] = icon;
            }
        }

        static string[] ProcInfo(int pid)
        {
            if (pid <= 0) return null;
            string path = Native.ProcessPath(pid);
            string name = null;
            if (path != null) name = Path.GetFileNameWithoutExtension(path);
            else
            {
                try { name = Process.GetProcessById(pid).ProcessName; } catch { }
            }
            if (string.IsNullOrEmpty(name)) return null;
            string title = null;
            if (path != null)
            {
                try { title = FileVersionInfo.GetVersionInfo(path).FileDescription; } catch { }
            }
            if (string.IsNullOrEmpty(title) || title.Trim().Length == 0)
                title = char.ToUpperInvariant(name[0]) + name.Substring(1);
            return new[] { name.ToLowerInvariant(), title.Trim(), path };
        }

        public void Tick(float dt)
        {
            if (epv == null) return;
            float decay = (float)Math.Exp(-dt / 1.5);
            float rel = 1f - (float)Math.Exp(-dt / 0.8);
            try { TickMaster(decay, rel); }
            catch { Status = "Устройство вывода отключено"; Drop(); return; }
            foreach (var a in s.Apps.Values)
                if (a.Present) TickApp(a, decay, rel);
        }

        void TickMaster(float decay, float rel)
        {
            var m = s.Master;
            float scalar = epv.GetMasterVolumeLevelScalar();
            if (lastScalar >= 0f && Math.Abs(scalar - lastScalar) > 0.01f)
            {
                // the user moved the Windows volume (keyboard, tray flyout…) — that is the new wish
                m.UserVol = scalar;
                float oldDb = userDb;
                userDb = epv.GetMasterVolumeLevel();
                lastSetDb = userDb;
                m.Env *= Db.ToLin(userDb - oldDb);
                m.R = 1f; m.UiVersion++;
                s.Dirty = true;
            }
            lastScalar = scalar;

            float userAmp = Db.ToLin(userDb);
            float p = epMeter.GetPeakValue();
            float cur = userAmp * m.R;
            float inLevel = s.MasterMeterPost ? (cur > 1e-4f ? p / cur : 0f) : p;
            float floor = Math.Min(1f, Db.ToLin(Math.Max(minDb - userDb, -60f)));
            Limiter.Step(ref m.R, ref m.Env, inLevel * userAmp, 1f, s.Enabled && m.LimitOn, m.LimitDb, floor, decay, rel);

            float db = userDb + Db.FromLin(m.R);
            if (db < minDb) db = minDb;
            if (float.IsNaN(lastSetDb) || Math.Abs(db - lastSetDb) > 0.1f)
            {
                epv.SetMasterVolumeLevel(db, ref ctx);
                lastSetDb = db;
                lastScalar = epv.GetMasterVolumeLevelScalar();
            }
            m.OutLevel = inLevel * userAmp * m.R;
            if (m.OutLevel > m.DisplayPeak) m.DisplayPeak = m.OutLevel;
            m.Limiting = m.R < 0.97f;
            bool pend = m.R < 0.99f;
            if (pend != m.Pending) { m.Pending = pend; s.Dirty = true; }
        }

        void TickApp(AppModel a, float decay, float rel)
        {
            float inPeak = 0f;
            foreach (var w in a.Sessions)
            {
                if (w.Dead) continue;
                try
                {
                    float cur = w.Vol.GetMasterVolume();
                    if (w.LastSet >= 0f && Math.Abs(cur - w.LastSet) > 0.015f)
                    {
                        // changed in the Windows volume mixer
                        a.UserVol = cur; a.Gain = cur; a.UiVersion++;
                        w.LastSet = cur;
                        s.Dirty = true;
                    }
                    float p = w.Meter.GetPeakValue();
                    float lin = s.SessionMeterPost ? (cur > 1e-3f ? p / cur : 0f) : p;
                    if (lin > inPeak) inPeak = lin;
                }
                catch { w.Dead = true; }
            }
            // A digital stream cannot be much louder than full scale. If the "meter is after the volume"
            // assumption yields that for a while, the meter is really before the volume: switch modes.
            if (s.SessionMeterPost && inPeak > 1.5f)
            {
                if (++postMismatch > 20) { s.SessionMeterPost = false; s.Dirty = true; postMismatch = 0; a.Env = 0f; }
            }
            else if (postMismatch > 0) postMismatch--;
            if (inPeak > 4f) inPeak = 4f;

            Limiter.Step(ref a.Gain, ref a.Env, inPeak, a.UserVol, s.Enabled && a.LimitOn, a.LimitDb,
                         Math.Min(a.UserVol, 0.02f), decay, rel);

            foreach (var w in a.Sessions)
            {
                if (w.Dead || Math.Abs(w.LastSet - a.Gain) <= 0.002f) continue;
                try { w.Vol.SetMasterVolume(a.Gain, ref ctx); w.LastSet = a.Gain; }
                catch { w.Dead = true; }
            }
            a.OutLevel = inPeak * a.Gain;
            if (a.OutLevel > a.DisplayPeak) a.DisplayPeak = a.OutLevel;
            a.Limiting = a.Gain < a.UserVol * 0.97f;
            bool pend = a.Gain < a.UserVol - 0.01f;
            if (pend != a.Pending) { a.Pending = pend; s.Dirty = true; }
        }

        public void SetAppVolume(AppModel a, float v)
        {
            a.UserVol = v;
            a.Gain = v;
            foreach (var w in a.Sessions)
            {
                try { w.Vol.SetMasterVolume(v, ref ctx); w.LastSet = v; } catch { w.Dead = true; }
            }
            s.Dirty = true;
        }

        public void SetAppMute(AppModel a, bool mute)
        {
            a.Muted = mute;
            foreach (var w in a.Sessions)
            {
                try { w.Vol.SetMute(mute, ref ctx); } catch { w.Dead = true; }
            }
            s.Dirty = true;
        }

        public void SetMasterVolume(float v)
        {
            if (epv == null) return;
            var m = s.Master;
            try
            {
                epv.SetMasterVolumeLevelScalar(v, ref ctx);
                float oldDb = userDb;
                userDb = epv.GetMasterVolumeLevel();
                m.Env *= Db.ToLin(userDb - oldDb);
                m.UserVol = v;
                float db = userDb;
                if (m.R < 0.999f)
                {
                    db = Math.Max(minDb, userDb + Db.FromLin(m.R));
                    epv.SetMasterVolumeLevel(db, ref ctx);
                }
                lastSetDb = db;
                lastScalar = epv.GetMasterVolumeLevelScalar();
                s.Dirty = true;
            }
            catch { }
        }
    }

    /// Fake audio for "--demo": lets the UI run anywhere, without Core Audio.
    class FakeEngine : IEngine
    {
        readonly Settings s;
        readonly Random rnd = new Random(7);
        float t;

        public FakeEngine(Settings s)
        {
            this.s = s;
            foreach (var a in s.Apps.Values) { a.Sessions.Add(new SessionWrap()); a.Adopted = true; a.Gain = a.UserVol; a.Active = a.Key != "#system"; }
        }

        public string Status { get { return null; } }
        public bool IsOpen { get { return true; } }
        public string DeviceName { get { return "Динамики (Realtek High Definition Audio)"; } }
        public List<KeyValuePair<string, string>> ListDevices()
        {
            return new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>("a", "Динамики (Realtek High Definition Audio)"),
                new KeyValuePair<string, string>("b", "Наушники (HyperX Cloud II)")
            };
        }
        public bool Open() { return true; }
        public void Close() { }
        public bool RefreshSessions() { return false; }
        public void OnEnabledChanged() { }
        public void SetAppVolume(AppModel a, float v) { a.UserVol = v; a.Gain = v; }
        public void SetAppMute(AppModel a, bool mute) { a.Muted = mute; }
        public void SetMasterVolume(float v) { s.Master.UserVol = v; }

        public void Tick(float dt)
        {
            t += dt;
            float decay = (float)Math.Exp(-dt / 1.5), rel = 1f - (float)Math.Exp(-dt / 0.8);
            float sum = 0f;
            int i = 0;
            foreach (var a in s.Apps.Values)
            {
                i++;
                float burst = (float)Math.Max(0, Math.Sin(t * (0.7 + i * 0.3) + i));
                float inPeak = a.Active ? (0.15f + 0.85f * burst * burst) * (0.6f + 0.4f * (float)rnd.NextDouble()) : 0f;
                Limiter.Step(ref a.Gain, ref a.Env, inPeak, a.UserVol, s.Enabled && a.LimitOn, a.LimitDb, 0.02f, decay, rel);
                a.OutLevel = a.Muted ? 0f : inPeak * a.Gain;
                a.DisplayPeak = Math.Max(a.DisplayPeak, a.OutLevel);
                a.Limiting = a.Gain < a.UserVol * 0.97f;
                sum = Math.Max(sum, a.OutLevel);
            }
            var m = s.Master;
            Limiter.Step(ref m.R, ref m.Env, sum * m.UserVol, 1f, s.Enabled && m.LimitOn, m.LimitDb, 0.01f, decay, rel);
            m.OutLevel = sum * m.UserVol * m.R;
            m.DisplayPeak = Math.Max(m.DisplayPeak, m.OutLevel);
            m.Limiting = m.R < 0.97f;
        }
    }
}
