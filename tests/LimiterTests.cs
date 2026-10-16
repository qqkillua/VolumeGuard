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
                float msRise = 1f - (float)Math.Exp(-dt / Limiter.RiseSec(Mode));
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
                    Limiter.Step(ref Gain, ref Env, ref Ms, ref Hold, p, User, true, LimitDb, Limiter.Floor(User, false),
                                 decay, rel, msCoef, msRise, Mode, holdDecay, attack);
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

            // 1b. Low limits are kept too: a full-scale sound with a −40 or −60 dB limit needs 1 % / 0.1 % volume.
            foreach (float lim in new[] { -40f, -60f })
            {
                var s = new Sim { LimitDb = lim };
                s.Run(1.0, t => 1f);
                Check(DbOf(1f * s.Gain) <= lim + 0.1f, "limit " + lim + " dB: output " + DbOf(s.Gain) + " dB");
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

            // 6b. A loud sound that starts after a calm and goes on (not a single shot): how long it stays more than
            //     3 dB over the limit. Before 2.7.3 the average needed ~0.3 s (loudness) and ~1 s (smooth).
            {
                float[] limits = { 0.15f, 0.35f };   // seconds, loudness / smooth (measured: ~0.09 / ~0.31)
                for (int mode = 1; mode <= 2; mode++)
                {
                    var s = new Sim { Mode = mode };
                    s.Run(2.0, t => Amb);
                    double start = s.T, loudFor = 0;
                    for (int i = 0; i < 1000; i++)
                    {
                        float before = s.Gain;
                        s.Run(0.002, t => Ad);
                        if (DbOf(Ad * before) - s.LimitDb > 3f) loudFor = s.T - start;
                    }
                    Check(loudFor < limits[mode - 1], (mode == 1 ? "loudness" : "smooth") + ": a sudden long loud sound stays >3 dB over the limit for " + loudFor.ToString("0.00") + " s");
                    Console.WriteLine((mode == 1 ? "loudness" : "smooth") + ": sudden loud sound over the limit by >3 dB for " + loudFor.ToString("0.000") + " s");
                }
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

            // 9. 2.7 settings: hotkeys and the game that switches the processing on.
            {
                var s = new Settings();
                Check(s.Hotkeys && s.ProcAuto && s.ProcAutoApp == "", "defaults: hotkeys on, auto on, no game");
                s.ParseLines(new[] { "opt|hotkeys|0", "opt|procauto|1|TslGame|PUBG: BATTLEGROUNDS" });
                Check(!s.Hotkeys, "hotkeys off should load");
                Check(s.ProcAuto && s.ProcAutoApp == "tslgame" && s.ProcAutoTitle == "PUBG: BATTLEGROUNDS",
                      "auto game: " + s.ProcAutoApp + " / " + s.ProcAutoTitle);
                s.ParseLines(new[] { "route|TslGame|{0.0.0.00000000}.{abc}", "route|discord|", "route||x", "route|short" });
                string prev;
                Check(s.ProcPrevRoute.TryGetValue("tslgame", out prev) && prev == "{0.0.0.00000000}.{abc}", "previous device of the game");
                Check(s.ProcPrevRoute.TryGetValue("discord", out prev) && prev == "", "previous device \"default\"");
                Check(s.ProcPrevRoute.Count == 2, "bad route lines must be skipped, got " + s.ProcPrevRoute.Count);
                s.ParseLines(new[] { "opt|procauto|0" });   // too short: ignored
                Check(s.ProcAuto && s.ProcAutoApp == "tslgame", "a short procauto line must be ignored");
            }

            // 10. Update check: the newest app tag wins, the plugin's tags are ignored, 2.8 == 2.8.0.0.
            {
                string json = "[{\"tag_name\":\"limiter-v9.0.0\"},{\"tag_name\": \"v2.7.3\"},{\"tag_name\":\"v2.10.0\"},{\"tag_name\":\"v2.8\"}]";
                var v = Updates.NewestIn(json);
                Check(v != null && v.ToString() == "2.10.0", "newest tag: " + v);
                Check(Updates.IsNewer(new Version(2, 8), new Version(2, 7, 3, 0)), "2.8 is newer than 2.7.3.0");
                Check(!Updates.IsNewer(new Version(2, 8), new Version(2, 8, 0, 0)), "2.8 is not newer than 2.8.0.0");
                Check(!Updates.IsNewer(null, new Version(2, 8)), "no tag: nothing newer");
                Check(Updates.NewestIn("garbage") == null, "garbage json");
            }

            // 11. Soak: one hour of game-like sound per mode — calm, footsteps, shots, bursts, explosions, silence —
            //     with the user moving the volume and the limit now and then. Nothing may go wrong over time:
            //     the volume stays within 0…user, no NaN, and after each calm stretch it comes back up.
            for (int mode = 0; mode < 3; mode++)
            {
                var rnd = new Random(7 + mode);
                var s = new Sim { Mode = mode, Memory = 0, LimitDb = -12f };   // memory would hold the volume on purpose
                double nextScene = 0, sceneEnd = 0;
                int scene = 0, notBack = 0, calms = 0;
                float worstStuck = 0f;
                Func<double, float> sound = t =>
                {
                    switch (scene)
                    {
                        case 0: return Lin(-35f + 4f * (float)rnd.NextDouble());                                    // calm
                        case 1: return (t % 0.45) < 0.05 ? Lin(-28f) : Lin(-40f);                                   // steps
                        case 2: return (t % 0.1) < 0.03 ? Lin(-3f + 2f * (float)rnd.NextDouble()) : Lin(-30f);      // burst
                        case 3: return Lin(-6f + 3f * (float)Math.Sin(t * 3));                                      // explosion
                        default: return 0f;                                                                          // silence
                    }
                };
                while (s.T < 3600.0)
                {
                    if (s.T >= nextScene)
                    {
                        int prev = scene;
                        scene = rnd.Next(5);
                        // after a calm stretch the volume must be back near what the calm allows
                        if (prev == 0 && sceneEnd > 0)
                        {
                            calms++;
                            float allowed = Math.Min(s.User, Lin(s.LimitDb) / Lin(-31f));
                            float back = DbOf(s.Gain) - DbOf(allowed);
                            if (back < -3f) { notBack++; worstStuck = Math.Min(worstStuck, back); }
                        }
                        double len = scene == 0 ? 8 + 10 * rnd.NextDouble() : 1 + 6 * rnd.NextDouble();
                        nextScene = s.T + len;
                        sceneEnd = nextScene;
                        if (rnd.NextDouble() < 0.05) s.User = 0.3f + 0.7f * (float)rnd.NextDouble();   // volume moved
                        if (rnd.NextDouble() < 0.03) s.LimitDb = -30f + 25f * (float)rnd.NextDouble(); // limit moved
                        if (s.Gain > s.User) s.Gain = s.User;
                    }
                    s.Run(0.1, sound);   // Run throws if the volume ever leaves 0…user or is NaN
                }
                Console.WriteLine("soak " + new[] { "peaks", "loudness", "smooth" }[mode] + ": 1 h, " + calms + " calm stretches, " +
                                  notBack + " without coming back (worst " + worstStuck.ToString("0.0") + " dB)");
                Check(notBack == 0, "soak mode " + mode + ": the volume did not come back after " + notBack + " of " + calms + " calm stretches");
            }

            if (failures > 0) { Console.WriteLine(failures + " check(s) failed"); Environment.Exit(1); }
            Console.WriteLine("all limiter checks passed");
        }
    }
}
