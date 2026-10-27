"""Render the radio's spoken clips with local Qwen3-TTS, cloning a reference voice.

    E:\\KevinFM\\LocalTTS\\qwen-env\\Scripts\\python.exe -X utf8 tools\\render_clips.py [--takes 3]

Reads clips_work/lines.tsv (name <TAB> text) and the reference clips_work/reference.wav +
reference.txt (its exact words). Writes clips_work/takes/<name>_<n>.wav. Same model and
"steady" settings as the station's calm lines (config/audio_bridge.json in the Worker).
Lines that already have all their takes are skipped, so a stopped run just continues.
"""
import argparse
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = ROOT / "clips_work"
MODEL_ID = "Qwen/Qwen3-TTS-12Hz-0.6B-Base"
STEADY = dict(temperature=0.68, top_k=40, top_p=0.95, repetition_penalty=1.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--takes", type=int, default=3)
    ap.add_argument("--only", nargs="*", help="render just these names")
    args = ap.parse_args()

    import numpy as np
    import soundfile as sf
    import torch
    from qwen_tts import Qwen3TTSModel

    lines = []
    for row in (WORK / "lines.tsv").read_text(encoding="utf-8").splitlines():
        if row.strip():
            name, text = row.split("\t", 1)
            if not args.only or name in args.only:
                lines.append((name.strip(), text.strip()))
    out = WORK / "takes"
    out.mkdir(parents=True, exist_ok=True)
    todo = [(n, t, k) for n, t in lines for k in range(1, args.takes + 1) if not (out / f"{n}_{k}.wav").exists()]
    print(f"{len(lines)} lines, {len(todo)} takes to render", flush=True)
    if not todo:
        return

    started = time.perf_counter()
    model = Qwen3TTSModel.from_pretrained(MODEL_ID, device_map="cuda:0", dtype=torch.bfloat16, attn_implementation=None)
    prompt = model.create_voice_clone_prompt(
        ref_audio=str(WORK / "reference.wav"),
        ref_text=(WORK / "reference.txt").read_text(encoding="utf-8").strip(),
        x_vector_only_mode=False,
    )
    print(f"model ready in {time.perf_counter() - started:.1f} s", flush=True)

    for i, (name, text, take) in enumerate(todo, 1):
        t0 = time.perf_counter()
        wavs, rate = model.generate_voice_clone(
            text=text, language="English", voice_clone_prompt=prompt,
            max_new_tokens=240, do_sample=True,
            top_k=STEADY["top_k"], top_p=STEADY["top_p"], temperature=STEADY["temperature"],
            repetition_penalty=STEADY["repetition_penalty"],
            subtalker_dosample=True, subtalker_top_k=STEADY["top_k"], subtalker_top_p=STEADY["top_p"],
            subtalker_temperature=STEADY["temperature"],
        )
        audio = np.asarray(wavs[0], dtype=np.float32).squeeze()
        sf.write(out / f"{name}_{take}.wav", audio, rate)
        print(f"[{i}/{len(todo)}] {name}_{take}: {len(audio) / rate:.2f} s audio in {time.perf_counter() - t0:.1f} s", flush=True)


if __name__ == "__main__":
    sys.exit(main())
