// Aether application installer (compiled by tools/build_installer.py with the .NET Framework compiler that ships with Windows,
// which is C# 5: no string interpolation, no ?. operator, no expression-bodied members, no nameof). The payload, aether-app.zip, is a
// managed resource of this executable. @VERSION@ is replaced by the build script.
//
//   AetherInstaller.exe                  a setup window: welcome, folder and options, progress, finish (to %LOCALAPPDATA%\Aether by default)
//   /silent       no window, no questions: install, and write what happens to the console (running programs from the install folder are closed)
//   /target:DIR   install to DIR instead (the window starts with it filled in)
//   /noshortcuts  skip the Start menu and Desktop shortcuts (the window starts with both unticked)
//   /nolaunch     do not offer to start the editor (the window starts with it unticked)
//
// The installing is the same code either way (Setup): the window and the console are two faces on it.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

// What the person chose.
class InstallOptions
{
    public string Target;
    public bool StartMenuShortcut = true;
    public bool DesktopShortcut = true;
    public bool Launch = true;
}

// Putting the files in place, with no idea whether a window or a console is watching: it reports through two callbacks.
static class Setup
{
    public const string Version = "@VERSION@";
    // The editor program inside bin\ (the Dear ImGui shell; the old Qt editor was aether_editor.exe). The installer checks it after unpacking,
    // points the shortcuts at it and offers to start it.
    public const string EditorExe = "aether_editor_shell.exe";
    public static readonly string LogPath = Path.Combine(Path.GetTempPath(), "aether-install-log.txt");

    public static string DefaultTarget()
    {
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Aether");
    }

    public static string EditorPath(string target)
    {
        return Path.Combine(Path.Combine(target, "bin"), EditorExe);
    }

    // How many bytes the payload takes once unpacked.
    public static long UnpackedBytes()
    {
        try
        {
            Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("aether-app.zip");
            if (payload == null) return 0;
            long total = 0;
            using (ZipArchive archive = new ZipArchive(payload, ZipArchiveMode.Read))
            {
                foreach (ZipArchiveEntry entry in archive.Entries) total += entry.Length;
            }
            return total;
        }
        catch { return 0; }
    }

    // Free bytes on the drive `target` is on, or -1 if that cannot be told (a network folder, a bad name).
    public static long FreeBytes(string target)
    {
        try
        {
            string root = Path.GetPathRoot(Path.GetFullPath(target));
            return root != null && root.Length > 0 ? new DriveInfo(root).AvailableFreeSpace : -1;
        }
        catch { return -1; }
    }

    // The version of an installation already in `target`, or null.
    public static string InstalledVersion(string target)
    {
        try
        {
            string file = Path.Combine(target, "INSTALLED_VERSION.txt");
            return File.Exists(file) ? File.ReadAllText(file).Trim() : null;
        }
        catch { return null; }
    }

    // Programs running from the install folder hold its files open, so the old installation could neither be deleted nor replaced.
    public static List<Process> RunningFrom(string target)
    {
        List<Process> running = new List<Process>();
        string prefix;
        try { prefix = Path.GetFullPath(target).TrimEnd('\\', '/') + Path.DirectorySeparatorChar; }
        catch { return running; }
        foreach (Process process in Process.GetProcesses())
        {
            try
            {
                string file = process.MainModule.FileName;
                if (file != null && file.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) running.Add(process);
            }
            catch { } // system processes refuse the query; none of them is ours
        }
        return running;
    }

    public static string Describe(List<Process> running)
    {
        StringBuilder text = new StringBuilder();
        foreach (Process process in running) text.AppendLine("  " + process.ProcessName + " (process " + process.Id + ")");
        return text.ToString();
    }

    public static void Close(List<Process> running)
    {
        foreach (Process process in running)
        {
            try
            {
                if (!process.CloseMainWindow() || !process.WaitForExit(4000))
                {
                    process.Kill();
                    process.WaitForExit(4000);
                }
            }
            catch { }
        }
    }

    // Moves the previous installation aside and deletes it. The move is all or nothing: if anything in the folder is still open it
    // fails and the old installation is left exactly as it was, instead of half deleted.
    static void ClearTarget(string target, Action<string> note)
    {
        if (!Directory.Exists(target)) return;
        note("Removing the previous installation...");
        string aside = target + ".old-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
        // A program that was just closed can take a moment to let go of its files, so try a few times before giving up.
        Exception moveError = null;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            try { Directory.Move(target, aside); moveError = null; break; }
            catch (Exception error) { moveError = error; Thread.Sleep(500); }
        }
        if (moveError != null)
        {
            throw new IOException("the previous installation in " + target + " is in use (" + moveError.Message +
                                  "). It has not been changed. Close Aether, including the editor, and run this installer again.");
        }
        try { Directory.Delete(aside, true); }
        catch (Exception error) { note("  could not delete " + aside + " (" + error.Message + "); it can be deleted by hand"); }
    }

    // Unpacks the payload, reporting how far along it is (0 to 100) by bytes. A payload built by Windows PowerShell names its entries with
    // backslashes, one built by Python with slashes: both become folders. An entry that would land outside the target is refused.
    static int Unpack(Stream payload, string target, Action<int> progress)
    {
        string root = Path.GetFullPath(target).TrimEnd('\\', '/') + Path.DirectorySeparatorChar;
        int files = 0;
        using (ZipArchive archive = new ZipArchive(payload, ZipArchiveMode.Read))
        {
            long total = 0;
            foreach (ZipArchiveEntry entry in archive.Entries) total += entry.Length;
            long done = 0;
            int last = -1;
            foreach (ZipArchiveEntry entry in archive.Entries)
            {
                string name = entry.FullName.Replace('\\', Path.DirectorySeparatorChar).Replace('/', Path.DirectorySeparatorChar);
                if (name.Length == 0) continue;
                string path = Path.GetFullPath(Path.Combine(target, name));
                if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new IOException("the payload has an entry outside the install folder: " + entry.FullName);
                if (name.EndsWith(Path.DirectorySeparatorChar.ToString()))
                {
                    Directory.CreateDirectory(path);
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                entry.ExtractToFile(path, true);
                ++files;
                done += entry.Length;
                int percent = total > 0 ? (int)(done * 100 / total) : 100;
                if (percent != last)
                {
                    last = percent;
                    progress(percent);
                }
            }
        }
        progress(100);
        return files;
    }

    static void MakeShortcut(string linkPath, string exe, string workingDirectory)
    {
        Type shellType = Type.GetTypeFromProgID("WScript.Shell");
        object shell = Activator.CreateInstance(shellType);
        object link = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { linkPath });
        Type linkType = link.GetType();
        linkType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, link, new object[] { exe });
        linkType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, link, new object[] { workingDirectory });
        linkType.InvokeMember("Description", BindingFlags.SetProperty, null, link, new object[] { "Aether Editor " + Version });
        linkType.InvokeMember("IconLocation", BindingFlags.SetProperty, null, link, new object[] { exe + ",0" });
        linkType.InvokeMember("Save", BindingFlags.InvokeMethod, null, link, null);
    }

    static void MakeShortcuts(InstallOptions options, string exe, string bin, Action<string> note)
    {
        // Same names every time: installing again replaces the shortcut rather than adding a second one.
        List<string> places = new List<string>();
        if (options.StartMenuShortcut)
            places.Add(Path.Combine(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Aether"), "Aether Editor.lnk"));
        if (options.DesktopShortcut)
            places.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Aether Editor.lnk"));
        foreach (string link in places)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(link));
                MakeShortcut(link, exe, bin);
                note("Shortcut: " + link);
            }
            catch (Exception error) { note("Could not create the shortcut " + link + ": " + error.Message); }
        }
    }

    // The whole installation. `running` are the programs from the install folder the caller has already got the person's consent to close.
    // Throws with a message a person can act on; returns the path of the editor.
    public static string Install(InstallOptions options, List<Process> running, Action<string> note, Action<int> progress)
    {
        Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("aether-app.zip");
        if (payload == null) throw new InvalidOperationException("the payload is not inside this executable");
        string target = Path.GetFullPath(options.Target);
        note("Payload found: " + (payload.Length / 1024 / 1024) + " MB");
        note("Installing to: " + target);
        if (running.Count > 0)
        {
            note("Closing the programs running from the install folder...");
            Close(running);
        }
        ClearTarget(target, note);
        Directory.CreateDirectory(target);
        note("Unpacking, this takes a few moments...");
        int files = Unpack(payload, target, progress);

        string bin = Path.Combine(target, "bin");
        string editor = Path.Combine(bin, EditorExe);
        if (!File.Exists(editor)) throw new IOException("the installation is incomplete: " + editor + " is missing after unpacking");
        File.WriteAllText(Path.Combine(target, "INSTALLED_VERSION.txt"), Version + Environment.NewLine);
        note("Installed " + (files + 1) + " files.");
        MakeShortcuts(options, editor, bin, note);
        return editor;
    }

    public static void StartEditor(string editor)
    {
        ProcessStartInfo start = new ProcessStartInfo(editor);
        start.WorkingDirectory = Path.GetDirectoryName(editor);
        start.UseShellExecute = false;
        Process.Start(start);
    }
}

// The setup window: a header, a page, and Back / Next / Cancel. Pages: welcome, options, installing, done (or failed).
class InstallerForm : Form
{
    enum Page { Welcome, Options, Installing, Done, Failed }

    readonly InstallOptions options;
    readonly long needed = Setup.UnpackedBytes();
    Page page;
    string editor;

    readonly Label headerTitle = new Label();
    readonly Label headerSubtitle = new Label();
    readonly Panel content = new Panel();
    readonly Button back = new Button();
    readonly Button next = new Button();
    readonly Button cancel = new Button();

    readonly Panel welcomePage = new Panel();
    readonly Panel optionsPage = new Panel();
    readonly Panel installingPage = new Panel();
    readonly Panel donePage = new Panel();

    readonly TextBox folder = new TextBox();
    readonly Label folderNote = new Label();
    readonly CheckBox startMenu = new CheckBox();
    readonly CheckBox desktop = new CheckBox();
    readonly CheckBox launchOption = new CheckBox();

    readonly Label status = new Label();
    readonly ProgressBar bar = new ProgressBar();
    readonly TextBox log = new TextBox();

    readonly Label doneText = new Label();
    readonly CheckBox launchNow = new CheckBox();

    public InstallerForm(InstallOptions initial)
    {
        options = initial;
        Text = "Aether Engine " + Setup.Version + " Setup";
        Font = new Font("Segoe UI", 9f);
        ClientSize = new Size(620, 430);
        MinimumSize = new Size(560, 400);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        AutoScaleMode = AutoScaleMode.Font;

        BuildHeader();
        BuildButtons();
        content.Dock = DockStyle.Fill;
        content.Padding = new Padding(24, 18, 24, 12);
        Controls.Add(content);
        content.BringToFront();
        BuildWelcome();
        BuildOptions();
        BuildInstalling();
        BuildDone();
        AcceptButton = next;
        CancelButton = cancel;
        FormClosing += OnFormClosing;
        GoTo(Page.Welcome);
    }

    void BuildHeader()
    {
        Panel header = new Panel();
        header.Dock = DockStyle.Top;
        header.Height = 70;
        header.BackColor = Color.White;
        headerTitle.Font = new Font("Segoe UI", 14f, FontStyle.Bold);
        headerTitle.AutoSize = true;
        headerTitle.Location = new Point(24, 12);
        headerSubtitle.AutoSize = true;
        headerSubtitle.ForeColor = Color.FromArgb(90, 90, 90);
        headerSubtitle.Location = new Point(26, 42);
        header.Controls.Add(headerTitle);
        header.Controls.Add(headerSubtitle);
        Panel rule = new Panel();
        rule.Dock = DockStyle.Bottom;
        rule.Height = 1;
        rule.BackColor = Color.FromArgb(205, 205, 205);
        header.Controls.Add(rule);
        Controls.Add(header);
    }

    void BuildButtons()
    {
        Panel bar = new Panel();
        bar.Dock = DockStyle.Bottom;
        bar.Height = 52;
        bar.BackColor = SystemColors.Control;
        Panel rule = new Panel();
        rule.Dock = DockStyle.Top;
        rule.Height = 1;
        rule.BackColor = Color.FromArgb(205, 205, 205);
        bar.Controls.Add(rule);
        FlowLayoutPanel flow = new FlowLayoutPanel();
        flow.Dock = DockStyle.Fill;
        flow.FlowDirection = FlowDirection.RightToLeft;
        flow.Padding = new Padding(12, 10, 12, 8);
        foreach (Button button in new Button[] { cancel, next, back })
        {
            button.Width = 96;
            button.Height = 30;
            button.Margin = new Padding(6, 0, 0, 0);
            flow.Controls.Add(button);
        }
        cancel.Text = "Cancel";
        back.Text = "< Back";
        // Enter presses whichever button has the focus, so Next must be the first stop, not Cancel (the first one added, which sits at the right).
        next.TabIndex = 0;
        back.TabIndex = 1;
        cancel.TabIndex = 2;
        cancel.Click += delegate { Close(); };
        back.Click += OnBack;
        next.Click += OnNext;
        bar.Controls.Add(flow);
        flow.BringToFront();
        Controls.Add(bar);
    }

    static Label Paragraph(string text, int top, int width)
    {
        Label label = new Label();
        label.Text = text;
        label.AutoSize = false;
        label.Location = new Point(0, top);
        label.Size = new Size(width, 44);
        return label;
    }

    void BuildWelcome()
    {
        welcomePage.Dock = DockStyle.Fill;
        Label hello = new Label();
        hello.Text = "Welcome to the Aether Engine setup";
        hello.Font = new Font("Segoe UI", 12f, FontStyle.Bold);
        hello.AutoSize = true;
        hello.Location = new Point(0, 8);
        welcomePage.Controls.Add(hello);
        welcomePage.Controls.Add(Paragraph("This installs the Aether editor, the player and the build tools, version " + Setup.Version + ", for your account. " +
                                           "It needs no administrator rights.", 48, 560));
        welcomePage.Controls.Add(Paragraph("If Aether is already installed in the folder you choose, it is replaced; " +
                                           "a running Aether editor from that folder is closed first (you are asked).", 100, 560));
        welcomePage.Controls.Add(Paragraph("Click Next to choose where to install.", 152, 560));
        content.Controls.Add(welcomePage);
    }

    void BuildOptions()
    {
        optionsPage.Dock = DockStyle.Fill;
        Label where = new Label();
        where.Text = "Install Aether in this folder:";
        where.AutoSize = true;
        where.Location = new Point(0, 6);
        optionsPage.Controls.Add(where);
        folder.Location = new Point(0, 28);
        folder.Width = 470;
        folder.TextChanged += delegate { UpdateFolderNote(); };
        optionsPage.Controls.Add(folder);
        Button browse = new Button();
        browse.Text = "Browse...";
        browse.Location = new Point(480, 26);
        browse.Size = new Size(84, 27);
        browse.Click += OnBrowse;
        optionsPage.Controls.Add(browse);
        folderNote.AutoSize = false;
        folderNote.Location = new Point(0, 58);
        folderNote.Size = new Size(560, 36);
        folderNote.ForeColor = Color.FromArgb(90, 90, 90);
        optionsPage.Controls.Add(folderNote);

        startMenu.Text = "Add Aether Editor to the Start menu";
        startMenu.AutoSize = true;
        startMenu.Location = new Point(0, 112);
        desktop.Text = "Add Aether Editor to the Desktop";
        desktop.AutoSize = true;
        desktop.Location = new Point(0, 140);
        launchOption.Text = "Offer to start the editor when setup finishes";
        launchOption.AutoSize = true;
        launchOption.Location = new Point(0, 168);
        optionsPage.Controls.Add(startMenu);
        optionsPage.Controls.Add(desktop);
        optionsPage.Controls.Add(launchOption);
        content.Controls.Add(optionsPage);
    }

    void BuildInstalling()
    {
        installingPage.Dock = DockStyle.Fill;
        status.AutoSize = false;
        status.Location = new Point(0, 6);
        status.Size = new Size(560, 22);
        bar.Location = new Point(0, 34);
        bar.Size = new Size(560, 22);
        bar.Minimum = 0;
        bar.Maximum = 100;
        log.Location = new Point(0, 68);
        log.Size = new Size(560, 200);
        log.Multiline = true;
        log.ReadOnly = true;
        log.ScrollBars = ScrollBars.Vertical;
        log.BackColor = Color.White;
        installingPage.Controls.Add(status);
        installingPage.Controls.Add(bar);
        installingPage.Controls.Add(log);
        content.Controls.Add(installingPage);
    }

    void BuildDone()
    {
        donePage.Dock = DockStyle.Fill;
        doneText.AutoSize = false;
        doneText.Location = new Point(0, 8);
        doneText.Size = new Size(560, 150);
        launchNow.Text = "Start the Aether editor";
        launchNow.AutoSize = true;
        launchNow.Location = new Point(0, 170);
        donePage.Controls.Add(doneText);
        donePage.Controls.Add(launchNow);
        content.Controls.Add(donePage);
    }

    void UpdateFolderNote()
    {
        string version = Setup.InstalledVersion(folder.Text.Trim());
        string size = "Setup needs " + (needed / 1024 / 1024) + " MB.";
        long free = Setup.FreeBytes(folder.Text.Trim());
        if (free >= 0) size += " " + (free / 1024 / 1024) + " MB are free on that drive.";
        folderNote.Text = (version != null ? "Aether " + version + " is installed there. It will be replaced by " + Setup.Version + ". " : "") + size;
    }

    void GoTo(Page which)
    {
        page = which;
        welcomePage.Visible = which == Page.Welcome;
        optionsPage.Visible = which == Page.Options;
        installingPage.Visible = which == Page.Installing;
        donePage.Visible = which == Page.Done || which == Page.Failed;
        back.Visible = which == Page.Options || which == Page.Failed;
        back.Enabled = true;
        cancel.Enabled = which != Page.Installing;
        cancel.Visible = which != Page.Done && which != Page.Failed;
        next.Enabled = which != Page.Installing;
        if (next.Enabled) ActiveControl = next;
        switch (which)
        {
            case Page.Welcome:
                headerTitle.Text = "Aether Engine " + Setup.Version;
                headerSubtitle.Text = "Setup";
                next.Text = "Next >";
                break;
            case Page.Options:
                headerTitle.Text = "Choose where to install";
                headerSubtitle.Text = "and what to add";
                next.Text = "Install";
                break;
            case Page.Installing:
                headerTitle.Text = "Installing";
                headerSubtitle.Text = "Please wait...";
                next.Text = "Install";
                break;
            case Page.Done:
                headerTitle.Text = "Setup is complete";
                headerSubtitle.Text = "Aether Engine " + Setup.Version + " is installed";
                next.Text = "Finish";
                break;
            case Page.Failed:
                headerTitle.Text = "Setup did not finish";
                headerSubtitle.Text = "Setup stopped; the reason is below";
                next.Text = "Close";
                break;
        }
    }

    void OnBrowse(object sender, EventArgs e)
    {
        using (FolderBrowserDialog dialog = new FolderBrowserDialog())
        {
            dialog.Description = "Choose the folder to install Aether in (it is made if it is not there).";
            dialog.SelectedPath = Directory.Exists(folder.Text) ? folder.Text : Path.GetDirectoryName(Setup.DefaultTarget());
            if (dialog.ShowDialog(this) == DialogResult.OK) folder.Text = dialog.SelectedPath;
        }
    }

    void OnBack(object sender, EventArgs e)
    {
        if (page == Page.Options) GoTo(Page.Welcome);
        else if (page == Page.Failed) GoTo(Page.Options);
    }

    void OnNext(object sender, EventArgs e)
    {
        switch (page)
        {
            case Page.Welcome:
                folder.Text = options.Target;
                startMenu.Checked = options.StartMenuShortcut;
                desktop.Checked = options.DesktopShortcut;
                launchOption.Checked = options.Launch;
                UpdateFolderNote();
                GoTo(Page.Options);
                break;
            case Page.Options:
                StartInstall();
                break;
            case Page.Done:
                if (launchNow.Checked && launchNow.Visible) { try { Setup.StartEditor(editor); } catch (Exception error) { MessageBox.Show(this, "The editor did not start: " + error.Message, Text); } }
                Close();
                break;
            case Page.Failed:
                Close();
                break;
        }
    }

    void OnFormClosing(object sender, FormClosingEventArgs e)
    {
        if (page == Page.Installing) e.Cancel = true; // half way through putting files in place: let it finish
    }

    void StartInstall()
    {
        string target = folder.Text.Trim().Trim('"');
        if (target.Length == 0)
        {
            MessageBox.Show(this, "Choose a folder to install in.", Text);
            return;
        }
        try { target = Path.GetFullPath(target); }
        catch (Exception)
        {
            MessageBox.Show(this, "That is not a folder name Windows accepts.", Text);
            return;
        }
        long free = Setup.FreeBytes(target);
        if (free >= 0 && needed > 0 && free < needed)
        {
            MessageBox.Show(this, "There is not enough room on that drive: setup needs " + (needed / 1024 / 1024) + " MB and " + (free / 1024 / 1024) + " MB are free.", Text);
            return;
        }
        InstallOptions chosen = new InstallOptions();
        chosen.Target = target;
        chosen.StartMenuShortcut = startMenu.Checked;
        chosen.DesktopShortcut = desktop.Checked;
        chosen.Launch = launchOption.Checked;

        List<Process> running = Setup.RunningFrom(target);
        if (running.Count > 0)
        {
            string question = "These programs are running from the install folder and must be closed first:" + Environment.NewLine + Environment.NewLine +
                              Setup.Describe(running) + Environment.NewLine + "Close them and continue?";
            if (MessageBox.Show(this, question, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        }

        log.Clear();
        bar.Value = 0;
        status.Text = "Starting...";
        GoTo(Page.Installing);
        Thread worker = new Thread(delegate() { RunInstall(chosen, running); });
        worker.IsBackground = true;
        worker.Start();
    }

    void Note(string line)
    {
        BeginInvoke(new Action(delegate
        {
            status.Text = line;
            log.AppendText(line + Environment.NewLine);
        }));
    }

    void RunInstall(InstallOptions chosen, List<Process> running)
    {
        StringBuilder record = new StringBuilder();
        try
        {
            string installed = Setup.Install(chosen, running,
                delegate(string line) { record.AppendLine(line); Note(line); },
                delegate(int percent) { BeginInvoke(new Action(delegate { bar.Value = Math.Max(0, Math.Min(100, percent)); })); });
            try { File.WriteAllText(Setup.LogPath, record.ToString()); } catch { }
            BeginInvoke(new Action(delegate
            {
                editor = installed;
                doneText.Text = "Aether Engine " + Setup.Version + " is installed in" + Environment.NewLine + Environment.NewLine + "    " + chosen.Target +
                                Environment.NewLine + Environment.NewLine + "The editor is bin\\" + Setup.EditorExe + ". If you start the editor from anywhere else " +
                                "(an old shortcut, an older download), you are running an older copy." + Environment.NewLine + Environment.NewLine +
                                "To use the SDK from CMake: cmake -DCMAKE_PREFIX_PATH=\"" + chosen.Target + "\"";
                launchNow.Visible = chosen.Launch;
                launchNow.Checked = chosen.Launch;
                GoTo(Page.Done);
                next.Focus();
            }));
        }
        catch (Exception error)
        {
            string message = error is IOException || error is InvalidOperationException ? error.Message : error.GetType().Name + ": " + error.Message;
            record.AppendLine("FAILED: " + message);
            try { File.WriteAllText(Setup.LogPath, record.ToString()); } catch { }
            BeginInvoke(new Action(delegate
            {
                doneText.Text = "Setup could not finish:" + Environment.NewLine + Environment.NewLine + message + Environment.NewLine + Environment.NewLine +
                                "The log is in " + Setup.LogPath;
                launchNow.Visible = false;
                GoTo(Page.Failed);
            }));
        }
    }
}

class AetherInstaller
{
    // The window has no console of its own; when started from one with /silent, write to that one.
    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int processId);

    [DllImport("user32.dll")]
    static extern bool SetProcessDPIAware();

    static void UseParentConsole()
    {
        try
        {
            if (!AttachConsole(-1)) return;
            StreamWriter output = new StreamWriter(Console.OpenStandardOutput());
            output.AutoFlush = true;
            Console.SetOut(output);
            StreamWriter errors = new StreamWriter(Console.OpenStandardError());
            errors.AutoFlush = true;
            Console.SetError(errors);
        }
        catch (Exception) { } // not Windows, or no console to attach to: the default streams stand
    }

    // /silent: no window, no questions.
    static int RunSilent(InstallOptions options)
    {
        UseParentConsole();
        StringBuilder record = new StringBuilder();
        Action<string> note = delegate(string line) { Console.WriteLine(line); record.AppendLine(line); };
        try
        {
            note("Aether Engine " + Setup.Version + " - application installer");
            note("");
            string target = Path.GetFullPath(options.Target);
            List<Process> running = Setup.RunningFrom(target);
            if (running.Count > 0)
            {
                note("These programs are running from the install folder and are closed first:");
                Console.Write(Setup.Describe(running));
            }
            string editor = Setup.Install(options, running, note, delegate(int percent) { });
            note("");
            note("The editor is " + editor);
            note("If you start the editor from anywhere else (an old shortcut, an older download), you are running an older copy.");
            note("");
            note("To use the SDK from CMake:");
            note("  cmake -S . -B build -DCMAKE_PREFIX_PATH=\"" + target + "\"");
            note("  find_package(Aether REQUIRED)");
            try { File.WriteAllText(Setup.LogPath, record.ToString()); } catch { }
            return 0;
        }
        catch (Exception error)
        {
            string message = error is IOException || error is InvalidOperationException ? error.Message : error.GetType().Name + ": " + error.Message;
            Console.Error.WriteLine();
            Console.Error.WriteLine("INSTALL FAILED: " + message);
            Console.Error.WriteLine("Log written to " + Setup.LogPath);
            record.AppendLine("FAILED: " + message);
            try { File.WriteAllText(Setup.LogPath, record.ToString()); } catch { }
            return 1;
        }
    }

    [STAThread]
    static int Main(string[] args)
    {
        bool silent = false;
        InstallOptions options = new InstallOptions();
        options.Target = Setup.DefaultTarget();
        foreach (string arg in args)
        {
            string a = arg.TrimStart('/', '-');
            if (a.Equals("silent", StringComparison.OrdinalIgnoreCase)) silent = true;
            else if (a.Equals("noshortcuts", StringComparison.OrdinalIgnoreCase)) { options.StartMenuShortcut = false; options.DesktopShortcut = false; }
            else if (a.Equals("nolaunch", StringComparison.OrdinalIgnoreCase)) options.Launch = false;
            else if (a.StartsWith("target:", StringComparison.OrdinalIgnoreCase))
            {
                string target = a.Substring("target:".Length).Trim('"');
                if (target.Length > 0) options.Target = Path.GetFullPath(target);
            }
        }
        if (silent) return RunSilent(options);

        try { SetProcessDPIAware(); } catch (Exception) { } // sharp, not stretched, on a high-DPI screen
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new InstallerForm(options));
        return 0;
    }
}
