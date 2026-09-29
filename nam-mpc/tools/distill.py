#!/usr/bin/env python3
"""Make a smaller (Feather) copy of a NAM model by training it to imitate the original ("distillation").

    distill.py <teacher.nam> <input.wav> <out-dir> [--epochs N] [--slim S] [--render nam-render]

1. The teacher model is run over NAM's standard training signal (input.wav, v3) with NAM's own C++ engine
   (nam-render: NeuralAmpModelerCore's tools/render.cpp), so every architecture works, A2/slimmable included.
2. A Feather WaveNet (channels 8 -> 4, head 4 -> 1, the classic NAM "feather" layout) is trained on
   (input.wav -> teacher output) with the NAM trainer, using input.wav's standard train/validation split.
3. The result is written as <out-dir>/F<teacher name>.nam, with the teacher's metadata carried over.

The teacher's output is used as-is (no level normalisation), so the copy keeps the original's gain.
Needs: torch, neural-amp-modeler (the trainer imports tkinter only for GUI pop-ups; a stub is fine).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

V3_TRAIN_START = 480_000      # input.wav v3: 0:10 onward is training data...
V3_VALIDATION = 432_000       # ...except the last 9 s, which are validation

FEATHER_DILATIONS = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]
FEATHER = {
    "net": {"name": "WaveNet", "config": {
        "layers_configs": [
            {"input_size": 1, "condition_size": 1, "channels": 8, "kernel_size": 3,
             "dilations": FEATHER_DILATIONS, "activation": "Tanh", "gated": False,
             "head": {"out_channels": 4, "kernel_size": 1, "bias": False}},
            {"input_size": 8, "condition_size": 1, "channels": 4, "kernel_size": 3,
             "dilations": FEATHER_DILATIONS, "activation": "Tanh", "gated": False,
             "head": {"out_channels": 1, "kernel_size": 1, "bias": True}},
        ],
        "head_scale": 0.02,
    }},
    "loss": {"val_loss": "esr", "mrstft_weight": 0.0002},
    "optimizer": {"lr": 0.004},
    "lr_scheduler": {"class": "ExponentialLR", "kwargs": {"gamma": 0.993}},
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("teacher")
    ap.add_argument("input_wav")
    ap.add_argument("outdir")
    ap.add_argument("--epochs", type=int, default=100)
    ap.add_argument("--slim", type=float, default=None, help="size for a slimmable teacher (0..1; default: full)")
    ap.add_argument("--render", default=str(Path(__file__).with_name("nam-render")))
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    import torch
    torch.set_num_threads(a.threads)
    from nam.train import full

    teacher = Path(a.teacher)
    out = Path(a.outdir)
    work = out / (teacher.stem + ".work")
    work.mkdir(parents=True, exist_ok=True)

    # 1. the teacher's output over the training signal
    y = work / "teacher_output.wav"
    cmd = [a.render] + (["--slim", str(a.slim)] if a.slim is not None else []) + [str(teacher), a.input_wav, str(y)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    # 2. train the Feather student
    data = {
        "train": {"ny": 8192, "start_samples": V3_TRAIN_START, "stop_samples": -V3_VALIDATION},
        "validation": {"ny": None, "start_samples": -V3_VALIDATION},
        "common": {"x_path": str(Path(a.input_wav).resolve()), "y_path": str(y.resolve()), "delay": 0,
                   "allow_unequal_lengths": True},
    }
    learning = {
        "train_dataloader": {"batch_size": 16, "shuffle": True, "pin_memory": False, "drop_last": True,
                             "num_workers": 0},
        "val_dataloader": {},
        "trainer": {"max_epochs": a.epochs, "accelerator": "cpu", "enable_progress_bar": False},
    }
    full.main(data, json.loads(json.dumps(FEATHER)), learning, work, no_show=True, make_plots=False)

    # 3. the exported model, renamed, with the teacher's metadata (name, gear, loudness) carried over
    exported = sorted(work.rglob("*.nam"), key=lambda p: p.stat().st_mtime)
    if not exported:
        sys.exit("training produced no .nam file")
    with open(exported[-1]) as fp:
        student = json.load(fp)
    with open(teacher) as fp:
        tmeta = json.load(fp).get("metadata") or {}
    smeta = student.get("metadata") or {}
    for k, v in tmeta.items():
        if k not in ("loudness", "validation_esr", "training"):   # measured values must come from the student
            smeta[k] = v
    smeta["name"] = "F " + (tmeta.get("name") or teacher.stem)
    student["metadata"] = smeta
    dest = out / ("F" + teacher.name)
    with open(dest, "w") as fp:
        json.dump(student, fp)

    # 4. how close is it? ESR on the held-out validation section, both run by the C++ engine the MPC uses
    ys = work / "student_output.wav"
    subprocess.run([a.render, str(dest), a.input_wav, str(ys)], check=True, stdout=subprocess.DEVNULL)
    import soundfile
    t, _ = soundfile.read(str(y), dtype="float64")
    s_, _ = soundfile.read(str(ys), dtype="float64")
    t, s_ = t[-V3_VALIDATION:], s_[-V3_VALIDATION:]
    esr = float(((t - s_) ** 2).sum() / max((t ** 2).sum(), 1e-12))
    student["metadata"]["validation_esr"] = esr
    with open(dest, "w") as fp:
        json.dump(student, fp)
    print("wrote %s  ESR vs original: %.4f" % (dest, esr))


if __name__ == "__main__":
    main()
