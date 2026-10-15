// Sound processing (2.0, beta): the game plays into a "source" device (a virtual cable or an unused output),
// vgaudio.dll records it, runs input gain → Game mode → 5 ms lookahead limiter and plays it on the headphones.
// This file: the P/Invoke wrapper, the controller that keeps the engine in line with the settings, and the window.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace VolumeGuard
{
    [StructLayout(LayoutKind.Sequential)]
    struct VgStatus
    {
        public int State, SourceRate, OutputRate, Underruns, Restarts;
        public float InPeakDb, OutPeakDb, ReductionDb, LatencyMs, BufferMs, CorrectionPpm;
        public float SourceMs, OutputMs, LookaheadMs;   // API 2: parts of LatencyMs
    }

    static class VgNative
    {
        const string Dll = "vgaudio.dll";

        public const int StateStopped = 0, StateStarting = 1, StateRunning = 2, StateBuffering = 3, StateReconnecting = 4;
        public const int ErrArgs = -1, ErrSame = -2, ErrSource = -3, ErrOutput = -4, ErrFormat = -5;

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vg_version();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int vg_list_devices(StringBuilder buffer, int capacity);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int vg_start(string sourceId, string outputId);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void vg_stop();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void vg_set_params(int mode, float amountPercent, float ceilingDb, float inputGainDb, float releaseMs, int bypass);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vg_get_status(ref VgStatus status, int size);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        public static extern int vg_last_error(StringBuilder buffer, int capacity);
    }

    class AudioDevice
    {
        public string Id, Name;
        public bool IsDefault;
    }

    /// Keeps vgaudio.dll running (or stopped) according to the settings. Starting can take a moment
    /// (opening two devices), so it happens on a worker thread; stop/start calls are serialized.
    class Processing
    {
        readonly Settings s;
        readonly object sync = new object();
        int generation;
        volatile bool starting;
        volatile int startResult;
        volatile string startError = "";

        public readonly bool Available;
        public readonly string UnavailableReason = "";

        readonly bool demo;
        readonly Random rnd = new Random(3);

        /// demo: for screenshots (--demo), no DLL needed, shows made-up devices and levels
        public Processing(Settings s, bool demo = false)
        {
            this.s = s;
            this.demo = demo;
            if (demo) { Available = true; return; }
            if (!Environment.Is64BitProcess)
            {
                UnavailableReason = "Обработка звука работает только в 64-битной Windows.";
                return;
            }
            string dll = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "vgaudio.dll");
            if (!File.Exists(dll))
            {
                UnavailableReason = "Нет файла vgaudio.dll рядом с VolumeGuard.exe — скачайте полный архив релиза.";
                return;
            }
            try
            {
                if (VgNative.vg_version() != 2)
                {
                    UnavailableReason = "vgaudio.dll другой версии — скачайте полный архив релиза заново.";
                    return;
                }
                Available = true;
            }
            catch (Exception ex)
            {
                UnavailableReason = "Не удалось загрузить vgaudio.dll: " + ex.Message;
            }
        }

        public bool Starting { get { return starting; } }
        public int StartResult { get { return startResult; } }
        public string StartError { get { return startError; } }

        public List<AudioDevice> ListDevices()
        {
            var list = new List<AudioDevice>();
            if (demo)
            {
                list.Add(new AudioDevice { Id = "cable", Name = "CABLE Input (VB-Audio Virtual Cable)" });
                list.Add(new AudioDevice { Id = "phones", Name = "Наушники (HyperX Cloud II)", IsDefault = true });
                list.Add(new AudioDevice { Id = "hdmi", Name = "LG ULTRAGEAR (NVIDIA High Definition Audio)" });
                return list;
            }
            if (!Available) return list;
            try
            {
                int need = VgNative.vg_list_devices(null, 0);
                var sb = new StringBuilder(Math.Max(need, 1) + 256);
                VgNative.vg_list_devices(sb, sb.Capacity);
                foreach (var line in sb.ToString().Split(new[] { '\n' }))
                {
                    var p = line.Split(new[] { '\t' });
                    if (p.Length < 3 || p[0].Length == 0) continue;
                    list.Add(new AudioDevice { Id = p[0], Name = p[1].Length > 0 ? p[1] : p[0], IsDefault = p[2] == "1" });
                }
            }
            catch { }
            return list;
        }

        public void SendParams()
        {
            if (!Available || demo) return;
            try { VgNative.vg_set_params(s.ProcMode, s.ProcAmount, s.ProcCeiling, s.ProcGain, 100f, 0); }
            catch { }
        }

        /// Start, restart or stop the engine to match the settings.
        public void Apply()
        {
            if (!Available || demo) return;
            SendParams();
            int gen = Interlocked.Increment(ref generation);
            bool want = s.ProcOn && s.ProcSource.Length > 0 && s.ProcOutput.Length > 0;
            if (!want)
            {
                starting = false;
                startResult = 0;
                startError = "";
                lock (sync) { try { VgNative.vg_stop(); } catch { } }
                return;
            }
            string src = s.ProcSource, dst = s.ProcOutput;
            starting = true;
            ThreadPool.QueueUserWorkItem(delegate
            {
                lock (sync)
                {
                    if (gen != Thread.VolatileRead(ref generation)) return;   // the settings changed again meanwhile
                    int r;
                    string err = "";
                    try
                    {
                        r = VgNative.vg_start(src, dst);
                        if (r != 0)
                        {
                            var sb = new StringBuilder(512);
                            VgNative.vg_last_error(sb, sb.Capacity);
                            err = sb.ToString();
                        }
                    }
                    catch (Exception ex) { r = -6; err = ex.Message; }
                    if (gen != Thread.VolatileRead(ref generation)) return;
                    startError = err;
                    startResult = r;
                    starting = false;
                }
            });
        }

        /// VB-CABLE (VB-Audio, donationware) ships in the VBCABLE folder next to the exe. Its setup, or null.
        public static string BundledCableSetup()
        {
            try
            {
                string dir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "VBCABLE");
                string arch = (Environment.GetEnvironmentVariable("PROCESSOR_ARCHITEW6432") ??
                               Environment.GetEnvironmentVariable("PROCESSOR_ARCHITECTURE") ?? "").ToUpperInvariant();
                var names = arch == "ARM64"
                    ? new[] { "VBCABLE_Setup_arm64.exe", "VBCABLE_Setup_ARM64.exe", "VBCABLE_Setup_x64.exe" }
                    : Environment.Is64BitOperatingSystem ? new[] { "VBCABLE_Setup_x64.exe" } : new[] { "VBCABLE_Setup.exe" };
                foreach (var n in names)
                {
                    string f = Path.Combine(dir, n);
                    if (File.Exists(f)) return f;
                }
            }
            catch { }
            return null;
        }

        public static bool CableInstalled(List<AudioDevice> devices)
        {
            foreach (var d in devices)
                if (d.Name.IndexOf("VB-Audio Virtual Cable", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    d.Name.IndexOf("CABLE Input", StringComparison.OrdinalIgnoreCase) >= 0)
                    return true;
            return false;
        }

        public VgStatus Status()
        {
            var st = new VgStatus();
            if (demo)
            {
                bool shot = rnd.NextDouble() < 0.15;
                st.State = s.ProcOn ? VgNative.StateRunning : VgNative.StateStopped;
                st.SourceRate = 48000; st.OutputRate = 48000;
                st.LatencyMs = 23.4f; st.SourceMs = 2.7f; st.BufferMs = 10.4f; st.OutputMs = 5.3f; st.LookaheadMs = 5f;
                st.InPeakDb = shot ? -2f - (float)rnd.NextDouble() * 3f : -30f + (float)rnd.NextDouble() * 8f;
                st.OutPeakDb = shot ? s.ProcCeiling - 0.2f : st.InPeakDb + 6f;
                st.ReductionDb = shot ? -9f - (float)rnd.NextDouble() * 4f : -1f;
                return st;
            }
            if (!Available) return st;
            try { VgNative.vg_get_status(ref st, Marshal.SizeOf(typeof(VgStatus))); }
            catch { }
            return st;
        }

        public void Shutdown()
        {
            if (!Available || demo) return;
            Interlocked.Increment(ref generation);
            lock (sync) { try { VgNative.vg_stop(); } catch { } }
        }

        /// One line for the window: what the engine is doing right now.
        public string Describe(VgStatus st, out bool problem)
        {
            problem = false;
            if (!Available) { problem = true; return UnavailableReason; }
            if (!s.ProcOn) return "Выключено. Звук игры идёт напрямую, без обработки.";
            if (s.ProcSource.Length == 0 || s.ProcOutput.Length == 0) { problem = true; return "Выберите оба устройства."; }
            if (s.ProcSource == s.ProcOutput) { problem = true; return "Источник и наушники должны быть разными устройствами."; }
            if (starting) return "Запуск…";
            if (startResult != 0)
            {
                problem = true;
                switch (startResult)
                {
                    case VgNative.ErrSame: return "Источник и наушники должны быть разными устройствами.";
                    case VgNative.ErrSource: return "Не открылось устройство-источник. Оно подключено и включено?";
                    case VgNative.ErrOutput: return "Не открылось устройство для наушников. Оно подключено и включено?";
                    case VgNative.ErrFormat: return "Устройство использует необычный формат звука. Поставьте 16 или 24 бит в его свойствах.";
                    default: return "Не удалось запустить: " + startError;
                }
            }
            switch (st.State)
            {
                case VgNative.StateRunning:
                    // the parts, so it is clear where the delay comes from
                    return "Работает · задержка ≈ " + Math.Round(st.LatencyMs) + " мс · " +
                           Khz(st.SourceRate) + " → " + Khz(st.OutputRate) + " кГц\n" +
                           "источник " + Ms(st.SourceMs) + " + буфер " + Ms(st.BufferMs) +
                           " + вывод " + Ms(st.OutputMs) + " + лимитер " + Ms(st.LookaheadMs) + " мс";
                case VgNative.StateBuffering: return "Ждём звук от источника…";
                case VgNative.StateReconnecting: problem = true; return "Устройство пропало или сменило формат — переподключаемся…";
                case VgNative.StateStarting: return "Запуск…";
                default: return "Остановлено.";
            }
        }

        static string Ms(float ms)
        {
            return Math.Max(0f, ms).ToString("0", System.Globalization.CultureInfo.InvariantCulture);
        }

        static string Khz(int hz)
        {
            return (hz / 1000.0).ToString(hz % 1000 == 0 ? "0" : "0.0", System.Globalization.CultureInfo.InvariantCulture);
        }
    }

    /// Rounded flat button; Selected draws it in the accent colour (used for the mode switch).
    class FlatButton : Control
    {
        bool selected, hover;

        public FlatButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw, true);
            Cursor = Cursors.Hand;
            ForeColor = Theme.Text;
            Height = Theme.D(30);
        }

        public bool Selected { get { return selected; } set { selected = value; Invalidate(); } }

        public void FitWidth()
        {
            Width = TextRenderer.MeasureText(Text ?? "", Font).Width + Theme.D(24);
        }

        protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }
        protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); hover = true; Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); hover = false; Invalidate(); }
        protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(Parent != null ? Parent.BackColor : Theme.Bg);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            Color fill = !Enabled ? Theme.Card : selected ? Theme.Accent : hover ? Theme.Track : Theme.Card;
            Theme.FillRound(g, fill, new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), Theme.D(7));
            TextRenderer.DrawText(g, Text, Font, ClientRectangle, Enabled ? (selected ? Color.White : ForeColor) : Theme.Sub,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
        }
    }

    /// Input / output level and the amount of reduction, with readable ballistics.
    class LevelMeter : Control
    {
        float inDb = -100f, outDb = -100f, grDb = 0f;
        public float CeilingDb = -1f;

        public LevelMeter()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw, true);
            Height = Theme.D(72);
        }

        /// New peaks since the last update; dt in seconds. Bars jump up at once and fall at 20 dB/s.
        public void Push(float inPeakDb, float outPeakDb, float reductionDb, float dt)
        {
            float fall = 20f * dt;
            inDb = Math.Max(inPeakDb, inDb - fall);
            outDb = Math.Max(outPeakDb, outDb - fall);
            grDb = Math.Min(reductionDb, Math.Min(0f, grDb + fall));
            Invalidate();
        }

        public void Clear() { inDb = outDb = -100f; grDb = 0f; Invalidate(); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(BackColor);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            int lw = Theme.D(78), rowH = Height / 3, barH = Theme.D(8);
            float x0 = lw, w = Width - lw - Theme.D(70);
            string[] names = { "Вход", "Выход", "Сжатие" };
            for (int row = 0; row < 3; row++)
            {
                int y = row * rowH;
                TextRenderer.DrawText(g, names[row], Font, new Rectangle(0, y, lw, rowH), Theme.Sub, TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
                var track = new RectangleF(x0, y + (rowH - barH) / 2f, w, barH);
                Theme.FillRound(g, Theme.Track, track, barH / 2f);
                string val;
                if (row < 2)
                {
                    float db = row == 0 ? inDb : outDb;
                    float pos = Db.DbToPos(db);
                    if (pos > 0.005f)
                    {
                        Color c = row == 0 && db > CeilingDb + 0.05f ? Theme.Warn : Theme.Meter;
                        Theme.FillRound(g, c, new RectangleF(x0, track.Y, Math.Max(barH, w * pos), barH), barH / 2f);
                    }
                    val = db <= -59.5f ? "—" : Math.Round(db).ToString("0") + " дБ";
                    if (row == 1)
                    {
                        // ceiling marker
                        float cx = x0 + w * Db.DbToPos(CeilingDb);
                        using (var b = new SolidBrush(Theme.Red))
                            g.FillRectangle(b, cx - Theme.D(1), track.Y - Theme.D(4), Theme.D(2), barH + Theme.D(8));
                    }
                }
                else
                {
                    // reduction grows from the right, 0…-24 dB
                    float frac = Math.Min(1f, -grDb / 24f);
                    if (frac > 0.005f)
                        Theme.FillRound(g, Theme.Accent, new RectangleF(x0 + w * (1f - frac), track.Y, Math.Max(barH, w * frac), barH), barH / 2f);
                    val = grDb > -0.5f ? "0 дБ" : Math.Round(grDb).ToString("0") + " дБ";
                }
                TextRenderer.DrawText(g, val, Font, new Rectangle((int)(x0 + w) + Theme.D(4), y, Theme.D(66), rowH), Theme.Text,
                    TextFormatFlags.VerticalCenter | TextFormatFlags.Right);
            }
        }
    }

    /// The "Sound processing" window. Closing it only hides it; the processing keeps running.
    class ProcessingForm : Form
    {
        readonly Settings s;
        readonly Processing proc;
        readonly ToggleSwitch onSw = new ToggleSwitch();
        readonly ComboBox srcBox = new ComboBox(), outBox = new ComboBox();
        readonly FlatButton[] modeBtns = new FlatButton[3];
        readonly Label modeInfo = new Label();
        readonly FlatSlider amount = new FlatSlider(), ceiling = new FlatSlider(), gain = new FlatSlider();
        readonly Label amountVal = new Label(), ceilingVal = new Label(), gainVal = new Label();
        readonly LevelMeter meter = new LevelMeter();
        readonly Label status = new Label();
        readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
        List<AudioDevice> devices = new List<AudioDevice>();
        bool filling;
        DateTime lastPoll = DateTime.UtcNow;

        static readonly string[] ModeNames = { "Только лимитер", "Comfort", "Tactical" };
        static readonly string[] ModeHelp =
        {
            "Громкие звуки не поднимаются выше потолка, всё остальное — без изменений.",
            "Мягко выравнивает громкость: взрывы и выстрелы тише, тихие звуки чуть громче. Для долгих сессий.",
            "Сильное выравнивание для шутеров: шаги и перезарядка слышны лучше, выстрелы и взрывы не оглушают."
        };

        public ProcessingForm(Settings s, Processing proc, Icon icon)
        {
            this.s = s;
            this.proc = proc;
            Text = "VolumeGuard — обработка звука";
            Icon = icon;
            BackColor = Theme.Bg;
            ForeColor = Theme.Text;
            Font = Theme.Font(9f);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ShowInTaskbar = false;
            ClientSize = new Size(Theme.D(470), Theme.D(640));

            int x = Theme.D(18), w = ClientSize.Width - 2 * x, y = Theme.D(14);

            var title = new Label { Text = "Обработка звука", Font = Theme.Bold(13f), AutoSize = true, Location = new Point(x - Theme.D(2), y) };
            var beta = new Label { Text = "БЕТА", Font = Theme.Bold(7.5f), ForeColor = Theme.Bg, BackColor = Theme.Warn, AutoSize = true, Padding = new Padding(Theme.D(3), Theme.D(1), Theme.D(3), Theme.D(1)) };
            Controls.Add(title);
            beta.Location = new Point(title.Right + Theme.D(6), y + Theme.D(7));
            Controls.Add(beta);
            onSw.Text = "Включить";
            onSw.Font = Theme.Bold(10f);
            onSw.BackColor = Theme.Bg;
            onSw.FitWidth();
            onSw.Height = Theme.D(28);
            onSw.Location = new Point(ClientSize.Width - x - onSw.Width, y + Theme.D(2));
            onSw.Checked = s.ProcOn;
            onSw.CheckedChanged += delegate { s.ProcOn = onSw.Checked; s.Dirty = true; proc.Apply(); };
            Controls.Add(onSw);
            title.SizeChanged += delegate { beta.Left = title.Right + Theme.D(6); };
            y += Theme.D(44);

            var intro = new Label
            {
                Text = "Игра выводит звук на отдельное устройство (виртуальный кабель или неиспользуемый выход). " +
                       "VolumeGuard обрабатывает его — с задержкой 5 мс на «взгляд вперёд», так что даже первый выстрел " +
                       "не бывает громче потолка — и отдаёт в наушники.",
                ForeColor = Theme.Sub, Location = new Point(x, y)
            };
            y = Para(intro, w) + Theme.D(8);

            y = AddDeviceRow("Игра играет в", srcBox, x, y, w);
            y = AddDeviceRow("Слушать в", outBox, x, y, w);
            // no virtual cable yet: offer the one that ships in the archive
            string cableSetup = Processing.BundledCableSetup();
            if (proc.Available && cableSetup != null && !Processing.CableInstalled(proc.ListDevices()))
            {
                var cableBtn = new FlatButton { Text = "Установить VB-CABLE", Font = Theme.Font(9f), Location = new Point(x + Theme.D(92), y) };
                cableBtn.FitWidth();
                cableBtn.Click += delegate { InstallCable(cableSetup); };
                Controls.Add(cableBtn);
                var cableInfo = new Label
                {
                    Text = "Виртуальный кабель для игры, в комплекте. VB-Audio, donationware — vb-cable.com",
                    ForeColor = Theme.Sub, Font = Theme.Font(8f), Location = new Point(cableBtn.Right + Theme.D(10), y)
                };
                int bottom = Para(cableInfo, w - Theme.D(92) - cableBtn.Width - Theme.D(10));
                y = Math.Max(bottom, cableBtn.Bottom) + Theme.D(10);
            }
            srcBox.SelectedIndexChanged += delegate { OnDeviceChosen(srcBox, true); };
            outBox.SelectedIndexChanged += delegate { OnDeviceChosen(outBox, false); };

            var how = new LinkLabel
            {
                Text = "Устройство для игры выбирается в Windows: Громкость приложений →",
                AutoSize = false, Location = new Point(x + Theme.D(92), y),
                LinkColor = Theme.Accent, ActiveLinkColor = Theme.Text, VisitedLinkColor = Theme.Accent
            };
            int linkFrom = how.Text.IndexOf("Громкость", StringComparison.Ordinal);
            how.LinkArea = new LinkArea(linkFrom, how.Text.Length - linkFrom);
            how.LinkClicked += delegate { OpenAppVolumeSettings(); };
            y = Para(how, w - Theme.D(92)) + Theme.D(14);

            Controls.Add(new Label { Text = "Режим", ForeColor = Theme.Sub, Location = new Point(x, y + Theme.D(6)), AutoSize = true });
            int bx = x + Theme.D(92), bw = (w - Theme.D(92) - Theme.D(12)) / 3;
            for (int i = 0; i < 3; i++)
            {
                int mode = i;
                var b = new FlatButton { Text = ModeNames[i], Font = Theme.Font(9f), Location = new Point(bx + i * (bw + Theme.D(6)), y), Width = bw };
                b.Click += delegate { s.ProcMode = mode; s.Dirty = true; SyncMode(); proc.SendParams(); };
                modeBtns[i] = b;
                Controls.Add(b);
            }
            y += Theme.D(36);
            modeInfo.ForeColor = Theme.Sub;
            modeInfo.Location = new Point(bx, y);
            int helpH = 0;
            foreach (var h in ModeHelp)
            {
                modeInfo.Text = h;
                helpH = Math.Max(helpH, Para(modeInfo, w - Theme.D(92)) - y);   // room for the longest one
            }
            modeInfo.Height = helpH;
            y += helpH + Theme.D(10);

            y = AddSlider("Сила", amount, amountVal, x, y, w);
            y = AddSlider("Потолок", ceiling, ceilingVal, x, y, w);
            y = AddSlider("Усиление", gain, gainVal, x, y, w);
            amount.Value = s.ProcAmount / 100f;
            ceiling.Value = 1f + s.ProcCeiling / 12f;
            gain.Value = (s.ProcGain + 12f) / 24f;
            amount.ValueChanged += delegate { s.ProcAmount = (float)Math.Round(amount.Value * 100f); OnParamsChanged(); };
            ceiling.ValueChanged += delegate { s.ProcCeiling = (float)Math.Round(-12f * (1f - ceiling.Value) * 2f) / 2f; OnParamsChanged(); };
            gain.ValueChanged += delegate { s.ProcGain = (float)Math.Round((gain.Value * 24f - 12f) * 2f) / 2f; OnParamsChanged(); };
            y += Theme.D(8);

            meter.BackColor = Theme.Bg;
            meter.Font = Theme.Font(9f);
            meter.Location = new Point(x, y);
            meter.Width = w;
            Controls.Add(meter);
            y += meter.Height + Theme.D(10);

            status.AutoSize = false;
            status.Location = new Point(x, y);
            status.Text = "Работает · задержка ≈ 20 мс · 48 → 48 кГц\nисточник 3 + буфер 0 + вывод 12 + лимитер 5 мс\n· пропусков: 0";   // room for three lines
            y = Para(status, w) + Theme.D(8);   // two lines of room
            status.Text = "";

            var note = new Label
            {
                Text = "Громкость самого устройства-источника держите на 100 %: Windows отдаёт его звук уже после этой громкости. " +
                       "Громкость игры регулируйте в VolumeGuard или в игре.",
                ForeColor = Theme.Sub, Font = Theme.Font(8f), Location = new Point(x, y)
            };
            y = Para(note, w) + Theme.D(4);
            ClientSize = new Size(ClientSize.Width, y + Theme.D(6));

            SyncMode();
            UpdateValueLabels();
            FillDevices();
            bool on = proc.Available;
            foreach (Control c in Controls) if (c != status && c != intro && c != title && c != beta) c.Enabled = on;

            timer.Interval = 50;
            timer.Tick += delegate { OnTick(); };
        }

        /// Adds a wrapped label of width w, as tall as its text needs; returns the y below it.
        int Para(Label l, int w)
        {
            l.AutoSize = false;
            if (!Controls.Contains(l)) Controls.Add(l);   // first, so it has the form's font
            var size = TextRenderer.MeasureText(l.Text, l.Font, new Size(w, 2000), TextFormatFlags.WordBreak);
            l.Size = new Size(w, Math.Max(size.Height, l.Font.Height) + Theme.D(4));
            return l.Bottom;
        }

        int AddDeviceRow(string label, ComboBox box, int x, int y, int w)
        {
            Controls.Add(new Label { Text = label, ForeColor = Theme.Sub, Location = new Point(x, y + Theme.D(4)), Size = new Size(Theme.D(92), Theme.D(22)) });
            box.DropDownStyle = ComboBoxStyle.DropDownList;
            box.FlatStyle = FlatStyle.Flat;
            box.BackColor = Theme.Card;
            box.ForeColor = Theme.Text;
            box.Location = new Point(x + Theme.D(92), y);
            box.Width = w - Theme.D(92);
            Controls.Add(box);
            return y + Theme.D(34);
        }

        int AddSlider(string label, FlatSlider sl, Label val, int x, int y, int w)
        {
            Controls.Add(new Label { Text = label, ForeColor = Theme.Sub, Location = new Point(x, y + Theme.D(3)), Size = new Size(Theme.D(92), Theme.D(22)) });
            sl.BackColor = Theme.Bg;
            sl.Location = new Point(x + Theme.D(86), y);
            sl.Width = w - Theme.D(86) - Theme.D(64);
            Controls.Add(sl);
            val.AutoSize = false;
            val.TextAlign = ContentAlignment.MiddleRight;
            val.Location = new Point(x + w - Theme.D(64), y);
            val.Size = new Size(Theme.D(64), Theme.D(24));
            Controls.Add(val);
            return y + Theme.D(34);
        }

        void SyncMode()
        {
            for (int i = 0; i < 3; i++) modeBtns[i].Selected = s.ProcMode == i;
            modeInfo.Text = ModeHelp[Math.Max(0, Math.Min(2, s.ProcMode))];
            amount.Enabled = proc.Available && s.ProcMode != 0;   // the strength is a Game mode setting
        }

        void OnParamsChanged()
        {
            s.Dirty = true;
            UpdateValueLabels();
            proc.SendParams();
        }

        void UpdateValueLabels()
        {
            var ic = System.Globalization.CultureInfo.InvariantCulture;
            amountVal.Text = s.ProcAmount.ToString("0", ic) + " %";
            ceilingVal.Text = s.ProcCeiling.ToString("0.#", ic) + " дБ";
            gainVal.Text = (s.ProcGain > 0 ? "+" : "") + s.ProcGain.ToString("0.#", ic) + " дБ";
            meter.CeilingDb = s.ProcCeiling;
        }

        public void FillDevices()
        {
            filling = true;
            try
            {
                devices = proc.ListDevices();
                // first time: guess a virtual cable as the source and the Windows default as the output
                if (s.ProcSource.Length == 0)
                    foreach (var d in devices)
                        if (d.Name.IndexOf("CABLE", StringComparison.OrdinalIgnoreCase) >= 0 || d.Name.IndexOf("Virtual", StringComparison.OrdinalIgnoreCase) >= 0)
                        { s.ProcSource = d.Id; s.Dirty = true; break; }
                if (s.ProcOutput.Length == 0)
                    foreach (var d in devices)
                        if (d.IsDefault && d.Id != s.ProcSource) { s.ProcOutput = d.Id; s.Dirty = true; break; }
                Fill(srcBox, s.ProcSource);
                Fill(outBox, s.ProcOutput);
            }
            finally { filling = false; }
        }

        void Fill(ComboBox box, string selectedId)
        {
            box.Items.Clear();
            box.Items.Add("— не выбрано —");
            int sel = 0;
            for (int i = 0; i < devices.Count; i++)
            {
                box.Items.Add(devices[i].Name + (devices[i].IsDefault ? "  (по умолчанию)" : ""));
                if (devices[i].Id == selectedId) sel = i + 1;
            }
            if (sel == 0 && selectedId.Length > 0) { box.Items.Add("(отключено) " + selectedId); sel = box.Items.Count - 1; }
            box.SelectedIndex = sel;
        }

        void OnDeviceChosen(ComboBox box, bool source)
        {
            if (filling) return;
            int i = box.SelectedIndex;
            string id;
            if (i <= 0) id = "";
            else if (i - 1 < devices.Count) id = devices[i - 1].Id;
            else return;   // the "(disconnected)" entry: keep the saved id
            if (source) { if (id == s.ProcSource) return; s.ProcSource = id; }
            else { if (id == s.ProcOutput) return; s.ProcOutput = id; }
            s.Dirty = true;
            proc.Apply();
        }

        void InstallCable(string setup)
        {
            var answer = MessageBox.Show(this,
                "VB-CABLE — бесплатный виртуальный аудиокабель от VB-Audio Software (donationware, www.vb-cable.com). " +
                "Он лежит в архиве VolumeGuard без изменений.\r\n\r\n" +
                "Сейчас откроется его установщик (нужны права администратора): нажмите «Install Driver». " +
                "После установки перезагрузите компьютер — появится устройство «CABLE Input».\r\n\r\nПродолжить?",
                "Установка VB-CABLE", MessageBoxButtons.OKCancel, MessageBoxIcon.Information);
            if (answer != DialogResult.OK) return;
            try
            {
                Process.Start(new ProcessStartInfo(setup)
                {
                    UseShellExecute = true,
                    Verb = "runas",
                    WorkingDirectory = Path.GetDirectoryName(setup)
                });
            }
            catch (System.ComponentModel.Win32Exception ex)
            {
                if (ex.NativeErrorCode != 1223)   // 1223: the user said "No" in the administrator prompt
                    MessageBox.Show(this, "Не удалось запустить установщик: " + ex.Message, "VolumeGuard", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        static void OpenAppVolumeSettings()
        {
            try { Process.Start("ms-settings:apps-volume"); }
            catch
            {
                try { Process.Start("sndvol.exe"); } catch { }
            }
        }

        void OnTick()
        {
            var now = DateTime.UtcNow;
            float dt = (float)Math.Min(0.25, Math.Max(0.01, (now - lastPoll).TotalSeconds));
            lastPoll = now;
            var st = proc.Status();
            bool problem;
            string text = proc.Describe(st, out problem);
            if (st.State == VgNative.StateRunning && st.Underruns > 0) text += " · пропусков: " + st.Underruns;
            if (status.Text != text) status.Text = text;
            status.ForeColor = problem ? Theme.Warn : (st.State == VgNative.StateRunning ? Theme.Meter : Theme.Sub);
            if (st.State == VgNative.StateRunning || st.State == VgNative.StateBuffering)
                meter.Push(st.InPeakDb, st.OutPeakDb, st.ReductionDb, dt);
            else meter.Clear();
        }

        protected override void OnVisibleChanged(EventArgs e)
        {
            base.OnVisibleChanged(e);
            if (Visible)
            {
                onSw.Checked = s.ProcOn;
                FillDevices();
                lastPoll = DateTime.UtcNow;
                OnTick();
                timer.Start();
            }
            else timer.Stop();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                Hide();
                return;
            }
            timer.Stop();
            base.OnFormClosing(e);
        }
    }
}
