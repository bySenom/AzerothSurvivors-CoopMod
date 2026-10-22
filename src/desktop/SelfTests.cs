using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace AzerothCoop.Desktop
{
    internal static class SelfTests
    {
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        static void Require(bool condition, string message) { if (!condition) throw new Exception(message); }
        static void Capture(ModWindow window, string file)
        {
            var foreground = GetForegroundWindow();
            using (window)
            {
                window.Offscreen = true; window.ShowInTaskbar = false; window.StartPosition = FormStartPosition.Manual;
                window.Location = new Point(-30000, -30000); window.Show();
                using (var image = new Bitmap(window.Width, window.Height))
                {
                    window.DrawToBitmap(image, new Rectangle(Point.Empty, image.Size)); image.Save(file, ImageFormat.Png);
                }
                window.Close();
            }
            Require(GetForegroundWindow() == foreground, "offscreen test changed foreground window");
        }
        public static int Run(string output, string supported, string unsupported)
        {
            if (string.IsNullOrWhiteSpace(output)) throw new Exception("self-test requires an output directory");
            output = Path.GetFullPath(output); Directory.CreateDirectory(output);
            var folder = Path.Combine(output, "installation-" + Guid.NewGuid().ToString("N"));
            var paths = new Locations(Path.Combine(folder, "Data ä"), Path.Combine(folder, "Desktop"), Path.Combine(folder, "Menu"));
            var result = new Dictionary<string, object>();
            try
            {
                Require(Installer.HasPayload, "setup embedded payload missing");
                Require(Backend.Quote(@"C:\Spiel ä\") == "\"C:\\Spiel ä\\\\\"", "trailing slash quoting failed");
                var installer = new Installer(paths);
                Json.Write(paths.Settings, new { schema = 1, gamePath = supported ?? @"C:\Games\AzerothSurvivors.exe" });
                Directory.CreateDirectory(paths.Profile);
                string save = Path.Combine(paths.Profile, "characters.txt"); File.WriteAllText(save, "preserve-me");
                string before = Json.Hash(save);
                string launcher = installer.Install(true, false, supported);
                Require(File.Exists(launcher) && File.Exists(paths.DesktopLink) && File.Exists(paths.MenuLink), "install or shortcuts missing");
                string root = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(launcher), ".."));
                var backend = new Backend(root, paths);
                Require(!File.Exists(Path.Combine(root, "Start-CompatibleBuild.cmd")), "machine-specific development shortcut shipped");
                Require(!System.Text.RegularExpressions.Regex.IsMatch(File.ReadAllText(Path.Combine(root, "mod.json")), @"(?i)[a-z]:\\\\users\\\\"), "machine-specific path shipped");
                UpdateTests.Run(output);
                if (!string.IsNullOrEmpty(supported))
                {
                    var check = backend.Probe(supported);
                    Require(check.Compatible, "installed backend rejected supported build: " + check.Reason + " (" + check.Game + ")");
                    Require(Json.Text(Json.Read(paths.Settings), "gamePath") == Path.GetFullPath(supported), "game selection not persisted");
                }
                if (!string.IsNullOrEmpty(unsupported))
                    Require(backend.Probe(unsupported).Reason == "unsupported_game_build", "unsupported build did not fail closed");
                if (!string.IsNullOrEmpty(supported))
                {
                    // Exercise the installed backend, without clicking controls
                    // or sending game input. Only this returned child is stopped.
                    var smoke = new Backend(root, paths, "NativeSmoke").Run("Play", supported, true);
                    using (var child = Process.GetProcessById(Convert.ToInt32(smoke["pid"])))
                    {
                        Require(string.Equals(child.MainModule.FileName, Path.GetFullPath(supported), StringComparison.OrdinalIgnoreCase), "unexpected smoke child image");
                        try
                        {
                            var info = new ProcessStartInfo(Path.Combine(root, "bin", "AzerothCoop.exe"),
                                "lobby " + child.Id + " status") { UseShellExecute = false, CreateNoWindow = true,
                                RedirectStandardOutput = true, RedirectStandardError = true };
                            using (var query = Process.Start(info))
                            {
                                string response = query.StandardOutput.ReadToEnd(); query.WaitForExit();
                                var lobby = Json.Parse(response);
                                Require(query.ExitCode == 0 && Json.Flag(lobby, "hooks_installed") && !Json.Flag(lobby, "visible"), "installed native lobby not initialized");
                            }
                        }
                        finally { if (!child.HasExited) { child.Kill(); child.WaitForExit(5000); } }
                    }
                    result["installedNativeGameStartup"] = true;
                }
                Require(new Check { Reason = "unsupported_game_build" }.Heading.Contains("noch nicht unterstützt"), "unsupported build copy missing");
                Require(new Check { Reason = "native_package_incomplete" }.Heading.Contains("erneut installieren"), "repair guidance missing");
                Require(!backend.Probe(Path.Combine(folder, "missing.exe")).Compatible, "missing game accepted");
                var ready = new LaunchWindow(backend); ready.SetCheck(new Check { Compatible = true, Game = supported ?? @"C:\Spiele\AzerothSurvivors.exe" });
                Require(ready.PlayEnabled, "supported game start disabled"); Capture(ready, Path.Combine(output, "launcher-ready.png"));
                var missing = new LaunchWindow(backend); missing.SetCheck(new Check());
                Require(!missing.PlayEnabled, "missing game start enabled"); Capture(missing, Path.Combine(output, "launcher-select.png"));
                var blocked = new LaunchWindow(backend); blocked.SetCheck(new Check { Game = unsupported ?? @"C:\Spiele\AzerothSurvivors.exe", Reason = "unsupported_game_build" });
                Require(!blocked.PlayEnabled, "unsupported game start enabled"); Capture(blocked, Path.Combine(output, "launcher-unsupported.png"));
                Capture(new SetupWindow(installer), Path.Combine(output, "setup.png"));
                string second = installer.Install(true, false, supported);
                Require(second != launcher && File.Exists(launcher) && File.Exists(second), "upgrade did not retain immutable old install");
                Require(Json.Hash(save) == before, "reinstall modified character save");
                bool denied = false; try { installer.DeleteOwnedTree(paths.Data); } catch (IOException) { denied = true; }
                Require(denied && File.Exists(save), "delete boundary failed");
                installer.Unregister(false); installer.DeleteOwnedTree(paths.App);
                Require(!Directory.Exists(paths.App) && !File.Exists(paths.DesktopLink) && !File.Exists(paths.MenuLink), "uninstall left app or shortcuts");
                Require(File.Exists(save) && Json.Hash(save) == before && File.Exists(paths.Settings), "uninstall removed persistent data");
                result["passed"] = true; result["installAndShortcuts"] = true; result["reinstallPreservesOldAppAndSaves"] = true;
                result["uninstallPreservesProfilesAndSettings"] = true; result["outsideAppDeleteRejected"] = true;
                result["uiStatesAndForegroundPreservation"] = true; result["nativeGameplayAcceptance"] = false;
                result["installedBackendSupportedBuild"] = !string.IsNullOrEmpty(supported);
                result["installedBackendUpdatedBuildRejected"] = !string.IsNullOrEmpty(unsupported);
                result["updateValidation"] = true;
                Json.Write(Path.Combine(output, "result.json"), result); return 0;
            }
            catch (Exception ex)
            {
                result["passed"] = false; result["error"] = ex.ToString(); Json.Write(Path.Combine(output, "result.json"), result); return 1;
            }
        }
    }
}
