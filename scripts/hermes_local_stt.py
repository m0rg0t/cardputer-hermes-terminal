#!/usr/bin/env python3
"""Isolated CPU Whisper command provider for a Hermes server.

Install faster-whisper==1.2.1 in a dedicated environment. The model must already
be cached: this command never downloads a model during a recording request.
"""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", default="base")
    parser.add_argument("--language", default="auto")
    args = parser.parse_args()
    from faster_whisper import WhisperModel

    model = WhisperModel(args.model, device="cpu", compute_type="int8",
                         cpu_threads=2, local_files_only=True)
    language = None if args.language == "auto" else args.language
    segments, _ = model.transcribe(str(args.input), language=language,
                                   beam_size=5, vad_filter=True)
    transcript = " ".join(segment.text.strip() for segment in segments).strip()
    # Hermes' command provider requires a non-empty output. A whitespace-only
    # result must be a visible failure rather than an invented prompt.
    if not transcript:
        parser.exit(1, "No speech detected\n")
    args.output.write_text(transcript + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
