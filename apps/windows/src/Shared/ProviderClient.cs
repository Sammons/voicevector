using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading.Tasks;

namespace VoiceVector.Shared
{
    public class TranscriptionResult
    {
        public string Text;
        public string Provider;
        public string Model;
    }

    /// <summary>
    /// All provider HTTP calls — see docs/providers.md. Portable HttpClient
    /// only (works on .NET Framework 4.8 and modern .NET alike).
    /// </summary>
    public class ProviderClient
    {
        public static readonly HttpClient Http = CreateClient();

        private static HttpClient CreateClient()
        {
            var client = new HttpClient();
            client.Timeout = TimeSpan.FromSeconds(300);
            return client;
        }

        private readonly ProviderProfile _profile;
        private readonly string _apiKey;

        public ProviderClient(ProviderProfile profile, string apiKey)
        {
            _profile = profile;
            _apiKey = apiKey ?? "";
        }

        private string BearerKey { get { return _apiKey.Length == 0 ? "voicevector" : _apiKey; } }

        private string Base { get { return _profile.BaseUrl.TrimEnd('/'); } }

        private HttpRequestMessage NewRequest(HttpMethod method, string url)
        {
            var request = new HttpRequestMessage(method, url);
            if (_profile.Kind == ProviderKind.ElevenLabs)
                request.Headers.Add("xi-api-key", _apiKey);
            else
                request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", BearerKey);
            return request;
        }

        private static async Task<Dictionary<string, object>> SendAsync(HttpRequestMessage request)
        {
            using (var response = await ProviderClient.Http.SendAsync(request).ConfigureAwait(false))
            {
                var body = await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                if (!response.IsSuccessStatusCode)
                {
                    var snippet = body.Length > 300 ? body.Substring(0, 300) : body;
                    throw new HttpRequestException("HTTP " + (int)response.StatusCode + ": " + snippet);
                }
                return Json.ParseObject(body);
            }
        }

        // -- STT --------------------------------------------------------------

        public async Task<TranscriptionResult> TranscribeAsync(byte[] audio, string filename,
                                                               IList<string> vocabulary)
        {
            switch (_profile.Kind)
            {
                case ProviderKind.Fireworks:
                case ProviderKind.Cerebras:
                    throw new InvalidOperationException(
                        _profile.Kind.DisplayName() + " does not offer transcription — pick another STT provider.");
                case ProviderKind.ElevenLabs:
                    return await TranscribeElevenLabsAsync(audio, filename, vocabulary).ConfigureAwait(false);
                case ProviderKind.VercelGateway:
                    return await TranscribeVercelAsync(audio).ConfigureAwait(false);
                default:
                    return await TranscribeOpenAIAsync(audio, filename, vocabulary).ConfigureAwait(false);
            }
        }

        private async Task<TranscriptionResult> TranscribeElevenLabsAsync(
            byte[] audio, string filename, IList<string> vocabulary)
        {
            var request = NewRequest(HttpMethod.Post, Base + "/v1/speech-to-text");
            var form = new MultipartFormDataContent();
            form.Add(new StringContent(_profile.SttModel), "model_id");
            form.Add(new StringContent("false"), "tag_audio_events");
            foreach (var term in vocabulary.Take(100))
                form.Add(new StringContent(term.Length > 50 ? term.Substring(0, 50) : term), "keyterms");
            var file = new ByteArrayContent(audio);
            file.Headers.ContentType = new MediaTypeHeaderValue("audio/wav");
            form.Add(file, "file", filename);
            request.Content = form;

            var json = await SendAsync(request).ConfigureAwait(false);
            return Result(json);
        }

        private async Task<TranscriptionResult> TranscribeOpenAIAsync(
            byte[] audio, string filename, IList<string> vocabulary)
        {
            var request = NewRequest(HttpMethod.Post, Base + "/audio/transcriptions");
            var form = new MultipartFormDataContent();
            form.Add(new StringContent(_profile.SttModel), "model");
            form.Add(new StringContent("json"), "response_format");
            if (vocabulary.Count > 0)
                form.Add(new StringContent(string.Join(", ", vocabulary)), "prompt");
            var file = new ByteArrayContent(audio);
            file.Headers.ContentType = new MediaTypeHeaderValue("audio/wav");
            form.Add(file, "file", filename);
            request.Content = form;

            var json = await SendAsync(request).ConfigureAwait(false);
            return Result(json);
        }

        private async Task<TranscriptionResult> TranscribeVercelAsync(byte[] audio)
        {
            var root = Base.EndsWith("/v1") ? Base.Substring(0, Base.Length - 3) : Base;
            var request = NewRequest(HttpMethod.Post, root + "/v4/ai/transcription-model");
            request.Headers.Add("ai-model-id", _profile.SttModel);
            // Both enforced by the gateway though its docs' cURL omits them
            // (values mirror @ai-sdk/gateway; verified empirically).
            request.Headers.Add("ai-gateway-protocol-version", "0.0.1");
            request.Headers.Add("ai-transcription-model-specification-version", "4");
            var payload = new Dictionary<string, object>
            {
                { "audio", Convert.ToBase64String(audio) },
                { "mediaType", "audio/wav" },
            };
            request.Content = new StringContent(Json.Write(payload), Encoding.UTF8, "application/json");

            var json = await SendAsync(request).ConfigureAwait(false);
            return Result(json);
        }

        private TranscriptionResult Result(Dictionary<string, object> json)
        {
            var text = Json.Str(json, "text", null);
            if (text == null)
                throw new InvalidOperationException("no `text` field in transcription response");
            return new TranscriptionResult { Text = text, Provider = _profile.Name, Model = _profile.SttModel };
        }

        // -- chat (cleanup) ----------------------------------------------------

        public Task<string> ChatAsync(string system, string user, double temperature = 0.2)
        {
            return ChatAsync(system, user, null, temperature);
        }

        /// <summary>`image` (JPEG) rides along as an OpenAI image_url content
        /// part; models without vision reject it, so callers retry without.</summary>
        public async Task<string> ChatAsync(string system, string user, byte[] image, double temperature = 0.2)
        {
            if (!_profile.Kind.SupportsChat())
                throw new InvalidOperationException(_profile.Kind.DisplayName() + " has no chat endpoint");
            object userContent = user;
            if (image != null)
            {
                userContent = new List<object>
                {
                    new Dictionary<string, object> { { "type", "text" }, { "text", user } },
                    ImagePart(image),
                };
            }
            var payload = new Dictionary<string, object>
            {
                { "model", _profile.ChatModel },
                { "messages", new List<object>
                    {
                        new Dictionary<string, object> { { "role", "system" }, { "content", system } },
                        new Dictionary<string, object> { { "role", "user" }, { "content", userContent } },
                    } },
                { "temperature", temperature },
            };
            return await ChatPayloadAsync(payload).ConfigureAwait(false);
        }

        private static Dictionary<string, object> ImagePart(byte[] jpeg)
        {
            return new Dictionary<string, object>
            {
                { "type", "image_url" },
                { "image_url", new Dictionary<string, object>
                    {
                        { "url", "data:image/jpeg;base64," + Convert.ToBase64String(jpeg) },
                        { "detail", "low" },
                    } },
            };
        }

        /// <summary>One-shot audio → text (OpenAI input_audio content part).</summary>
        public async Task<string> ChatWithAudioAsync(string system, byte[] audio, double temperature = 0.2)
        {
            if (!_profile.Kind.SupportsChat())
                throw new InvalidOperationException(_profile.Kind.DisplayName() + " has no chat endpoint");
            var payload = new Dictionary<string, object>
            {
                { "model", _profile.ChatModel },
                { "messages", new List<object>
                    {
                        new Dictionary<string, object> { { "role", "system" }, { "content", system } },
                        new Dictionary<string, object>
                        {
                            { "role", "user" },
                            { "content", new List<object>
                                {
                                    new Dictionary<string, object>
                                    {
                                        { "type", "input_audio" },
                                        { "input_audio", new Dictionary<string, object>
                                            {
                                                { "data", Convert.ToBase64String(audio) },
                                                { "format", "wav" },
                                            } },
                                    },
                                } },
                        },
                    } },
                { "temperature", temperature },
            };
            return await ChatPayloadAsync(payload).ConfigureAwait(false);
        }

        private async Task<string> ChatPayloadAsync(Dictionary<string, object> payload)
        {
            var request = NewRequest(HttpMethod.Post, Base + "/chat/completions");
            request.Content = new StringContent(Json.Write(payload), Encoding.UTF8, "application/json");
            var json = await SendAsync(request).ConfigureAwait(false);
            var choices = Json.Arr(json, "choices");
            if (choices != null && choices.Count > 0)
            {
                var message = Json.Obj(choices[0] as Dictionary<string, object> ?? new Dictionary<string, object>(), "message");
                if (message != null)
                {
                    var content = Json.Str(message, "content", null);
                    if (content != null) return content;
                }
            }
            throw new InvalidOperationException("no choices[0].message.content in chat response");
        }

        // -- models & connectivity test ---------------------------------------

        public async Task<List<string>> ListModelsAsync()
        {
            if (!_profile.Kind.SupportsModelListing()) return new List<string>();
            var json = await SendAsync(NewRequest(HttpMethod.Get, Base + "/models")).ConfigureAwait(false);
            var data = Json.Arr(json, "data");
            if (data == null)
                throw new InvalidOperationException("no `data` array in models response");
            return data.OfType<Dictionary<string, object>>()
                .Select(m => Json.Str(m, "id", null))
                .Where(id => id != null)
                .OrderBy(id => id, StringComparer.Ordinal)
                .ToList();
        }

        public async Task<string> TestAsync()
        {
            switch (_profile.Kind)
            {
                case ProviderKind.ElevenLabs:
                    await TranscribeSampleAsync().ConfigureAwait(false);
                    return "Connected — speech-to-text works (" + _profile.SttModel + ").";
                case ProviderKind.Fireworks:
                case ProviderKind.Cerebras:
                case ProviderKind.VercelGateway:
                    {
                        var models = await ListModelsAsync().ConfigureAwait(false);
                        return models.Count == 0 ? "Connected."
                            : "Connected — " + models.Count + " models available.";
                    }
                default:
                    try
                    {
                        var models = await ListModelsAsync().ConfigureAwait(false);
                        return models.Count == 0 ? "Connected."
                            : "Connected — " + models.Count + " models available.";
                    }
                    catch
                    {
                        await TranscribeSampleAsync().ConfigureAwait(false);
                        return "Connected — speech-to-text works (" + _profile.SttModel + ").";
                    }
            }
        }

        private Task<TranscriptionResult> TranscribeSampleAsync()
        {
            return TranscribeAsync(WavWriter.SilentWav(0.3), "test.wav", new List<string>());
        }
    }
}
