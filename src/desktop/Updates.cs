using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace AzerothCoop.Desktop
{
    internal sealed class ModVersion : IComparable<ModVersion>
    {
        internal readonly int Major, Minor, Patch;
        internal readonly string[] Pre;
        internal readonly string Text;
        internal ModVersion(string value)
        {
            var match = Regex.Match(value ?? "", @"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$");
            if (!match.Success || value.Length > 80) throw new InvalidDataException("Ungültige Mod-Version.");
            Major = int.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture);
            Minor = int.Parse(match.Groups[2].Value, CultureInfo.InvariantCulture);
            Patch = int.Parse(match.Groups[3].Value, CultureInfo.InvariantCulture);
            Pre = match.Groups[4].Success ? match.Groups[4].Value.Split('.') : new string[0]; Text = value;
            foreach (string part in Pre)
                if (part.Length > 1 && part[0] == '0' && part.All(char.IsDigit)) throw new InvalidDataException("Ungültige Vorabversion.");
        }
        public int CompareTo(ModVersion other)
        {
            int result = Major.CompareTo(other.Major); if (result != 0) return result;
            result = Minor.CompareTo(other.Minor); if (result != 0) return result;
            result = Patch.CompareTo(other.Patch); if (result != 0) return result;
            if (Pre.Length == 0 || other.Pre.Length == 0)
                return Pre.Length == other.Pre.Length ? 0 : Pre.Length == 0 ? 1 : -1;
            for (int index = 0; index < Math.Min(Pre.Length, other.Pre.Length); ++index)
            {
                string left = Pre[index], right = other.Pre[index];
                bool ln = left.All(char.IsDigit), rn = right.All(char.IsDigit);
                if (ln && rn) result = left.Length == right.Length ? string.CompareOrdinal(left, right) : left.Length.CompareTo(right.Length);
                else if (ln != rn) result = ln ? -1 : 1;
                else result = string.CompareOrdinal(left, right);
                if (result != 0) return result;
            }
            return Pre.Length.CompareTo(other.Pre.Length);
        }
    }
    internal sealed class UpdateOffer
    {
        internal string Version, Sha256;
        internal Uri Setup;
        internal long Size;
    }
    internal sealed class Updates
    {
        internal const string SetupName = "AzerothSurvivors-Koop-Setup.exe";
        const int MaximumSetup = 128 * 1024 * 1024;
        readonly Backend backend;
        internal readonly string Current;
        internal Updates(Backend value) { backend = value; Current = Json.Text(Json.Read(Path.Combine(value.Root, "mod.json")), "version"); }
        string Settings { get { return Path.Combine(backend.Paths.Data, "updater.json"); } }
        internal bool Automatic
        {
            get { try { return !File.Exists(Settings) || Json.Flag(Json.Read(Settings), "automatic"); } catch { return true; } }
            set { Json.Write(Settings, new { schema = 1, automatic = value }); }
        }
        static bool AllowedHost(Uri uri)
        {
            return uri.Scheme == Uri.UriSchemeHttps && uri.UserInfo.Length == 0 && uri.Port == 443 &&
                (uri.Host.Equals("api.github.com", StringComparison.OrdinalIgnoreCase) ||
                 uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase) ||
                 uri.Host.Equals("release-assets.githubusercontent.com", StringComparison.OrdinalIgnoreCase) ||
                 uri.Host.Equals("objects.githubusercontent.com", StringComparison.OrdinalIgnoreCase) ||
                 uri.Host.Equals("github-releases.githubusercontent.com", StringComparison.OrdinalIgnoreCase));
        }
        internal static Uri AssetUri(string value, string tag, string name)
        {
            Uri uri;
            if (!Uri.TryCreate(value, UriKind.Absolute, out uri) || !AllowedHost(uri) || uri.Host != "github.com" ||
                uri.Query.Length != 0 || uri.Fragment.Length != 0 ||
                !uri.AbsolutePath.Equals("/" + UpdateTrust.Repository + "/releases/download/" +
                    Uri.EscapeDataString(tag) + "/" + name, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Ungültige Update-Quelle.");
            return uri;
        }
        static void Download(Uri source, Stream destination, long maximum, Action<long> progress)
        {
            Uri current = source;
            for (int redirects = 0; redirects < 6; ++redirects)
            {
                if (!AllowedHost(current)) throw new InvalidDataException("Unsichere Update-Weiterleitung.");
                var request = (HttpWebRequest)WebRequest.Create(current);
                request.UserAgent = "AzerothSurvivors-CoopMod-Updater/1";
                request.Accept = "application/vnd.github+json";
                request.Timeout = 12000; request.ReadWriteTimeout = 12000; request.AllowAutoRedirect = false;
                request.AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate;
                using (var response = (HttpWebResponse)request.GetResponse())
                {
                    if ((int)response.StatusCode >= 300 && (int)response.StatusCode < 400)
                    {
                        current = new Uri(current, response.Headers["Location"]); continue;
                    }
                    if (response.StatusCode != HttpStatusCode.OK || response.ContentLength > maximum)
                        throw new InvalidDataException("Das Update-Paket ist ungültig.");
                    using (var stream = response.GetResponseStream())
                    {
                        byte[] buffer = new byte[65536]; long total = 0; int count;
                        while ((count = stream.Read(buffer, 0, buffer.Length)) != 0)
                        {
                            total += count;
                            if (total > maximum) throw new InvalidDataException("Das Update ist unerwartet groß.");
                            destination.Write(buffer, 0, count); if (progress != null) progress(total);
                        }
                    }
                    return;
                }
            }
            throw new InvalidDataException("Zu viele Update-Weiterleitungen.");
        }
        static byte[] Bytes(Uri uri, int maximum)
        {
            using (var output = new MemoryStream()) { Download(uri, output, maximum, null); return output.ToArray(); }
        }
        internal static UpdateOffer Verify(byte[] bytes, byte[] signature, string tag, Uri setup, long advertisedSize, string githubDigest)
        {
            using (var rsa = new RSACryptoServiceProvider())
            {
                rsa.PersistKeyInCsp = false; rsa.ImportCspBlob(Convert.FromBase64String(UpdateTrust.PublicKey));
                if (signature.Length != rsa.KeySize / 8 || !rsa.VerifyData(bytes, CryptoConfig.MapNameToOID("SHA256"), signature))
                    throw new InvalidDataException("Die Signatur des Updates ist ungültig. Die vorhandene Mod bleibt erhalten.");
            }
            var data = Json.Parse(new UTF8Encoding(false, true).GetString(bytes));
            if (Convert.ToInt32(data["schema"]) != 1 || Json.Text(data, "channel") != "preview" ||
                Json.Text(data, "asset") != SetupName) throw new InvalidDataException("Unbekanntes Update-Format.");
            string version = Json.Text(data, "version"), digest = Json.Text(data, "sha256");
            new ModVersion(version);
            if (tag != "v" + version || !Regex.IsMatch(digest, "^[0-9a-f]{64}$"))
                throw new InvalidDataException("Update-Version oder Prüfsumme stimmen nicht.");
            long size = Convert.ToInt64(data["size"]);
            if (size < 1 || size > MaximumSetup || size != advertisedSize ||
                (!string.IsNullOrEmpty(githubDigest) && githubDigest != "sha256:" + digest))
                throw new InvalidDataException("Die Paketdaten stimmen nicht überein.");
            return new UpdateOffer { Version = version, Sha256 = digest, Size = size, Setup = setup };
        }
        internal UpdateOffer Check()
        {
            // Explicit HTTPS and signed manifests are used even on older Windows
            // .NET installations whose default TLS configuration predates TLS 1.2.
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            var endpoint = new Uri("https://api.github.com/repos/" + UpdateTrust.Repository + "/releases?per_page=10");
            string json = new UTF8Encoding(false, true).GetString(Bytes(endpoint, 2 * 1024 * 1024));
            var releases = new System.Web.Script.Serialization.JavaScriptSerializer().Deserialize<object[]>(json);
            var current = new ModVersion(Current); UpdateOffer chosen = null;
            foreach (Dictionary<string, object> release in releases)
            {
                if (Json.Flag(release, "draft")) continue;
                string tag = Json.Text(release, "tag_name");
                ModVersion candidate;
                try { candidate = new ModVersion(tag.StartsWith("v") ? tag.Substring(1) : ""); } catch { continue; }
                if (candidate.CompareTo(current) <= 0 || (chosen != null && candidate.CompareTo(new ModVersion(chosen.Version)) <= 0)) continue;
                var assets = ((IEnumerable)release["assets"]).Cast<Dictionary<string, object>>().ToList();
                var manifest = assets.Where(a => Json.Text(a, "name") == "update.json").ToArray();
                var signature = assets.Where(a => Json.Text(a, "name") == "update.json.sig").ToArray();
                var setup = assets.Where(a => Json.Text(a, "name") == SetupName).ToArray();
                if (manifest.Length != 1 || signature.Length != 1 || setup.Length != 1) continue;
                chosen = Verify(Bytes(AssetUri(Json.Text(manifest[0], "browser_download_url"), tag, "update.json"), 65536),
                    Bytes(AssetUri(Json.Text(signature[0], "browser_download_url"), tag, "update.json.sig"), 1024),
                    tag, AssetUri(Json.Text(setup[0], "browser_download_url"), tag, SetupName),
                    Convert.ToInt64(setup[0]["size"]), Json.Text(setup[0], "digest"));
            }
            return chosen;
        }
        internal static void VerifyFile(string path, UpdateOffer offer)
        {
            if (new FileInfo(path).Length != offer.Size || Json.Hash(path) != offer.Sha256)
                throw new InvalidDataException("Das heruntergeladene Update ist beschädigt. Es wird nicht ausgeführt.");
        }
        internal string Stage(UpdateOffer offer, Action<long> progress)
        {
            if (new ModVersion(offer.Version).CompareTo(new ModVersion(Current)) <= 0)
                throw new InvalidDataException("Ein älteres Update wird nicht installiert.");
            string folder = Path.Combine(backend.Paths.Data, "updates", offer.Version + "-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            string pending = Path.Combine(folder, "download.part"), final = Path.Combine(folder, SetupName);
            try
            {
                using (var output = new FileStream(pending, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    Download(offer.Setup, output, offer.Size, progress);
                VerifyFile(pending, offer); File.Move(pending, final); return final;
            }
            finally { if (File.Exists(pending)) File.Delete(pending); }
        }
        internal static bool GameRunning(Locations paths)
        {
            foreach (var process in Process.GetProcessesByName("AzerothSurvivors"))
            {
                using (process)
                {
                    try
                    {
                        if (process.Modules.Cast<ProcessModule>().Any(m => Program.InstalledImage(m.FileName, paths))) return true;
                    }
                    catch { return true; } // If a game cannot be inspected, defer installation.
                }
            }
            return false;
        }
        internal void HandOff(string setup, UpdateOffer offer, int parent, bool open = true)
        {
            VerifyFile(setup, offer);
            if (GameRunning(backend.Paths)) throw new IOException("Beende zuerst dein Mod-Spiel. Das Update wartet auf dich.");
            string portable = backend.PortableGameRoot;
            Process.Start(new ProcessStartInfo(setup, "/update " + parent + " /quiet" + (open ? "" : " /no-open") +
                (portable == null ? "" : " /portable " + Backend.Quote(portable))) { UseShellExecute = false, CreateNoWindow = true });
        }
    }
}
