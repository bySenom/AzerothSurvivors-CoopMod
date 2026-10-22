using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace AzerothCoop.Desktop
{
    // An independent process holds the game handle before its primary thread is
    // resumed. It keeps observing if the UI/PowerShell launcher exits.
    internal static class CrashWatch
    {
        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        struct DumpException { public uint ThreadId; public IntPtr ExceptionPointers; public int ClientPointers; }
        [DllImport("dbghelp.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool MiniDumpWriteDump(IntPtr process, uint pid, SafeFileHandle file, uint type,
            ref DumpException exception, IntPtr streams, IntPtr callback);

        static readonly string[] Prefixes = { "AzerothCoop-", "AzerothCoopTransport-", "AzerothCoopGameplay-", "AzerothCoopLobby-" };
        static readonly string[] Names = { "base", "transport", "gameplay", "lobby" };
        // Never collect arbitrary RPC commands, lobby codes, settings or saves.
        // Numeric/bool counters and these fixed status strings are sufficient.
        static object Status(int pid, string prefix)
        {
            try
            {
                using (var pipe = new NamedPipeClientStream(".", prefix + pid, PipeDirection.InOut, PipeOptions.Asynchronous))
                {
                    pipe.Connect(40);
                    byte[] request = Encoding.ASCII.GetBytes("status\n"), bytes = new byte[16384];
                    var write = pipe.BeginWrite(request, 0, request.Length, null, null);
                    using (var wait = write.AsyncWaitHandle)
                    { if (!wait.WaitOne(150)) return new { available = false, reason = "write_timeout" }; pipe.EndWrite(write); }
                    var read = pipe.BeginRead(bytes, 0, bytes.Length, null, null);
                    int count;
                    using (var wait = read.AsyncWaitHandle)
                    { if (!wait.WaitOne(150)) return new { available = false, reason = "read_timeout" }; count = pipe.EndRead(read); }
                    var fields = Json.Parse(Encoding.UTF8.GetString(bytes, 0, count));
                    var result = new Dictionary<string, object>();
                    foreach (var pair in fields)
                    {
                        if (pair.Value is bool || pair.Value is int || pair.Value is long || pair.Value is decimal || pair.Value is double)
                            result[pair.Key] = pair.Value;
                        else if (pair.Key == "phase" || pair.Key == "failure") result[pair.Key] = pair.Value;
                    }
                    return result;
                }
            }
            catch (Exception ex) { return new { available = false, reason = ex.GetType().Name, error = ex.HResult }; }
        }
        static void Modules(Process game, string session)
        {
            try
            {
                game.Refresh();
                var items = game.Modules.Cast<ProcessModule>().Select(m => new {
                    name = m.ModuleName, imageBase = "0x" + m.BaseAddress.ToInt64().ToString("x"), size = m.ModuleMemorySize }).ToArray();
                Json.Write(Path.Combine(session, "modules.json"), items);
            }
            catch { } // Process may already have exited; retain the last map.
        }
        static void Dump(Process game, string session)
        {
            try
            {
                string content;
                using (var file = new FileStream(Path.Combine(session, "native-crash.txt"), FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var reader = new StreamReader(file)) content = reader.ReadToEnd();
                var fields = content.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                    .Where(x => x.Contains("=")).Select(x => x.Split(new[] { '=' }, 2)).ToDictionary(x => x[0], x => x[1]);
                var exception = new DumpException { ThreadId = uint.Parse(fields["thread"], CultureInfo.InvariantCulture),
                    ExceptionPointers = new IntPtr(long.Parse(fields["exception_pointers"].Substring(2), NumberStyles.HexNumber)), ClientPointers = 1 };
                Modules(game, session);
                // Out-of-process DbgHelp avoids relying on the crashed game's
                // loader/heap. The native handler keeps EXCEPTION_POINTERS live.
                // https://learn.microsoft.com/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
                using (var file = new FileStream(Path.Combine(session, "crash.dmp"), FileMode.CreateNew, FileAccess.ReadWrite, FileShare.Read))
                {
                    bool ok = MiniDumpWriteDump(game.Handle, (uint)game.Id, file.SafeFileHandle,
                        0x00001020 /* WithThreadInfo | WithUnloadedModules, no full-memory dump */, ref exception, IntPtr.Zero, IntPtr.Zero);
                    int error = ok ? 0 : Marshal.GetLastWin32Error();
                    Json.Write(Path.Combine(session, "dump-result.json"), new { success = ok, windowsError = error });
                }
            }
            catch (Exception ex) { Json.Write(Path.Combine(session, "dump-result.json"), new { success = false, error = ex.GetType().Name }); }
        }
        static void Bundle(string session)
        {
            string crashes = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(session)), "crashes");
            Directory.CreateDirectory(crashes);
            string target = Path.Combine(crashes, Path.GetFileName(session) + ".zip"), temporary = target + ".tmp";
            // Explicit allowlist: a future file in the session folder must not
            // silently broaden what the user shares.
            using (var archive = ZipFile.Open(temporary, ZipArchiveMode.Create))
                foreach (string name in new[] { "session.json", "watcher.json", "exit.json", "status.json", "modules.json",
                    "mod-files.json", "native-crash.txt", "dump-result.json", "crash.dmp", "watcher-error.txt" })
                {
                    string source = Path.Combine(session, name);
                    if (File.Exists(source)) archive.CreateEntryFromFile(source, name, CompressionLevel.Optimal);
                }
            File.Move(temporary, target);
        }
        public static int Run(int pid, string directory)
        {
            string session = Path.GetFullPath(directory); Directory.CreateDirectory(session);
            try
            {
                using (var game = Process.GetProcessById(pid))
                using (var requested = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\AzerothCoopCrashRequested-" + pid))
                using (var finished = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\AzerothCoopCrashFinished-" + pid))
                {
                    IntPtr handle = game.Handle; // Hold the actual process identity, including across PID reuse.
                    Json.Write(Path.Combine(session, "watcher.json"), new { schema = 1, pid = pid, watcherPid = Process.GetCurrentProcess().Id,
                        startedUtc = DateTime.UtcNow.ToString("o"), gameStartedUtc = game.StartTime.ToUniversalTime().ToString("o") });
                    string root = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(System.Windows.Forms.Application.ExecutablePath), ".."));
                    string manifest = Path.Combine(root, "package-manifest.json");
                    if (File.Exists(manifest)) File.Copy(manifest, Path.Combine(session, "mod-files.json"));
                    using (var ready = EventWaitHandle.OpenExisting(@"Local\AzerothCoopWatchReady-" + pid)) ready.Set();
                    var samples = new Queue<object>(); int ticks = 0;
                    string role = "unknown"; bool dumped = false;
                    while (!game.WaitForExit(200))
                    {
                        if (!dumped && requested.WaitOne(0))
                        {
                            dumped = true;
                            try { Dump(game, session); } finally { finished.Set(); }
                        }
                        if (++ticks % 10 != 0) continue;
                        var sample = new Dictionary<string, object>(); sample["utc"] = DateTime.UtcNow.ToString("o");
                        for (int i = 0; i < Prefixes.Length; ++i) sample[Names[i]] = Status(pid, Prefixes[i]);
                        var transport = sample["transport"] as Dictionary<string, object>;
                        if (transport != null)
                        {
                            if (Json.Flag(transport, "joining")) role = "guest";
                            else if (Json.Flag(transport, "listening")) role = "host";
                        }
                        samples.Enqueue(sample); if (samples.Count > 8) samples.Dequeue();
                        Json.Write(Path.Combine(session, "status.json"), new { role = role, samples = samples.ToArray() });
                        if (ticks % 50 == 10) Modules(game, session);
                    }
                    bool nativeException = File.Exists(Path.Combine(session, "native-crash.txt")) && new FileInfo(Path.Combine(session, "native-crash.txt")).Length > 0;
                    uint code = unchecked((uint)game.ExitCode);
                    Json.Write(Path.Combine(session, "exit.json"), new { schema = 1, pid = pid, endedUtc = DateTime.UtcNow.ToString("o"),
                        exitCode = code, exitCodeHex = "0x" + code.ToString("x8"), role = role,
                        unexpected = code != 0 || nativeException, nativeException = nativeException,
                        classification = nativeException ? "native_exception" : code == 0 ? "exit_code_zero" : "nonzero_exit_or_external_termination" });
                    if (code != 0 || nativeException) Bundle(session);
                }
                return 0;
            }
            catch (Exception ex)
            {
                File.WriteAllText(Path.Combine(session, "watcher-error.txt"), ex.GetType().Name + ": " + ex.Message);
                return 1;
            }
        }
    }
}
