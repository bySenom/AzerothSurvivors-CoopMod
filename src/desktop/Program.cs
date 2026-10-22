using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("Azeroth Survivors Koop")]
[assembly: AssemblyDescription("Starter und Installation der Community-Koop-Mod")]
[assembly: AssemblyProduct("Azeroth Survivors Koop-Mod")]
[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8")]

namespace AzerothCoop.Desktop
{
    internal static class Program
    {
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindow(string cls, string caption);
        [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int command);
        [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
        static string Option(string[] args, string key)
        {
            int index = Array.FindIndex(args, x => x.Equals(key, StringComparison.OrdinalIgnoreCase));
            return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
        }
        static bool Flag(string[] args, string key) { return args.Any(x => x.Equals(key, StringComparison.OrdinalIgnoreCase)); }
        internal static bool InstalledImage(string image, Locations paths)
        {
            if (image.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) image = @"\\" + image.Substring(8);
            else if (image.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase)) image = image.Substring(4);
            return image.StartsWith(paths.App + "\\", StringComparison.OrdinalIgnoreCase) ||
                image.StartsWith(Path.Combine(paths.Data, "module-cache") + "\\", StringComparison.OrdinalIgnoreCase);
        }
        internal static string RuntimeRoot
        {
            get { return Path.GetFullPath(Path.Combine(Path.GetDirectoryName(Application.ExecutablePath), "..")); }
        }
        [STAThread]
        static int Main(string[] args)
        {
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            try
            {
                var paths = Locations.User();
                if (Flag(args, "/update-probe"))
                    return UpdateTests.Live(Option(args, "/update-probe"), Flag(args, "/apply"), paths);
                if (Flag(args, "/update"))
                {
                    if (!Installer.HasPayload) throw new IOException("Ein Update benötigt das vollständige Setup.");
                    int parent;
                    if (!int.TryParse(Option(args, "/update"), out parent)) return 2;
                    try { using (var process = Process.GetProcessById(parent)) if (!process.WaitForExit(20000)) return 3; }
                    catch (ArgumentException) { }
                    if (Updates.GameRunning(paths)) throw new IOException("Das Update wartet, bis das Mod-Spiel beendet ist.");
                    bool desktopShortcut = File.Exists(paths.DesktopLink);
                    string stateFile = Path.Combine(paths.App, "installation.json");
                    if (File.Exists(stateFile))
                    {
                        var previous = Json.Read(stateFile);
                        if (previous.ContainsKey("desktopShortcut")) desktopShortcut = Json.Flag(previous, "desktopShortcut");
                    }
                    string launcher = new Installer(paths).Install(desktopShortcut, true, null);
                    Json.Write(Path.Combine(paths.Data, "updates", "last-result.json"), new { schema = 1,
                        installedVersion = Json.Text(Json.Read(Path.Combine(Path.GetDirectoryName(launcher), "..", "mod.json")), "version"),
                        success = true, installedAt = DateTimeOffset.Now.ToString("o") });
                    if (!Flag(args, "/no-open")) Process.Start(new ProcessStartInfo(launcher) { UseShellExecute = true });
                    return 0;
                }
                if (Flag(args, "/self-test")) return SelfTests.Run(Option(args, "/self-test"), Option(args, "/game"), Option(args, "/unsupported"));
                if (Flag(args, "/cleanup"))
                {
                    int parent;
                    if (!int.TryParse(Option(args, "/cleanup"), out parent)) return 2;
                    try { using (var process = Process.GetProcessById(parent)) if (!process.WaitForExit(20000)) return 3; }
                    catch (ArgumentException) { }
                    for (int attempt = 0; ; ++attempt)
                    {
                        try { new Installer(paths).DeleteOwnedTree(paths.App); return 0; }
                        catch (IOException) { if (attempt >= 20) throw; Thread.Sleep(250); }
                    }
                }
                if (Flag(args, "/uninstall"))
                {
                    var installer = new Installer(paths); installer.ValidateOwnedApp();
                    foreach (var process in Process.GetProcessesByName("Azeroth Survivors Koop"))
                    {
                        using (process)
                        {
                            if (process.Id == Process.GetCurrentProcess().Id) continue;
                            bool launcherOpen = false;
                            try { launcherOpen = InstalledImage(process.MainModule.FileName, paths); } catch { }
                            if (launcherOpen) { MessageBox.Show("Schließe zuerst den Mod-Starter und öffne die Deinstallation erneut.", "Koop-Mod entfernen"); return 1; }
                        }
                    }
                    foreach (var process in Process.GetProcessesByName("AzerothSurvivors"))
                    {
                        using (process)
                        {
                            bool usesMod = false;
                            try { usesMod = process.Modules.Cast<ProcessModule>().Any(m => InstalledImage(m.FileName, paths)); } catch { }
                            if (usesMod) { MessageBox.Show("Beende zuerst das laufende Mod-Spiel und öffne die Deinstallation erneut.", "Koop-Mod entfernen"); return 1; }
                        }
                    }
                    if (MessageBox.Show("Koop-Mod und Verknüpfungen entfernen?\nDeine Mod-Spielstände bleiben erhalten.",
                        "Koop-Mod entfernen", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return 0;
                    installer.Unregister(true);
                    string helper = Path.Combine(Path.GetTempPath(), "AzerothKoop-Remove-" + Guid.NewGuid().ToString("N") + ".exe");
                    File.Copy(Application.ExecutablePath, helper);
                    Process.Start(new ProcessStartInfo(helper, "/cleanup " + Process.GetCurrentProcess().Id) { UseShellExecute = false, CreateNoWindow = true });
                    return 0;
                }
                if (Flag(args, "/install"))
                {
                    string launcher = new Installer(paths).Install(!Flag(args, "/no-desktop"), true, Option(args, "/game"));
                    if (!Flag(args, "/quiet")) Process.Start(new ProcessStartInfo(launcher) { UseShellExecute = true });
                    return 0;
                }
                bool setup = Installer.HasPayload;
                string mutexName = @"Local\AzerothCoop-" + (setup ? "Setup-" : "Desktop-") + WindowsIdentity.GetCurrent().User.Value;
                bool created;
                using (var mutex = new Mutex(true, mutexName, out created))
                {
                    if (!created)
                    {
                        var window = FindWindow(null, setup ? "Azeroth Survivors Koop – Installation" : "Azeroth Survivors Koop");
                        if (window != IntPtr.Zero) { ShowWindow(window, 9); SetForegroundWindow(window); }
                        return 0;
                    }
                    try
                    {
                        Application.Run(setup ? (Form)new SetupWindow(new Installer(paths)) :
                            new LaunchWindow(new Backend(RuntimeRoot, paths)));
                    }
                    finally { mutex.ReleaseMutex(); }
                }
                return 0;
            }
            catch (Exception ex)
            {
                try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "AzerothKoop-Setup-Fehler.txt"), ex.ToString()); } catch { }
                if (!Flag(args, "/quiet")) MessageBox.Show(ex.Message, "Azeroth Survivors Koop", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return 1;
            }
        }
    }
}
