import pandas as pd
import pytest

from june_events.decode import decode_registry_column, load_registry

REAL_EVENTS_FILE = "/home/gavin/Documents/Modern_Day/Xiying_Project/simulation_events.h5"

requires_real_file = pytest.mark.skipif(
    not pytest.importorskip("os").path.exists(REAL_EVENTS_FILE),
    reason="real simulation_events.h5 fixture not available on this machine",
)


@requires_real_file
def test_load_registry_returns_ordered_strings_for_encounter_types():
    registry = load_registry(REAL_EVENTS_FILE, "encounter_types")

    assert registry == ["social_encounters"]


@requires_real_file
def test_load_registry_returns_ordered_strings_for_symptoms():
    registry = load_registry(REAL_EVENTS_FILE, "symptoms")

    assert registry[:3] == ["recovered", "healthy", "exposed"]


def test_load_registry_returns_none_for_absent_registry(tmp_path):
    minimal_path = tmp_path / "minimal_simulation_events.h5"
    import h5py

    with h5py.File(minimal_path, "w") as fh:
        fh.create_dataset("events/deaths", data=[(1, 2, 0.5)])

    result = load_registry(str(minimal_path), "encounter_types")

    assert result is None


def test_decode_registry_column_raises_for_index_outside_registry():
    df = pd.DataFrame({"symptom_id": [0, 5]})

    with pytest.raises(KeyError, match="symptom_id"):
        decode_registry_column(df, "symptom_id", ["healthy", "exposed"])
