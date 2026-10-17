// Main window, tray icon, autostart, program entry.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace VolumeGuard
{
    class MainForm : Form
    {
        readonly Settings s;
        readonly IEngine eng;
        readonly bool startHidden;
        readonly bool demo;

        readonly ToggleSwitch enabledSw = new ToggleSwitch();
        readonly ComboBox deviceBox = new ComboBox();
        readonly ChannelCard masterCard;
        readonly FlowLayoutPanel appsPanel = new FlowLayoutPanel();
        readonly Label emptyLabel = new Label();
        readonly Label statusLabel = new Label();
        readonly ToggleSwitch autostartSw = new ToggleSwitch();
        readonly ToggleSwitch traySw = new ToggleSwitch();
        readonly ToggleSwitch antiClickSw = new ToggleSwitch();
        readonly Dictionary<string, ChannelCard> cards = new Dictionary<string, ChannelCard>();
        readonly NotifyIcon tray = new NotifyIcon();
        readonly ToolStripMenuItem trayEnabled = new ToolStripMenuItem("Лимитер включён");
        readonly Processing processing;
        readonly FlatButton procBtn = new FlatButton();
        ProcessingForm procForm;
        readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
        readonly Stopwatch clock = Stopwatch.StartNew();
        List<KeyValuePair<string, string>> devices = new List<KeyValuePair<string, string>>();
        Icon iconOn, iconOff;
        double lastT, lastRefresh, lastReopen, lastUi, lastAutoCheck;
        // auto processing: the game was running at the last check; the processing was switched on by us
        bool gameWasRunning, autoEnabled;
        // "processing on" is told only once the engine has really started (2.8.4): the text, and since when
        string startNotice;
        double startNoticeSince;
        bool hotkeysRegistered;
        double nextUpdateCheck = 15.0;   // seconds after start, then daily
        Version newerVersion;
        bool balloonIsUpdate;
        readonly ToolStripMenuItem trayUpdates = new ToolStripMenuItem("Проверять обновления");
        readonly ToolStripMenuItem trayDownload = new ToolStripMenuItem();
        readonly ToggleSwitch hotkeysSw = new ToggleSwitch();
        const int HkProcessing = 1, HkMode = 2, HkLimiter = 3;
        bool exiting, fillingDevices;

        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string RunName = "VolumeGuard";

        public MainForm(Settings s, IEngine eng, bool startHidden, bool demo)
        {
            this.s = s;
            this.eng = eng;
            this.startHidden = startHidden;
            this.demo = demo;

            Text = "VolumeGuard";
            BackColor = Theme.Bg;
            ForeColor = Theme.Text;
            Font = Theme.Font(9f);
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(Theme.D(500), Theme.D(832));
            MinimumSize = new Size(Theme.D(420), Theme.D(460));
            iconOn = MakeIcon(true);
            iconOff = MakeIcon(false);
            Icon = iconOn;

            // header
            var header = new Panel { Dock = DockStyle.Top, Height = Theme.D(66), BackColor = Theme.Bg };
            var title = new Label { Text = "VolumeGuard", Font = Theme.Bold(15f), ForeColor = Theme.Text, AutoSize = true, Location = new Point(Theme.D(16), Theme.D(10)) };
            var sub = new Label { Text = "умная защита громкости для Windows", Font = Theme.Font(9f), ForeColor = Theme.Sub, AutoSize = true, Location = new Point(Theme.D(18), Theme.D(40)) };
            enabledSw.Text = "Лимитер";
            enabledSw.Font = Theme.Bold(10f);
            enabledSw.BackColor = Theme.Bg;
            enabledSw.FitWidth();
            enabledSw.Height = Theme.D(28);
            enabledSw.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            enabledSw.Location = new Point(header.Width - enabledSw.Width - Theme.D(16), Theme.D(18));
            enabledSw.CheckedChanged += delegate { SetEnabled(enabledSw.Checked); };
            // sound processing (2.0): opens its own window
            processing = new Processing(s, demo);
            procBtn.Text = "Обработка";   // short: must not cover the subtitle
            procBtn.Font = Theme.Font(9f);
            procBtn.FitWidth();
            procBtn.Height = Theme.D(26);   // stays above the subtitle line
            procBtn.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            procBtn.Location = new Point(enabledSw.Left - procBtn.Width - Theme.D(14), Theme.D(19));
            procBtn.Click += delegate { ShowProcessing(); };
            header.Controls.AddRange(new Control[] { title, sub, procBtn, enabledSw });

            // device row
            var devRow = new Panel { Dock = DockStyle.Top, Height = Theme.D(44), BackColor = Theme.Bg, Padding = new Padding(Theme.D(16), Theme.D(6), Theme.D(16), Theme.D(8)) };
            var devLbl = new Label { Text = "Устройство", ForeColor = Theme.Sub, Dock = DockStyle.Left, Width = Theme.D(92), TextAlign = ContentAlignment.MiddleLeft };
            deviceBox.DropDownStyle = ComboBoxStyle.DropDownList;
            deviceBox.FlatStyle = FlatStyle.Flat;
            deviceBox.BackColor = Theme.Card;
            deviceBox.ForeColor = Theme.Text;
            deviceBox.Dock = DockStyle.Fill;
            deviceBox.SelectedIndexChanged += delegate { OnDeviceChosen(); };
            devRow.Controls.Add(deviceBox);
            devRow.Controls.Add(devLbl);

            // how the limiter behaves: return speed and peak / loudness mode
            var optRow = new Panel { Dock = DockStyle.Top, Height = Theme.D(112), BackColor = Theme.Bg };
            var tips = new ToolTip { AutoPopDelay = 15000, InitialDelay = 300 };
            // the two kinds of protection, said plainly where you choose between them
            tips.SetToolTip(procBtn,
                "Лимиты в этом окне убавляют громкость приложений через Windows: это быстро и без задержки, " +
                "но начало внезапного громкого звука (~10 мс) может проскочить.\r\n" +
                "«Обработка звука» пропускает звук игры через VolumeGuard и видит его на 5 мс вперёд: " +
                "ни один пик не громче потолка, но звук идёт с задержкой ~20–50 мс.");
            AddSegments(optRow, tips, "Возврат громкости", 2,
                new[] { "Быстро", "Средне", "Медленно" },
                new[] { "Громкость возвращается примерно за 0,6 с после громкого звука",
                        "Примерно за 1 с",
                        "Примерно за 2 с — громкость меньше «качает», удобно для фильмов" },
                () => s.ReturnSpeed, v => { s.ReturnSpeed = v; s.Dirty = true; });
            AddSegments(optRow, tips, "Ограничивать", Theme.D(38) + 2,
                new[] { "По пикам", "По громкости", "Плавно" },
                new[] { "Ни один пик не громче лимита, громкость убавляется мгновенно. Самая сильная защита, но начало внезапного громкого звука может щёлкнуть",
                        "Средняя громкость (за 0,4 с) не выше лимита, короткие пики — до 6 дБ громче; громкость убавляется за ~20 мс. Звук не проседает после одиночных выстрелов",
                        "Следит за средней громкостью за ~3 с и убавляет плавно (~150 мс). Щелчков нет, но короткий громкий звук проходит как есть" },
                () => s.LimitMode, v => { s.LimitMode = v; s.Dirty = true; });
            AddSegments(optRow, tips, "Память громких", Theme.D(74) + 2,
                new[] { "Выкл", "10 с", "30 с" },
                new[] { "Громкость возвращается полностью — следующий внезапный громкий звук снова проскочит громче лимита",
                        "Программа помнит громкие звуки ~10 с и не возвращает громкость настолько, чтобы такой же звук снова оказался громче лимита больше чем на 3 дБ. Меньше щелчков в перестрелке; тихие звуки в это время чуть тише",
                        "То же, но помнит ~30 с" },
                () => s.Memory, v => { s.Memory = v; s.Dirty = true; });

            // master card
            var masterWrap = new Panel { Dock = DockStyle.Top, Height = Theme.D(112), BackColor = Theme.Bg, Padding = new Padding(Theme.D(12), 0, Theme.D(12), 0) };
            masterCard = new ChannelCard("Общий звук устройства", iconOn, false) { Dock = DockStyle.Fill };
            masterCard.Vol.ValueChanged += delegate { eng.SetMasterVolume(masterCard.Vol.Value); };
            masterCard.Lim.ValueChanged += delegate { s.Master.LimitDb = Db.PosToDb(masterCard.Lim.Value); s.Dirty = true; };
            masterCard.LimOn.CheckedChanged += delegate { s.Master.LimitOn = masterCard.LimOn.Checked; s.Dirty = true; };
            masterWrap.Controls.Add(masterCard);

            var section = new Label
            {
                Text = "ПРИЛОЖЕНИЯ", Dock = DockStyle.Top, Height = Theme.D(30), ForeColor = Theme.Sub, Font = Theme.Bold(8.5f),
                Padding = new Padding(Theme.D(16), Theme.D(8), 0, 0), BackColor = Theme.Bg
            };

            // footer
            var footer = new Panel { Dock = DockStyle.Bottom, Height = Theme.D(130), BackColor = Theme.Card };
            autostartSw.Text = "Запускать вместе с Windows";
            traySw.Text = "Сворачивать в трей при закрытии";
            antiClickSw.Text = "Без щелчка в начале громкого звука";
            int fy = Theme.D(10);
            hotkeysSw.Text = "Горячие клавиши: Ctrl+Alt+P обработка, M режим, L лимитер";
            foreach (var sw in new[] { antiClickSw, autostartSw, traySw, hotkeysSw })
            {
                sw.BackColor = Theme.Card;
                sw.Font = Theme.Font(9f);
                sw.FitWidth();
                sw.Location = new Point(Theme.D(16), fy);
                fy += Theme.D(26);
                footer.Controls.Add(sw);
            }
            autostartSw.Checked = AutostartOn();
            autostartSw.CheckedChanged += delegate { SetAutostart(autostartSw.Checked); };
            traySw.Checked = s.Tray;
            antiClickSw.Checked = s.AntiClick;
            antiClickSw.CheckedChanged += delegate { s.AntiClick = antiClickSw.Checked; s.Dirty = true; };
            traySw.CheckedChanged += delegate { s.Tray = traySw.Checked; s.Dirty = true; };
            hotkeysSw.Checked = s.Hotkeys;
            hotkeysSw.CheckedChanged += delegate { s.Hotkeys = hotkeysSw.Checked; s.Dirty = true; UpdateHotkeys(); };
            statusLabel.ForeColor = Theme.Warn;
            statusLabel.BackColor = Theme.Card;
            statusLabel.AutoSize = false;
            statusLabel.TextAlign = ContentAlignment.MiddleRight;
            statusLabel.Anchor = AnchorStyles.Right | AnchorStyles.Bottom;
            statusLabel.SetBounds(footer.Width - Theme.D(220), Theme.D(98), Theme.D(206), Theme.D(24));
            footer.Controls.Add(statusLabel);

            // app list
            appsPanel.Dock = DockStyle.Fill;
            appsPanel.FlowDirection = FlowDirection.TopDown;
            appsPanel.WrapContents = false;
            appsPanel.AutoScroll = true;
            appsPanel.BackColor = Theme.Bg;
            appsPanel.Padding = new Padding(Theme.D(12), 0, Theme.D(12), Theme.D(8));
            appsPanel.Resize += delegate { FitCards(); };
            emptyLabel.Text = "Сейчас ни одно приложение не выводит звук.\nЗапустите видео или музыку — оно появится здесь.";
            emptyLabel.ForeColor = Theme.Sub;
            emptyLabel.AutoSize = false;
            emptyLabel.TextAlign = ContentAlignment.MiddleCenter;
            emptyLabel.Height = Theme.D(80);

            // docking: Fill first, then edges in reverse visual order
            Controls.Add(appsPanel);
            Controls.Add(footer);
            Controls.Add(section);
            Controls.Add(masterWrap);
            Controls.Add(optRow);
            Controls.Add(devRow);
            Controls.Add(header);

            // tray
            var menu = new ContextMenuStrip();
            menu.Items.Add("Открыть", null, delegate { ShowFromTray(); });
            trayEnabled.Click += delegate { SetEnabled(!s.Enabled); };
            menu.Items.Add(trayEnabled);
            menu.Items.Add("Обработка звука…", null, delegate { ShowFromTray(); ShowProcessing(); });
            trayUpdates.Checked = s.CheckUpdates;
            trayUpdates.Click += delegate { s.CheckUpdates = !s.CheckUpdates; trayUpdates.Checked = s.CheckUpdates; s.Dirty = true; };
            menu.Items.Add(trayUpdates);
            trayDownload.Visible = false;
            trayDownload.Click += delegate { OpenReleases(); };
            menu.Items.Add(trayDownload);
            tray.BalloonTipClicked += delegate { if (balloonIsUpdate) OpenReleases(); };
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Выход", null, delegate { ExitApp(); });
            tray.ContextMenuStrip = menu;
            tray.Text = "VolumeGuard";
            tray.MouseClick += (o, e) => { if (e.Button == MouseButtons.Left) ToggleWindow(); };
            tray.Visible = !demo;

            enabledSw.Checked = s.Enabled;
            UpdateEnabledUi();

            if (!demo && s.ProcOn) processing.Apply();
            procBtn.Selected = s.ProcOn && processing.Available;

            eng.Open();
            FillDevices();
            SyncMaster(true);
            RebuildCards();

            // The limiter runs on its own thread (AudioThread.cs); this timer only drives the window.
            if (!demo) { try { Native.timeBeginPeriod(1); } catch { } }
            timer.Interval = demo ? 10 : 20;
            timer.Tick += delegate { OnTick(); };
            timer.Start();
        }

        protected override void SetVisibleCore(bool value)
        {
            if (startHidden && !IsHandleCreated)
            {
                CreateHandle();
                value = false;
            }
            base.SetVisibleCore(value);
        }

        // ---------- periodic work ----------

        void OnTick()
        {
            double now = clock.Elapsed.TotalSeconds;
            float dt = (float)Math.Min(0.25, Math.Max(0.002, now - lastT));
            lastT = now;

            if (!eng.IsOpen)
            {
                if (now - lastReopen > 2.0)
                {
                    lastReopen = now;
                    if (eng.Open()) { FillDevices(); SyncMaster(true); RebuildCards(); }
                }
            }
            else
            {
                try { eng.Tick(dt); } catch { }
                if (now - lastRefresh > 1.0)
                {
                    lastRefresh = now;
                    bool changed = false;
                    try { changed = eng.RefreshSessions(); } catch { }
                    if (changed) { FillDevices(); SyncMaster(true); RebuildCards(); }
                }
            }

            if (now >= nextUpdateCheck && !demo)
            {
                nextUpdateCheck = now + 24 * 3600;
                if (s.CheckUpdates)
                    Updates.CheckAsync(v => { try { BeginInvoke(new Action(() => OnNewerVersion(v))); } catch { } });
            }

            if (now - lastAutoCheck > 1.0)
            {
                lastAutoCheck = now;
                CheckAutoProcessing();
            }
            if (startNotice != null) CheckStartNotice(now);

            bool procOn = s.ProcOn && processing.Available;
            if (procBtn.Selected != procOn) procBtn.Selected = procOn;

            // anti-click needs a device that changes its volume in hardware
            bool hw = eng.IsOpen && eng.HardwareVolume;
            string acText = hw || !eng.IsOpen ? "Без щелчка в начале громкого звука" : "Без щелчка — устройство не поддерживает";
            if (antiClickSw.Text != acText) { antiClickSw.Text = acText; antiClickSw.FitWidth(); }
            if (antiClickSw.Enabled != (hw || !eng.IsOpen)) antiClickSw.Enabled = hw || !eng.IsOpen;

            string st = eng.Status ?? "";
            if (statusLabel.Text != st) statusLabel.Text = st;

            // the window itself only needs ~20 updates per second
            if (Visible && WindowState != FormWindowState.Minimized && now - lastUi >= 0.045)
            lock (s.Sync)   // the limiter thread changes the same models
            {
                lastUi = now;
                SyncMaster(false);
                foreach (var a in s.Apps.Values)
                {
                    ChannelCard c;
                    if (!cards.TryGetValue(a.Key, out c)) continue;
                    if (c.Version != a.UiVersion) { c.Version = a.UiVersion; c.SetValues(a.UserVol, a.LimitOn, a.LimitDb, a.Muted); }
                    // show the volume the limiter actually set, in the same % as the slider and the Windows mixer
                    string badge = s.Enabled && a.LimitOn && a.Limiting
                        ? "громкость сейчас " + Math.Round(a.Gain * 100) + "%" : "";
                    c.SetLive(a.Muted ? 0f : a.DisplayPeak, badge, a.Active);
                    a.DisplayPeak = 0f;
                }
            }
        }

        void SyncMaster(bool force)
        {
            var m = s.Master;
            if (force || masterCard.Version != m.UiVersion)
            {
                masterCard.Version = m.UiVersion;
                masterCard.SetValues(m.UserVol, m.LimitOn, m.LimitDb, false);
            }
            float red = Db.FromLin(m.R);
            string badge = s.Enabled && m.LimitOn && m.Limiting && red < -0.5f
                ? "звук тише на " + Math.Round(-red) + " дБ" : "";
            masterCard.SetLive(m.DisplayPeak, badge, true);
            m.DisplayPeak = 0f;
        }

        // ---------- app cards ----------

        void RebuildCards()
        {
            lock (s.Sync) RebuildCardsLocked();
        }

        void RebuildCardsLocked()
        {
            var present = s.Apps.Values.Where(a => a.Present)
                .OrderBy(a => a.Key == "#system" ? 1 : 0)
                .ThenBy(a => a.Title, StringComparer.CurrentCultureIgnoreCase)
                .ToList();

            appsPanel.SuspendLayout();
            foreach (var key in cards.Keys.ToList())
            {
                if (present.Any(a => a.Key == key)) continue;
                var c = cards[key];
                appsPanel.Controls.Remove(c);
                c.Dispose();
                cards.Remove(key);
            }
            int idx = 0;
            foreach (var a in present)
            {
                ChannelCard c;
                if (!cards.TryGetValue(a.Key, out c))
                {
                    c = MakeCard(a);
                    cards[a.Key] = c;
                    appsPanel.Controls.Add(c);
                }
                else c.Title = CardTitle(a);
                appsPanel.Controls.SetChildIndex(c, idx++);
            }
            if (present.Count == 0)
            {
                if (!appsPanel.Controls.Contains(emptyLabel)) appsPanel.Controls.Add(emptyLabel);
            }
            else appsPanel.Controls.Remove(emptyLabel);
            FitCards();
            appsPanel.ResumeLayout();
        }

        ChannelCard MakeCard(AppModel a)
        {
            var c = new ChannelCard(CardTitle(a), a.Icon, true);
            c.Margin = new Padding(0);
            c.Version = a.UiVersion;
            c.SetValues(a.UserVol, a.LimitOn, a.LimitDb, a.Muted);
            c.Vol.ValueChanged += delegate { eng.SetAppVolume(a, c.Vol.Value); };
            c.Lim.ValueChanged += delegate { a.LimitDb = Db.PosToDb(c.Lim.Value); s.Dirty = true; };
            c.LimOn.CheckedChanged += delegate { a.LimitOn = c.LimOn.Checked; s.Dirty = true; };
            c.Mute.Toggled += delegate { eng.SetAppMute(a, c.Mute.Muted); };
            return c;
        }

        void FitCards()
        {
            int w = appsPanel.ClientSize.Width - appsPanel.Padding.Horizontal;
            int total = appsPanel.Padding.Vertical;
            foreach (Control c in appsPanel.Controls) total += c.Height + c.Margin.Vertical;
            if (!appsPanel.VerticalScroll.Visible && total > appsPanel.ClientSize.Height)
                w -= SystemInformation.VerticalScrollBarWidth;
            if (w < 50) return;
            foreach (Control c in appsPanel.Controls) c.Width = w;
        }

        // ---------- devices ----------

        void FillDevices()
        {
            if (demo && deviceBox.Items.Count > 0) return;
            fillingDevices = true;
            try
            {
                devices = eng.ListDevices();
                deviceBox.Items.Clear();
                string def = eng.DeviceName;
                deviceBox.Items.Add(string.IsNullOrEmpty(s.DeviceId) && def != null ? "По умолчанию — " + def : "По умолчанию Windows");
                foreach (var d in devices) deviceBox.Items.Add(d.Value);
                int sel = 0;
                for (int i = 0; i < devices.Count; i++)
                    if (devices[i].Key == s.DeviceId) sel = i + 1;
                deviceBox.SelectedIndex = sel;
            }
            finally { fillingDevices = false; }
        }

        void OnDeviceChosen()
        {
            if (fillingDevices) return;
            int i = deviceBox.SelectedIndex;
            string id = i <= 0 || i - 1 >= devices.Count ? "" : devices[i - 1].Key;
            if (id == s.DeviceId) return;
            s.DeviceId = id;
            s.Dirty = true;
            eng.Close();
            eng.Open();
            BeginInvoke(new Action(() => { FillDevices(); SyncMaster(true); RebuildCards(); }));
        }

        // ---------- enable / tray / autostart ----------

        void SetEnabled(bool on)
        {
            s.Enabled = on;
            eng.OnEnabledChanged();
            UpdateEnabledUi();
        }

        void UpdateEnabledUi()
        {
            enabledSw.Checked = s.Enabled;
            trayEnabled.Checked = s.Enabled;
            tray.Icon = s.Enabled ? iconOn : iconOff;
            tray.Text = s.Enabled ? "VolumeGuard — лимитер включён" : "VolumeGuard — лимитер выключен";
        }

        public void OpenProcessing() { ShowProcessing(); }

        /// "PUBG → CABLE Input" for an app that plays to another device than the one chosen in the window.
        static string CardTitle(AppModel a)
        {
            if (a.Where.Length == 0) return a.Title;
            string d = a.Where;
            int paren = d.IndexOf(" (", StringComparison.Ordinal);
            if (paren > 0) d = d.Substring(0, paren);   // "CABLE Input (VB-Audio Virtual Cable)" → "CABLE Input"
            return a.Title + "  → " + d;
        }

        // ---------- processing that follows the game, hotkeys ----------

        /// Every 2 s: switch the processing on when the chosen game starts, and off when it exits (only if it
        /// was switched on here; a manual choice is left alone).
        void CheckAutoProcessing()
        {
            if (demo || !processing.Available || !s.ProcAuto || s.ProcAutoApp.Length == 0)
            {
                gameWasRunning = false;
                autoEnabled = false;
                return;
            }
            if (!s.ProcOn) autoEnabled = false;
            bool running = false;
            try
            {
                foreach (var p in Process.GetProcessesByName(s.ProcAutoApp)) { running = true; p.Dispose(); }
            }
            catch { }
            string game = s.ProcAutoTitle.Length > 0 ? s.ProcAutoTitle : s.ProcAutoApp;
            if (running && !gameWasRunning && !s.ProcOn && s.ProcSource.Length > 0 && s.ProcOutput.Length > 0)
            {
                SetProcessing(true);
                autoEnabled = true;
                NotifyWhenStarted(game + " запущена");
            }
            else if (!running && gameWasRunning && autoEnabled)
            {
                if (s.ProcOn) { SetProcessing(false); Notify("Обработка звука выключена", game + " закрыта"); }
                autoEnabled = false;
            }
            gameWasRunning = running;
        }

        void SetProcessing(bool on)
        {
            if (s.ProcOn == on) return;
            s.ProcOn = on;
            s.Dirty = true;
            processing.Apply();
            if (procForm != null && !procForm.IsDisposed) procForm.SyncFromSettings();
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            UpdateHotkeys();
        }

        void UpdateHotkeys()
        {
            if (demo || !IsHandleCreated) return;
            if (hotkeysRegistered)
            {
                foreach (int id in new[] { HkProcessing, HkMode, HkLimiter }) Native.UnregisterHotKey(Handle, id);
                hotkeysRegistered = false;
            }
            if (!s.Hotkeys) return;
            const uint mods = Native.MOD_CONTROL | Native.MOD_ALT | Native.MOD_NOREPEAT;
            var failed = new List<string>();
            if (!Native.RegisterHotKey(Handle, HkProcessing, mods, (uint)Keys.P)) failed.Add("Ctrl+Alt+P");
            if (!Native.RegisterHotKey(Handle, HkMode, mods, (uint)Keys.M)) failed.Add("Ctrl+Alt+M");
            if (!Native.RegisterHotKey(Handle, HkLimiter, mods, (uint)Keys.L)) failed.Add("Ctrl+Alt+L");
            hotkeysRegistered = true;
            if (failed.Count > 0) Log.Line("hotkeys already taken by another program: " + string.Join(", ", failed.ToArray()));
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == Native.WM_HOTKEY) { OnHotkey(m.WParam.ToInt32()); return; }
            base.WndProc(ref m);
        }

        static readonly string[] ProcModeNames = { "только лимитер", "Comfort", "Tactical" };

        void OnHotkey(int id)
        {
            switch (id)
            {
                case HkProcessing:
                    if (!processing.Available) { Notify("Обработка звука недоступна", processing.UnavailableReason); return; }
                    if (!s.ProcOn && (s.ProcSource.Length == 0 || s.ProcOutput.Length == 0))
                    {
                        Notify("Обработка звука", "Сначала выберите устройства в окне «Обработка».");
                        return;
                    }
                    autoEnabled = false;   // a manual choice: the game exiting does not undo it
                    SetProcessing(!s.ProcOn);
                    if (s.ProcOn) NotifyWhenStarted("Режим: " + ProcModeNames[Math.Max(0, Math.Min(2, s.ProcMode))]);
                    else { startNotice = null; Notify("Обработка звука выключена", "Звук игры идёт напрямую."); }
                    break;
                case HkMode:
                    s.ProcMode = (Math.Max(0, Math.Min(2, s.ProcMode)) + 1) % 3;
                    s.Dirty = true;
                    processing.SendParams();
                    if (procForm != null && !procForm.IsDisposed) procForm.SyncFromSettings();
                    Notify("Режим обработки: " + ProcModeNames[s.ProcMode], s.ProcOn ? "Обработка включена." : "Обработка сейчас выключена (Ctrl+Alt+P).");
                    break;
                case HkLimiter:
                    SetEnabled(!s.Enabled);
                    Notify(s.Enabled ? "Лимитер включён" : "Лимитер выключен", s.Enabled ? "Лимиты приложений снова действуют." : "Громкость приложений больше не ограничивается.");
                    break;
            }
        }

        void OnNewerVersion(Version v)
        {
            bool first = newerVersion == null || newerVersion != v;
            newerVersion = v;
            string name = v.Build > 0 ? v.ToString(3) : v.ToString(2);
            trayDownload.Text = "Скачать VolumeGuard " + name + "…";
            trayDownload.Visible = true;
            Text = "VolumeGuard — доступна версия " + name;
            if (first) Notify("Вышла VolumeGuard " + name, "Нажмите, чтобы открыть страницу загрузки.", true);
        }

        static void OpenReleases()
        {
            try { Process.Start(Updates.ReleasesPage); } catch { }
        }

        /// A short notification next to the tray icon.
        void NotifyWhenStarted(string text)
        {
            startNotice = text;
            startNoticeSince = lastT;
        }

        /// The engine starts on a worker thread: say "on" when it runs, or why it did not start.
        void CheckStartNotice(double now)
        {
            if (!s.ProcOn) { startNotice = null; return; }
            if (processing.Starting)
            {
                if (now - startNoticeSince > 10.0)
                {
                    Notify("Обработка звука запускается…", "Это дольше обычного. Подробности — в окне «Обработка».");
                    startNotice = null;
                }
                return;
            }
            if (processing.StartResult == 0) Notify("Обработка звука включена", startNotice);
            else Notify("Обработка звука не запустилась", processing.StartErrorText() + " Если игра выведена в кабель, её сейчас не слышно — откройте окно «Обработка».");
            startNotice = null;
        }

        void Notify(string title, string text, bool update = false)
        {
            if (demo || !tray.Visible) return;
            balloonIsUpdate = update;
            try { tray.ShowBalloonTip(1500, title, string.IsNullOrEmpty(text) ? " " : text, ToolTipIcon.None); } catch { }
        }

        /// A label and a row of buttons of which one is selected.
        static void AddSegments(Panel row, ToolTip tips, string label, int y, string[] names, string[] hints, Func<int> get, Action<int> set)
        {
            row.Controls.Add(new Label
            {
                Text = label, ForeColor = Theme.Sub, Location = new Point(Theme.D(16), y), Size = new Size(Theme.D(130), Theme.D(30)),
                TextAlign = ContentAlignment.MiddleLeft
            });
            var buttons = new List<FlatButton>();
            int x = Theme.D(150);
            for (int i = 0; i < names.Length; i++)
            {
                int index = i;
                var b = new FlatButton { Text = names[i], Font = Theme.Font(9f), Location = new Point(x, y), Height = Theme.D(30) };
                b.FitWidth();
                b.Width = Math.Max(b.Width, Theme.D(84));
                b.Selected = get() == i;
                b.Click += delegate
                {
                    set(index);
                    foreach (var o in buttons) o.Selected = o == b;
                };
                tips.SetToolTip(b, hints[i]);
                buttons.Add(b);
                row.Controls.Add(b);
                x += b.Width + Theme.D(6);
            }
        }

        void ShowProcessing()
        {
            if (procForm == null || procForm.IsDisposed) procForm = new ProcessingForm(s, processing, iconOn);
            if (!procForm.Visible) procForm.Show(this);
            procForm.Activate();
        }

        void ToggleWindow()
        {
            if (Visible && WindowState != FormWindowState.Minimized) { Hide(); Native.TrimMemory(); }
            else ShowFromTray();
        }

        public void ShowFromTray()
        {
            Show();
            if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
            Activate();
            SyncMaster(true);
            lock (s.Sync)
                foreach (var a in s.Apps.Values)
                {
                    ChannelCard c;
                    if (cards.TryGetValue(a.Key, out c)) c.Version = -1;
                }
        }

        void ExitApp()
        {
            exiting = true;
            Close();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (!exiting && e.CloseReason == CloseReason.UserClosing && s.Tray && !demo)
            {
                e.Cancel = true;
                Hide();
                Native.TrimMemory();
                if (!s.TrayHintShown)
                {
                    s.TrayHintShown = true;
                    s.Dirty = true;
                    tray.ShowBalloonTip(4000, "VolumeGuard работает в трее", "Лимитер продолжает работать. Выйти можно через меню значка в трее.", ToolTipIcon.Info);
                }
                return;
            }
            timer.Stop();
            if (hotkeysRegistered)
                foreach (int id in new[] { HkProcessing, HkMode, HkLimiter }) Native.UnregisterHotKey(Handle, id);
            if (!demo) { try { Native.timeEndPeriod(1); } catch { } }
            try
            {
                var te = eng as ThreadedEngine;
                if (te != null) te.Stop();   // puts the volumes back, then ends the limiter thread
                else eng.Close();
            }
            catch { }
            processing.Shutdown();   // stop the audio threads before the DLL is unloaded
            if (!demo) { s.StopSaver(); s.Save(); }
            tray.Visible = false;
            base.OnFormClosing(e);
        }

        /// The program used to be called "Sound Lock+": move its autostart entry over to the new name.
        public static void MigrateAutostart()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RunKey, true))
                {
                    if (k == null || k.GetValue("SoundLockPlus") == null) return;
                    k.DeleteValue("SoundLockPlus", false);
                    k.SetValue(RunName, "\"" + Application.ExecutablePath + "\" --tray");
                }
            }
            catch { }
        }

        static bool AutostartOn()
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RunKey))
                    return k != null && k.GetValue(RunName) != null;
            }
            catch { return false; }
        }

        static void SetAutostart(bool on)
        {
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(RunKey))
                {
                    if (on) k.SetValue(RunName, "\"" + Application.ExecutablePath + "\" --tray");
                    else k.DeleteValue(RunName, false);
                }
            }
            catch { }
        }

        static Icon MakeIcon(bool on)
        {
            int sz = 32;
            using (var bmp = new Bitmap(sz, sz))
            {
                using (var g = Graphics.FromImage(bmp))
                {
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.Clear(Color.Transparent);
                    Theme.FillRound(g, on ? Theme.Accent : Theme.Disabled, new RectangleF(1, 1, 30, 30), 8);
                    using (var b = new SolidBrush(Color.White))
                    {
                        g.FillPolygon(b, new[]
                        {
                            new PointF(6, 12), new PointF(11, 12), new PointF(17, 6), new PointF(17, 26), new PointF(11, 20), new PointF(6, 20)
                        });
                    }
                    using (var pen = new Pen(Color.White, 2.4f))
                    {
                        g.DrawArc(pen, 15, 10, 8, 12, -55, 110);
                        if (on) g.DrawLine(pen, 25, 8, 25, 24);
                        else g.DrawArc(pen, 15, 6, 14, 20, -55, 110);
                    }
                }
                IntPtr h = bmp.GetHicon();
                var icon = (Icon)Icon.FromHandle(h).Clone();
                try { Native.DestroyIcon(h); } catch { }
                return icon;
            }
        }
    }

    static class Program
    {
        static void ReportCrash(Exception ex)
        {
            string text = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "\r\n" + ex + "\r\n\r\n";
            string log = null;
            try
            {
                System.IO.Directory.CreateDirectory(Settings.Dir);
                log = System.IO.Path.Combine(Settings.Dir, "crash.log");
                System.IO.File.AppendAllText(log, text);
            }
            catch { }
            try
            {
                MessageBox.Show("VolumeGuard столкнулся с ошибкой:\r\n\r\n" + ex.GetType().Name + ": " + ex.Message +
                    (log != null ? "\r\n\r\nПодробности записаны в:\r\n" + log : ""),
                    "VolumeGuard", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            catch { }
        }

        [STAThread]
        static void Main(string[] args)
        {
            AppDomain.CurrentDomain.UnhandledException += (o, e) =>
            {
                var ex = e.ExceptionObject as Exception;
                if (ex != null) ReportCrash(ex);
            };
            Application.ThreadException += (o, e) => ReportCrash(e.Exception);
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
            try { Run(args); }
            catch (Exception ex) { ReportCrash(ex); }
        }

        static void Run(string[] args)
        {
            bool demo = args.Contains("--demo");
            bool hidden = args.Contains("--tray");
            if (!demo) { try { Native.SetProcessDPIAware(); } catch { } }

            bool created = true;
            Mutex mutex = null;
            EventWaitHandle showEvt = null;
            if (!demo)
            {
                try
                {
                    mutex = new Mutex(true, "VolumeGuard_SingleInstance", out created);
                    showEvt = new EventWaitHandle(false, EventResetMode.AutoReset, "VolumeGuard_Show");
                }
                catch { }
                if (!created)
                {
                    // already running: ask the first copy to show its window
                    if (showEvt != null) showEvt.Set();
                    return;
                }
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            try
            {
                using (var g = Graphics.FromHwnd(IntPtr.Zero)) Theme.Scale = Math.Max(1f, g.DpiX / 96f);
            }
            catch { }

            if (!demo) MainForm.MigrateAutostart();
            Settings s = demo ? DemoSettings() : Settings.Load();
            if (!demo)
            {
                if (s.LoadedFromBackup) Log.Line("settings.txt was damaged: loaded the previous copy (settings.bak)");
                s.StartSaver();   // settings.txt is written on its own thread (at once when a volume is lowered)
            }
            IEngine eng = demo ? (IEngine)new FakeEngine(s) : new ThreadedEngine(s);
            var form = new MainForm(s, eng, hidden && !demo, demo);
            if (demo && args.Contains("--processing")) form.Shown += delegate { form.OpenProcessing(); };
            if (showEvt != null)
            {
                ThreadPool.RegisterWaitForSingleObject(showEvt, delegate
                {
                    try { form.BeginInvoke(new Action(form.ShowFromTray)); } catch { }
                }, null, -1, false);
            }
            Application.Run(form);
            GC.KeepAlive(mutex);
        }

        static Settings DemoSettings()
        {
            var s = new Settings();
            s.ProcOn = true; s.ProcMode = 2; s.ProcAmount = 60f; s.ProcCeiling = -3f;
            s.Master.UserVol = 0.62f; s.Master.LimitOn = true; s.Master.LimitDb = -8f;
            Action<string, string, float, bool, float, bool> add = (k, t, v, lo, ld, mu) =>
                s.Apps[k] = new AppModel { Key = k, Title = t, UserVol = v, LimitOn = lo, LimitDb = ld, Muted = mu };
            add("chrome", "Google Chrome", 0.85f, true, -14f, false);
            add("discord", "Discord", 0.7f, false, -12f, false);
            add("spotify", "Spotify", 0.45f, true, -10f, false);
            add("#system", "Системные звуки", 0.35f, false, -12f, true);
            return s;
        }
    }
}
