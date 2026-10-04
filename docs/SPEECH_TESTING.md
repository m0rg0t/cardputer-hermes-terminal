# Speech setup and verification

The Cardputer calls the Dashboard's `/api/audio/speak` and
`/api/audio/transcribe` routes with the selected `hermes_profile`. Speech
providers and their credentials belong on the Hermes server. A configured
provider name alone does not establish that speech works.

## Repeatable checks

`scripts/check_hermes_speech.py` synthesizes a known phrase, decodes it with
ffmpeg into the device's 16 kHz mono PCM WAV format, and uploads that WAV for
transcription. It verifies the returned words, prints JSON results, and exits
nonzero on a failure. Results include synthesis, transcription and total
elapsed seconds. Audio files are temporary. Provider error bodies and
credentials are not printed.

For a complete HTTP check, set `HERMES_TEST_COOKIE`, `HERMES_TEST_TOKEN`, or
both `HERMES_TEST_USERNAME` and `HERMES_TEST_PASSWORD` in the shell environment.
Avoid putting credentials directly in a command or saving them in Git.

```bash
python3 scripts/check_hermes_speech.py \
  --base-url https://hermes.example.com --profile default --profile work
```

An SSH owner can test the installed route handlers without a Dashboard login:

```bash
ssh minironyserver_remote \
  'cd ~/.hermes/hermes-agent && venv/bin/python - --handlers --profile default --profile work' \
  < scripts/check_hermes_speech.py
```

Use the interpreter that runs your Dashboard if its installation differs.
Handler mode exercises profile resolution and the actual speech providers,
but does **not** verify cookies, the HTTP middleware, or the reverse proxy.

For Russian, add:

```bash
--text 'Терминал готов. Проверка записи голоса.' \
--expected 'терминал готов проверка голоса'
```

The device's microphone, speaker, retry, cancellation, and reconnect still
need the [device acceptance tests](DEVICE_TEST_PLAN.md#audio).

## Isolated local transcription

When a cloud provider lacks credentials or access, an isolated Whisper command
provider can use the server's existing CPU and cached multilingual `base`
model. This avoids changing the main Hermes Python environment. Install
`faster-whisper==1.2.1` in a dedicated Python environment and copy
`scripts/hermes_local_stt.py` to the server. Populate the model cache during
setup: the wrapper deliberately refuses to download a model during an upload.

Configure the named command provider on each intended profile, using absolute
paths for its Python executable and wrapper:

```yaml
stt:
  enabled: true
  provider: cardputer_whisper
  providers:
    cardputer_whisper:
      type: command
      command: /PATH/TO/venv/bin/python /PATH/TO/hermes_local_stt.py --input {input_path} --output {output_path} --model {model} --language {language}
      model: base
      language: auto
      output_format: txt
      timeout_seconds: 120
tts:
  provider: edge
  edge:
    voice: en-US-AvaMultilingualNeural
```

Merge these fields into the existing speech sections, preserving other settings.
The installed Hermes version must support `stt.providers.<name>` command
providers. Back up the configuration before changing it. Edge synthesis still
requires network access; the Whisper wrapper runs locally on CPU with int8
weights and two threads. A multilingual voice lets English and Russian share
the same settings.

After installing the runtime and wrapper, the migration helper can prepare
existing profiles without exposing their credentials:

```bash
ssh minironyserver_remote \
  '~/.hermes/hermes-agent/venv/bin/python - --profile health --profile miniksuhealth' \
  < scripts/configure_hermes_speech.py
```

Add `--apply` after the profile arguments to save private backups and apply
the changes. The helper preserves unrelated settings and supports profiles
with missing speech sections. Run the English and Russian round trips above
after applying a migration. It requires PyYAML in the server interpreter.
The helper passed a server dry run for all four profiles, applied the two
remaining migrations, and then reported no changes on a second dry run.

## Review findings, 2026-10-04

The server audit used `minironyserver_remote`, Hermes `0.21.3`, source commit
`c2af461706`. Initial checks found:

| Profile | TTS | STT |
|---|---|---|
| `default` | Edge succeeded | OpenAI selected without an available key |
| `work` | xAI credentials unavailable in the profile scope | xAI selected; initial TTS failure prevented the round trip |
| `health` | ElevenLabs returned a Cloudflare HTTP 403 | Local selected; round trip stopped at TTS |
| `miniksuhealth` | Edge succeeded | xAI rejected the account for credits/subscription access |

All four profiles now use the configuration above. Previous configurations
are backed up under:

- `~/.hermes/maintenance/cardputer-speech-20261004T024608Z/` (`default`, `work`).
- `~/.hermes/maintenance/cardputer-speech-20261004T074620Z-jxz185jq/`
  (`health`, `miniksuhealth`).

The isolated runtime and scripts live under
`~/.hermes/tools/cardputer-speech-venv/` and
`~/.hermes/tools/cardputer-speech/`. The migration preserves unrelated
configuration values. All four profiles passed an idempotent dry run after
the switch.

All four profiles passed separate English and Russian handler round trips
(eight successful checks): Edge returned MP3, ffmpeg decoded it to the device
WAV format, and `cardputer_whisper` recognized the expected words in each
language. The Dashboard and all four gateways remained active after the
changes; Edge is available in both the Dashboard and managed gateway runtimes.

The Russian phrase was `Терминал готов. Проверка записи голоса.`. Timings
include command-provider startup and loading the cached model:

| Profile | Edge TTS (seconds) | Local STT (seconds) | Total (seconds) |
|---|---:|---:|---:|
| `default` | 1.34 | 41.56 | 43.16 |
| `work` | 1.21 | 39.30 | 40.79 |
| `health` | 0.88 | 37.95 | 39.09 |
| `miniksuhealth` | 0.84 | 42.72 | 43.84 |

Speech works in these handler tests, but local transcription latency on this
CPU remains a usage limitation. Longer recordings and concurrent requests
have not been benchmarked. The command-provider timeout is 120 seconds.

The public HTTPS Dashboard and loopback Dashboard both report Basic
authentication and reject unauthenticated audio requests with HTTP 401.
Authenticated HTTP speech testing requires an existing login or Cookie;
the server has only the password hash, so handler testing cannot certify that
last authentication step.

The firmware review also fixed the TTS `data_url` parser consuming its colon,
an undersized base64 upload buffer, truncated/overflowing WAV chunk handling,
JSON provider-error parsing, and timers that failed when `millis()` wrapped.
Portable regression tests cover the stream, WAV chunks, and clock rollover;
the upload tests can additionally use real mbedTLS with
`-DHERMES_TEST_MBEDTLS` and `-lmbedcrypto`.

All eight native test programs, the real-mbedTLS upload tests, and the WAV
tests under UndefinedBehaviorSanitizer passed. AddressSanitizer on this Mac
stalled in its runtime initialization before `main`; that run was stopped.
Both firmware profiles built successfully. Final M5Apps slot margins are
5,072 bytes for the standard profile and 2,432 bytes for the Web profile.

## Nemotron 3.5 multilingual alternative

NVIDIA publishes
[`nemotron-3.5-asr-streaming-0.6b`](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b),
a 600M-parameter streaming ASR model. English and Russian are among its
transcription-ready languages. It is a separate STT model, not a Whisper
variant or a TTS provider.

The official [NeMo-Speech.cpp runtime](https://github.com/NVIDIA/NeMo-Speech.cpp)
supports CPU execution, including Linux x86_64. Its
[CLI](https://github.com/NVIDIA/NeMo-Speech.cpp/blob/main/docs/cli.md) can accept
the device's PCM WAV and write a transcript file, so it could be exposed as
another Hermes command provider. Automatic language selection is supported.
The [installation requirements](https://github.com/NVIDIA/NeMo-Speech.cpp/blob/main/docs/install.md)
must be checked against the server before installation.

The model's [Q8 GGUF artifact](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b/tree/main)
is about 742 MB. The latest server check had only 964 MiB of free storage, before
accounting for the runtime, temporary downloads and normal Hermes operation.
Keep the tested local Whisper `base` configuration for this switch. After
freeing server storage, benchmark Nemotron on actual English and Russian mic
recordings before selecting it. CPU latency and memory use on this server
have not been measured; Nemotron was not installed in this audit.

The host has a four-core Pentium N3700, 7.7 GiB RAM, glibc 2.39, and Intel
integrated graphics. Its CPU flags do not include AVX or AVX2. The glibc version
meets the documented release requirement, but binary instruction compatibility
has not been established. Consider a native CPU build and test it explicitly
on this older processor; general CPU benchmarks do not establish its latency.
