# Fuzzing integration (OSS-Fuzz format)

These files build the three libFuzzer harnesses in `../fuzz/` in the OSS-Fuzz
toolchain image, and are shared by two consumers:

- **ClusterFuzzLite** — runs continuous fuzzing in this repo's own GitHub
  Actions (`.github/workflows/cflite-pr.yml` on PRs, `cflite-cron.yml` daily).
  No external approval needed; active now.
- **OSS-Fuzz** — the same `project.yaml` / `Dockerfile` / `build.sh` can be
  submitted to <https://github.com/google/oss-fuzz> under `projects/libzarr/`
  for Google-hosted continuous fuzzing. That submission needs a maintainer as
  `primary_contact` and Google's acceptance; see
  <https://google.github.io/oss-fuzz/getting-started/new-project-guide/>.

Local reproduction of the container build (needs Docker):

```sh
git clone https://github.com/google/oss-fuzz
python3 oss-fuzz/infra/helper.py build_image --external libzarr .
python3 oss-fuzz/infra/helper.py build_fuzzers --external --sanitizer address libzarr .
python3 oss-fuzz/infra/helper.py run_fuzzer --external libzarr fuzz_metadata
```

Corpus persistence between batch runs is not yet configured (each run starts
from the synthetic seeds in `fuzz/gen_seeds.py`); add a ClusterFuzzLite
`storage-repo` when a persistent corpus is wanted.
