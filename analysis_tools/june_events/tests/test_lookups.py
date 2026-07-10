import h5py
import numpy as np
import pytest

from june_events.io import load_people_lookup, load_venues_lookup

REAL_EVENTS_FILE = "/home/gavin/Documents/Modern_Day/Xiying_Project/simulation_events.h5"

requires_real_file = pytest.mark.skipif(
    not pytest.importorskip("os").path.exists(REAL_EVENTS_FILE),
    reason="real simulation_events.h5 fixture not available on this machine",
)


@requires_real_file
def test_load_venues_lookup_decodes_byte_columns_to_str():
    venues = load_venues_lookup(REAL_EVENTS_FILE)

    assert isinstance(venues["name"].iloc[0], str)
    assert isinstance(venues["type"].iloc[0], str)


@requires_real_file
def test_load_people_lookup_decodes_byte_columns_to_str():
    people = load_people_lookup(REAL_EVENTS_FILE, include_properties=False)

    assert isinstance(people["sex"].iloc[0], str)
    assert isinstance(people["schedule_type"].iloc[0], str)


@requires_real_file
def test_load_people_lookup_merges_properties_by_position():
    people = load_people_lookup(REAL_EVENTS_FILE, include_properties=True)

    assert "ethnicity" in people.columns
    assert len(people) == 439560
    assert isinstance(people["ethnicity"].iloc[0], str)


def test_load_people_lookup_handles_file_missing_properties_group(tmp_path):
    path = tmp_path / "minimal_simulation_events.h5"
    dtype = [("person_id", "<i4"), ("age", "<f8")]
    with h5py.File(path, "w") as fh:
        fh.create_dataset(
            "lookups/people", data=np.array([(1, 30.0)], dtype=dtype)
        )

    people = load_people_lookup(str(path), include_properties=True)

    assert list(people.columns) == ["person_id", "age"]
