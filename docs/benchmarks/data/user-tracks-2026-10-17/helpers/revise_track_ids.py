#!/usr/bin/env python3
"""Record the runner-bound ID correction without changing or decoding audio."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import wave

BASE = Path(__file__).resolve().parent
MAPPING = {"excision_level_up_club_xl_subsidia": "club_xl",
           "said_the_sky_illenium_heather_sommer_when_the_light_breaks_lyric_video": "when_the_light_breaks",
           "slander_spiritbox_vastive_under_my_skin": "under_my_skin",
           "t_pain_dj_diesel_san_holo_nghtmre_kompany_samplifire_ivory_stfu_dubstep_monstercat": "stfu"}


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest}


def write(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def main():
    names = ("excerpt-plan", "manifest", "corpus-provenance")
    if any((BASE / (n + "-attempt1.json")).exists() for n in names) or (BASE / "id-revision.json").exists():
        raise FileExistsError("ID revision must be fresh")
    documents = {n: json.loads((BASE / (n + ".json")).read_text()) for n in names}
    plan, manifest, provenance = (documents[n] for n in names)
    if provenance["status"] != "ready" or not provenance["originals_unchanged"]:
        raise ValueError("Initial complete corpus is required")
    if any(set(t["id"] for t in d["tracks"]) != set(MAPPING) for d in documents.values()):
        raise ValueError("Unexpected initial IDs")
    preserved_helper = pin(BASE / "prepare_user_tracks-attempt1.py")
    if any(preserved_helper[k] != plan["preparation_source"][k] for k in ("bytes", "sha256")):
        raise ValueError("Initial decoder helper not preserved exactly")
    revision = {"schema": 1, "completed_at_utc": datetime.now(timezone.utc).isoformat(),
                "reason": "Two filename-derived IDs exceeded the unchanged runner's 64-character bound; use root-specified short IDs for all four.",
                "scope": "Only IDs and prepared WAV pathnames changed after decoding. Source selections, decode commands, gain, PCM bytes and coverage are unchanged; no repeat decoding.",
                "revision_source": pin(Path(__file__).resolve()), "initial_decoder_source_preserved_as": preserved_helper,
                "corrected_preparation_source": pin(BASE / "prepare_user_tracks.py"), "initial_documents": {}, "tracks": []}
    for track in provenance["tracks"]:
        old, new = track["id"], MAPPING[track["id"]]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", new):
            raise ValueError("Invalid revised ID")
        source = Path(track["input_wav"])
        destination = BASE / "assets" / (new + "-30s-23p4s.wav")
        if pin(source) != {k: track["prepared_wav"][k] for k in ("path", "bytes", "sha256")} or destination.exists():
            raise ValueError("Initial WAV changed or destination exists")
        with wave.open(str(source), "rb") as audio:
            if audio.getparams()[:4] != (2, 2, 44100, 1031940):
                raise ValueError("Initial WAV geometry changed")
        revision["tracks"].append({"old_id": old, "new_id": new, "source_filename": track["source_filename"],
                                   "old_wav": pin(source), "new_path": str(destination)})
    for name in names:
        source = BASE / (name + ".json")
        destination = BASE / (name + "-attempt1.json")
        source.rename(destination)
        revision["initial_documents"][name] = pin(destination)
    for track in revision["tracks"]:
        Path(track["old_wav"]["path"]).rename(track["new_path"])
        track["new_wav"] = pin(Path(track["new_path"]))
        if any(track["new_wav"][k] != track["old_wav"][k] for k in ("bytes", "sha256")):
            raise RuntimeError("Rename changed WAV identity")
    write(BASE / "id-revision.json", revision)
    revision_pin = pin(BASE / "id-revision.json")
    for document in (plan, provenance):
        document.update(id_revision=revision_pin, initial_decoder_source_preserved_as=preserved_helper,
                        corrected_preparation_source=revision["corrected_preparation_source"])
        for track in document["tracks"]:
            track["id"] = MAPPING[track["id"]]
            track["input_wav"] = str(BASE / "assets" / (track["id"] + "-30s-23p4s.wav"))
            if "prepared_wav" in track:
                track["prepared_wav"]["path"] = track["input_wav"]
    write(BASE / "excerpt-plan.json", plan)
    manifest["excerpt_plan"] = {k: v for k, v in plan.items() if k != "tracks"}
    for track in manifest["tracks"]:
        track["id"] = MAPPING[track["id"]]
        track["input_wav"] = str(BASE / "assets" / (track["id"] + "-30s-23p4s.wav"))
        track["input_pin"]["path"] = track["input_wav"]
    write(BASE / "manifest.json", manifest)
    provenance.update(excerpt_plan=pin(BASE / "excerpt-plan.json"), manifest=pin(BASE / "manifest.json"))
    write(BASE / "corpus-provenance.json", provenance)
    print(json.dumps({"status": "ready", **{n: pin(BASE / (n + ".json")) for n in names},
                      "id_revision": revision_pin}, indent=2))


if __name__ == "__main__":
    main()
