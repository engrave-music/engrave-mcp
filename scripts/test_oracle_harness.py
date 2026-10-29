"""Unit tests for oracle harness improvements (A2 review findings).

Tests:
  1. Semantic checker mutations:
     - Removes all spanners of a type -> detected
     - Shifts a spanner endpoint -> detected
     - Drops a note tie -> detected
     - Changes a note pitch -> detected
  2. Spectral centroid near-silence tolerances:
     - Silence below 16-bit quantisation floor (< 5e-6, stored 0.0) -> ignored
     - Quiet tone above floor (e.g. RMS 0.00010) within tolerance -> passed
     - Quiet tone above floor (e.g. RMS 0.00010) diverging (> 2.0 Hz) -> detected/failed
     - Asymmetric silence vs quiet tone -> detected/failed
  3. EID token normalization:
     - All 9 EID-valued XML tags covered
     - Identity renaming preservation
     - Deliberate target modification detected
     - Blanked EID fallback support for 4.7.6 baseline compatibility
"""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = Path(__file__).resolve().parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import check_semantic_dump as csd
import golden
from test_roundtrip import parse_mscx


class TestSemanticCheckerMutations(unittest.TestCase):
    def setUp(self):
        self.fixtures_dir = ROOT / "tests/golden/windows"
        self.hairpins_fixture = self.fixtures_dir / "hairpins_and_repeats.mscx"
        self.linked_fixture = self.fixtures_dir / "linked_parts.mscx"

        self.hairpins_dump = json.loads((self.hairpins_fixture / "semantic.json").read_text(encoding="utf-8"))
        self.hairpins_parsed = parse_mscx((self.hairpins_fixture / "score.mscx").read_text(encoding="utf-8"))

        self.linked_dump = json.loads((self.linked_fixture / "semantic.json").read_text(encoding="utf-8"))
        self.linked_parsed = parse_mscx((self.linked_fixture / "score.mscx").read_text(encoding="utf-8"))

    def test_baseline_matches(self):
        results = []
        for chk in csd.CHECKS:
            results.extend(chk(self.hairpins_dump, self.hairpins_parsed, limit=3))
        self.assertTrue(all(r.matched for r in results), "Base hairpins fixture should match")

    def test_mutation_remove_all_spanners_of_type(self):
        """Verify that check_spanners fails when all spanners of a supported type vanish."""
        mutated_dump = copy.deepcopy(self.hairpins_dump)
        # Verify fixture has HairPin spanners initially
        hp_count = sum(1 for s in mutated_dump.get("spanners", []) if s["type"] == "HairPin")
        self.assertGreater(hp_count, 0, "Fixture must contain HairPin spanners")

        # Mutate: remove all HairPins from dump
        mutated_dump["spanners"] = [s for s in mutated_dump["spanners"] if s["type"] != "HairPin"]

        results = csd.check_spanners(mutated_dump, self.hairpins_parsed, limit=3)
        failed = [r for r in results if not r.matched]
        self.assertTrue(len(failed) > 0, "Removing all HairPin spanners must be detected")
        # Ensure details mention the missing HairPin spanner
        all_details = " ".join(d for r in failed for d in r.details)
        self.assertIn("HairPin", all_details)

    def test_mutation_shift_spanner_endpoint(self):
        """Verify that shifting a spanner tick2 endpoint is detected."""
        mutated_dump = copy.deepcopy(self.hairpins_dump)
        shifted = False
        for s in mutated_dump["spanners"]:
            if s["type"] == "HairPin":
                s["tick2"] += 480
                shifted = True
                break
        self.assertTrue(shifted, "Must have shifted a spanner endpoint")

        results = csd.check_spanners(mutated_dump, self.hairpins_parsed, limit=3)
        failed = [r for r in results if not r.matched]
        self.assertTrue(len(failed) > 0, "Shifting a spanner endpoint must be detected")

    def test_mutation_drop_tie(self):
        """Verify that dropping a tie on a note is detected by check_events."""
        mutated_dump = copy.deepcopy(self.linked_dump)
        dropped_tie = False
        for staff in mutated_dump["events"]:
            for voice in staff:
                for event in voice:
                    for note in event.get("notes", []):
                        if "tieTo" in note:
                            del note["tieTo"]
                            dropped_tie = True
                            break
                    if dropped_tie:
                        break
                if dropped_tie:
                    break
        self.assertTrue(dropped_tie, "Must have found a note tie in linked_parts")

        results = csd.check_events(mutated_dump, self.linked_parsed, limit=3)
        failed = [r for r in results if not r.matched]
        self.assertTrue(len(failed) > 0, "Dropping a note tie must be detected")

    def test_mutation_change_pitch(self):
        """Verify that changing a note pitch is detected by check_events."""
        mutated_dump = copy.deepcopy(self.hairpins_dump)
        changed_pitch = False
        for staff in mutated_dump["events"]:
            for voice in staff:
                for event in voice:
                    notes = event.get("notes", [])
                    if notes:
                        notes[0]["pitch"] += 2
                        changed_pitch = True
                        break
                if changed_pitch:
                    break
        self.assertTrue(changed_pitch, "Must have changed a note pitch")

        results = csd.check_events(mutated_dump, self.hairpins_parsed, limit=3)
        failed = [r for r in results if not r.matched]
        self.assertTrue(len(failed) > 0, "Changing note pitch must be detected")


class TestCentroidNearSilenceTolerances(unittest.TestCase):
    def test_numerical_silence_below_quantization_floor_ignored(self):
        """When both golden and actual are 0.0 RMS (below 16-bit noise floor), centroid differences are ignored."""
        golden_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0, 0.0], "rms": [0.0, 0.0],
            "rmsEnvelope": [[0.0, 0.0], [0.0, 0.0]],
            "centroidEnvelope": [0.0, 0.0],
            "spectralCentroidHz": 0.0,
        }
        actual_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0, 0.0], "rms": [0.0, 0.0],
            "rmsEnvelope": [[0.0, 0.0], [0.0, 0.0]],
            # Centroid calculated on sub-quantization floating point noise (e.g. 1500 Hz vs 0 Hz)
            "centroidEnvelope": [1500.0, 2300.0],
            "spectralCentroidHz": 0.0,
        }
        self.assertTrue(golden.audio_equal(golden_audio, actual_audio))

    def test_quiet_tone_above_floor_within_tolerance_passes(self):
        """When both golden and actual have quiet nonzero signals, centroids within tolerance pass."""
        golden_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0002, 0.0002], "rms": [0.00010, 0.00010],
            "rmsEnvelope": [[0.00010, 0.00010]],
            "centroidEnvelope": [440.0],
            "spectralCentroidHz": 440.0,
        }
        actual_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0002, 0.0002], "rms": [0.00010, 0.00010],
            "rmsEnvelope": [[0.00010, 0.00010]],
            "centroidEnvelope": [441.5],  # diff 1.5 Hz <= 2.0 Hz
            "spectralCentroidHz": 440.5,
        }
        self.assertTrue(golden.audio_equal(golden_audio, actual_audio))

    def test_quiet_tone_above_floor_diverging_centroid_fails(self):
        """Quiet nonzero signals (e.g. RMS 0.00010) must still compare centroids and fail if diff > 2.0 Hz."""
        golden_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0002, 0.0002], "rms": [0.00010, 0.00010],
            "rmsEnvelope": [[0.00010, 0.00010]],
            "centroidEnvelope": [100.0],
            "spectralCentroidHz": 100.0,
        }
        actual_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0002, 0.0002], "rms": [0.00010, 0.00010],
            "rmsEnvelope": [[0.00010, 0.00010]],
            "centroidEnvelope": [10000.0],  # diff 9900 Hz > 2.0 Hz
            "spectralCentroidHz": 100.0,
        }
        self.assertFalse(golden.audio_equal(golden_audio, actual_audio))

    def test_asymmetric_silence_fails(self):
        """If one side is silent (0.0) and the other side is quiet tone, must not be ignored."""
        golden_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0, 0.0], "rms": [0.0, 0.0],
            "rmsEnvelope": [[0.0, 0.0]],
            "centroidEnvelope": [0.0],
            "spectralCentroidHz": 0.0,
        }
        actual_audio = {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0002, 0.0002], "rms": [0.00010, 0.00010],
            "rmsEnvelope": [[0.00010, 0.00010]],
            "centroidEnvelope": [440.0],
            "spectralCentroidHz": 440.0,
        }
        self.assertFalse(golden.audio_equal(golden_audio, actual_audio))


class TestEidTokenNormalization(unittest.TestCase):
    def test_all_eid_tags_normalized_and_identity_preserved(self):
        """Verify all 9 EID tags (including PageLocks and SharedPart) map consistently."""
        mscx_input = """<museScore version="5.00">
  <Score>
    <eid>98765432101</eid>
    <Staff id="1">
      <Measure>
        <eid>10001</eid>
        <voice>
          <Chord>
            <eid>20001</eid>
            <Note>
              <eid>30001</eid>
            </Note>
          </Chord>
        </voice>
      </Measure>
      <Measure>
        <eid>10002</eid>
        <mmRestLast>10001</mmRestLast>
      </Measure>
    </Staff>
    <PageLocks>
      <pageLock>
        <startMeasure>10001</startMeasure>
        <endMeasure>10002</endMeasure>
      </pageLock>
    </PageLocks>
    <SystemLocks>
      <systemLock>
        <startMeasure>10001</startMeasure>
        <endMeasure>10002</endMeasure>
      </systemLock>
    </SystemLocks>
    <Part>
      <eid>40001</eid>
      <sharedPart>50001</sharedPart>
    </Part>
    <SharedPart>
      <eid>50001</eid>
    </SharedPart>
    <Spanner type="Slur">
      <eid>60001</eid>
      <startElement>30001</startElement>
      <endElement>30001</endElement>
    </Spanner>
  </Score>
</museScore>"""
        normalized = golden.normalize_mscx(mscx_input, blank_eids=False)

        # Confirm all 9 tags are normalized to eN tokens
        self.assertIn("<eid>e1</eid>", normalized)
        self.assertIn("<eid>e2</eid>", normalized)
        # mmRestLast refers to 10001 (which was e2)
        self.assertIn("<mmRestLast>e2</mmRestLast>", normalized)
        # startMeasure and endMeasure refer to 10001 (e2) and 10002 (e5)
        self.assertIn("<startMeasure>e2</startMeasure>", normalized)
        self.assertIn("<endMeasure>e5</endMeasure>", normalized)
        # sharedPart in Part refers to 50001
        self.assertIn("<sharedPart>e7</sharedPart>", normalized)
        self.assertIn("<SharedPart>\n      <eid>e7</eid>", normalized)
        # startElement and endElement refer to Note 30001 (e4)
        self.assertIn("<startElement>e4</startElement>", normalized)
        self.assertIn("<endElement>e4</endElement>", normalized)

    def test_deliberate_target_change_detected(self):
        """Verify that changing the lock target changes the normalized representation."""
        score_a = """<Score>
  <Measure><eid>101</eid></Measure>
  <Measure><eid>102</eid></Measure>
  <PageLocks><pageLock><startMeasure>101</startMeasure><endMeasure>102</endMeasure></pageLock></PageLocks>
</Score>"""
        score_b = """<Score>
  <Measure><eid>101</eid></Measure>
  <Measure><eid>102</eid></Measure>
  <PageLocks><pageLock><startMeasure>102</startMeasure><endMeasure>102</endMeasure></pageLock></PageLocks>
</Score>"""
        norm_a = golden.normalize_mscx(score_a, blank_eids=False)
        norm_b = golden.normalize_mscx(score_b, blank_eids=False)
        self.assertNotEqual(norm_a, norm_b, "Scores targeting different measures must produce different normalized tokens")

    def test_blanked_mode_fallback(self):
        """When blank_eids is True, all tags are replaced with self-closing tags (<tag/>)."""
        sample = """<Score>
  <eid>101</eid>
  <startMeasure>101</startMeasure>
  <sharedPart>501</sharedPart>
</Score>"""
        blanked = golden.normalize_mscx(sample, blank_eids=True)
        self.assertIn("<eid/>", blanked)
        self.assertIn("<startMeasure/>", blanked)
        self.assertIn("<sharedPart/>", blanked)


if __name__ == "__main__":
    unittest.main()
