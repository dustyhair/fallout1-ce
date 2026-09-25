"""Exercise the campaign's pass gate without installed game data or X11."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MultiplayerCampaignTest(unittest.TestCase):
    def run_campaign(self, mode, full=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "game"
            data.mkdir()
            (data / "data").mkdir()
            for name in ("master.dat", "critter.dat", "fallout.cfg"):
                (data / name).touch()
            commands = root / "bin"
            commands.mkdir()
            scripts = {
                "fallout-ce": '''#!/bin/bash
role=guest
[[ $1 == --multiplayer-host=* ]] && role=host
port=${1##*:}
if [[ $role == host ]]; then
    port=${1##*=}
    /bin/sleep 0.02
    touch "$CAMPAIGN_TEST_READY/$port"
    echo 'MULTIPLAYER HOST: WAITING ON PORT'
else
    [[ -f "$CAMPAIGN_TEST_READY/$port" ]] || exit 1
fi
digest=123
if [[ $* == *--multiplayer-smoke-scenario=recovery* && $role == guest ]]; then
    [[ $CAMPAIGN_TEST_MODE == mismatch ]] && digest=456
    [[ $CAMPAIGN_TEST_MODE == missing ]] && digest=
fi
printf 'MULTIPLAYER_SMOKE_TEST_PASS role=%s' "$role"
[[ -n $digest ]] && printf ' digest=%s' "$digest"
printf '\\n'
''',
                "fallout-multiplayer-core-test": "#!/bin/sh\nexit 0\n",
                "xvfb-run": '#!/bin/sh\nshift\nexec "$@"\n',
                "sleep": "#!/bin/sh\nexit 0\n",
            }
            for name, contents in scripts.items():
                path = commands / name
                path.write_text(contents)
                path.chmod(0o755)
            runner = Path(__file__).resolve().parents[1] / "run_multiplayer_compatibility_campaign.sh"
            env = dict(os.environ, PATH=str(commands) + os.pathsep + os.environ["PATH"],
                       CAMPAIGN_TEST_MODE=mode, CAMPAIGN_TEST_READY=str(root),
                       FALLOUT_CAMPAIGN_FULL="1" if full else "0")
            result = subprocess.run(["bash", str(runner), str(commands / "fallout-ce"),
                                     str(data), str(root / "output")],
                                    env=env, capture_output=True, text=True, timeout=15)
            summary = (root / "output" / "summary.tsv").read_text()
            return result, summary

    def test_matching_digests_pass(self):
        result, summary = self.run_campaign("matching")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("scenarios=13", result.stdout)
        self.assertNotIn("FAIL", summary)

    def test_full_matrix_has_unique_scenarios(self):
        result, summary = self.run_campaign("matching", full=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("scenarios=42", result.stdout)
        names = [line.split("\t")[0] for line in summary.splitlines()[1:]]
        self.assertEqual(len(names), len(set(names)))

    def test_divergent_recovery_fails(self):
        result, summary = self.run_campaign("mismatch")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("digest mismatch", result.stderr)
        self.assertIn("recovery\t0\t0\tFAIL", summary)

    def test_missing_recovery_digest_fails(self):
        result, summary = self.run_campaign("missing")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("recovery\t0\t0\tFAIL", summary)


if __name__ == "__main__":
    unittest.main()
