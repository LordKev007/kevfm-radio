"""Pick the best take of each clip and make the finished MP3s plus a listening page.

    E:\\KevinFM\\LocalTTS\\qwen-env\\Scripts\\python.exe -X utf8 tools\\finish_clips.py

A take qualifies if Whisper heard the line (match >= 0.9 in clips_work/transcripts.json).
Of those, the one whose median pitch is closest to the reference voice wins (so every clip
sounds like the same calm voice; pitch is what tracked "same voice" in the Dr. Fever work).
Each winner is trimmed, padded 0.15 s, brought to -16 LUFS (peaks limited at -1.5 dB),
and saved as mono 32 kbps / 22 kHz MP3 in clips/<name>.mp3 (small: they live in the radio's file storage). clips/index.html lists them with every
take, so a different take can be chosen by ear (clips_work/choices.json overrides the pick).
"""
import html
import json
import pathlib
import re
import shutil
import subprocess

import librosa
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = ROOT / "clips_work"
OUT = ROOT / "clips"
MIN_MATCH = 0.9
LUFS = -16


def median_pitch(path):
    y, sr = librosa.load(str(path), sr=16000, mono=True)
    f0, voiced, _ = librosa.pyin(y, fmin=60, fmax=400, sr=sr, frame_length=1024)
    f0 = f0[voiced & ~np.isnan(f0)]
    return float(np.median(f0)) if len(f0) else float("nan"), float(np.std(f0)) if len(f0) else float("nan")


def tail_level(path):
    """Level of the take's last 50 ms relative to its loudest 20 ms (dB)."""
    y, sr = librosa.load(str(path), sr=None, mono=True)
    hop = int(sr * 0.01)
    db = 20 * np.log10(librosa.feature.rms(y=y, frame_length=hop * 2, hop_length=hop)[0] + 1e-9)
    return float(db[-5:].max() - db.max())


def ffmpeg(*args):
    return subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-y", *args], capture_output=True, text=True)


def speech_bounds(path):
    """Start and end (s) of the speech: frames within 45 dB of the take's loudest frame.
    A fixed -45 dBFS threshold cut quiet word endings (the final "s" of "minutes" sits ~30 dB
    below the peak, around -50 dBFS), so the threshold is relative, with margins added."""
    y, sr = librosa.load(str(path), sr=None, mono=True)
    hop = int(sr * 0.01)
    db = 20 * np.log10(librosa.feature.rms(y=y, frame_length=hop * 2, hop_length=hop)[0] + 1e-9)
    on = np.flatnonzero(db > db.max() - 45)
    start = max(0.0, on[0] * hop / sr - 0.06)
    end = min(len(y) / sr, (on[-1] + 2) * hop / sr + 0.12)
    return start, end


def finish(src, dst):
    start, end = speech_bounds(src)
    trim = f"atrim=start={start:.3f}:end={end:.3f},asetpts=PTS-STARTPTS,adelay=120|120,apad=pad_dur=0.12"
    probe = ffmpeg("-i", str(src), "-af", f"{trim},loudnorm=I={LUFS}:TP=-1.5:LRA=7:print_format=json", "-f", "null", "-")
    m = json.loads(re.search(r"\{[^{}]*\"input_i\"[^{}]*\}", probe.stderr).group(0))
    # straight gain to the target plus a peak limiter: loudnorm's linear mode can't reach -16 on
    # 1-3 s speech clips without breaking its true-peak limit, so it left them 3-6 dB quiet and uneven
    gain = LUFS - float(m["input_i"])
    norm = f"volume={gain:.2f}dB,alimiter=limit=0.84:attack=2:release=50:level=false"
    r = ffmpeg("-i", str(src), "-af", f"{trim},{norm}", "-ac", "1", "-ar", "22050", "-c:a", "libmp3lame", "-b:a", "32k",
               "-map_metadata", "-1", "-id3v2_version", "0", "-write_id3v1", "0", str(dst))   # no tags: yoRadio would show them as titles
    if r.returncode:
        raise RuntimeError(r.stderr[-500:])


def main():
    lines = [row.split("\t", 1) for row in (WORK / "lines.tsv").read_text(encoding="utf-8").splitlines() if row.strip()]
    tpath = WORK / "transcripts.json"   # optional (tools/check_clips.py); without it, a length check stands in
    heard = json.loads(tpath.read_text(encoding="utf-8")) if tpath.exists() else None
    choices_path = WORK / "choices.json"
    choices = json.loads(choices_path.read_text(encoding="utf-8")) if choices_path.exists() else {}
    ref_f0, _ = median_pitch(WORK / "reference.wav")
    print(f"reference median pitch {ref_f0:.0f} Hz")
    OUT.mkdir(exist_ok=True)
    (OUT / "takes").mkdir(exist_ok=True)
    report, problems = [], []
    for name, text in lines:
        takes = sorted((WORK / "takes").glob(f"{name}_*.wav"))
        rows = []
        for t in takes:
            f0, sd = median_pitch(t)
            h = heard.get(t.stem, {}) if heard is not None else {"match": 1.0, "heard": ""}
            secs = librosa.get_duration(path=str(t))
            rows.append({"take": t.stem, "f0": f0, "sd": sd, "match": h.get("match", 0), "heard": h.get("heard", ""), "secs": secs})
            shutil.copyfile(t, OUT / "takes" / t.name)
        # a take far longer or shorter than its siblings ran on (babble) or dropped words
        mid = float(np.median([r["secs"] for r in rows])) if rows else 0
        for r in rows:
            if not (0.75 * mid <= r["secs"] <= 1.35 * mid):
                r["match"] = 0
            # Qwen sometimes stops mid-sound ("Light always o-"): the take must end in near-silence
            r["end_db"] = tail_level(WORK / "takes" / f"{r['take']}.wav")
            if r["end_db"] > -45:
                r["match"] = 0
        good = [r for r in rows if r["match"] >= MIN_MATCH and not np.isnan(r["f0"])]
        pick = choices.get(name)
        if not pick:
            pick = min(good, key=lambda r: abs(r["f0"] - ref_f0))["take"] if good else None
        if pick:
            finish(WORK / "takes" / f"{pick}.wav", OUT / f"{name}.mp3")
        else:
            problems.append(name)
        report.append((name, text, pick, rows))
        print(f"{name:12} -> {pick or 'NO GOOD TAKE'}  " + "  ".join(f"{r['take'][-1]}:{r['f0']:.0f}Hz/{r['secs']:.1f}s{'' if r['match'] >= MIN_MATCH else ' X'}" for r in rows))

    page = ["<!doctype html><meta charset=utf-8><title>KevFM radio clips</title>",
            "<style>body{font:15px system-ui;background:#0b0f14;color:#eef2f6;max-width:900px;margin:20px auto;padding:0 16px}"
            "td{padding:6px 8px;border-bottom:1px solid #2a3542;vertical-align:top}audio{height:32px;width:220px}"
            ".pick{color:#f2a33a;font-weight:600}.bad{color:#ff7a6b}.muted{color:#8b98a7;font-size:12px}</style>",
            f"<h1>KevFM radio clips</h1><p>Reference voice: <audio controls src='../clips_work/reference.mp3'></audio> "
            f"<span class=muted>median pitch {ref_f0:.0f} Hz</span></p>",
            "<p class=muted>Finished clip (trimmed, -16 LUFS, mono MP3) first, then every raw take. Orange = picked automatically "
            "(ends cleanly, normal length for its line; pitch closest to the reference). To use another take, tell Claude e.g. \"sleep_30 take 2\".</p><table>"]
    for name, text, pick, rows in report:
        cells = [f"<td><b>{name}</b><br>{html.escape(text)}<br>" +
                 (f"<audio controls src='{name}.mp3'></audio>" if pick else "<span class=bad>no good take</span>") + "</td><td>"]
        for r in rows:
            cls = "pick" if r["take"] == pick else ("bad" if r["match"] < MIN_MATCH else "")
            cells.append(f"<div class='{cls}'>take {r['take'][-1]}: <audio controls src='takes/{r['take']}.wav'></audio> "
                         f"<span class=muted>{r['f0']:.0f} Hz, {r['secs']:.1f} s" + (", cut off" if r.get('end_db', -99) > -45 else "") + (f", heard \"{html.escape(r['heard'])}\"" if r['heard'] else "") + "</span></div>")
        page.append("<tr>" + "".join(cells) + "</td></tr>")
    page.append("</table>")
    (OUT / "index.html").write_text("\n".join(page), encoding="utf-8")
    radio = ROOT / "yoRadio" / "data" / "clips"          # goes into the radio's file storage image
    radio.mkdir(parents=True, exist_ok=True)
    for old in radio.glob("*.mp3"):
        old.unlink()
    for mp3 in OUT.glob("*.mp3"):
        shutil.copyfile(mp3, radio / mp3.name)
    print(f"\n{len(lines) - len(problems)} clips made in {OUT} (copied to {radio}); problems: {problems or 'none'}")


if __name__ == "__main__":
    main()
