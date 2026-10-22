// Update check: once at start-up and then once a day, VolumeGuard asks GitHub which releases exist
// (a plain HTTPS GET to api.github.com, nothing about the user is sent) and tells if a newer one is out.
using System;
using System.IO;
using System.Net;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace VolumeGuard
{
    static class Updates
    {
        public const string ReleasesPage = "https://github.com/qqkillua/VolumeGuard/releases";
        const string Api = "https://api.github.com/repos/qqkillua/VolumeGuard/releases?per_page=15";

        public static Version Current
        {
            get { return Assembly.GetExecutingAssembly().GetName().Version; }
        }

        /// The newest app version among the release tags ("v2.8.0"; the plugin's "limiter-v1.3.0" are not the app).
        /// Pre-releases are skipped (2.8.7): a version is tried there first, and only a normal release is offered.
        public static Version NewestIn(string json)
        {
            json = json ?? "";
            Version best = null;
            var tags = Regex.Matches(json, "\"tag_name\"\\s*:\\s*\"v(\\d+\\.\\d+(?:\\.\\d+){0,2})\"");
            var all = Regex.Matches(json, "\"tag_name\"\\s*:");
            foreach (Match m in tags)
            {
                // this release's fields: from its tag_name up to the next release's tag_name
                int end = json.Length;
                foreach (Match t in all) if (t.Index > m.Index) { end = t.Index; break; }
                if (Regex.IsMatch(json.Substring(m.Index, end - m.Index), "\"prerelease\"\\s*:\\s*true")) continue;
                Version v;
                if (TryParse(m.Groups[1].Value, out v) && (best == null || v > best)) best = v;
            }
            return best;
        }

        static bool TryParse(string s, out Version v)
        {
            v = null;
            try { v = new Version(s); return true; } catch { return false; }
        }

        /// 2.8 and 2.8.0.0 are the same version.
        public static bool IsNewer(Version candidate, Version current)
        {
            if (candidate == null || current == null) return false;
            Func<Version, long> key = v => ((long)Math.Max(0, v.Major) << 48) | ((long)Math.Max(0, v.Minor) << 32) |
                                           ((long)Math.Max(0, v.Build) << 16) | (long)Math.Max(0, v.Revision);
            return key(candidate) > key(current);
        }

        /// On a worker thread; calls back (on that thread) with the newer version, or not at all.
        public static void CheckAsync(Action<Version> onNewer)
        {
            ThreadPool.QueueUserWorkItem(delegate
            {
                try
                {
                    // .NET 4.x on Windows 7/8 does not offer TLS 1.2 unless asked; GitHub requires it
                    ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
                    var req = (HttpWebRequest)WebRequest.Create(Api);
                    req.UserAgent = "VolumeGuard/" + Current;
                    req.Accept = "application/vnd.github+json";
                    req.Timeout = 15000;
                    req.ReadWriteTimeout = 15000;
                    string json;
                    using (var resp = (HttpWebResponse)req.GetResponse())
                    using (var sr = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                        json = sr.ReadToEnd();
                    Version newest = NewestIn(json);
                    if (IsNewer(newest, Current)) onNewer(newest);
                }
                catch (Exception ex) { Log.Line("update check failed: " + ex.Message); }
            });
        }
    }
}
