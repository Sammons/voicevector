using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace VoiceVector.Shared
{
    /// <summary>One dictation on disk — see docs/storage-format.md.</summary>
    public class Entry
    {
        public string Id = "";
        public string Folder = "Inbox";
        public DateTimeOffset Date = DateTimeOffset.FromUnixTimeSeconds(0);
        public double Duration;
        public string SttLabel = "";
        public string CleanupLabel = "";
        public string Status = "complete";
        public string Cleaned = "";
        public string Raw = "";

        /// <summary>Screenshot context saved beside the entry (`<id>-screen-N.jpg`):
        /// count, 1-based index of the active display (0 = unknown), and whether
        /// the target window is outlined in that image.</summary>
        public int Screenshots;
        public int ActiveScreenshot;
        public bool ScreenshotOutline;

        public string AudioFilename { get { return Id + ".wav"; } }
        public string MarkdownFilename { get { return Id + ".md"; } }
        public string ScreenshotFilename(int index) { return Id + "-screen-" + index + ".jpg"; }

        /// <summary>Records the set's shape in the front matter fields.</summary>
        public void Attach(ScreenshotSet set)
        {
            Screenshots = set == null ? 0 : set.Images.Count;
            ActiveScreenshot = set == null || set.ActiveIndex < 0 ? 0 : set.ActiveIndex + 1;
            ScreenshotOutline = set != null && set.ActiveIndex >= 0 && set.Outlined;
        }
        public bool IsError { get { return Status.StartsWith("error"); } }
    }

    /// <summary>
    /// Files-first store: folders are directories under the library root,
    /// entries are WAV + Markdown pairs. Byte-compatible with the macOS app.
    /// </summary>
    public class Library
    {
        public string Root { get; private set; }

        public Library(string root)
        {
            Root = root;
            Directory.CreateDirectory(Path.Combine(root, "Inbox"));
        }

        public List<string> FolderNames()
        {
            var names = new List<string>();
            if (Directory.Exists(Root))
                names = Directory.EnumerateDirectories(Root)
                    .Select(Path.GetFileName)
                    .Where(n => n != null && !n.StartsWith("."))
                    .Where(n => n != "Inbox")
                    .OrderBy(n => n, StringComparer.OrdinalIgnoreCase)
                    .ToList();
            names.Insert(0, "Inbox");
            return names;
        }

        public static string Sanitize(string name)
        {
            return name.Trim().Replace("/", "-").Replace("\\", "-").Replace(":", "-");
        }

        public void CreateFolder(string name)
        {
            var sanitized = Sanitize(name);
            if (sanitized.Length > 0)
                Directory.CreateDirectory(Path.Combine(Root, sanitized));
        }

        public string FolderPath(string folder)
        {
            return Path.Combine(Root, folder);
        }

        public List<string> EntryIds(string folder)
        {
            var dir = FolderPath(folder);
            if (!Directory.Exists(dir)) return new List<string>();
            return Directory.EnumerateFiles(dir, "*.md")
                .Select(Path.GetFileNameWithoutExtension)
                .Where(id => id != null)
                .OrderByDescending(id => id, StringComparer.Ordinal)
                .ToList();
        }

        public int EntryCount(string folder)
        {
            return EntryIds(folder).Count;
        }

        public Entry GetEntry(string folder, string id)
        {
            var path = Path.Combine(FolderPath(folder), id + ".md");
            if (!File.Exists(path)) return null;
            return Parse(File.ReadAllText(path), id, folder);
        }

        public List<Entry> Entries(string folder, int offset, int limit)
        {
            return EntryIds(folder).Skip(offset).Take(limit)
                .Select(id => GetEntry(folder, id))
                .Where(e => e != null)
                .ToList();
        }

        public KeyValuePair<string, string> NewEntrySlot(string folder)
        {
            var dir = FolderPath(folder);
            Directory.CreateDirectory(dir);
            var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
            var id = stamp;
            int suffix = 1;
            while (File.Exists(Path.Combine(dir, id + ".wav")))
                id = stamp + "-" + (++suffix);
            return new KeyValuePair<string, string>(id, Path.Combine(dir, id + ".wav"));
        }

        public void Save(Entry entry)
        {
            File.WriteAllText(Path.Combine(FolderPath(entry.Folder), entry.MarkdownFilename),
                              Render(entry));
        }

        public void Delete(Entry entry)
        {
            var dir = FolderPath(entry.Folder);
            try { File.Delete(Path.Combine(dir, entry.MarkdownFilename)); } catch { }
            try { File.Delete(Path.Combine(dir, entry.AudioFilename)); } catch { }
            DeleteScreenshots(entry.Id, entry.Folder);
        }

        // -- screenshots (`<id>-screen-N.jpg`) --

        /// <summary>Writes the set's JPEGs beside the entry (replacing any earlier set).</summary>
        public void SaveScreenshots(string id, string folder, ScreenshotSet set)
        {
            DeleteScreenshots(id, folder);
            if (set == null) return;
            var dir = FolderPath(folder);
            for (int i = 0; i < set.Images.Count; i++)
            {
                try { File.WriteAllBytes(Path.Combine(dir, id + "-screen-" + (i + 1) + ".jpg"), set.Images[i]); }
                catch (Exception e) { Log.Error("Failed to save screenshot " + (i + 1) + " for " + id + ": " + e.Message); }
            }
        }

        /// <summary>The set recorded in the entry's front matter, if its files are present.</summary>
        public ScreenshotSet LoadScreenshots(Entry entry)
        {
            if (entry.Screenshots <= 0) return null;
            var dir = FolderPath(entry.Folder);
            var set = new ScreenshotSet();
            for (int i = 1; i <= entry.Screenshots; i++)
            {
                try { set.Images.Add(File.ReadAllBytes(Path.Combine(dir, entry.ScreenshotFilename(i)))); }
                catch { }
            }
            if (set.IsEmpty) return null;
            if (entry.ActiveScreenshot > 0 && entry.ActiveScreenshot <= set.Images.Count)
            {
                set.ActiveIndex = entry.ActiveScreenshot - 1;
                set.Outlined = entry.ScreenshotOutline;
            }
            return set;
        }

        public void DeleteScreenshots(string id, string folder)
        {
            var dir = FolderPath(folder);
            if (!Directory.Exists(dir)) return;
            foreach (var file in Directory.GetFiles(dir, id + "-screen-*.jpg"))
                try { File.Delete(file); } catch { }
        }

        public string AudioPath(Entry entry)
        {
            return Path.Combine(FolderPath(entry.Folder), entry.AudioFilename);
        }

        // -- markdown format (must match apps/macos, docs/storage-format.md) --

        public static string RenderDate(DateTimeOffset date)
        {
            return date.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'",
                                                   CultureInfo.InvariantCulture);
        }

        public static string Render(Entry entry)
        {
            var sb = new StringBuilder();
            sb.Append("---\n");
            sb.Append("date: ").Append(RenderDate(entry.Date)).Append('\n');
            sb.Append("duration: ").Append(entry.Duration.ToString("F1", CultureInfo.InvariantCulture)).Append('\n');
            sb.Append("audio: ").Append(entry.AudioFilename).Append('\n');
            sb.Append("stt: ").Append(entry.SttLabel).Append('\n');
            if (entry.CleanupLabel.Length > 0)
                sb.Append("cleanup: ").Append(entry.CleanupLabel).Append('\n');
            sb.Append("status: ").Append(entry.Status).Append('\n');
            if (entry.Screenshots > 0)
            {
                sb.Append("screenshots: ").Append(entry.Screenshots).Append('\n');
                if (entry.ActiveScreenshot > 0)
                {
                    sb.Append("activeScreenshot: ").Append(entry.ActiveScreenshot).Append('\n');
                    if (entry.ScreenshotOutline) sb.Append("screenshotOutline: true\n");
                }
            }
            sb.Append("---\n\n");
            sb.Append(entry.Cleaned);
            if (entry.Raw.Length > 0 && entry.Raw != entry.Cleaned)
            {
                sb.Append("\n\n## Raw transcript\n\n");
                sb.Append(entry.Raw);
            }
            sb.Append('\n');
            return sb.ToString();
        }

        public static Entry Parse(string markdown, string id, string folder)
        {
            var entry = new Entry { Id = id, Folder = folder };
            var lines = markdown.Replace("\r\n", "\n").Split('\n').ToList();

            if (lines.Count > 0 && lines[0] == "---")
            {
                int end = lines.FindIndex(1, l => l == "---");
                if (end > 0)
                {
                    for (int i = 1; i < end; i++)
                    {
                        var line = lines[i];
                        int colon = line.IndexOf(':');
                        if (colon < 0) continue;
                        var key = line.Substring(0, colon);
                        var value = line.Substring(colon + 1).Trim();
                        switch (key)
                        {
                            case "date":
                                DateTimeOffset date;
                                if (DateTimeOffset.TryParse(value, CultureInfo.InvariantCulture,
                                        DateTimeStyles.AdjustToUniversal, out date))
                                    entry.Date = date;
                                break;
                            case "duration":
                                double duration;
                                if (double.TryParse(value, NumberStyles.Float,
                                        CultureInfo.InvariantCulture, out duration))
                                    entry.Duration = duration;
                                break;
                            case "stt": entry.SttLabel = value; break;
                            case "cleanup": entry.CleanupLabel = value; break;
                            case "status": entry.Status = value; break;
                            case "screenshots": int.TryParse(value, out entry.Screenshots); break;
                            case "activeScreenshot": int.TryParse(value, out entry.ActiveScreenshot); break;
                            case "screenshotOutline": entry.ScreenshotOutline = value == "true"; break;
                        }
                    }
                    lines.RemoveRange(0, end + 1);
                }
            }

            var body = string.Join("\n", lines);
            const string marker = "\n## Raw transcript\n";
            int rawIndex = body.IndexOf(marker, StringComparison.Ordinal);
            if (rawIndex >= 0)
            {
                entry.Cleaned = body.Substring(0, rawIndex).Trim();
                entry.Raw = body.Substring(rawIndex + marker.Length).Trim();
            }
            else
            {
                entry.Cleaned = body.Trim();
                entry.Raw = entry.Cleaned;
            }

            if (entry.Date == DateTimeOffset.FromUnixTimeSeconds(0))
            {
                DateTime fromId;
                var head = id.Length >= 15 ? id.Substring(0, 15) : id;
                if (DateTime.TryParseExact(head, "yyyyMMdd-HHmmss",
                        CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out fromId))
                    entry.Date = fromId;
            }
            return entry;
        }
    }
}
