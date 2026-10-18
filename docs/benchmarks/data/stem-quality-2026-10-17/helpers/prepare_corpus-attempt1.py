#!/usr/bin/env python3
"""Prepare the three predeclared, locally retained CC-BY M0 music excerpts."""
import base64
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import urllib.parse
import urllib.request
import wave

import numpy as np

BASE = Path(__file__).resolve().parent
ASSETS = BASE / "assets"
EVIDENCE = ASSETS / "source-evidence"
RATE, OFFSET, FRAMES = 44100, 1323000, 1031940
CAP = 40000000


def utc():
    return datetime.now(timezone.utc).isoformat()


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def main():
    plan = json.loads((BASE / "excerpt-plan.json").read_text())
    assert (plan["source_offset_frames"], plan["excerpt_frames"], plan["sample_rate"],
            plan["audio_download_cap_bytes"], plan["looping"]) == (OFFSET, FRAMES, RATE, CAP, False)
    page = (EVIDENCE / "parting-glass-page.html").read_text()
    assert 'rel="license" href="http://creativecommons.org/licenses/by/3.0/"' in page
    assert "Doxent Zsigmond" in page and "<td>rubato</td>" in page
    api = json.loads((EVIDENCE / "parting-glass-official-api.json").read_text())[0]
    source = next(item for item in api["files"] if item["file_id"] == 91505)
    assert source["download_url"] in page
    tags = list(dict.fromkeys(re.findall(r'https://ccmixter.org/tags/([^"<>]+)', page)))
    tracks = [{"id": "parting_glass", "title": "The Parting Glass", "artist": "Doxent Zsigmond",
               "source_page": api["file_page_url"], "metadata_url": "https://ccmixter.org/api/query/api?ids=56347&f=json&dataview=links_dl",
               "download_url": source["download_url"], "license": "CC BY 3.0",
               "license_url": "https://creativecommons.org/licenses/by/3.0/", "official_tags": tags,
               "bpm": "rubato", "official_duration": source["file_format_info"]["ps"],
               "official_instruments_or_credits": "Music and lyrics - traditional; Doxent - vocals, piano",
               "official_audio_metadata": source,
               "published_bytes": source["file_rawsize"], "published_sha1_base32": source["file_extra"]["sha1"],
               "attribution": '"The Parting Glass" by Doxent Zsigmond (ccMixter), CC BY 3.0; traditional music/lyrics. Local 30.0–53.4 s excerpt, decoded/resampled and quantized for M0 evaluation.'}]
    pieces = json.loads((EVIDENCE / "incompetech-pieces.json").read_text())
    for identifier, isrc, genre, page_name in (("funkorama", "USUAN1100474", "Funk", "funkorama-page.html"),
                                              ("club_diver", "USUAN1200020", "Electronica", "club-diver-page.html")):
        html = (EVIDENCE / page_name).read_text()
        assert "Creative Commons: By Attribution 4.0 License" in html
        assert "http://creativecommons.org/licenses/by/4.0/" in html
        assert '`/music/royalty-free/mp3-royaltyfree/${encodeURIComponent(p.filename)}`' in html
        record = next(item for item in pieces if item["isrc"] == isrc)
        assert re.search(r'"id": ' + re.escape(record["genre"]) + r', "genre": "' + genre + '"', html)
        url = "https://incompetech.com/music/royalty-free/mp3-royaltyfree/" + urllib.parse.quote(record["filename"], safe="")
        tracks.append({"id": identifier, "title": record["title"], "artist": "Kevin MacLeod",
                       "source_page": "https://incompetech.com/music/royalty-free/index.html?isrc=" + isrc,
                       "metadata_url": "https://incompetech.com/music/royalty-free/pieces.json", "download_url": url,
                       "download_link_evidence": "Official page renderTrack constructs its MP3 link from official pieces.json filename using encodeURIComponent.",
                       "license": "CC BY 4.0", "license_url": "https://creativecommons.org/licenses/by/4.0/",
                       "official_genre": genre, "official_tags": record["feel"].split(", "), "bpm": int(record["bpm"]),
                       "official_duration": record["length"], "official_instruments_or_credits": record["instruments"],
                       "official_audio_metadata": record, "published_cryptographic_digest": None,
                       "attribution": f'"{record["title"]}" Kevin MacLeod (incompetech.com), CC BY 4.0. Local 30.0–53.4 s excerpt, decoded/resampled and quantized for M0 evaluation.'})
    write_json(EVIDENCE / "selected-track-metadata.json", tracks)
    provenance = {"schema": 1, "started_at_utc": utc(), "status": "preparing", "excerpt_plan": pin(BASE / "excerpt-plan.json"),
                  "preparation_source": pin(Path(__file__).resolve()),
                  "scope": "Three CC-BY full mixtures; no isolated reference stems. All source/excerpt audio retained locally in ignored assets; no inference or playback by this preparation helper.",
                  "source_page_license_note": "ccMixter's CC BY-NC 4.0 footer applies to website text/images; the selected audio upload separately and explicitly grants CC BY 3.0. Incompetech individual-track templates explicitly grant CC BY 4.0.",
                  "license_obligations": "Retain author/title/source and license links; indicate excerpting/resampling/gain/quantization modifications. No audio will be published in Git.",
                  "audio_download_cap_bytes": CAP, "audio_download_bytes": 0, "tracks": [],
                  "ffmpeg_version": subprocess.run(["/bin/ffmpeg", "-version"], check=True, capture_output=True, text=True, timeout=10).stdout,
                  "source_evidence": [pin(path) for path in sorted(EVIDENCE.iterdir()) if path.is_file()]}
    manifest = {"schema": 1, "excerpt_plan": plan, "tracks": []}
    try:
        for track in tracks:
            item = {"id": track["id"], "annotations": track, "status": "downloading"}
            provenance["tracks"].append(item)
            destination = ASSETS / (track["id"] + ".mp3")
            temporary = destination.with_suffix(".mp3.part")
            requested_at = utc()
            request = urllib.request.Request(track["download_url"], headers={"User-Agent": "Conflux-M0-local-quality-provenance/1.0"})
            digest, sha1, count = hashlib.sha256(), hashlib.sha1(), 0
            with urllib.request.urlopen(request, timeout=30) as response, temporary.open("xb") as output:
                headers = dict(response.headers.items())
                length = response.headers.get("Content-Length")
                if length is not None and int(length) + provenance["audio_download_bytes"] > CAP:
                    raise RuntimeError("Declared MP3 content length exceeds total audio-download cap")
                if not response.headers.get("Content-Type", "").lower().startswith(("audio/", "application/octet-stream")):
                    raise RuntimeError("Unexpected MP3 response content type")
                item["http"] = {"requested_url": track["download_url"], "resolved_url": response.url,
                                "status": response.status, "headers": headers, "retrieval_start_utc": requested_at}
                while True:
                    remaining = CAP - provenance["audio_download_bytes"]
                    if remaining <= 0:
                        raise RuntimeError("Total audio-download cap reached")
                    payload = response.read(min(65536, remaining))
                    if not payload:
                        break
                    count += len(payload)
                    provenance["audio_download_bytes"] += len(payload)
                    digest.update(payload)
                    sha1.update(payload)
                    output.write(payload)
                item["http"].update(retrieval_end_utc=utc(), received_bytes=count, sha256=digest.hexdigest())
                if length is not None and count != int(length):
                    raise RuntimeError("MP3 byte count differs from HTTP Content-Length")
            if track.get("published_bytes") is not None and count != track["published_bytes"]:
                raise RuntimeError("MP3 byte count differs from official file metadata")
            if track.get("published_sha1_base32"):
                expected_sha1 = base64.b32decode(track["published_sha1_base32"]).hex()
                item["published_sha1_verification"] = {"expected_hex": expected_sha1, "observed_hex": sha1.hexdigest(), "matches": expected_sha1 == sha1.hexdigest()}
                if expected_sha1 != sha1.hexdigest():
                    raise RuntimeError("MP3 digest differs from official published SHA1")
            temporary.rename(destination)
            item["source_mp3"] = pin(destination)
            raw_path = ASSETS / (track["id"] + ".decoded-f32le.raw")
            decode = ["/bin/ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-i", str(destination),
                      "-map", "0:a:0", "-ac", "2", "-ar", str(RATE), "-acodec", "pcm_f32le", "-f", "f32le", str(raw_path)]
            decoded = subprocess.run(decode, check=True, capture_output=True, text=True, timeout=90)
            if raw_path.stat().st_size % 8:
                raise RuntimeError("Decoded stereo float32 stream has a partial frame")
            samples = np.memmap(raw_path, dtype="<f4", mode="r").reshape(-1, 2)
            if samples.shape[0] < OFFSET + FRAMES:
                raise RuntimeError("Source is too short for predeclared excerpt")
            excerpt = np.array(samples[OFFSET:OFFSET + FRAMES], dtype="<f4")
            if excerpt.shape != (FRAMES, 2) or not np.isfinite(excerpt).all():
                raise RuntimeError("Excerpt is malformed/nonfinite")
            peak = float(np.max(np.abs(excerpt)))
            gain = 0.95 / peak if peak > 1.0 else 1.0
            scaled = excerpt.astype(np.float64) * gain
            quantized = np.rint(scaled * 32767)
            if not np.isfinite(scaled).all() or float(np.max(np.abs(scaled))) > 1.0 or quantized.min() < -32768 or quantized.max() > 32767:
                raise RuntimeError("PCM16 conversion would clip")
            wav_path = ASSETS / (track["id"] + "-30s-23p4s.wav")
            with wave.open(str(wav_path), "wb") as output:
                output.setparams((2, 2, RATE, FRAMES, "NONE", "not compressed"))
                output.writeframes(quantized.astype("<i2").tobytes())
            item["decode"] = {"command": decode, "stderr": decoded.stderr, "whole_decoded_frames": int(samples.shape[0]),
                              "whole_decoded_float32_identity": pin(raw_path), "temporary_raw_retained": False,
                              "source_frame_offset": OFFSET, "source_offset_seconds": 30.0, "frames": FRAMES, "duration_seconds": 23.4,
                              "source_timeline": "ffmpeg decoded and resampled stereo 44.1 kHz timeline, with decoder delay handling; no looping",
                              "decoded_excerpt_float32_sha256": hashlib.sha256(excerpt.tobytes()).hexdigest(),
                              "decoded_excerpt_peak": peak, "common_stereo_gain": gain, "gain_required": peak > 1.0,
                              "attenuation_target_if_required": 0.95, "scaled_excerpt_peak": float(np.max(np.abs(scaled))),
                              "quantization": "numpy.rint(float64(decoded_float32) * common_stereo_gain * 32767).astype('<i2'); no clipping",
                              "pcm16_min": int(quantized.min()), "pcm16_max": int(quantized.max()), "sample_rate": RATE, "channels": 2, "sample_bytes": 2}
            del samples
            raw_path.unlink()  # Only this helper's temporary full decode; source MP3 and exact WAV remain.
            with wave.open(str(wav_path), "rb") as check:
                assert check.getparams()[:4] == (2, 2, RATE, FRAMES)
                assert len(check.readframes(FRAMES + 1)) == FRAMES * 4
            item["input_wav"] = pin(wav_path)
            item["status"] = "ready"
            annotations = dict(track)
            annotations.update(source_mp3_sha256=item["source_mp3"]["sha256"], excerpt_source_offset_seconds=30.0,
                               excerpt_frames=FRAMES, excerpt_seconds=23.4, common_stereo_gain=gain,
                               reference_stems_available=False, human_listening_required=True)
            manifest["tracks"].append({"id": track["id"], "input_wav": str(wav_path), "annotations": annotations})
            print(json.dumps({"id": track["id"], "download_bytes": count, "wav": item["input_wav"],
                              "frames": FRAMES, "decoded_excerpt_peak": peak, "common_stereo_gain": gain}), flush=True)
        assert provenance["audio_download_bytes"] <= CAP and len(manifest["tracks"]) == 3
        write_json(BASE / "manifest.json", manifest)
        provenance["manifest"] = pin(BASE / "manifest.json")
        provenance["status"] = "ready"
    except Exception as error:
        provenance["status"] = "failed"
        provenance["failure"] = {"type": type(error).__name__, "message": str(error)}
        raise
    finally:
        provenance["finished_at_utc"] = utc()
        write_json(BASE / "corpus-provenance.json", provenance)
    print(json.dumps({"status": "ready", "audio_download_bytes": provenance["audio_download_bytes"],
                      "manifest": provenance["manifest"], "provenance": pin(BASE / "corpus-provenance.json")}), flush=True)


if __name__ == "__main__":
    main()
