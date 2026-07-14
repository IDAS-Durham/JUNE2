import pandas as pd
import pytest

from june_events.decode import decode_registry_column, load_registry
from june_events.io import load_raw_table

REAL_EVENTS_FILE = "/home/gavin/Documents/Modern_Day/Xiying_Project/simulation_events.h5"

requires_real_file = pytest.mark.skipif(
    not pytest.importorskip("os").path.exists(REAL_EVENTS_FILE),
    reason="real simulation_events.h5 fixture not available on this machine",
)


@requires_real_file
def test_decode_registry_column_maps_unset_encounter_type_to_unknown():
    infections = load_raw_table(REAL_EVENTS_FILE, "events/infections")
    encounter_types = load_registry(REAL_EVENTS_FILE, "encounter_types")

    decoded = decode_registry_column(
        infections, "encounter_type_id", encounter_types, unset_value=255
    )

    unset_rows = infections["encounter_type_id"] == 255
    assert (decoded[unset_rows] == "unknown").all()


@requires_real_file
def test_decode_registry_column_maps_known_index_to_registry_string():
    infections = load_raw_table(REAL_EVENTS_FILE, "events/infections")
    encounter_types = load_registry(REAL_EVENTS_FILE, "encounter_types")

    decoded = decode_registry_column(
        infections, "encounter_type_id", encounter_types, unset_value=255
    )

    known_rows = infections["encounter_type_id"] == 0
    assert (decoded[known_rows] == "social_encounters").all()


def test_decode_registry_column_maps_nan_to_no_match_label():
    df = pd.DataFrame({"symptom_id": [0.0, 1.0, float("nan"), 255.0]})

    decoded = decode_registry_column(
        df, "symptom_id", ["recovered", "healthy"], unset_value=255
    )

    assert list(decoded) == ["recovered", "healthy", "not_recorded", "unknown"]
