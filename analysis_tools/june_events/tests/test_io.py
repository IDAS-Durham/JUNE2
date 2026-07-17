import h5py
import pytest

from june_events.inspect import inspect_file
from june_events.io import load_raw_table

from .conftest import REAL_EVENTS_FIXTURE as REAL_EVENTS_FILE
from .conftest import requires_real_events_fixture as requires_real_file


@requires_real_file
@pytest.mark.parametrize("event_type", ["infections", "symptom_changes", "deaths"])
def test_load_raw_table_matches_inspect_file_row_count_for_other_event_types(event_type):
    summary = inspect_file(REAL_EVENTS_FILE)
    expected = next(
        d for d in summary.datasets if d.path == f"events/{event_type}"
    ).n_rows

    df = load_raw_table(REAL_EVENTS_FILE, f"events/{event_type}")

    assert len(df) == expected


def test_load_raw_table_returns_none_and_warns_for_absent_dataset_path(tmp_path, caplog):
    minimal_path = tmp_path / "minimal_simulation_events.h5"
    with h5py.File(minimal_path, "w") as fh:
        fh.create_dataset("events/deaths", data=[(1, 2, 0.5)])

    with caplog.at_level("WARNING"):
        result = load_raw_table(str(minimal_path), "lookups/people_properties")

    assert result is None
    assert "lookups/people_properties" in caplog.text


def test_load_raw_table_chunked_read_matches_single_read(tmp_path):
    import numpy as np

    path = tmp_path / "chunked_simulation_events.h5"
    dtype = [("person_id", "<i4"), ("venue_id", "<i4"), ("time", "<f8")]
    rows = np.array([(i, i * 10, float(i)) for i in range(10)], dtype=dtype)
    with h5py.File(path, "w") as fh:
        fh.create_dataset("events/deaths", data=rows)

    chunked = load_raw_table(
        str(path), "events/deaths", chunk_threshold_bytes=1, chunk_rows=3
    )

    assert len(chunked) == 10
    assert list(chunked["person_id"]) == list(range(10))
