using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;

namespace AzerothCoop.Desktop
{
    internal static class PortableTests
    {
        static void Require(bool value, string message) { if (!value) throw new Exception(message); }
        internal static void Run(string folder, Locations paths, string output, string zip)
        {
            string game = Path.Combine(folder, "Portable Game ä"); Directory.CreateDirectory(game);
            Directory.CreateDirectory(Path.Combine(game, "Data"));
            string executable = Path.Combine(game, "AzerothSurvivors.exe"); File.WriteAllText(executable, "game-placeholder");
            string asset = Path.Combine(game, "Data", "asset.bin"); File.WriteAllText(asset, "game-assets-untouched");
            string saveHash = Json.Hash(Path.Combine(paths.Profile, "characters.txt"));
            string launcher = Path.Combine(game, Startup.LauncherName);
            if (zip != null)
            {
                using (var archive = ZipFile.OpenRead(zip))
                    foreach (var entry in archive.Entries)
                        Require(entry.FullName == Startup.LauncherName || entry.FullName == "Koop-Mod-Liesmich.txt" ||
                            entry.FullName.StartsWith(Portable.Folder + "/", StringComparison.Ordinal), "portable ZIP contains unexpected files");
                ZipFile.ExtractToDirectory(zip, game);
            }
            else Portable.Install(game, false);
            var target = Startup.Resolve(launcher, paths);
            Require(target.Action == "launch" && target.Root.StartsWith(Portable.ModDirectory(game) + "\\"), "ZIP launcher did not select its own runtime");
            string previousRoot = target.Root;
            var backend = new Backend(target.Root, paths);
            Require(backend.PortableGameRoot == game && backend.Candidates().SequenceEqual(new[] { executable }), "portable launcher selected a different game installation");
            Require(backend.Probe(executable).Reason == "unsupported_game_build", "portable launcher accepted an unknown game EXE");
            string probe = Path.Combine(output, "startup-portable.json");
            var start = new ProcessStartInfo(launcher, "/startup-probe " + Backend.Quote(probe) + " /data " + Backend.Quote(paths.Data) + " /quiet")
                { UseShellExecute = false, CreateNoWindow = true };
            using (var child = Process.Start(start))
            {
                if (!child.WaitForExit(15000)) { child.Kill(); throw new Exception("portable launcher probe timed out"); }
                Require(child.ExitCode == 0 && Json.Text(Json.Read(probe), "action") == "launch", "portable launcher entry point failed");
            }
            // Exercise the same embedded update helper used after a verified
            // download. Reinstalling this test version checks replacement and
            // preservation; it is not a public older-to-newer portable upgrade.
            var update = new ProcessStartInfo(Assembly.GetExecutingAssembly().Location, "/update 2147483647 /quiet /no-open /portable " +
                Backend.Quote(game) + " /data " + Backend.Quote(paths.Data)) { UseShellExecute = false, CreateNoWindow = true };
            using (var helper = Process.Start(update))
            {
                if (!helper.WaitForExit(30000)) { helper.Kill(); throw new Exception("portable update helper timed out"); }
                Require(helper.ExitCode == 0, "portable update helper failed");
            }
            target = Startup.Resolve(launcher, paths);
            Require(target.Action == "launch" && target.Root != previousRoot && Directory.Exists(previousRoot), "portable update did not preserve immutable previous runtime");
            Require(Json.Flag(Json.Read(Path.Combine(paths.Data, "updates", "last-result.json")), "portable"), "helper installed outside portable mode");
            Require(File.ReadAllText(executable) == "game-placeholder" && File.ReadAllText(asset) == "game-assets-untouched", "portable update modified game files");
            Require(Json.Hash(Path.Combine(paths.Profile, "characters.txt")) == saveHash, "portable update modified characters");
            string marker = Path.Combine(Portable.ModDirectory(game), ".azs-portable-owned");
            File.Move(marker, marker + ".backup");
            try
            {
                bool denied = false; try { Portable.Install(game, true); } catch (IOException) { denied = true; }
                Require(denied, "portable update accepted an unowned mod directory");
            }
            finally { File.Move(marker + ".backup", marker); }
            Json.Write(Path.Combine(output, "portable-tests.json"), new { passed = true, zipExtraction = zip != null,
                actualPortableLauncherProcess = true, adjacentGameSelection = true, unknownGameRejected = true,
                actualUpdateHelper = true, updateTestReinstalledSameVersion = true, oldRuntimePreserved = true,
                gameFilesAndCharactersPreserved = true, unownedDirectoryRejected = true, nativeGameplayAcceptance = false });
        }
    }
}
