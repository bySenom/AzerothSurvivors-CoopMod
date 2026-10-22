using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace AzerothCoop.Desktop
{
    internal sealed class Locations
    {
        public readonly string Data, App, DesktopLink, MenuLink;
        public Locations(string data, string desktop, string menu)
        {
            Data = Path.GetFullPath(data);
            App = Path.Combine(Data, "app");
            DesktopLink = Path.Combine(desktop, "Azeroth Survivors Koop.lnk");
            MenuLink = Path.Combine(menu, "Azeroth Survivors Koop", "Azeroth Survivors Koop.lnk");
        }
        public static Locations User()
        {
            return new Locations(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "AzerothSurvivorsCoopMod"), Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                Environment.GetFolderPath(Environment.SpecialFolder.Programs));
        }
        public string Settings { get { return Path.Combine(Data, "launcher.json"); } }
        public string Log { get { return Path.Combine(Data, "logs", "desktop.log"); } }
        public string Profile { get { return Path.Combine(Data, "profiles", "Main"); } }
    }
    internal static class Json
    {
        public static Dictionary<string, object> Parse(string text)
        {
            var result = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(text.TrimStart('\uFEFF'));
            if (result == null) throw new ArgumentException("Empty JSON response");
            return result;
        }
        public static Dictionary<string, object> Read(string path)
        {
            return Parse(File.ReadAllText(path, Encoding.UTF8));
        }
        public static string Text(IDictionary<string, object> data, string key)
        {
            object value;
            return data != null && data.TryGetValue(key, out value) && value != null ? Convert.ToString(value) : "";
        }
        public static bool Flag(IDictionary<string, object> data, string key)
        {
            object value;
            return data != null && data.TryGetValue(key, out value) && value is bool && (bool)value;
        }
        public static void Write(string path, object value)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            File.WriteAllText(temporary, new JavaScriptSerializer().Serialize(value), new UTF8Encoding(false));
            if (File.Exists(path)) File.Replace(temporary, path, null);
            else File.Move(temporary, path);
        }
        public static string Hash(Stream stream)
        {
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
        }
        public static string Hash(string file)
        {
            using (var stream = File.OpenRead(file)) return Hash(stream);
        }
    }
    internal sealed class Check
    {
        public string Game = "", Reason = "game_not_found", Version = "";
        public bool Compatible;
        public string Heading
        {
            get
            {
                if (Compatible) return "Bereit zum Spielen";
                if (Reason == "game_not_found") return "Spiel auswählen";
                if (Reason == "unsupported_game_build") return "Spielversion noch nicht unterstützt";
                if (Reason == "game_data_missing") return "Spieldateien fehlen";
                if (Reason.StartsWith("native_package") || Reason == "build_mod_first") return "Mod bitte erneut installieren";
                return "Spiel konnte nicht geprüft werden";
            }
        }
        public string Description
        {
            get
            {
                if (Compatible) return "Die Koop-Mod passt zu deinem Spiel. Solo oder Koop wählst du anschließend im Spiel.";
                if (Reason == "game_not_found") return "Wähle einmal deine AzerothSurvivors.exe. Die App merkt sich deine Auswahl.";
                if (Reason == "unsupported_game_build") return "Für dieses Spielupdate fehlt noch die passende Mod-Anpassung. Wähle eine unterstützte Installation (0.104.0 / 9845e5c7). Deine Spielstände bleiben erhalten.";
                if (Reason == "game_data_missing") return "Neben der Spieldatei muss der vollständige Data-Ordner des Spiels liegen.";
                if (Reason.StartsWith("native_package") || Reason == "build_mod_first") return "Öffne die Setup-Datei erneut, um die Mod-Dateien zu reparieren. Deine Spielstände bleiben erhalten.";
                return "Wähle die Spieldatei erneut. Einzelheiten findest du unter Hilfe.";
            }
        }
    }
    internal sealed class Backend
    {
        public readonly string Root;
        public readonly Locations Paths;
        readonly string profile;
        public Backend(string root, Locations paths, string profileName = "Main")
        {
            Root = Path.GetFullPath(root); Paths = paths; profile = profileName;
        }
        // Windows command-line quoting, including backslashes immediately before a quote.
        public static string Quote(string value)
        {
            var output = new StringBuilder("\""); int slashes = 0;
            foreach (char c in value)
            {
                if (c == '\\') { ++slashes; continue; }
                if (c == '"') output.Append('\\', slashes * 2 + 1);
                else output.Append('\\', slashes);
                output.Append(c); slashes = 0;
            }
            output.Append('\\', slashes * 2); output.Append('"'); return output.ToString();
        }
        public void Log(string text)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(Paths.Log));
                File.AppendAllText(Paths.Log, DateTimeOffset.Now.ToString("o") + " " + text + Environment.NewLine, Encoding.UTF8);
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
        public Dictionary<string, object> Run(string mode, string game, bool minimized)
        {
            string script = Path.Combine(Root, "Launch-CoopMod.ps1");
            if (!File.Exists(script)) throw new IOException("Die Mod-Dateien fehlen. Bitte das Setup erneut öffnen.");
            var arguments = new List<string> { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                "-File", script, "-Mode", mode, "-GamePath", Path.GetFullPath(game), "-Profile", profile,
                "-DataRoot", Paths.Data, "-NoErrorDialog" };
            if (minimized) arguments.Add("-Minimized");
            var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                @"WindowsPowerShell\v1.0\powershell.exe"), string.Join(" ", arguments.Select(Quote)))
            {
                UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = Root,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            };
            // A launcher started from PowerShell 7 can inherit its incompatible
            // module search path. Let Windows PowerShell initialize its own path.
            info.EnvironmentVariables.Remove("PSModulePath");
            using (var process = Process.Start(info))
            {
                var outputTask = process.StandardOutput.ReadToEndAsync();
                var errorTask = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(45000))
                {
                    process.Kill(); Log("backend timeout: " + mode);
                    throw new IOException("Das Spiel antwortet beim Start nicht. Prüfe, ob bereits ein Mod-Spiel läuft.");
                }
                string output = outputTask.GetAwaiter().GetResult(), error = errorTask.GetAwaiter().GetResult();
                if (process.ExitCode != 0 && mode != "Check")
                {
                    Log(mode + ": " + error + " " + output);
                    if ((error + output).Contains("already running"))
                        throw new IOException("Dein Mod-Spiel läuft bereits. Wechsle zum vorhandenen Spielfenster.");
                    throw new IOException("Das Mod-Spiel konnte nicht gestartet werden. Prüfe die gewählte Spielversion oder öffne die Hilfe.");
                }
                try { return Json.Parse(output); }
                catch (Exception ex)
                {
                    if (!(ex is ArgumentException) && !(ex is InvalidOperationException)) throw;
                    Log(mode + ": invalid response: " + output + " " + error);
                    throw new IOException("Die Spielprüfung ist fehlgeschlagen. Bitte das Mod-Setup erneut öffnen.");
                }
            }
        }
        public Check Probe(string game)
        {
            if (string.IsNullOrWhiteSpace(game) || !File.Exists(game)) return new Check { Game = game ?? "" };
            var result = Run("Check", game, false);
            if (!Json.Flag(result, "compatible")) Log("compatibility: " + new JavaScriptSerializer().Serialize(result));
            return new Check { Game = Path.GetFullPath(game), Compatible = Json.Flag(result, "compatible"),
                Reason = Json.Text(result, "reason"), Version = Json.Text(result, "gameVersion") };
        }
        public IEnumerable<string> Candidates()
        {
            var candidates = new List<string>();
            foreach (var name in new[] { Paths.Settings, Path.Combine(Paths.Data, "settings.json") })
            {
                try { if (File.Exists(name)) candidates.Add(Json.Text(Json.Read(name), "gamePath")); }
                catch (Exception ex) { Log("settings: " + ex.Message); }
            }
            candidates.Add(Path.Combine(Root, "AzerothSurvivors.exe"));
            foreach (var process in Process.GetProcessesByName("AzerothSurvivors"))
            {
                using (process) { try { candidates.Add(process.MainModule.FileName); } catch { } }
            }
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            foreach (var suffix in new[] {
                @"Downloads\Compressed\AzerothSurvivorsWindowsNew\AzerothSurvivors\AzerothSurvivors.exe",
                @"Downloads\AzerothSurvivors\AzerothSurvivors.exe", @"Desktop\AzerothSurvivors\AzerothSurvivors.exe" })
                candidates.Add(Path.Combine(home, suffix));
            return candidates.Where(p => !string.IsNullOrWhiteSpace(p) && File.Exists(p)).Distinct(StringComparer.OrdinalIgnoreCase);
        }
        public Check Discover()
        {
            Check first = null;
            foreach (var path in Candidates())
            {
                var check = Probe(path);
                if (check.Compatible) { Save(check.Game); return check; }
                if (first == null) first = check;
            }
            return first ?? new Check();
        }
        public void Save(string game)
        {
            Json.Write(Paths.Settings, new { schema = 1, gamePath = Path.GetFullPath(game) });
        }
    }
}
