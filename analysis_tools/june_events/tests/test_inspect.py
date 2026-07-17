import h5py
import pytest

from june_events.inspect import inspect_file

from .conftest import REAL_EVENTS_FIXTURE as REAL_EVENTS_FILE
from .conftest import requires_real_events_fixture as requires_real_file


@requires_real_file
def test_inspect_file_reports_dataset_shape_dtype_and_nbytes():
    summary = inspect_file(REAL_EVENTS_FILE)

    deaths = next(d for d in summary.datasets if d.path == "events/deaths")

    with h5py.File(REAL_EVENTS_FILE, "r") as fh:
        expected_dataset = fh["events/deaths"]
        assert deaths.n_rows == expected_dataset.shape[0]
        assert deaths.nbytes == expected_dataset.nbytes
    assert "person_id" in deaths.dtype


@requires_real_file
def test_inspect_file_decodes_registries_to_plain_strings():
    summary = inspect_file(REAL_EVENTS_FILE)

    with h5py.File(REAL_EVENTS_FILE, "r") as fh:
        expected_symptoms = [
            value.decode() for value in fh["metadata/registries/symptoms"][:]
        ]
    assert summary.registries["symptoms"] == expected_symptoms
    assert all(isinstance(name, str) for name in summary.registries["symptoms"])


@requires_real_file
def test_inspect_file_includes_nested_group_datasets():
    summary = inspect_file(REAL_EVENTS_FILE)
    paths = {d.path for d in summary.datasets}

    assert "lookups/people_properties/ethnicity" in paths
    assert "lookups/population_networks/friendships/person_id" in paths


@requires_real_file
def test_inspect_file_never_reads_event_or_lookup_row_data(monkeypatch):
    original_getitem = h5py.Dataset.__getitem__

    def guarded_getitem(self, key):
        if not self.name.startswith("/metadata/registries/"):
            raise AssertionError(
                f"inspect_file() read row data from {self.name!r} via __getitem__"
            )
        return original_getitem(self, key)

    monkeypatch.setattr(h5py.Dataset, "__getitem__", guarded_getitem)

    summary = inspect_file(REAL_EVENTS_FILE)

    encounters = next(
        d for d in summary.datasets if d.path == "events/coordinated_encounters"
    )
    with h5py.File(REAL_EVENTS_FILE, "r") as fh:
        assert encounters.n_rows == fh["events/coordinated_encounters"].shape[0]


def test_inspect_file_handles_file_missing_optional_tables(tmp_path):
    minimal_path = tmp_path / "minimal_simulation_events.h5"
    with h5py.File(minimal_path, "w") as fh:
        fh.create_dataset("events/deaths", data=[(1, 2, 0.5)])
        fh.create_dataset("lookups/people", data=[(1, 30.0)])
        # No people_properties, population_networks, or coordinated_encounters.

    summary = inspect_file(str(minimal_path))

    paths = {d.path for d in summary.datasets}
    assert paths == {"events/deaths", "lookups/people"}
    assert summary.registries == {}
