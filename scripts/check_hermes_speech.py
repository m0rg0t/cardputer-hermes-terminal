#!/usr/bin/env python3
"""Round-trip a synthetic phrase through Hermes TTS and WAV transcription.

HTTP mode uses HERMES_TEST_COOKIE, HERMES_TEST_TOKEN, or
HERMES_TEST_USERNAME/HERMES_TEST_PASSWORD. --handlers runs on the Hermes host
against the installed route handlers; it does not verify HTTP authentication.
Requires ffmpeg. Temporary speech files are removed after each check.
"""

import argparse
import asyncio
import base64
import http.cookiejar
import json
import logging
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class HttpClient:
    def __init__(self, base_url):
        url = urllib.parse.urlsplit(base_url)
        if url.scheme != "https" and not (
                url.scheme == "http" and url.hostname in ("127.0.0.1", "::1", "localhost")):
            raise ValueError("Use HTTPS or a loopback HTTP address")
        if url.username or url.password or url.query or url.fragment:
            raise ValueError("Use a dashboard origin without credentials, query, or fragment")
        self.base_url = base_url.rstrip("/")
        self.opener = urllib.request.build_opener(
            NoRedirect(), urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.headers = {}
        if os.getenv("HERMES_TEST_COOKIE"):
            self.headers["Cookie"] = os.environ["HERMES_TEST_COOKIE"]
        elif os.getenv("HERMES_TEST_TOKEN"):
            self.headers["X-Hermes-Session-Token"] = os.environ["HERMES_TEST_TOKEN"]
        elif os.getenv("HERMES_TEST_USERNAME") and os.getenv("HERMES_TEST_PASSWORD"):
            self.request("/auth/password-login", {
                "provider": "basic", "username": os.environ["HERMES_TEST_USERNAME"],
                "password": os.environ["HERMES_TEST_PASSWORD"], "next": "/"})
        else:
            raise ValueError("Set HERMES_TEST_COOKIE, HERMES_TEST_TOKEN, or test login variables")

    def request(self, path, payload, profile=None):
        query = "?" + urllib.parse.urlencode({"profile": profile}) if profile else ""
        req = urllib.request.Request(self.base_url + path + query,
                                     data=json.dumps(payload).encode(),
                                     headers={**self.headers, "Content-Type": "application/json"})
        with self.opener.open(req, timeout=180) as response:
            return json.load(response)


def wav_payload(speech, directory):
    header, encoded = speech["data_url"].split(",", 1)
    if not header.startswith("data:audio/") or not header.endswith(";base64"):
        raise ValueError("TTS did not return an audio data URL")
    audio = base64.b64decode(encoded, validate=True)
    if len(audio) < 16:
        raise ValueError("TTS returned empty/truncated audio")
    source = Path(directory) / "speech.bin"
    wav = Path(directory) / "recording.wav"
    source.write_bytes(audio)
    result = subprocess.run([
        "ffmpeg", "-v", "error", "-y", "-i", str(source), "-ar", "16000",
        "-ac", "1", "-c:a", "pcm_s16le", str(wav)], capture_output=True, timeout=30)
    if result.returncode:
        raise ValueError("ffmpeg could not decode the synthesized audio")
    return {"data_url": "data:audio/wav;base64," + base64.b64encode(wav.read_bytes()).decode(),
            "mime_type": "audio/wav"}, len(audio)


def transcript_matches(transcript, expected):
    heard = set(re.findall(r"\w+", transcript.casefold()))
    words = set(re.findall(r"\w+", expected.casefold()))
    return bool(words) and len(heard & words) / len(words) >= 0.75


def safe_error(exc):
    if isinstance(exc, urllib.error.HTTPError):
        # Never print a response body: proxies/providers can echo credentials.
        return f"HTTP {exc.code}"
    if hasattr(exc, "status_code"):
        return f"Handler HTTP {exc.status_code} (inspect server diagnostics)"
    # Keep external-library exception messages out of logs as well.
    if isinstance(exc, (ValueError, FileNotFoundError)):
        return str(exc) if type(exc) in (ValueError, FileNotFoundError) else type(exc).__name__
    return type(exc).__name__


async def run(args):
    if args.handlers:
        logging.disable(logging.CRITICAL)
        sys.path.insert(0, str(Path(args.hermes_root).expanduser().resolve()))
        from hermes_cli.config import load_env
        load_env()
        from hermes_cli.web_models import TTSSpeakRequest, AudioTranscriptionRequest
        from hermes_cli.web_routers.audio import speak_text, transcribe_audio_upload
        client = None
    else:
        client = HttpClient(args.base_url)
    failed = False
    for profile in args.profile or ["default"]:
        result = {"profile": profile, "mode": "handlers" if args.handlers else "http"}
        started = time.monotonic()
        try:
            payload = {"text": args.text}
            result["stage"] = "tts"
            speech = (await speak_text(TTSSpeakRequest(**payload), profile=profile)
                      if client is None else client.request("/api/audio/speak", payload, profile))
            result["tts_seconds"] = round(time.monotonic() - started, 2)
            if not speech.get("ok"):
                raise ValueError("TTS returned an unsuccessful result")
            result["tts_provider"] = speech.get("provider")
            result["mime_type"] = speech.get("mime_type")
            result["stage"] = "decode"
            with tempfile.TemporaryDirectory(prefix="hermes-speech-check-") as directory:
                payload, result["audio_bytes"] = wav_payload(speech, directory)
                result["stage"] = "stt"
                stt_started = time.monotonic()
                stt = (await transcribe_audio_upload(AudioTranscriptionRequest(**payload), profile=profile)
                       if client is None else client.request("/api/audio/transcribe", payload, profile))
                result["stt_seconds"] = round(time.monotonic() - stt_started, 2)
            transcript = str(stt.get("transcript") or "")
            result["stt_provider"] = stt.get("provider")
            result["transcript"] = transcript
            result["pass"] = bool(stt.get("ok")) and transcript_matches(transcript, args.expected)
            failed |= not result["pass"]
            if not result["pass"]:
                result["error"] = "Transcript did not match the expected test words"
        except Exception as exc:
            result.update({"pass": False, "error": safe_error(exc)})
            failed = True
        result["elapsed_seconds"] = round(time.monotonic() - started, 2)
        print(json.dumps(result, ensure_ascii=False), flush=True)
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--base-url")
    modes.add_argument("--handlers", action="store_true")
    parser.add_argument("--hermes-root", default="~/.hermes/hermes-agent")
    parser.add_argument("--profile", action="append")
    parser.add_argument("--text", default="The terminal is ready for the voice test.")
    parser.add_argument("--expected", default="terminal ready voice test")
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except Exception as exc:
        parser.exit(1, safe_error(exc) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
