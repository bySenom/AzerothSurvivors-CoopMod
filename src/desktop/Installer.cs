using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace AzerothCoop.Desktop
{
    internal sealed class Installer
    {
        const string Ownership = "AzerothSurvivorsCoopMod.App.v1";
        const string RegistryPath = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\AzerothSurvivorsCoopMod";
        public readonly Locations Paths;
        public Installer(Locations paths) { Paths = paths; }
        public static Stream Payload()
        {
            var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("AzerothCoop.Runtime.zip");
            if (stream == null) throw new IOException("Das Setup-Paket fehlt.");
            return stream;
        }
        public static bool HasPayload { get { return Assembly.GetExecutingAssembly().GetManifestResourceNames().Contains("AzerothCoop.Runtime.zip"); } }
        static bool Under(string path, string root)
        {
            return Path.GetFullPath(path).StartsWith(Path.GetFullPath(root).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase);
        }
        internal static void NoReparse(string directory)
        {
            var current = new DirectoryInfo(directory);
            while (current != null)
            {
                if (current.Exists && (current.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Der Mod-Installationsordner darf keine Verzeichnisverknüpfung sein.");
                current = current.Parent;
            }
        }
        public void ValidateOwnedApp()
        {
            if (!string.Equals(Path.GetFullPath(Paths.App), Path.Combine(Path.GetFullPath(Paths.Data), "app"), StringComparison.OrdinalIgnoreCase))
                throw new IOException("Ungültiger Mod-Installationsordner.");
            NoReparse(Paths.App);
            if (Directory.Exists(Paths.App))
            {
                var marker = Path.Combine(Paths.App, ".azs-mod-owned");
                if (!File.Exists(marker) || File.ReadAllText(marker) != Ownership)
                    throw new IOException("Der Zielordner gehört nicht zu dieser Mod-Installation.");
            }
        }
        internal static void VerifyPackage(string root)
        {
            var metadata = Json.Read(Path.Combine(root, "mod.json"));
            var manifest = Json.Read(Path.Combine(root, "package-manifest.json"));
            if (Json.Text(metadata, "id") != "azeroth-survivors-coop") throw new IOException("Das Mod-Paket ist ungültig.");
            new ModVersion(Json.Text(metadata, "version"));
            if (Json.Text(metadata, "version") != Json.Text(manifest, "modVersion")) throw new IOException("Die Mod-Version im Setup stimmt nicht überein.");
            var records = ((IEnumerable)manifest["files"]).Cast<Dictionary<string, object>>().ToList();
            var expected = new[] { "AzerothCoop.exe", "azs_coop.dll", "azs_transport.dll", "azs_gameplay.dll", "azs_lobby.dll", Startup.LauncherName };
            var listed = ((IEnumerable)metadata["nativeFiles"]).Cast<string>().Concat(((IEnumerable)metadata["desktopFiles"]).Cast<string>()).ToArray();
            if (listed.Length != expected.Length || !listed.OrderBy(n => n).SequenceEqual(expected.OrderBy(n => n)))
                throw new IOException("Das Mod-Paket ist unvollständig.");
            foreach (var group in new[] { "nativeFiles", "desktopFiles" })
            {
                foreach (var name in ((IEnumerable)metadata[group]).Cast<string>())
                {
                    var matches = records.Where(e => Json.Text(e, "name") == name).ToArray();
                    var file = Path.GetFullPath(Path.Combine(root, "bin", name));
                    if (!Under(file, Path.Combine(root, "bin")) || matches.Length != 1 ||
                        !File.Exists(file) || Json.Hash(file) != Json.Text(matches[0], "sha256"))
                        throw new IOException("Eine Mod-Datei im Setup ist beschädigt: " + name);
                }
            }
            if (!File.Exists(Path.Combine(root, "Launch-CoopMod.ps1"))) throw new IOException("Das Setup ist unvollständig.");
            var adapters = ((IEnumerable)metadata["compatibleBuilds"]).Cast<string>().ToArray();
            if (adapters.Length == 0) throw new IOException("Das Setup ist unvollständig.");
            foreach (string adapter in adapters)
            {
                string file = Path.GetFullPath(Path.Combine(root, adapter));
                if (!Under(file, Path.Combine(root, "compat")) || !File.Exists(file)) throw new IOException("Das Setup ist unvollständig.");
                Json.Read(file);
            }
        }
        internal static void ExtractRuntime(string stage)
        {
            Directory.CreateDirectory(stage);
            using (var stream = Payload())
            using (var archive = new ZipArchive(stream, ZipArchiveMode.Read))
            {
                foreach (var entry in archive.Entries)
                {
                    string relative = entry.FullName.Replace('/', '\\');
                    string target = Path.GetFullPath(Path.Combine(stage, relative));
                    if (Path.IsPathRooted(relative) || !Under(target, stage) || entry.Length > 32 * 1024 * 1024)
                        throw new IOException("Das Setup enthält einen ungültigen Dateipfad.");
                    if (entry.FullName.EndsWith("/")) { Directory.CreateDirectory(target); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    entry.ExtractToFile(target, false);
                }
            }
            VerifyPackage(stage);
        }
        public string Install(bool desktopShortcut, bool register, string game)
        {
            ValidateOwnedApp();
            Directory.CreateDirectory(Paths.App);
            File.WriteAllText(Path.Combine(Paths.App, ".azs-mod-owned"), Ownership);
            string stage = Path.Combine(Paths.App, ".install-" + Guid.NewGuid().ToString("N"));
            string destination = null;
            try
            {
                ExtractRuntime(stage);
                string version = Json.Text(Json.Read(Path.Combine(stage, "mod.json")), "version");
                if (version.Length == 0 || version.Any(c => !char.IsLetterOrDigit(c) && c != '.' && c != '-'))
                    throw new IOException("Ungültige Mod-Version.");
                destination = Path.Combine(Paths.App, "versions", version + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
                Directory.CreateDirectory(Path.GetDirectoryName(destination));
                Directory.Move(stage, destination);
                string launcher = Path.Combine(destination, "bin", "Azeroth Survivors Koop.exe");
                CreateShortcut(Paths.MenuLink, launcher);
                if (desktopShortcut) CreateShortcut(Paths.DesktopLink, launcher);
                else RemoveOwnedShortcut(Paths.DesktopLink);
                if (register)
                {
                    using (var key = Registry.CurrentUser.CreateSubKey(RegistryPath))
                    {
                        key.SetValue("DisplayName", "Azeroth Survivors Koop-Mod");
                        key.SetValue("DisplayVersion", version);
                        key.SetValue("Publisher", "Azeroth Survivors Koop-Mod (Community)");
                        key.SetValue("InstallLocation", destination);
                        key.SetValue("DisplayIcon", launcher);
                        key.SetValue("UninstallString", Backend.Quote(launcher) + " /uninstall");
                        key.SetValue("NoModify", 1); key.SetValue("NoRepair", 1);
                    }
                }
                Json.Write(Path.Combine(Paths.App, "installation.json"), new { schema = 1, version = version,
                    launcher = launcher, desktopShortcut = desktopShortcut, installedAt = DateTimeOffset.Now.ToString("o") });
                if (!string.IsNullOrWhiteSpace(game))
                {
                    var backend = new Backend(destination, Paths);
                    var check = backend.Probe(game);
                    if (check.Compatible) backend.Save(check.Game);
                }
                return launcher;
            }
            finally
            {
                if (Directory.Exists(stage)) DeleteOwnedTree(stage);
            }
        }
        void CreateShortcut(string path, string launcher)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            object shell = null, shortcut = null;
            try
            {
                shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
                dynamic sh = shell; shortcut = sh.CreateShortcut(path); dynamic link = shortcut;
                if (File.Exists(path) && !string.IsNullOrWhiteSpace((string)link.TargetPath) && !Under((string)link.TargetPath, Paths.App))
                    throw new IOException("Eine andere Verknüpfung mit dem Namen „Azeroth Survivors Koop“ existiert bereits.");
                link.TargetPath = launcher; link.Arguments = ""; link.WorkingDirectory = Path.GetDirectoryName(launcher);
                link.Description = "Azeroth Survivors mit Solo- und Koop-Auswahl starten";
                link.IconLocation = launcher + ",0"; link.Save();
            }
            finally
            {
                if (shortcut != null) Marshal.FinalReleaseComObject(shortcut);
                if (shell != null) Marshal.FinalReleaseComObject(shell);
            }
        }
        void RemoveOwnedShortcut(string path)
        {
            if (!File.Exists(path)) return;
            object shell = null, shortcut = null;
            try
            {
                shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
                dynamic sh = shell; shortcut = sh.CreateShortcut(path); dynamic link = shortcut;
                if (Under((string)link.TargetPath, Paths.App)) File.Delete(path);
            }
            finally
            {
                if (shortcut != null) Marshal.FinalReleaseComObject(shortcut);
                if (shell != null) Marshal.FinalReleaseComObject(shell);
            }
        }
        public void Unregister(bool registry)
        {
            ValidateOwnedApp();
            RemoveOwnedShortcut(Paths.DesktopLink); RemoveOwnedShortcut(Paths.MenuLink);
            var folder = Path.GetDirectoryName(Paths.MenuLink);
            if (Directory.Exists(folder) && !Directory.EnumerateFileSystemEntries(folder).Any()) Directory.Delete(folder);
            if (registry) Registry.CurrentUser.DeleteSubKeyTree(RegistryPath, false);
        }
        public void DeleteOwnedTree(string target)
        {
            ValidateOwnedApp();
            string full = Path.GetFullPath(target);
            if (!string.Equals(full, Paths.App, StringComparison.OrdinalIgnoreCase) && !Under(full, Paths.App))
                throw new IOException("Löschen außerhalb des Mod-App-Ordners wurde verhindert.");
            DeleteTreeWithin(full, Paths.App);
        }
        internal static void DeleteTreeWithin(string target, string boundary)
        {
            string full = Path.GetFullPath(target);
            if (!string.Equals(full, Path.GetFullPath(boundary), StringComparison.OrdinalIgnoreCase) && !Under(full, boundary))
                throw new IOException("Löschen außerhalb des Mod-Ordners wurde verhindert.");
            if (!Directory.Exists(full)) return;
            // Validate every directory before any recursive deletion. Profiles
            // are siblings of App and are deliberately outside this boundary.
            var pending = new Stack<string>(); pending.Push(full);
            while (pending.Count > 0)
            {
                var folder = pending.Pop(); NoReparse(folder);
                foreach (var file in Directory.GetFiles(folder))
                    if ((File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0)
                        throw new IOException("Eine Dateiverknüpfung verhindert das Entfernen.");
                foreach (var child in Directory.GetDirectories(folder)) pending.Push(child);
            }
            Directory.Delete(full, true);
        }
    }
}
