"""Transcribe every rendered take with Whisper (CPU) and record whether it says the line.

    C:\\Users\\Kev\\AppData\\Local\\Programs\\Python\\Python312\\python.exe -I tools\\check_clips.py

Writes clips_work/transcripts.json: {take: {"heard": ..., "match": 0..1}}. Takes already
transcribed are skipped. Python 3.12 is used because it has openai-whisper; its numba
refuses its numpy, and whisper only needs numba for word timings, so it gets a stand-in.
"""
import difflib
import json
import pathlib
import re
import sys
import types

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = ROOT / "clips_work"

NUMBERS = {"five": "5", "ten": "10", "fifteen": "15", "twenty": "20", "twenty five": "25", "thirty": "30",
           "thirty five": "35", "forty": "40", "forty five": "45", "fifty": "50", "fifty five": "55",
           "sixty": "60", "one": "1"}


def canon(text):
    t = text.lower().replace("-", " ")
    t = re.sub(r"[^a-z0-9 ]", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    for words in sorted(NUMBERS, key=len, reverse=True):
        t = re.sub(rf"\b{words}\b", NUMBERS[words], t)
    t = t.replace("kev fm", "kevfm").replace("good night", "goodnight").replace("wi fi", "wifi")
    return t


def main():
    fake = types.ModuleType("numba")
    fake.jit = lambda *a, **k: (a[0] if a and callable(a[0]) else (lambda f: f))
    fake.njit = fake.jit
    sys.modules["numba"] = fake
    import whisper

    lines = dict(row.split("\t", 1) for row in (WORK / "lines.tsv").read_text(encoding="utf-8").splitlines() if row.strip())
    out_path = WORK / "transcripts.json"
    done = json.loads(out_path.read_text(encoding="utf-8")) if out_path.exists() else {}
    takes = sorted((WORK / "takes").glob("*.wav"))
    todo = [p for p in takes if p.stem not in done]
    print(f"{len(takes)} takes, {len(todo)} to transcribe", flush=True)
    if not todo:
        return
    model = whisper.load_model("small", device="cpu")
    for p in todo:
        name = p.stem.rsplit("_", 1)[0]
        heard = model.transcribe(str(p), language="en", fp16=False)["text"].strip()
        match = difflib.SequenceMatcher(None, canon(lines[name]), canon(heard)).ratio()
        done[p.stem] = {"heard": heard, "match": round(match, 3)}
        out_path.write_text(json.dumps(done, indent=1), encoding="utf-8")
        print(f"{p.stem}: {match:.2f}  {heard}", flush=True)


if __name__ == "__main__":
    main()
