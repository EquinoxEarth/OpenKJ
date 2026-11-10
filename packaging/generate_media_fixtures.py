#!/usr/bin/env python3
"""Generate tiny synthetic playback fixtures with the build SDK's FFmpeg."""
import argparse
from pathlib import Path
import subprocess


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    # Encoders are build-time tools only; the bundle uses its playback decoders.
    fixtures = (
        ("audio.mp3", False, ["-c:a", "libmp3lame", "-id3v2_version", "3"]),
        ("audio.flac", False, ["-c:a", "flac"]),
        ("audio.ogg", False, ["-c:a", "vorbis", "-strict", "experimental", "-ac", "2"]),
        ("opus.ogg", False, ["-c:a", "libopus"]),
        ("audio.m4a", False, ["-c:a", "aac"]),
        ("audio.wma", False, ["-c:a", "wmav2"]),
        ("video.mp4", True, ["-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"]),
        ("video.mkv", True, ["-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"]),
        ("video.avi", True, ["-c:v", "mpeg4", "-c:a", "libmp3lame"]),
        ("video.mpg", True, ["-c:v", "mpeg2video", "-c:a", "mp2"]),
        ("transport.mpeg", True, ["-c:v", "mpeg2video", "-c:a", "mp2", "-f", "mpegts"]),
        ("video.wmv", True, ["-c:v", "wmv2", "-c:a", "wmav2"]),
    )
    for name, video, codecs in fixtures:
        command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y"]
        if video:
            command += ["-f", "lavfi", "-i", "testsrc2=size=160x120:rate=25"]
        command += ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
                    "-t", "0.4", *codecs, str(directory / name)]
        subprocess.run(command, check=True)
    print(f"Generated {len(fixtures)} media fixtures in {directory}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    generate(parser.parse_args().directory)
