// The limiter's own thread. The window's timer is too slow and too uneven for a limiter that works by
// moving volume knobs: it fires every 10–16 ms and waits while the window paints, so the start of a
// loud sound got through for 10–30 ms (heard as a click). Here the meters are read every 2 ms, with
// MMCSS priority, and a volume is turned down as soon as Windows reports the loud sound.
//
// All Core Audio objects are created and used on this thread only (a COM object made on the window's
// thread cannot simply be called from another one). The window talks to it through this class.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace VolumeGuard
{
    class ThreadedEngine : IEngine
    {
        readonly AudioEngine inner;
        readonly Settings s;
        readonly Thread thread;
        readonly AutoResetEvent wake = new AutoResetEvent(false);
        readonly Queue<Action> queue = new Queue<Action>();
        volatile bool quit, changed;
        volatile string status;
        volatile bool isOpen;
        volatile string deviceName;
        volatile bool hardwareVolume;

        /// Fast step: meters + turning down. Full step (volume back up, changes made in Windows): every 10 ms.
        const int FastMs = 2, FullMs = 10;

        public ThreadedEngine(Settings s)
        {
            this.s = s;
            inner = new AudioEngine(s);
            thread = new Thread(Run) { Name = "VolumeGuard limiter", IsBackground = true, Priority = ThreadPriority.Highest };
            thread.SetApartmentState(ApartmentState.MTA);
            thread.Start();
        }

        public string Status { get { return status; } }
        public bool IsOpen { get { return isOpen; } }
        public string DeviceName { get { return deviceName; } }
        public bool HardwareVolume { get { return hardwareVolume; } }

        public List<KeyValuePair<string, string>> ListDevices()
        {
            return Call(() => inner.ListDevices()) ?? new List<KeyValuePair<string, string>>();
        }

        public bool Open() { return Call(() => inner.Open()); }
        public void Close() { Call(() => { inner.Close(); return true; }); }

        /// The audio thread ticks by itself.
        public void Tick(float dt) { }

        /// The audio thread refreshes the sessions every second; this tells the window whether the list changed.
        public bool RefreshSessions()
        {
            bool c = changed;
            changed = false;
            return c;
        }

        public void SetAppVolume(AppModel a, float v) { Post(() => inner.SetAppVolume(a, v)); }
        public void SetAppMute(AppModel a, bool mute) { Post(() => inner.SetAppMute(a, mute)); }
        public void SetMasterVolume(float v) { Post(() => inner.SetMasterVolume(v)); }
        public void OnEnabledChanged() { Post(() => inner.OnEnabledChanged()); }

        /// Puts all volumes back and ends the thread (on exit).
        public void Stop()
        {
            Close();
            quit = true;
            wake.Set();
            thread.Join(2000);
        }

        void Post(Action a)
        {
            lock (queue) queue.Enqueue(a);
            wake.Set();
        }

        /// Runs f on the audio thread and waits for it. The window must not hold Settings.Sync while calling.
        T Call<T>(Func<T> f)
        {
            if (Thread.CurrentThread == thread) return f();
            if (quit) return default(T);
            var done = new ManualResetEvent(false);
            T result = default(T);
            Post(() =>
            {
                try { result = f(); }
                finally { done.Set(); }
            });
            if (!done.WaitOne(5000)) return default(T);   // the thread is stuck: do not hang the window
            done.Close();
            return result;
        }

        void Run()
        {
            IntPtr mmcss = IntPtr.Zero;
            try
            {
                int task = 0;
                mmcss = AvSetMmThreadCharacteristics("Games", ref task);
            }
            catch { }
            try { Native.timeBeginPeriod(1); } catch { }

            var clock = Stopwatch.StartNew();
            double last = 0, lastFull = 0, lastRefresh = 0;
            while (!quit)
            {
                wake.WaitOne(FastMs);
                // queued work from the window
                while (true)
                {
                    Action a = null;
                    lock (queue) if (queue.Count > 0) a = queue.Dequeue();
                    if (a == null) break;
                    lock (s.Sync) { try { a(); } catch (Exception ex) { Report("command", ex); } }
                    Publish();
                }
                if (quit) break;

                double now = clock.Elapsed.TotalSeconds;
                if (now - last < FastMs / 1000.0 * 0.5) continue;   // woken early by a posted action
                float dt = (float)Math.Min(0.25, Math.Max(0.0005, now - last));
                last = now;
                bool full = now - lastFull >= FullMs / 1000.0;
                if (full) lastFull = now;
                lock (s.Sync)
                {
                    try
                    {
                        inner.Tick(dt, full);   // every step advances the limiter by the time since the previous one
                        if (now - lastRefresh >= 1.0)
                        {
                            lastRefresh = now;
                            if (inner.IsOpen && inner.RefreshSessions()) changed = true;
                        }
                    }
                    catch (Exception ex) { Report(full ? "full step" : "fast step", ex); }
                }
                Publish();
            }
            try { Native.timeEndPeriod(1); } catch { }
            if (mmcss != IntPtr.Zero) { try { AvRevertMmThreadCharacteristics(mmcss); } catch { } }
        }

        // Errors are survived (a device can vanish at any moment), but written to limiter.log —
        // each kind at most once every 10 s, so a repeating one cannot flood the file.
        readonly Dictionary<string, DateTime> lastReport = new Dictionary<string, DateTime>();
        readonly Dictionary<string, int> reportCount = new Dictionary<string, int>();

        void Report(string where, Exception ex)
        {
            string key = where + ":" + ex.GetType().Name + ":" + System.Runtime.InteropServices.Marshal.GetHRForException(ex).ToString("X8");
            int n;
            reportCount.TryGetValue(key, out n);
            reportCount[key] = ++n;
            DateTime last;
            if (lastReport.TryGetValue(key, out last) && (DateTime.UtcNow - last).TotalSeconds < 10) return;
            lastReport[key] = DateTime.UtcNow;
            Log.Line("error in " + where + " (" + n + " so far): " + ex.GetType().Name + " 0x" + System.Runtime.InteropServices.Marshal.GetHRForException(ex).ToString("X8") + " " + ex.Message);
        }

        void Publish()
        {
            status = inner.Status;
            isOpen = inner.IsOpen;
            deviceName = inner.DeviceName;
            hardwareVolume = inner.HardwareVolume;
        }

        [DllImport("avrt.dll", CharSet = CharSet.Unicode, EntryPoint = "AvSetMmThreadCharacteristicsW")]
        static extern IntPtr AvSetMmThreadCharacteristics(string task, ref int taskIndex);

        [DllImport("avrt.dll")]
        static extern bool AvRevertMmThreadCharacteristics(IntPtr handle);
    }
}
