using System;
using System.IO;

namespace AzerothCoop.Desktop
{
    internal static class Portable
    {
        internal const string Folder = "AzerothCoopMod";
        internal const string Ownership = "AzerothSurvivorsCoopMod.Portable.v1";
        internal const string ArchiveName = "AzerothSurvivors-Koop-Mod.zip";
        internal static string ModDirectory(string game) { return Path.Combine(Path.GetFullPath(game), Folder); }
        internal static void Validate(string game, bool existing)
        {
            game = Path.GetFullPath(game); Installer.NoReparse(game);
            if (!File.Exists(Path.Combine(game, "AzerothSurvivors.exe")) || !Directory.Exists(Path.Combine(game, "Data")))
                throw new IOException("Entpacke die Mod in den Spielordner mit AzerothSurvivors.exe und Data.");
            string directory = ModDirectory(game); Installer.NoReparse(directory);
            if (existing || Directory.Exists(directory))
            {
                string marker = Path.Combine(directory, ".azs-portable-owned");
                if (!File.Exists(marker) || File.ReadAllText(marker) != Ownership)
                    throw new IOException("Der Mod-Ordner ist unvollständig. Bitte das vollständige Mod-ZIP erneut entpacken.");
            }
        }
        internal static string RuntimeForImage(string image)
        {
            string game = Path.GetDirectoryName(Path.GetFullPath(image));
            if (!string.Equals(Path.GetFileName(image), Startup.LauncherName, StringComparison.OrdinalIgnoreCase)) return null;
            Validate(game, true);
            string directory = ModDirectory(game);
            var state = Json.Read(Path.Combine(directory, "portable.json"));
            string relative = Json.Text(state, "runtime");
            if (Path.IsPathRooted(relative)) throw new IOException("Ungültiger Mod-Pfad.");
            string root = Path.GetFullPath(Path.Combine(directory, relative));
            if (!root.StartsWith(Path.Combine(directory, "versions") + "\\", StringComparison.OrdinalIgnoreCase))
                throw new IOException("Ungültiger Mod-Pfad.");
            Installer.NoReparse(root); Installer.VerifyPackage(root);
            if (Json.Text(state, "version") != Json.Text(Json.Read(Path.Combine(root, "mod.json")), "version"))
                throw new IOException("Das Mod-Paket ist unvollständig.");
            return root;
        }
        internal static string GameDirectory(string runtime)
        {
            string versions = Path.GetDirectoryName(Path.GetFullPath(runtime));
            string directory = Path.GetDirectoryName(versions);
            if (!string.Equals(Path.GetFileName(versions), "versions", StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(Path.GetFileName(directory), Folder, StringComparison.OrdinalIgnoreCase)) return null;
            string game = Path.GetDirectoryName(directory);
            try { Validate(game, true); return game; } catch { return null; }
        }
        internal static string Install(string game, bool existing)
        {
            game = Path.GetFullPath(game); Validate(game, existing);
            string directory = ModDirectory(game);
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, ".azs-portable-owned"), Ownership);
            string stage = Path.Combine(directory, ".install-" + Guid.NewGuid().ToString("N"));
            string temporary = Path.Combine(game, ".azs-launcher-" + Guid.NewGuid().ToString("N") + ".exe");
            string launcher = Path.Combine(game, Startup.LauncherName), previousLauncher = null;
            try
            {
                Installer.ExtractRuntime(stage);
                string version = Json.Text(Json.Read(Path.Combine(stage, "mod.json")), "version");
                string name = version + "-" + Guid.NewGuid().ToString("N").Substring(0, 8);
                string destination = Path.Combine(directory, "versions", name);
                Directory.CreateDirectory(Path.GetDirectoryName(destination)); Installer.NoReparse(Path.GetDirectoryName(destination));
                Directory.Move(stage, destination);
                File.Copy(Path.Combine(destination, "bin", Startup.LauncherName), temporary);
                if (File.Exists(launcher))
                {
                    string retired = Path.Combine(directory, "retired"); Directory.CreateDirectory(retired); Installer.NoReparse(retired);
                    previousLauncher = Path.Combine(retired, "launcher-" + Guid.NewGuid().ToString("N") + ".exe");
                    File.Move(launcher, previousLauncher);
                }
                File.Move(temporary, launcher);
                Json.Write(Path.Combine(directory, "portable.json"), new { schema = 1, version = version, runtime = "versions/" + name });
                return launcher;
            }
            catch
            {
                if (!File.Exists(launcher) && previousLauncher != null && File.Exists(previousLauncher)) File.Move(previousLauncher, launcher);
                throw;
            }
            finally
            {
                if (Directory.Exists(stage)) Installer.DeleteTreeWithin(stage, directory);
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }
    }
}
