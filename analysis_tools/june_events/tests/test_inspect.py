import h5py
import pytest

from june_events.inspect import inspect_file

REAL_EVENTS_FILE = "/home/gavin/Documents/Modern_Day/Xiying_Project/simulation_events.h5"

requires_real_file = pytest.mark.skipif(
    not pytest.importorskip("os").path.exists(REAL_EVENTS_FILE),
    reason="real simulation_events.h5 fixture not available on this machine",
)


@requires_real_file
def test_inspect_file_reports_dataset_shape_dtype_and_nbytes():
    summary = inspect_file(REAL_EVENTS_FILE)

    deaths = next(d for d in summary.datasets if d.path == "events/deaths")

    assert deaths.n_rows == 1910
    assert deaths.nbytes == 30560
    assert "person_id" in deaths.dtype


@requires_real_file
def test_inspect_file_decodes_registries_to_plain_strings():
    summary = inspect_file(REAL_EVENTS_FILE)

    assert summary.registries["symptoms"] == [
        "recovered",
        "healthy",
        "exposed",
        "asymptomatic",
        "mild",
        "severe",
        "hospitalised",
        "intensive_care",
        "dead_home",
        "dead_hospital",
        "dead_icu",
    ]
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
    assert encounters.n_rows == 3001776


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
