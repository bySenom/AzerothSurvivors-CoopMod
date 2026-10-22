using System;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace AzerothCoop.Desktop
{
    internal sealed class StartupTarget
    {
        internal string Root, Launcher;
        internal bool Redirect;
        internal string Action { get { return Root == null ? "repair" : Redirect ? "redirect" : "launch"; } }
    }
    internal static class Startup
    {
        internal const string LauncherName = "Azeroth Survivors Koop.exe";
        internal const string Releases = "https://github.com/" + UpdateTrust.Repository + "/releases";
        static bool Same(string left, string right)
        {
            return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
        }
        internal static string LocalRoot(string image)
        {
            try { string portable = Portable.RuntimeForImage(image); if (portable != null) return portable; }
            catch (Exception ex) { if (ex is OutOfMemoryException) throw; }
            string directory = Path.GetDirectoryName(Path.GetFullPath(image));
            foreach (string root in new[] { directory, string.Equals(Path.GetFileName(directory), "bin", StringComparison.OrdinalIgnoreCase) ?
                Path.GetDirectoryName(directory) : null })
            {
                if (root == null) continue;
                try { Installer.VerifyPackage(root); return root; }
                catch (Exception ex)
                {
                    if (ex is OutOfMemoryException) throw;
                    // A lone EXE, interrupted install or malformed metadata all
                    // lead to recovery before the launcher reads its metadata.
                }
            }
            return null;
        }
        internal static StartupTarget Resolve(string image, Locations paths)
        {
            string root = LocalRoot(image);
            if (root != null) return new StartupTarget { Root = root, Launcher = image };
            try
            {
                new Installer(paths).ValidateOwnedApp();
                string stateFile = Path.Combine(paths.App, "installation.json");
                if (!File.Exists(stateFile)) return new StartupTarget();
                var state = Json.Read(stateFile);
                string launcher = Json.Text(state, "launcher");
                string versions = Path.GetFullPath(Path.Combine(paths.App, "versions")) + "\\";
                if (!Path.IsPathRooted(launcher) || !File.Exists(launcher) || Same(launcher, image) ||
                    !Path.GetFullPath(launcher).StartsWith(versions, StringComparison.OrdinalIgnoreCase)) return new StartupTarget();
                root = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(launcher)));
                if (!Same(launcher, Path.Combine(root, "bin", LauncherName))) return new StartupTarget();
                Installer.NoReparse(root);
                Installer.VerifyPackage(root);
                if (Json.Text(state, "version") != Json.Text(Json.Read(Path.Combine(root, "mod.json")), "version")) return new StartupTarget();
                return new StartupTarget { Root = root, Launcher = launcher, Redirect = true };
            }
            catch (Exception ex) { if (ex is OutOfMemoryException) throw; return new StartupTarget(); }
        }
        internal static void Open(StartupTarget target)
        {
            Process.Start(new ProcessStartInfo(target.Launcher) {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetDirectoryName(target.Launcher) });
        }
        internal static string InstanceSuffix(string root)
        {
            string portable = root == null ? null : Portable.GameDirectory(root);
            if (portable == null) return "";
            using (var bytes = new MemoryStream(Encoding.UTF8.GetBytes(portable.ToUpperInvariant()))) return "-" + Json.Hash(bytes).Substring(0, 24);
        }
    }
}
