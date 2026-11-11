"""Read-only CLI contract check against a supplied live_stack checkout.

Run with that checkout's .venv/bin/python. Only synthetic temporary artifacts
are used; no TensorRT, model loading, activation, service or hardware operation.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    runtime = Path(sys.argv[1]).resolve()
    validator = runtime / "run/check_prepared_model.py"
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    checks = 0
    with tempfile.TemporaryDirectory(prefix="gui-report-contract-") as directory:
        bundle = Path(directory)
        config = bundle / "train_config.json"
        engine = bundle / "engine.bin"
        checkpoint = bundle / "checkpoint.bin"
        report = bundle / "validation_report.json"
        for mode in ("current", "delayed_100hz"):
            cfg = dict(modelname="heart", prediction_mode=mode, ecg3_col="ecg3_raw")
            config.write_text(json.dumps(cfg))
            engine.write_bytes(b"synthetic engine fingerprint fixture")
            checkpoint.write_bytes(b"synthetic checkpoint fingerprint fixture")
            evidence = dict(
                schema="dkfz-prepared-model-v1", modelname="heart", model_variant="best",
                validation_passed=True, source_unchanged=True, disable_recovery=False,
                ecg3_source="ecg2_minus_ecg1",
                stream_validation=dict(passed=True, recordings=[dict(input="synthetic")]),
                sha256={key: hashlib.sha256(path.read_bytes()).hexdigest()
                        for key, path in dict(config=config, engine=engine, checkpoint=checkpoint).items()},
            )
            cases = [("matching", {}, True),
                     ("failed validation", dict(validation_passed=False), False),
                     ("changed source", dict(source_unchanged=False), False),
                     ("model identity", dict(modelname="other"), False),
                     ("variant", dict(model_variant="tuned"), False),
                     ("recovery policy", dict(disable_recovery=True), False)]
            for key in ("config", "engine", "checkpoint"):
                cases.append((key + " fingerprint", dict(sha256={**evidence["sha256"], key: "0" * 64}), False))
            if mode == "delayed_100hz":
                cases.extend([("ECG source", dict(ecg3_source="configured"), False),
                              ("missing streams", dict(stream_validation={}), False)])
            for name, changes, expected in cases:
                report.write_text(json.dumps({**evidence, **changes}))
                result = subprocess.run(
                    [sys.executable, str(validator), "--report", str(report), "--config", str(config),
                     "--engine", str(engine), "--checkpoint", str(checkpoint), "--model-variant", "best"],
                    cwd=runtime, env=env, capture_output=True, text=True, timeout=30,
                )
                if (result.returncode == 0) != expected:
                    raise AssertionError(f"{mode}: {name}: unexpected exit {result.returncode}\n{result.stderr}")
                checks += 1
    print(f"Passed {checks} read-only runtime validation-report CLI checks (synthetic fingerprints only)")


if __name__ == "__main__":
    main()
