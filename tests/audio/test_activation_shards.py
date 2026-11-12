#!/usr/bin/env python3
"""Verify activation partitions through the real Qt test executable."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    tags = ("typing-laptop-40-mono", "typing-laptop-25-mono")
    with tempfile.TemporaryDirectory(prefix="squad-activation-shards-") as output:
        for tag in tags:
            selected = 0
            for shard in range(2):
                env = os.environ.copy()
                env.update(SQUAD_CORPUS_SUITE="smoke", SQUAD_CORPUS_SHARDS="2",
                           SQUAD_CORPUS_SHARD=str(shard), SQUAD_CORPUS_OUTPUT=output)
                result = subprocess.run([str(binary), "speechActivation:" + tag], env=env,
                                        capture_output=True, text=True, timeout=30)
                text = result.stdout + result.stderr
                if result.returncode == 0 and f"PASS   : CorpusTests::speechActivation({tag})" in text:
                    selected += 1
                elif result.returncode != 1 or "Unknown testdata for function speechActivation()" not in text:
                    raise SystemExit(f"Unexpected result for shard {shard}, {tag}:\n{text}")
            if selected != 1:
                raise SystemExit(f"Expected {tag} in exactly one shard, found {selected}.")
    print("Activation shard selection passed for both real audio cases.")


if __name__ == "__main__":
    main()
