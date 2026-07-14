import h5py
import numpy as np
import pandas as pd
import pytest

from june_events.load_enriched import load_enriched_events

REAL_EVENTS_FILE = "/home/gavin/Documents/Modern_Day/Xiying_Project/simulation_events.h5"

requires_real_file = pytest.mark.skipif(
    not pytest.importorskip("os").path.exists(REAL_EVENTS_FILE),
    reason="real simulation_events.h5 fixture not available on this machine",
)


def _write_minimal_file(path):
    with h5py.File(path, "w") as fh:
        infections = np.array(
            [(1, 10, 0.5, 0), (2, 20, 1.5, 255)],
            dtype=[
                ("person_id", "<i4"),
                ("venue_id", "<i4"),
                ("time", "<f8"),
                ("encounter_type_id", "u1"),
            ],
        )
        fh.create_dataset("events/infections", data=infections)

        people = np.array([(1, "f"), (2, "m")], dtype=[("person_id", "<i4"), ("sex", "S1")])
        fh.create_dataset("lookups/people", data=people)

        venues = np.array([(10, "school")], dtype=[("venue_id", "<i4"), ("type", "S10")])
        fh.create_dataset("lookups/venues", data=venues)

        fh.create_dataset(
            "metadata/registries/encounter_types",
            data=np.array(["social_encounters"], dtype="S30"),
        )
    return path


def test_load_enriched_events_decodes_and_joins_by_default(tmp_path):
    path = _write_minimal_file(str(tmp_path / "events.h5"))

    enriched = load_enriched_events(path, "events/infections")

    assert list(enriched["encounter_type"]) == ["social_encounters", "unknown"]
    assert list(enriched["person_sex"]) == ["f", "m"]
    assert enriched["venue_type"].iloc[0] == "school"
    assert pd.isna(enriched["venue_type"].iloc[1])


def test_load_enriched_events_can_opt_out_of_joins(tmp_path):
    path = _write_minimal_file(str(tmp_path / "events.h5"))

    enriched = load_enriched_events(path, "events/infections", with_people=False, with_venues=False)

    assert "person_sex" not in enriched.columns
    assert "venue_type" not in enriched.columns
    assert "encounter_type" in enriched.columns


def test_load_enriched_events_returns_none_for_missing_dataset(tmp_path):
    path = _write_minimal_file(str(tmp_path / "events.h5"))

    assert load_enriched_events(path, "events/deaths") is None


@requires_real_file
def test_load_enriched_events_matches_manual_chain_on_real_infections():
    from june_events.decode import decode_registry_column, load_registry
    from june_events.enrich import enrich_with_people, enrich_with_venues
    from june_events.io import load_people_lookup, load_raw_table, load_venues_lookup

    manual = load_raw_table(REAL_EVENTS_FILE, "events/infections")
    encounter_types = load_registry(REAL_EVENTS_FILE, "encounter_types")
    manual["encounter_type"] = decode_registry_column(manual, "encounter_type_id", encounter_types)
    manual = enrich_with_people(manual, load_people_lookup(REAL_EVENTS_FILE))
    manual = enrich_with_venues(manual, load_venues_lookup(REAL_EVENTS_FILE))

    enriched = load_enriched_events(REAL_EVENTS_FILE, "events/infections")

    assert list(enriched["encounter_type"]) == list(manual["encounter_type"])
    assert list(enriched["person_sex"]) == list(manual["person_sex"])
    assert len(enriched) == len(manual)
