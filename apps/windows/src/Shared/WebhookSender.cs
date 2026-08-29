using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading.Tasks;

namespace VoiceVector.Shared
{
    /// <summary>Folder webhook delivery — payload per docs/webhook-payload.md.</summary>
    public static class WebhookSender
    {
        public static string BuildPayload(Entry entry, string app = "voicevector-windows")
        {
            return Json.Write(new Dictionary<string, object>
            {
                { "app", app },
                { "id", entry.Id },
                { "folder", entry.Folder },
                { "date", Library.RenderDate(entry.Date) },
                { "duration", entry.Duration },
                { "raw", entry.Raw },
                { "cleaned", entry.Cleaned },
                { "stt", entry.SttLabel },
                { "cleanup", entry.CleanupLabel },
            });
        }

        public static async Task SendAsync(Entry entry, string audioPath, WebhookConfig config)
        {
            Uri url;
            if (!config.Enabled || !Uri.TryCreate(config.Url, UriKind.Absolute, out url)) return;

            for (int attempt = 1; attempt <= 3; attempt++)
            {
                try
                {
                    using (var request = new HttpRequestMessage(HttpMethod.Post, url))
                    {
                        if (config.IncludeAudio && File.Exists(audioPath))
                        {
                            var form = new MultipartFormDataContent();
                            form.Add(new StringContent(BuildPayload(entry)), "payload");
                            var audio = new ByteArrayContent(File.ReadAllBytes(audioPath));
                            audio.Headers.ContentType = new MediaTypeHeaderValue("audio/wav");
                            form.Add(audio, "audio", entry.AudioFilename);
                            request.Content = form;
                        }
                        else
                        {
                            request.Content = new StringContent(BuildPayload(entry),
                                                                Encoding.UTF8, "application/json");
                        }
                        using (var response = await ProviderClient.Http.SendAsync(request).ConfigureAwait(false))
                        {
                            response.EnsureSuccessStatusCode();
                        }
                        return;
                    }
                }
                catch (Exception e)
                {
                    Log.Error("Webhook attempt " + attempt + " for " + entry.Id + " failed: " + e.Message);
                    if (attempt < 3)
                        await Task.Delay(TimeSpan.FromSeconds(2 * attempt)).ConfigureAwait(false);
                }
            }
        }
    }
}
