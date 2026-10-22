using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;

namespace AzerothCoop.Desktop
{
    internal static class UpdateTests
    {
        static void Require(bool value, string message) { if (!value) throw new Exception(message); }
        static void Reject(Action action, string message)
        {
            bool rejected = false; try { action(); } catch { rejected = true; } Require(rejected, message);
        }
        static byte[] Resource(string name)
        {
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("AzerothCoop.UpdateTest." + name))
            using (var result = new MemoryStream()) { stream.CopyTo(result); return result.ToArray(); }
        }
        internal static void Run(string output)
        {
            Require(new ModVersion("0.1.0-dev.10").CompareTo(new ModVersion("0.1.0-dev.9")) > 0, "numeric preview ordering failed");
            Require(new ModVersion("0.1.0").CompareTo(new ModVersion("0.1.0-dev.99")) > 0, "stable ordering failed");
            Require(new ModVersion("0.2.0").CompareTo(new ModVersion("0.1.99")) > 0, "minor ordering failed");
            foreach (string invalid in new[] { "1", "0.01.0", "0.1.0-dev.01", "../file", "0.1.0-" })
                Reject(() => new ModVersion(invalid), "invalid version accepted");
            byte[] manifest = Resource("json"), signature = Resource("sig");
            var uri = Updates.AssetUri("https://github.com/" + UpdateTrust.Repository + "/releases/download/v0.0.0-test.1/" + Updates.SetupName, "v0.0.0-test.1", Updates.SetupName);
            var offer = Updates.Verify(manifest, signature, "v0.0.0-test.1", uri, 4, "");
            Require(offer.Version == "0.0.0-test.1", "signed fixture rejected");
            byte[] corrupt = (byte[])manifest.Clone(); corrupt[0] ^= 1;
            Reject(() => Updates.Verify(corrupt, signature, "v0.0.0-test.1", uri, 4, ""), "modified manifest accepted");
            byte[] wrong = (byte[])signature.Clone(); wrong[0] ^= 1;
            Reject(() => Updates.Verify(manifest, wrong, "v0.0.0-test.1", uri, 4, ""), "wrong signature accepted");
            Reject(() => Updates.Verify(manifest, new byte[0], "v0.0.0-test.1", uri, 4, ""), "unsigned update accepted");
            Reject(() => Updates.Verify(manifest, signature, "v0.1.0", uri, 4, ""), "release tag mismatch accepted");
            Reject(() => Updates.Verify(manifest, signature, "v0.0.0-test.1", uri, 5, ""), "size mismatch accepted");
            Reject(() => Updates.Verify(manifest, signature, "v0.0.0-test.1", uri, 4, "sha256:" + new string('0', 64)), "GitHub digest mismatch accepted");
            foreach (string bad in new[] { "http://github.com/file", "https://example.com/file", "file:///C:/file", "https://github.com/other/repo/releases/download/v0.0.0-test.1/setup.exe" })
                Reject(() => Updates.AssetUri(bad, "v0.0.0-test.1", Updates.SetupName), "foreign update source accepted");
            string file = Path.Combine(output, "checksum-test.bin");
            File.WriteAllBytes(file, Encoding.ASCII.GetBytes("test")); Updates.VerifyFile(file, offer);
            File.WriteAllBytes(file, Encoding.ASCII.GetBytes("tEst")); Reject(() => Updates.VerifyFile(file, offer), "corrupt executable accepted");
            File.Delete(file);
            string root = Path.Combine(output, "update-unit-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
            Json.Write(Path.Combine(root, "mod.json"), new { version = "0.1.0-dev.5" });
            var paths = new Locations(Path.Combine(root, "data"), Path.Combine(root, "desktop"), Path.Combine(root, "menu"));
            var updates = new Updates(new Backend(root, paths));
            Reject(() => updates.Stage(offer, null), "downgrade accepted");
            Require(updates.Automatic, "automatic updates not enabled by default"); updates.Automatic = false;
            Require(!new Updates(new Backend(root, paths)).Automatic, "update preference not saved");
            Json.Write(Path.Combine(output, "update-tests.json"), new { passed = true, signature = true, tamperRejection = true,
                versionOrdering = true, downgradeRejection = true, foreignSourceRejection = true, checksum = true, preferences = true });
        }
        internal static int Live(string output, bool apply, Locations paths)
        {
            if (string.IsNullOrWhiteSpace(output)) throw new ArgumentException("An output JSON path is required.");
            var updates = new Updates(new Backend(Program.RuntimeRoot, paths));
            var offer = updates.Check();
            var result = new Dictionary<string, object> { { "installedVersion", updates.Current }, { "publicGitHubRequest", true },
                { "offeredVersion", offer == null ? "" : offer.Version }, { "signatureVerified", offer != null } };
            if (apply && offer != null)
            {
                string setup = updates.Stage(offer, null);
                result["downloadHashVerified"] = true; result["gameRunning"] = Updates.GameRunning(paths);
                updates.HandOff(setup, offer, System.Diagnostics.Process.GetCurrentProcess().Id, false);
                result["installerHandOff"] = true;
            }
            Json.Write(Path.GetFullPath(output), result); return 0;
        }
    }
}
