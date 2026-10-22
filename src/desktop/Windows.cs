using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AzerothCoop.Desktop
{
    internal sealed class ModButton : Button
    {
        bool pressed;
        protected override void OnMouseDown(MouseEventArgs e) { pressed = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { pressed = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnPaint(PaintEventArgs e)
        {
            Color fill = Enabled ? (pressed ? ControlPaint.Dark(BackColor) : BackColor) : Color.FromArgb(42, 36, 31);
            using (var brush = new SolidBrush(fill)) e.Graphics.FillRectangle(brush, ClientRectangle);
            using (var pen = new Pen(Enabled ? FlatAppearance.BorderColor : Color.FromArgb(78, 68, 52)))
                e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
            TextRenderer.DrawText(e.Graphics, Text, Font, ClientRectangle,
                Enabled ? ForeColor : Color.FromArgb(174, 161, 139), TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, Rectangle.Inflate(ClientRectangle, -5, -5), ForeColor, fill);
        }
    }
    internal class ModWindow : Form
    {
        internal static readonly Color Gold = Color.FromArgb(244, 201, 113), Muted = Color.FromArgb(186, 177, 162);
        internal bool Offscreen;
        protected override bool ShowWithoutActivation { get { return Offscreen; } }
        protected override CreateParams CreateParams
        {
            get
            {
                var settings = base.CreateParams;
                if (Offscreen) settings.ExStyle |= 0x08000000 | 0x00000080; // NOACTIVATE, TOOLWINDOW
                return settings;
            }
        }
        internal ModWindow()
        {
            Text = "Azeroth Survivors Koop"; ClientSize = new Size(760, 532);
            BackColor = Color.FromArgb(24, 20, 18); ForeColor = Color.WhiteSmoke;
            Font = new Font("Segoe UI", 10); AutoScaleMode = AutoScaleMode.Dpi;
            StartPosition = FormStartPosition.CenterScreen; FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
        }
        protected Label TextAt(string text, int x, int y, int width, int height, float size, Color color)
        {
            var label = new Label { Text = text, Location = new Point(x, y), Size = new Size(width, height),
                ForeColor = color, Font = new Font("Segoe UI", size), AutoEllipsis = true };
            Controls.Add(label); return label;
        }
        protected Button ButtonAt(string text, int x, int y, int width, int height, bool primary)
        {
            var button = new ModButton { Text = text, Location = new Point(x, y), Size = new Size(width, height),
                FlatStyle = FlatStyle.Flat, BackColor = primary ? Color.FromArgb(116, 32, 24) : Color.FromArgb(43, 37, 32),
                ForeColor = Gold, Cursor = Cursors.Hand, UseVisualStyleBackColor = false };
            button.FlatAppearance.BorderColor = primary ? Gold : Color.FromArgb(86, 74, 56);
            Controls.Add(button); return button;
        }
        protected void Header(string subtitle)
        {
            TextAt("AZEROTH SURVIVORS", 30, 25, 560, 26, 11, Muted);
            TextAt("Koop-Mod", 28, 57, 620, 58, 30, Gold);
            TextAt(subtitle, 31, 122, 697, 48, 11, Muted);
            TextAt("TESTVERSION", 625, 30, 110, 25, 9, Gold);
        }
        internal static void OpenFolder(string folder)
        {
            Directory.CreateDirectory(folder);
            Process.Start(new ProcessStartInfo("explorer.exe", Backend.Quote(folder)) { UseShellExecute = true });
        }
    }
    internal sealed class LaunchWindow : ModWindow
    {
        readonly Backend backend;
        readonly Label heading, description, pathLabel, footer;
        readonly Button play, select;
        readonly Timer timer = new Timer { Interval = 1000 };
        readonly Updates updates;
        readonly Label updateStatus;
        readonly Button updateButton;
        readonly CheckBox automatic;
        UpdateOffer offer;
        string stagedUpdate;
        bool updateBusy, updating;
        Check current = new Check();
        int generation;
        bool starting;
        Process game;
        internal LaunchWindow(Backend value)
        {
            backend = value;
            updates = new Updates(backend); ClientSize = new Size(760, 630);
            Header("Dein Spiel starten. Solo oder Koop wählst du direkt im Spiel.");
            heading = TextAt("Spiel wird gesucht …", 31, 198, 695, 39, 20, Gold);
            description = TextAt("Die Installation und Spielversion werden geprüft.", 32, 247, 690, 77, 11, Muted);
            pathLabel = TextAt("", 32, 337, 685, 40, 9, Muted);
            play = ButtonAt("Mit Mod spielen", 32, 389, 332, 56, true); play.Enabled = false;
            select = ButtonAt("Spiel auswählen", 384, 389, 342, 56, false);
            var saves = ButtonAt("Spielstände", 32, 465, 150, 34, false);
            var help = ButtonAt("Hilfe", 196, 465, 100, 34, false);
            footer = TextAt("Koop über zwei PCs ist noch nicht vollständig getestet.", 319, 468, 409, 39, 9, Muted);
            updateStatus = TextAt("Mod " + updates.Current + " · Updates über GitHub", 32, 524, 457, 44, 10, Muted);
            updateButton = ButtonAt("Nach Updates suchen", 510, 520, 216, 42, false);
            automatic = new CheckBox { Text = "Mod-Updates beim Start automatisch installieren", Checked = updates.Automatic,
                ForeColor = Gold, Location = new Point(32, 582), Size = new Size(694, 30) };
            Controls.Add(automatic);
            automatic.CheckedChanged += (s, e) => { try { updates.Automatic = automatic.Checked; } catch (Exception ex) { ShowError(ex); } };
            updateButton.Click += async (s, e) => { if (offer == null) await CheckUpdates(false); else await ApplyUpdate(); };
            select.Click += async (s, e) =>
            {
                using (var picker = new OpenFileDialog { Title = "Azeroth Survivors auswählen",
                    Filter = "Azeroth Survivors|AzerothSurvivors.exe", CheckFileExists = true })
                    if (picker.ShowDialog(this) == DialogResult.OK) await RefreshGame(picker.FileName);
            };
            saves.Click += (s, e) => { try { OpenFolder(backend.Paths.Profile); } catch (Exception ex) { ShowError(ex); } };
            help.Click += (s, e) => MessageBox.Show(this,
                "1. Wähle einmal deine Spieldatei aus.\n2. Klicke auf „Mit Mod spielen“.\n3. Klicke im Spiel auf „Wald von Elwynn betreten“.\n4. Wähle Solo oder Koop. Bei Koop: Lobby erstellen oder Code eingeben.\n\n" +
                "Beide Spieler benötigen dieselbe unterstützte Spiel- und Mod-Version sowie eine erreichbare Netzwerkverbindung. Der Lobbycode übernimmt keine NAT-Weiterleitung.\n\n" +
                "Diese Mod ist in Entwicklung. Ein kompletter Koop-Lauf über zwei PCs ist noch nicht abgenommen.\n\n" +
                "Bei Startproblemen liegt das Protokoll hier:\n" + backend.Paths.Log,
                "Hilfe zur Koop-Mod", MessageBoxButtons.OK, MessageBoxIcon.Information);
            play.Click += async (s, e) => await StartGame();
            Shown += async (s, e) =>
            {
                if (Offscreen) return;
                await RefreshGame(null);
                if (!IsDisposed && automatic.Checked) await CheckUpdates(true);
            };
            timer.Tick += (s, e) =>
            {
                if (game != null && game.HasExited) { game.Dispose(); game = null; SetCheck(current); }
                if (!Offscreen && offer != null && automatic.Checked && !starting && !updateBusy && !updating &&
                    !Updates.GameRunning(backend.Paths)) BeginInvoke(new Action(async () => await ApplyUpdate()));
            };
            FormClosed += (s, e) => { timer.Dispose(); if (game != null) game.Dispose(); };
        }
        internal void SetCheck(Check check)
        {
            current = check; heading.Text = check.Heading; description.Text = check.Description;
            pathLabel.Text = string.IsNullOrEmpty(check.Game) ? "" : "Gewähltes Spiel: " + check.Game;
            play.Text = game == null ? "Mit Mod spielen" : "Spiel läuft";
            play.Enabled = check.Compatible && !starting && !updating && game == null;
        }
        async Task CheckUpdates(bool applyAutomatically)
        {
            if (updateBusy || updating) return;
            updateBusy = true; updateButton.Enabled = false; updateStatus.Text = "Suche nach Mod-Updates …";
            try
            {
                offer = await Task.Run(() => updates.Check());
                if (IsDisposed) return;
                updateStatus.Text = offer == null ? "Mod " + updates.Current + " · Keine neuere Version verfügbar." :
                    "Mod " + offer.Version + " ist verfügbar.";
                updateButton.Text = offer == null ? "Nach Updates suchen" : "Update installieren";
            }
            catch (Exception ex)
            {
                backend.Log("update check: " + ex);
                if (!IsDisposed) updateStatus.Text = ex is System.Net.WebException ?
                    "Updateserver nicht erreichbar. Die vorhandene Mod bleibt nutzbar." :
                    "Update konnte nicht verifiziert werden. Die vorhandene Mod bleibt erhalten.";
            }
            finally { updateBusy = false; if (!IsDisposed) updateButton.Enabled = true; }
            if (!IsDisposed && applyAutomatically && offer != null) await ApplyUpdate();
        }
        async Task ApplyUpdate()
        {
            if (offer == null || updateBusy || updating) return;
            if (starting || game != null || Updates.GameRunning(backend.Paths))
            {
                updateStatus.Text = "Update bereit. Installation nach dem Beenden des Mod-Spiels."; timer.Start(); return;
            }
            updating = true; updateButton.Enabled = play.Enabled = select.Enabled = false;
            automatic.Enabled = false; updateStatus.Text = "Mod " + offer.Version + " wird geladen und geprüft …";
            try
            {
                if (stagedUpdate == null) stagedUpdate = await Task.Run(() => updates.Stage(offer, null));
                if (IsDisposed) return;
                updates.HandOff(stagedUpdate, offer, Process.GetCurrentProcess().Id);
                Close();
            }
            catch (Exception ex)
            {
                backend.Log("update install: " + ex);
                if (!IsDisposed) updateStatus.Text = ex.Message;
                updating = false;
                if (!IsDisposed) { updateButton.Enabled = select.Enabled = automatic.Enabled = true; SetCheck(current); }
            }
        }
        async Task RefreshGame(string path)
        {
            int request = ++generation;
            play.Enabled = false; heading.Text = "Spiel wird geprüft …";
            description.Text = "Einen Moment bitte."; pathLabel.Text = path ?? "";
            try
            {
                var result = await Task.Run(() => path == null ? backend.Discover() : backend.Probe(path));
                if (request != generation || IsDisposed) return;
                if (result.Compatible) backend.Save(result.Game);
                SetCheck(result);
            }
            catch (Exception ex)
            {
                if (request != generation || IsDisposed) return;
                backend.Log(ex.ToString()); SetCheck(new Check { Game = path ?? "", Reason = "check_failed" });
            }
        }
        async Task StartGame()
        {
            if (!current.Compatible || starting || updating || game != null) return;
            starting = true; play.Enabled = select.Enabled = false; play.Text = "Spiel startet …";
            try
            {
                string chosen = current.Game;
                var result = await Task.Run(() => backend.Run("Play", chosen, false));
                int id = Convert.ToInt32(result["pid"]);
                try { game = Process.GetProcessById(id); timer.Start(); } catch (ArgumentException) { }
                heading.Text = "Mod-Spiel gestartet";
                description.Text = "Wähle deinen Helden und klicke auf „Wald von Elwynn betreten“. Dort öffnet sich Solo / Koop.";
            }
            catch (Exception ex) { backend.Log(ex.ToString()); ShowError(ex); }
            finally
            {
                starting = false; select.Enabled = true; play.Text = game == null ? "Mit Mod spielen" : "Spiel läuft";
                play.Enabled = current.Compatible && game == null;
            }
        }
        void ShowError(Exception ex)
        {
            MessageBox.Show(this, ex.Message, "Azeroth Survivors Koop", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        internal bool PlayEnabled { get { return play.Enabled; } }
        internal string StatusHeading { get { return heading.Text; } }
    }
    internal sealed class RepairWindow : ModWindow
    {
        internal RepairWindow()
        {
            Header("Mod-ZIP in den Spielordner entpacken. Danach den Koop-Starter öffnen.");
            TextAt("Koop-Mod-Dateien fehlen", 31, 194, 690, 39, 21, Gold);
            TextAt("Diese Starter-Datei wurde einzeln kopiert oder das Mod-Paket ist unvollständig.\n\nEntpacke den gesamten Inhalt des Mod-ZIPs in den Spielordner. Neben dem Koop-Starter muss auch der Ordner AzerothCoopMod liegen. Mod-Spielstände bleiben erhalten.",
                32, 246, 692, 131, 11, Muted);
            var download = ButtonAt("Mod-ZIP herunterladen", 32, 407, 494, 56, true);
            var close = ButtonAt("Schließen", 546, 407, 180, 56, false);
            TextAt("Auf GitHub: AzerothSurvivors-Koop-Mod.zip unter der aktuellen Preview öffnen.", 32, 486, 695, 31, 9, Muted);
            download.Click += (s, e) =>
            {
                try { Process.Start(new ProcessStartInfo(Startup.Releases) { UseShellExecute = true }); }
                catch { MessageBox.Show(this, "Öffne die Download-Seite im Browser:\n" + Startup.Releases, "Mod-ZIP herunterladen"); }
            };
            close.Click += (s, e) => Close();
        }
    }
    internal sealed class SetupWindow : ModWindow
    {
        readonly Installer installer;
        readonly Button action;
        readonly CheckBox desktop;
        readonly Label heading, body;
        string installed;
        internal SetupWindow(Installer value)
        {
            installer = value; Text = "Azeroth Survivors Koop – Installation";
            Header("Einmal installieren. Danach direkt vom Desktop oder Startmenü öffnen.");
            heading = TextAt("Koop-Mod installieren", 31, 194, 690, 39, 21, Gold);
            body = TextAt("Die Mod wird für dein Windows-Benutzerkonto eingerichtet.\nDas Spiel wählst du anschließend in der App aus.\nVorhandene Mod-Spielstände bleiben erhalten.", 32, 246, 692, 91, 11, Muted);
            desktop = new CheckBox { Text = "Verknüpfung auf dem Desktop erstellen", Checked = true,
                Location = new Point(32, 350), Size = new Size(650, 34), ForeColor = Gold };
            Controls.Add(desktop);
            action = ButtonAt("Installieren", 32, 407, 694, 56, true);
            TextAt("Testversion · Das Spiel ist nicht enthalten. Vollständiger Koop-Test steht noch aus.", 32, 486, 695, 31, 9, Muted);
            action.Click += async (s, e) =>
            {
                if (installed != null)
                {
                    Process.Start(new ProcessStartInfo(installed) { UseShellExecute = true }); Close(); return;
                }
                bool shortcut = desktop.Checked; action.Enabled = desktop.Enabled = false;
                heading.Text = "Mod wird eingerichtet …"; action.Text = "Installation läuft …";
                try
                {
                    installed = await Task.Run(() => installer.Install(shortcut, true, null));
                    heading.Text = "Installation abgeschlossen";
                    body.Text = "Du findest „Azeroth Survivors Koop“ jetzt im Startmenü" +
                        (shortcut ? " und auf deinem Desktop." : ".") + "\nÖffne die App und wähle dein Spiel aus.";
                    action.Text = "Mod öffnen";
                }
                catch (Exception ex)
                {
                    heading.Text = "Installation konnte nicht abgeschlossen werden";
                    body.Text = ex.Message; action.Text = "Erneut versuchen"; desktop.Enabled = true;
                }
                finally { action.Enabled = true; }
            };
        }
    }
}
