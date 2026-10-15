// Custom dark-theme controls: slider, toggle, mute button, channel card.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Windows.Forms;

namespace VolumeGuard
{
    static class Theme
    {
        public static readonly Color Bg = Color.FromArgb(24, 25, 28);
        public static readonly Color Card = Color.FromArgb(35, 37, 42);
        public static readonly Color Text = Color.FromArgb(233, 234, 237);
        public static readonly Color Sub = Color.FromArgb(146, 150, 160);
        public static readonly Color Track = Color.FromArgb(58, 61, 68);
        public static readonly Color Accent = Color.FromArgb(92, 141, 255);
        public static readonly Color Meter = Color.FromArgb(67, 181, 129);
        public static readonly Color Warn = Color.FromArgb(242, 176, 58);
        public static readonly Color Red = Color.FromArgb(240, 71, 71);
        public static readonly Color Disabled = Color.FromArgb(90, 94, 103);

        public static float Scale = 1f;
        public static int D(float px) { return (int)Math.Round(px * Scale); }
        public static Font Font(float pt) { return new Font("Segoe UI", pt, FontStyle.Regular); }
        public static Font Bold(float pt) { return new Font("Segoe UI Semibold", pt, FontStyle.Regular); }

        public static GraphicsPath Round(RectangleF r, float rad)
        {
            var p = new GraphicsPath();
            rad = Math.Min(rad, Math.Min(r.Width, r.Height) / 2f);
            if (rad <= 0.5f) { p.AddRectangle(r); return p; }
            float d = rad * 2;
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static void FillRound(Graphics g, Color c, RectangleF r, float rad)
        {
            if (r.Width <= 0 || r.Height <= 0) return;
            using (var b = new SolidBrush(c))
            using (var p = Round(r, rad))
                g.FillPath(b, p);
        }
    }

    class FlatSlider : Control
    {
        float val;
        public bool MeterMode;      // limit slider: draw the level meter, knob = limit marker
        public float Meter = -1f;   // 0..1 position of the current level
        public Color Accent = Theme.Accent;
        public bool Dragging { get; private set; }
        public event EventHandler ValueChanged;

        public FlatSlider()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
            TabStop = true;
            Cursor = Cursors.Hand;
            Height = Theme.D(24);
        }

        public float Value
        {
            get { return val; }
            set { val = Db.Clamp(value); Invalidate(); }
        }

        int Pad { get { return Theme.D(9); } }
        float PosFromX(int x) { return (x - Pad) / (float)Math.Max(1, Width - 2 * Pad); }

        void SetFromUser(float v)
        {
            v = Db.Clamp(v);
            if (Math.Abs(v - val) < 1e-4f) return;
            val = v;
            Invalidate();
            if (ValueChanged != null) ValueChanged(this, EventArgs.Empty);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (!Enabled || e.Button != MouseButtons.Left) return;
            Focus();
            Dragging = true;
            Capture = true;
            SetFromUser(PosFromX(e.X));
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (Dragging) SetFromUser(PosFromX(e.X));
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            Dragging = false;
            Capture = false;
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e);
            if (Enabled) SetFromUser(val + Math.Sign(e.Delta) * 0.02f);
        }

        protected override bool IsInputKey(Keys k)
        {
            return k == Keys.Left || k == Keys.Right || k == Keys.Up || k == Keys.Down || base.IsInputKey(k);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (e.KeyCode == Keys.Left || e.KeyCode == Keys.Down) SetFromUser(val - 0.01f);
            if (e.KeyCode == Keys.Right || e.KeyCode == Keys.Up) SetFromUser(val + 0.01f);
        }

        protected override void OnGotFocus(EventArgs e) { base.OnGotFocus(e); Invalidate(); }
        protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); Invalidate(); }
        protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(BackColor);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float h = Theme.D(6);
            float cy = Height / 2f;
            float x0 = Pad, w = Width - 2 * Pad;
            var track = new RectangleF(x0, cy - h / 2f, w, h);
            Theme.FillRound(g, Theme.Track, track, h / 2f);
            float vx = x0 + w * val;
            float kr = Theme.D(7);

            if (MeterMode)
            {
                if (Meter > 0.001f)
                {
                    float mx = x0 + w * Db.Clamp(Meter);
                    float under = Math.Min(mx, vx);
                    Theme.FillRound(g, Enabled ? Theme.Meter : Theme.Disabled, new RectangleF(x0, track.Y, under - x0, h), h / 2f);
                    if (mx > vx) Theme.FillRound(g, Enabled ? Theme.Red : Theme.Disabled, new RectangleF(vx, track.Y, mx - vx, h), h / 2f);
                }
                Color kc = Enabled ? Theme.Red : Theme.Disabled;
                using (var b = new SolidBrush(kc))
                    g.FillRectangle(b, vx - Theme.D(1), cy - Theme.D(10), Theme.D(2), Theme.D(20));
                using (var b = new SolidBrush(kc))
                    g.FillEllipse(b, vx - kr * 0.75f, cy - kr * 0.75f, kr * 1.5f, kr * 1.5f);
                using (var pen = new Pen(BackColor, Theme.D(2)))
                    g.DrawEllipse(pen, vx - kr * 0.75f, cy - kr * 0.75f, kr * 1.5f, kr * 1.5f);
            }
            else
            {
                Theme.FillRound(g, Enabled ? Accent : Theme.Disabled, new RectangleF(x0, track.Y, vx - x0, h), h / 2f);
                using (var b = new SolidBrush(Enabled ? Color.White : Theme.Sub))
                    g.FillEllipse(b, vx - kr, cy - kr, kr * 2, kr * 2);
            }
            if (Focused)
            {
                using (var pen = new Pen(Color.FromArgb(90, Accent), 1))
                    g.DrawEllipse(pen, vx - kr - 3, cy - kr - 3, kr * 2 + 6, kr * 2 + 6);
            }
        }
    }

    class ToggleSwitch : Control
    {
        bool on;
        public event EventHandler CheckedChanged;
        public Color OnColor = Theme.Accent;

        public ToggleSwitch()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            Cursor = Cursors.Hand;
            ForeColor = Theme.Text;
            Height = Theme.D(22);
        }

        public bool Checked
        {
            get { return on; }
            set { on = value; Invalidate(); }
        }

        public void FitWidth()
        {
            int tw = string.IsNullOrEmpty(Text) ? 0 : TextRenderer.MeasureText(Text, Font).Width + Theme.D(8);
            Width = Theme.D(36) + tw;
        }

        protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }

        protected override void OnClick(EventArgs e)
        {
            base.OnClick(e);
            on = !on;
            Invalidate();
            if (CheckedChanged != null) CheckedChanged(this, EventArgs.Empty);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(BackColor);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float pw = Theme.D(34), ph = Theme.D(18);
            float y = (Height - ph) / 2f;
            Theme.FillRound(g, on ? OnColor : Theme.Track, new RectangleF(1, y, pw, ph), ph / 2f);
            float kd = ph - Theme.D(6);
            float kx = on ? 1 + pw - Theme.D(3) - kd : 1 + Theme.D(3);
            using (var b = new SolidBrush(Color.White))
                g.FillEllipse(b, kx, y + Theme.D(3), kd, kd);
            if (!string.IsNullOrEmpty(Text))
            {
                var r = new Rectangle((int)pw + Theme.D(10), 0, Width - (int)pw - Theme.D(10), Height);
                TextRenderer.DrawText(g, Text, Font, r, ForeColor, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            }
        }
    }

    class MuteButton : Control
    {
        bool muted;
        public event EventHandler Toggled;

        public MuteButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
            Cursor = Cursors.Hand;
            Size = new Size(Theme.D(30), Theme.D(26));
        }

        public bool Muted { get { return muted; } set { muted = value; Invalidate(); } }

        protected override void OnClick(EventArgs e)
        {
            base.OnClick(e);
            muted = !muted;
            Invalidate();
            if (Toggled != null) Toggled(this, EventArgs.Empty);
        }

        protected override void OnMouseEnter(EventArgs e) { base.OnMouseEnter(e); Invalidate(); }
        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); Invalidate(); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(BackColor);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            bool hover = ClientRectangle.Contains(PointToClient(MousePosition));
            if (hover) Theme.FillRound(g, Theme.Track, new RectangleF(0, 0, Width - 1, Height - 1), Theme.D(5));
            Color c = muted ? Theme.Red : Theme.Sub;
            float s = Theme.Scale;
            float cx = Width / 2f - 4 * s, cy = Height / 2f;
            using (var b = new SolidBrush(c))
            {
                var pts = new[]
                {
                    new PointF(cx - 6 * s, cy - 3 * s), new PointF(cx - 2 * s, cy - 3 * s), new PointF(cx + 3 * s, cy - 7 * s),
                    new PointF(cx + 3 * s, cy + 7 * s), new PointF(cx - 2 * s, cy + 3 * s), new PointF(cx - 6 * s, cy + 3 * s)
                };
                g.FillPolygon(b, pts);
            }
            using (var pen = new Pen(c, 1.6f * s))
            {
                if (muted)
                {
                    g.DrawLine(pen, cx + 6 * s, cy - 4 * s, cx + 12 * s, cy + 4 * s);
                    g.DrawLine(pen, cx + 12 * s, cy - 4 * s, cx + 6 * s, cy + 4 * s);
                }
                else
                {
                    g.DrawArc(pen, cx + 1 * s, cy - 5 * s, 8 * s, 10 * s, -50, 100);
                    g.DrawArc(pen, cx - 1 * s, cy - 9 * s, 16 * s, 18 * s, -50, 100);
                }
            }
        }
    }

    /// One row: icon, name, volume slider, limiter switch and limit slider with a live meter.
    class ChannelCard : Control
    {
        public readonly FlatSlider Vol = new FlatSlider();
        public readonly FlatSlider Lim = new FlatSlider { MeterMode = true };
        public readonly ToggleSwitch LimOn = new ToggleSwitch();
        public readonly MuteButton Mute = new MuteButton();
        readonly Label volCaption = new Label(), volValue = new Label(), limValue = new Label();
        Bitmap iconBmp;
        string title = "";
        string badge = "";
        bool active = true;
        float meterShown;
        public int Version = -1;

        public ChannelCard(string titleText, Icon icon, bool showMute)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            BackColor = Theme.Bg;
            Height = Theme.D(112);
            title = titleText ?? "";
            if (icon != null)
            {
                try { iconBmp = icon.ToBitmap(); } catch { iconBmp = null; }
            }

            foreach (Control c in new Control[] { Vol, Lim, LimOn, Mute, volCaption, volValue, limValue })
            {
                c.BackColor = Theme.Card;
                Controls.Add(c);
            }
            volCaption.Text = "Громкость";
            volCaption.ForeColor = Theme.Sub;
            volCaption.Font = Theme.Font(9f);
            volCaption.TextAlign = ContentAlignment.MiddleLeft;
            foreach (var l in new[] { volValue, limValue })
            {
                l.ForeColor = Theme.Text;
                l.Font = Theme.Font(9f);
                l.TextAlign = ContentAlignment.MiddleRight;
            }
            LimOn.Text = "Лимит";
            LimOn.Font = Theme.Font(9f);
            LimOn.ForeColor = Theme.Sub;
            LimOn.OnColor = Theme.Red;
            Mute.Visible = showMute;
            Vol.ValueChanged += delegate { volValue.Text = Pct(Vol.Value); };
            Lim.ValueChanged += delegate { limValue.Text = DbText(Db.PosToDb(Lim.Value)); };
            LimOn.CheckedChanged += delegate { Lim.Enabled = LimOn.Checked; LimOn.ForeColor = LimOn.Checked ? Theme.Text : Theme.Sub; };
            DoLayout();
        }

        static string Pct(float v) { return Math.Round(v * 100) + "%"; }
        static string DbText(float db) { return db >= 0 ? "0 дБ" : "−" + Math.Round(-db) + " дБ"; }

        public string Title { set { title = value ?? ""; Invalidate(); } }

        public void SetValues(float vol, bool limOn, float limitDb, bool muted)
        {
            if (!Vol.Dragging) { Vol.Value = vol; volValue.Text = Pct(vol); }
            if (!Lim.Dragging) { Lim.Value = Db.DbToPos(limitDb); limValue.Text = DbText(limitDb); }
            LimOn.Checked = limOn;
            Lim.Enabled = limOn;
            LimOn.ForeColor = limOn ? Theme.Text : Theme.Sub;
            Mute.Muted = muted;
        }

        /// badgeText: what the limiter is doing right now, or "" when it is idle.
        public void SetLive(float outLevel, string badgeText, bool isActive)
        {
            float pos = Db.LevelToPos(outLevel);
            meterShown = Math.Max(pos, meterShown - 0.035f);
            if (Math.Abs(Lim.Meter - meterShown) > 0.002f) { Lim.Meter = meterShown; Lim.Invalidate(); }
            string b = badgeText ?? "";
            if (b != badge || isActive != active) { badge = b; active = isActive; Invalidate(new Rectangle(0, 0, Width, Theme.D(40))); }
        }

        protected override void OnResize(EventArgs e) { base.OnResize(e); DoLayout(); }

        void DoLayout()
        {
            int pad = Theme.D(14), right = Width - pad;
            int capW = Theme.D(88), valW = Theme.D(54);
            Mute.Location = new Point(right - Mute.Width + Theme.D(4), Theme.D(9));
            int y1 = Theme.D(42), y2 = Theme.D(74), rowH = Theme.D(26);
            volCaption.SetBounds(pad, y1, capW, rowH);
            Vol.SetBounds(pad + capW - Theme.D(6), y1 + 1, right - valW - (pad + capW) + Theme.D(6), rowH - 2);
            volValue.SetBounds(right - valW, y1, valW, rowH);
            LimOn.SetBounds(pad, y2, capW, rowH);
            Lim.SetBounds(pad + capW - Theme.D(6), y2 + 1, right - valW - (pad + capW) + Theme.D(6), rowH - 2);
            limValue.SetBounds(right - valW, y2, valW, rowH);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(BackColor);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            Theme.FillRound(g, Theme.Card, new RectangleF(0, 0, Width - 1, Height - Theme.D(8)), Theme.D(10));

            int pad = Theme.D(14), isz = Theme.D(22);
            var ir = new Rectangle(pad, Theme.D(11), isz, isz);
            if (iconBmp != null)
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.DrawImage(iconBmp, ir);
            }
            else
            {
                using (var b = new SolidBrush(Theme.Track)) g.FillEllipse(b, ir);
                string letter = title.Length > 0 ? title.Substring(0, 1).ToUpperInvariant() : "?";
                using (var f = Theme.Bold(9f))
                    TextRenderer.DrawText(g, letter, f, ir, Theme.Text, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            }

            int tx = ir.Right + Theme.D(10);
            int badgeW = 0;
            if (badge.Length > 0)
            {
                using (var f = Theme.Font(8.5f))
                {
                    badgeW = TextRenderer.MeasureText(badge, f).Width + Theme.D(12);
                    int bx = Mute.Visible ? Mute.Left - badgeW - Theme.D(6) : Width - Theme.D(14) - badgeW;
                    var br = new Rectangle(bx, Theme.D(12), badgeW, Theme.D(20));
                    Theme.FillRound(g, Color.FromArgb(60, Theme.Warn), br, Theme.D(10));
                    TextRenderer.DrawText(g, badge, f, br, Theme.Warn, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                }
            }
            int tw = (Mute.Visible ? Mute.Left : Width - Theme.D(14)) - tx - badgeW - Theme.D(10);
            using (var f = Theme.Bold(10.5f))
                TextRenderer.DrawText(g, title, f, new Rectangle(tx, Theme.D(9), Math.Max(10, tw), Theme.D(26)),
                    active ? Theme.Text : Theme.Sub, TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && iconBmp != null) { iconBmp.Dispose(); iconBmp = null; }
            base.Dispose(disposing);
        }
    }
}
