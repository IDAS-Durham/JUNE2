import h5py
import numpy as np
import pytest

from june_events.io import load_people_lookup, load_venues_lookup

from .conftest import REAL_EVENTS_FIXTURE as REAL_EVENTS_FILE
from .conftest import requires_real_events_fixture as requires_real_file


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
    with h5py.File(REAL_EVENTS_FILE, "r") as fh:
        assert len(people) == fh["lookups/people"].shape[0]
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


def test_load_people_lookup_raises_when_property_length_mismatches_people(tmp_path):
    path = tmp_path / "minimal_simulation_events.h5"
    dtype = [("person_id", "<i4"), ("age", "<f8")]
    with h5py.File(path, "w") as fh:
        fh.create_dataset(
            "lookups/people", data=np.array([(1, 30.0), (2, 40.0)], dtype=dtype)
        )
        fh.create_dataset("lookups/people_properties/ethnicity", data=np.array(["a"], dtype="S1"))

    with pytest.raises(ValueError, match="ethnicity"):
        load_people_lookup(str(path), include_properties=True)
