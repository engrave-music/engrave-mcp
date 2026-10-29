"""Unit tests for oracle harness improvements (A2 review findings).

Tests:
  1. Semantic checker mutations:
     - Removes all spanners of a type -> detected
     - Shifts a spanner endpoint -> detected
     - Drops a note tie -> detected
     - Changes a note pitch -> detected
  2. Spectral centroid silence rule (exact PCM evidence, no rounded-RMS shortcut):
     - Windows with zero nonzero samples on both sides -> centroid ignored
     - Rounded-zero RMS with nonzero samples on either side -> centroid compared
     - Quiet tone (e.g. RMS 0.00010) within / beyond 2.0 Hz -> passed / detected
     - Asymmetric silence vs quiet tone -> detected/failed
     - Baseline without the evidence field -> rejected, not compared weakly
  3. EID token normalization:
     - All 9 EID-valued XML tags covered
     - Identity renaming preservation
     - Deliberate target modification detected
     - Legacy blanked tree rejected unless the explicit migration flag is given
  4. Rendered-page geometry and mutation controls (test_rendered_page.py).
"""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = Path(__file__).resolve().parent
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import check_semantic_dump as csd
import golden
from test_rendered_page import TestRenderedPageControls, TestSvgGeometry
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


class TestCentroidSilenceEvidence(unittest.TestCase):
    def audio(self, centroid, nonzero, rms=None):
        """A minimal audio.json with exact PCM silence evidence for one window."""
        return {
            "sampleRate": 44100, "channels": 2, "frames": 22050,
            "peak": [0.0, 0.0], "rms": [0.0, 0.0],
            "rmsEnvelope": [rms if rms is not None else [0.0, 0.0]],
            "centroidEnvelope": [centroid],
            "nonzeroSamples": [nonzero],
            "spectralCentroidHz": 0.0,
        }

    def test_exactly_silent_windows_ignore_centroid(self):
        """Both windows have no nonzero PCM sample, so their centroids are meaningless."""
        golden_audio = self.audio(0.0, [0, 0])
        actual_audio = self.audio(1500.0, [0, 0])
        self.assertTrue(golden.audio_equal(golden_audio, actual_audio))

    def test_rounded_zero_rms_with_samples_still_compared(self):
        """A rounded-zero RMS is not silence: one or two nonzero samples keep the 2 Hz rule.

        This is the A2 re-review case: a single 1-LSB sample stores RMS 0.0 in the 5-decimal
        envelope but the centroid moved from 11024.0 to 8011.4 Hz.
        """
        golden_audio = self.audio(11024.0, [1, 1])
        actual_audio = self.audio(8011.4, [2, 2])
        self.assertFalse(golden.audio_equal(golden_audio, actual_audio))

    def test_quiet_tone_within_tolerance_passes(self):
        """Quiet nonzero signals compare centroids against the 2.0 Hz tolerance."""
        golden_audio = self.audio(440.0, [22050, 22050], rms=[0.00010, 0.00010])
        actual_audio = self.audio(441.5, [22050, 22050], rms=[0.00010, 0.00010])
        self.assertTrue(golden.audio_equal(golden_audio, actual_audio))

    def test_quiet_tone_diverging_centroid_fails(self):
        """Quiet nonzero signals (e.g. RMS 0.00010) must still fail a > 2.0 Hz centroid move."""
        golden_audio = self.audio(100.0, [22050, 22050], rms=[0.00010, 0.00010])
        actual_audio = self.audio(10000.0, [22050, 22050], rms=[0.00010, 0.00010])
        self.assertFalse(golden.audio_equal(golden_audio, actual_audio))

    def test_asymmetric_silence_fails(self):
        """One all-zero window against a window with samples must not be skipped."""
        golden_audio = self.audio(0.0, [0, 0])
        actual_audio = self.audio(440.0, [2, 2], rms=[0.0002, 0.0002])
        self.assertFalse(golden.audio_equal(golden_audio, actual_audio))

    def test_missing_silence_evidence_is_rejected(self):
        """A baseline captured before the evidence field fails clearly instead of skipping."""
        golden_audio = self.audio(0.0, [0, 0])
        legacy = {key: value for key, value in golden_audio.items()
                  if key != golden.SILENCE_EVIDENCE}
        with tempfile.TemporaryDirectory(prefix="golden-audio-") as temporary:
            directory = Path(temporary)
            (directory / "golden").write_text(json.dumps(legacy), encoding="utf-8")
            (directory / "actual").write_text(json.dumps(golden_audio), encoding="utf-8")
            with self.assertRaises(golden.GoldenFormatError) as raised:
                golden.files_equal(directory / "golden", directory / "actual", "audio.json")
        message = str(raised.exception)
        self.assertIn(golden.SILENCE_EVIDENCE, message)
        self.assertIn("recapture", message)

    def test_silence_evidence_length_must_match_envelope(self):
        """Evidence for a different window count cannot gate the centroid comparison."""
        golden_audio = self.audio(0.0, [0, 0])
        actual_audio = self.audio(0.0, [0, 0])
        actual_audio["nonzeroSamples"] = []
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
        normalized = golden.normalize_mscx(mscx_input)

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
        norm_a = golden.normalize_mscx(score_a)
        norm_b = golden.normalize_mscx(score_b)
        self.assertIn("<startMeasure>e1</startMeasure>", norm_a)
        self.assertIn("<startMeasure>e2</startMeasure>", norm_b)
        self.assertNotEqual(norm_a, norm_b, "Scores targeting different measures must produce different normalized tokens")

    def test_legacy_blank_eids_hides_target_change(self):
        """The 4.7.6 blanked representation cannot see a retargeted lock (why it is not automatic)."""
        score_a = """<Score>
  <Measure><eid>101</eid></Measure>
  <Measure><eid>102</eid></Measure>
  <PageLocks><pageLock><startMeasure>101</startMeasure></pageLock></PageLocks>
</Score>"""
        score_b = """<Score>
  <Measure><eid>101</eid></Measure>
  <Measure><eid>102</eid></Measure>
  <PageLocks><pageLock><startMeasure>102</startMeasure></pageLock></PageLocks>
</Score>"""
        blank_a = golden.normalize_mscx(score_a, legacy_blank_eids=True)
        blank_b = golden.normalize_mscx(score_b, legacy_blank_eids=True)
        self.assertIn("<startMeasure/>", blank_a)
        self.assertIn("<sharedPart/>", golden.normalize_mscx(
            "<Score><sharedPart>501</sharedPart></Score>", legacy_blank_eids=True))
        self.assertEqual(blank_a, blank_b)

    def test_legacy_tree_rejected_without_explicit_flag(self):
        """A blanked-EID baseline is only readable through --legacy-blank-eids."""
        with tempfile.TemporaryDirectory(prefix="golden-legacy-") as temporary:
            root = Path(temporary) / "legacy"
            (root / "fixture.mscx").mkdir(parents=True)
            (root / "fixture.mscx" / "score.mscx").write_text(
                '<museScore version="4.70">\n<Score><eid/></Score>\n</museScore>\n',
                encoding="utf-8")
            self.assertTrue(golden.uses_blanked_eids(root))
            with self.assertRaises(SystemExit) as raised:
                golden.require_eid_mode(root, legacy_blank_eids=False)
            self.assertIn("--legacy-blank-eids", str(raised.exception))
            golden.require_eid_mode(root, legacy_blank_eids=True)
        # A token-mode tree needs no flag.
        with tempfile.TemporaryDirectory(prefix="golden-token-") as temporary:
            root = Path(temporary) / "token"
            (root / "fixture.mscx").mkdir(parents=True)
            (root / "fixture.mscx" / "score.mscx").write_text(
                '<museScore version="5.00">\n<Score><eid>e1</eid></Score>\n</museScore>\n',
                encoding="utf-8")
            self.assertFalse(golden.uses_blanked_eids(root))
            golden.require_eid_mode(root, legacy_blank_eids=False)


if __name__ == "__main__":
    unittest.main()
