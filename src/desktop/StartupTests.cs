using System;
using System.Diagnostics;
using System.IO;

namespace AzerothCoop.Desktop
{
    internal static class StartupTests
    {
        static void Require(bool value, string message) { if (!value) throw new Exception(message); }
        static string Probe(string image, Locations paths, string output)
        {
            var info = new ProcessStartInfo(image, "/startup-probe " + Backend.Quote(output) +
                " /data " + Backend.Quote(paths.Data) + " /quiet") { UseShellExecute = false, CreateNoWindow = true };
            using (var child = Process.Start(info))
            {
                if (!child.WaitForExit(15000)) { child.Kill(); throw new Exception("relocated launcher probe timed out"); }
                Require(child.ExitCode == 0, "relocated launcher process failed");
            }
            return Json.Text(Json.Read(output), "action");
        }
        internal static void Run(string folder, string launcher, Locations paths, string output)
        {
            string root = Path.GetDirectoryName(Path.GetDirectoryName(launcher));
            Require(Startup.Resolve(launcher, paths).Action == "launch", "complete installed layout not recognized");
            Require(Startup.LocalRoot(launcher) == root, "installed root resolved incorrectly");
            string game = Path.Combine(folder, "Game files ä"); Directory.CreateDirectory(game);
            string loose = Path.Combine(game, Startup.LauncherName); File.Copy(launcher, loose);
            string sentinel = Path.Combine(game, "game-data.txt"); File.WriteAllText(sentinel, "untouched-game-files");
            // Reproduce the screenshot: a bare launcher beside game files, with
            // no metadata in either its own directory or the parent directory.
            var empty = new Locations(Path.Combine(folder, "Uninstalled"), Path.Combine(folder, "unused-desktop"), Path.Combine(folder, "unused-menu"));
            Require(Startup.Resolve(loose, empty).Action == "repair", "bare launcher without install not recoverable");
            Require(Probe(loose, empty, Path.Combine(output, "startup-no-install.json")) == "repair", "bare launcher entry point crashed instead of recovering");
            var redirected = Startup.Resolve(loose, paths);
            Require(redirected.Action == "redirect" && redirected.Launcher == launcher && redirected.Root == root,
                "bare launcher did not find complete installed app");
            Require(Probe(loose, paths, Path.Combine(output, "startup-redirect.json")) == "redirect", "bare launcher process failed to find installation");
            Require(Probe(launcher, paths, Path.Combine(output, "startup-installed.json")) == "launch", "installed launcher process failed to resolve local runtime");
            File.WriteAllText(Path.Combine(game, "mod.json"), "{ broken metadata");
            Require(Startup.Resolve(loose, empty).Action == "repair", "malformed copied metadata crashed recovery");
            Require(Startup.Resolve(loose, paths).Action == "redirect", "malformed local metadata prevented installed fallback");
            string stateFile = Path.Combine(paths.App, "installation.json"), state = File.ReadAllText(stateFile);
            try
            {
                Json.Write(stateFile, new { version = Json.Text(Json.Read(Path.Combine(root, "mod.json")), "version"), launcher = loose });
                Require(Startup.Resolve(loose, paths).Action == "repair", "self-redirect accepted");
                Json.Write(stateFile, new { version = "0.0.0", launcher = launcher });
                Require(Startup.Resolve(loose, paths).Action == "repair", "mismatched installation version accepted");
                string external = Path.Combine(game, "unrelated.exe"); File.Copy(launcher, external);
                Json.Write(stateFile, new { version = "0.0.0", launcher = external });
                Require(Startup.Resolve(loose, paths).Action == "repair", "external installation target accepted");
                File.WriteAllText(stateFile, "broken state");
                Require(Startup.Resolve(loose, paths).Action == "repair", "malformed installation state crashed recovery");
            }
            finally { File.WriteAllText(stateFile, state); }
            string script = Path.Combine(root, "Launch-CoopMod.ps1");
            File.Move(script, script + ".test-backup");
            try { Require(Startup.Resolve(loose, paths).Action == "repair", "incomplete installed runtime accepted"); }
            finally { File.Move(script + ".test-backup", script); }
            string module = Path.Combine(root, "bin", "azs_lobby.dll"); byte[] original = File.ReadAllBytes(module);
            try
            {
                var corrupt = (byte[])original.Clone(); corrupt[corrupt.Length - 1] ^= 1; File.WriteAllBytes(module, corrupt);
                Require(Startup.Resolve(loose, paths).Action == "repair", "corrupt installed runtime accepted");
            }
            finally { File.WriteAllBytes(module, original); }
            Require(Startup.Resolve(loose, paths).Action == "redirect", "repaired runtime not recognized");
            Require(File.ReadAllText(sentinel) == "untouched-game-files", "startup detection modified game files");
            Json.Write(Path.Combine(output, "startup-tests.json"), new { passed = true, installedLayout = true,
                relocatedExecutableProcess = true, missingInstallRecovery = true, installedFallback = true,
                malformedMetadataRecovery = true, invalidTargetRejection = true, incompleteAndCorruptPackageRejection = true,
                gameFilesPreserved = true, interactiveClickAcceptance = false });
        }
    }
}
