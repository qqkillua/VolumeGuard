// Offline tests for the app's limiter (Limiter.Step, as the audio thread runs it every 2 ms) and for reading
// settings.txt. Built and run by CI on Windows:
//   csc /main:VolumeGuard.Tests.TestProgram /out:LimiterTests.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll
//       /r:System.Core.dll src\*.cs tests\LimiterTests.cs
// and with Mono: mcs -main:VolumeGuard.Tests.TestProgram ... (same files).
using System;
using System.Collections.Generic;

namespace VolumeGuard.Tests
{
    static class TestProgram
    {
        static int failures;

        static void Check(bool ok, string what)
        {
            if (!ok) { failures++; Console.WriteLine("FAIL: " + what); }
        }

        static float Lin(float db) { return Db.ToLin(db); }
        static float DbOf(float lin) { return Db.FromLin(lin); }

        /// One app with one stream, stepped like the audio thread (2 ms). level(t) gives the input peak.
        class Sim
        {
            public float Gain = 1f, Env, Ms, Hold;
            public float User = 1f, LimitDb = -12f;
            public int Speed = 1, Mode = 0, Memory = 0;
            public double T;
            public float MaxOverDb = -100f;   // how far the output got over the limit (dB)

            public void Run(double seconds, Func<double, float> level)
            {
                const double dt = 0.002;
                float decay = (float)Math.Exp(-dt / Limiter.EnvSec(Speed));
                float rel = 1f - (float)Math.Exp(-dt / Limiter.ReturnSec(Speed));
                float msCoef = 1f - (float)Math.Exp(-dt / Limiter.LevelSec(Mode));
                double mem = Limiter.MemorySec(Memory);
                float holdDecay = mem > 0 ? (float)Math.Exp(-dt / mem) : 1f;
                double att = Limiter.AttackSec(Mode);
                float attack = att > 0 ? 1f - (float)Math.Exp(-dt / att) : 1f;
                for (double end = T + seconds; T < end; T += dt)
                {
                    float p = level(T);
                    // the volume in effect while this packet plays is the one set before it was measured
                    float outDb = DbOf(p * Gain) - LimitDb;
                    if (p >= Limiter.Gate && outDb > MaxOverDb) MaxOverDb = outDb;
                    Limiter.Step(ref Gain, ref Env, ref Ms, ref Hold, p, User, true, LimitDb, Math.Min(User, 0.02f),
                                 decay, rel, msCoef, Mode, holdDecay, attack);
                    if (float.IsNaN(Gain) || Gain > User + 1e-6f || Gain < 0f) throw new Exception("gain out of range: " + Gain);
                }
            }

            public float GainDb { get { return DbOf(Gain); } }
        }

        static float Amb = Lin(-30f), Shot = Lin(-2f), Ad = Lin(-6f);

        static void Main()
        {
            // 1. Peak mode holds a steady loud sound at the limit (after the first packet).
            {
                var s = new Sim();
                s.Run(1.0, t => Amb);
                s.Run(2.0, t => Ad);
                Check(Math.Abs(s.GainDb - (-6f)) < 0.3f, "peaks: steady -6 dB sound should be turned down 6 dB, got " + s.GainDb);
            }

            // 2. Return speed: time to get back to 90 % volume after a loud sound ends.
            double[] want = { 0.6, 1.1, 1.9 };
            for (int speed = 0; speed < 3; speed++)
            {
                var s = new Sim { Speed = speed };
                s.Run(1.0, t => Amb);
                s.Run(0.5, t => Shot);
                double start = s.T, back = -1;
                while (s.T - start < 6.0)
                {
                    s.Run(0.01, t => Amb);
                    if (s.Gain >= 0.9f) { back = s.T - start; break; }
                }
                Check(back > 0 && Math.Abs(back - want[speed]) < 0.35, "speed " + speed + ": back to 90 % after " + back.ToString("0.00") + " s, expected ~" + want[speed]);
            }

            // 3. Silence holds the reduced volume (a loud sound after a pause must not come at full volume).
            {
                var s = new Sim();
                s.Run(1.0, t => Shot);
                float before = s.Gain;
                s.Run(5.0, t => 0f);
                Check(Math.Abs(s.Gain - before) < 0.01f, "silence: volume changed from " + before + " to " + s.Gain);
            }

            // 4. Memory of loud sounds: repeated shots every 3 s overshoot less.
            float[] over = new float[3];
            for (int mem = 0; mem < 3; mem++)
            {
                var s = new Sim { Memory = mem };
                Func<double, float> fire = t => (t % 3.0) < 0.05 ? Shot : Amb;   // a shot every 3 s (t = 3, 6, 9…)
                s.Run(3.1, fire);                 // the first shot after a calm always gets through: not counted
                s.MaxOverDb = -100f;
                s.Run(12.0, fire);
                over[mem] = s.MaxOverDb;
            }
            Check(over[0] > 9f, "memory off: repeated shot should overshoot ~10 dB, got " + over[0]);
            Check(over[1] < 6.5f, "memory 10 s: overshoot should be <= ~6 dB, got " + over[1]);
            Check(over[2] < over[1], "memory 30 s should overshoot less than 10 s");

            // 5. Loudness mode: a single shot dips the volume far less than in peak mode; a long loud sound is
            //    still brought to the limit within 2 s.
            {
                var peak = new Sim { Mode = 0 };
                var loud = new Sim { Mode = 1 };
                Func<double, float> shot = t => (t >= 1.0 && t < 1.03) ? Shot : Amb;
                peak.Run(1.05, shot);
                loud.Run(1.05, shot);
                Check(peak.GainDb < -9f, "peaks: single shot should cut ~10 dB, got " + peak.GainDb);
                Check(loud.GainDb > -5f, "loudness: single shot should dip only ~3.5 dB, got " + loud.GainDb);
                var ad = new Sim { Mode = 1 };
                ad.Run(1.0, t => Amb);
                ad.Run(2.0, t => Ad);
                Check(ad.GainDb < -5.5f, "loudness: long loud sound should reach ~-6 dB in 2 s, got " + ad.GainDb);
            }

            // 6. Smooth mode: a single shot does not move the volume at all; a long loud sound is lowered within ~3 s,
            //    and never by a sudden cut (no step bigger than 0.5 dB per 2 ms).
            {
                var s = new Sim { Mode = 2 };
                s.Run(1.0, t => Amb);
                s.Run(0.03, t => Shot);
                s.Run(0.5, t => Amb);
                Check(s.GainDb > -0.5f, "smooth: single shot changed the volume by " + s.GainDb + " dB");
                var ad = new Sim { Mode = 2 };
                ad.Run(1.0, t => Amb);
                float prev = ad.GainDb, worstStep = 0f;
                for (int i = 0; i < 1500; i++)
                {
                    ad.Run(0.002, t => Ad);
                    worstStep = Math.Max(worstStep, prev - ad.GainDb);
                    prev = ad.GainDb;
                }
                Check(ad.GainDb < -3.5f, "smooth: long loud sound should be lowered >= 3.5 dB in 3 s, got " + ad.GainDb);
                Check(worstStep < 0.5f, "smooth: volume dropped " + worstStep + " dB in one 2 ms step");
            }

            // 7. Several streams of one app add up.
            {
                float sum = 0f;
                foreach (var p in new[] { Lin(-12f), Lin(-12f) }) sum = Limiter.CombinePeak(sum, p);
                Check(Math.Abs(DbOf(sum) - (-6f)) < 0.1f, "two -12 dB streams should count as -6 dB, got " + DbOf(sum));
                float quiet = 0f;
                foreach (var p in new[] { Lin(-12f), Lin(-70f), 0f }) quiet = Limiter.CombinePeak(quiet, p);
                Check(Math.Abs(DbOf(quiet) - (-12f)) < 0.01f, "silent streams must not add, got " + DbOf(quiet));
            }

            // 8. A damaged settings.txt: bad numbers fall back to defaults, the rest still loads.
            {
                var s = new Settings();
                s.ParseLines(new[]
                {
                    "opt|return|NaN", "opt|limitmode|7", "opt|memory|-3",
                    "master|Infinity|1|NaN|0",
                    "app|game|Game|NaN|1|-1e30|0|0|NaN",
                    "app|ok|Ok|0.5|1|-18|0|0|0.2",
                    "garbage line without separators",
                    "app|short|line",
                    "proc|1|a|b|x|1e9|NaN|-99",
                });
                Check(s.ReturnSpeed == 1, "NaN return speed should give the default, got " + s.ReturnSpeed);
                Check(s.LimitMode == 2 && s.Memory == 0, "modes should be clamped, got " + s.LimitMode + " / " + s.Memory);
                Check(s.Master.UserVol == 0.5f && s.Master.LimitDb == -12f, "master: " + s.Master.UserVol + " / " + s.Master.LimitDb);
                AppModel g;
                Check(s.Apps.TryGetValue("game", out g) && g.UserVol == 1f && g.LimitDb == Db.MinDb && g.Env == 0f,
                      "game: bad numbers should be replaced");
                AppModel ok;
                Check(s.Apps.TryGetValue("ok", out ok) && ok.UserVol == 0.5f && ok.LimitDb == -18f, "a good line must still load");
                Check(!s.Apps.ContainsKey("short"), "a short line must be skipped");
                Check(s.ProcAmount == 100f && s.ProcCeiling == -1f && s.ProcGain == -12f, "proc values should be clamped");
                foreach (var a in s.Apps.Values)
                    Check(!float.IsNaN(a.UserVol) && !float.IsNaN(a.LimitDb) && !float.IsNaN(a.Env), "NaN in " + a.Key);
            }

            if (failures > 0) { Console.WriteLine(failures + " check(s) failed"); Environment.Exit(1); }
            Console.WriteLine("all limiter checks passed");
        }
    }
}
