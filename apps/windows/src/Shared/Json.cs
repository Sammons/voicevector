using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace VoiceVector.Shared
{
    /// <summary>
    /// Minimal JSON parser/writer (netstandard2-compatible, zero dependencies —
    /// old-framework .NET has no built-in JSON). Values map to: Dictionary
    /// &lt;string, object&gt;, List&lt;object&gt;, string, double, bool, null.
    /// We control every schema this touches (config, provider APIs, webhooks),
    /// so a small strict parser is sufficient.
    /// </summary>
    public static class Json
    {
        // -- parse ------------------------------------------------------------

        public static object Parse(string text)
        {
            int pos = 0;
            var value = ParseValue(text, ref pos);
            SkipWhitespace(text, ref pos);
            if (pos != text.Length) throw new FormatException("Trailing content at " + pos);
            return value;
        }

        public static Dictionary<string, object> ParseObject(string text)
        {
            return Parse(text) as Dictionary<string, object>
                   ?? throw new FormatException("Expected a JSON object");
        }

        private static object ParseValue(string s, ref int pos)
        {
            SkipWhitespace(s, ref pos);
            if (pos >= s.Length) throw new FormatException("Unexpected end of JSON");
            char c = s[pos];
            switch (c)
            {
                case '{': return ParseObjectBody(s, ref pos);
                case '[': return ParseArray(s, ref pos);
                case '"': return ParseString(s, ref pos);
                case 't': Expect(s, ref pos, "true"); return true;
                case 'f': Expect(s, ref pos, "false"); return false;
                case 'n': Expect(s, ref pos, "null"); return null;
                default: return ParseNumber(s, ref pos);
            }
        }

        private static Dictionary<string, object> ParseObjectBody(string s, ref int pos)
        {
            var result = new Dictionary<string, object>();
            pos++; // {
            SkipWhitespace(s, ref pos);
            if (Peek(s, pos) == '}') { pos++; return result; }
            while (true)
            {
                SkipWhitespace(s, ref pos);
                var key = ParseString(s, ref pos);
                SkipWhitespace(s, ref pos);
                if (Peek(s, pos) != ':') throw new FormatException("Expected ':' at " + pos);
                pos++;
                result[key] = ParseValue(s, ref pos);
                SkipWhitespace(s, ref pos);
                char n = Peek(s, pos);
                if (n == ',') { pos++; continue; }
                if (n == '}') { pos++; return result; }
                throw new FormatException("Expected ',' or '}' at " + pos);
            }
        }

        private static List<object> ParseArray(string s, ref int pos)
        {
            var result = new List<object>();
            pos++; // [
            SkipWhitespace(s, ref pos);
            if (Peek(s, pos) == ']') { pos++; return result; }
            while (true)
            {
                result.Add(ParseValue(s, ref pos));
                SkipWhitespace(s, ref pos);
                char n = Peek(s, pos);
                if (n == ',') { pos++; continue; }
                if (n == ']') { pos++; return result; }
                throw new FormatException("Expected ',' or ']' at " + pos);
            }
        }

        private static string ParseString(string s, ref int pos)
        {
            if (Peek(s, pos) != '"') throw new FormatException("Expected string at " + pos);
            pos++;
            var sb = new StringBuilder();
            while (true)
            {
                if (pos >= s.Length) throw new FormatException("Unterminated string");
                char c = s[pos++];
                if (c == '"') return sb.ToString();
                if (c != '\\') { sb.Append(c); continue; }
                if (pos >= s.Length) throw new FormatException("Unterminated escape");
                char e = s[pos++];
                switch (e)
                {
                    case '"': sb.Append('"'); break;
                    case '\\': sb.Append('\\'); break;
                    case '/': sb.Append('/'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'u':
                        if (pos + 4 > s.Length) throw new FormatException("Bad \\u escape");
                        sb.Append((char)ushort.Parse(s.Substring(pos, 4), NumberStyles.HexNumber,
                                                     CultureInfo.InvariantCulture));
                        pos += 4;
                        break;
                    default: throw new FormatException("Bad escape '\\" + e + "'");
                }
            }
        }

        private static double ParseNumber(string s, ref int pos)
        {
            int start = pos;
            while (pos < s.Length && (char.IsDigit(s[pos]) || "+-.eE".IndexOf(s[pos]) >= 0)) pos++;
            var slice = s.Substring(start, pos - start);
            if (!double.TryParse(slice, NumberStyles.Float, CultureInfo.InvariantCulture, out var value))
                throw new FormatException("Bad number '" + slice + "' at " + start);
            return value;
        }

        private static void Expect(string s, ref int pos, string word)
        {
            if (pos + word.Length > s.Length || s.Substring(pos, word.Length) != word)
                throw new FormatException("Expected '" + word + "' at " + pos);
            pos += word.Length;
        }

        private static char Peek(string s, int pos)
        {
            return pos < s.Length ? s[pos] : '\0';
        }

        private static void SkipWhitespace(string s, ref int pos)
        {
            while (pos < s.Length && char.IsWhiteSpace(s[pos])) pos++;
        }

        // -- write ------------------------------------------------------------

        public static string Write(object value, bool indented = false)
        {
            var sb = new StringBuilder();
            WriteValue(sb, value, indented, 0);
            return sb.ToString();
        }

        private static void WriteValue(StringBuilder sb, object value, bool indented, int depth)
        {
            if (value == null) { sb.Append("null"); return; }
            if (value is string s) { WriteString(sb, s); return; }
            if (value is bool b) { sb.Append(b ? "true" : "false"); return; }
            if (value is Dictionary<string, object> dict) { WriteDict(sb, dict, indented, depth); return; }
            if (value is List<object> list) { WriteList(sb, list, indented, depth); return; }
            if (value is double d)
            {
                sb.Append(d == Math.Floor(d) && Math.Abs(d) < 1e15
                    ? ((long)d).ToString(CultureInfo.InvariantCulture)
                    : d.ToString("R", CultureInfo.InvariantCulture));
                return;
            }
            if (value is int i) { sb.Append(i.ToString(CultureInfo.InvariantCulture)); return; }
            if (value is long l) { sb.Append(l.ToString(CultureInfo.InvariantCulture)); return; }
            throw new ArgumentException("Unsupported JSON value type: " + value.GetType());
        }

        private static void WriteDict(StringBuilder sb, Dictionary<string, object> dict,
                                      bool indented, int depth)
        {
            if (dict.Count == 0) { sb.Append("{}"); return; }
            sb.Append('{');
            bool first = true;
            foreach (var pair in dict)
            {
                if (!first) sb.Append(',');
                first = false;
                NewlineIndent(sb, indented, depth + 1);
                WriteString(sb, pair.Key);
                sb.Append(indented ? ": " : ":");
                WriteValue(sb, pair.Value, indented, depth + 1);
            }
            NewlineIndent(sb, indented, depth);
            sb.Append('}');
        }

        private static void WriteList(StringBuilder sb, List<object> list, bool indented, int depth)
        {
            if (list.Count == 0) { sb.Append("[]"); return; }
            sb.Append('[');
            bool first = true;
            foreach (var item in list)
            {
                if (!first) sb.Append(',');
                first = false;
                NewlineIndent(sb, indented, depth + 1);
                WriteValue(sb, item, indented, depth + 1);
            }
            NewlineIndent(sb, indented, depth);
            sb.Append(']');
        }

        private static void WriteString(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\b': sb.Append("\\b"); break;
                    case '\f': sb.Append("\\f"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
        }

        private static void NewlineIndent(StringBuilder sb, bool indented, int depth)
        {
            if (!indented) return;
            sb.Append('\n');
            for (int i = 0; i < depth; i++) sb.Append("  ");
        }

        // -- typed accessors ---------------------------------------------------

        public static string Str(Dictionary<string, object> d, string key, string fallback = "")
        {
            return d.TryGetValue(key, out var v) && v is string s ? s : fallback;
        }

        public static bool Bool(Dictionary<string, object> d, string key, bool fallback)
        {
            return d.TryGetValue(key, out var v) && v is bool b ? b : fallback;
        }

        public static double Num(Dictionary<string, object> d, string key, double fallback)
        {
            return d.TryGetValue(key, out var v) && v is double n ? n : fallback;
        }

        public static Dictionary<string, object> Obj(Dictionary<string, object> d, string key)
        {
            return d.TryGetValue(key, out var v) ? v as Dictionary<string, object> : null;
        }

        public static List<object> Arr(Dictionary<string, object> d, string key)
        {
            return d.TryGetValue(key, out var v) ? v as List<object> : null;
        }
    }
}
