import pandas as pd
import pytest

from june_events.enrich import enrich_with_state_at_time
from june_events.io import load_raw_table

from .conftest import REAL_EVENTS_FIXTURE as REAL_EVENTS_FILE
from .conftest import requires_real_events_fixture as requires_real_file


def test_enrich_with_state_at_time_attaches_most_recent_prior_state():
    event_df = pd.DataFrame({"infector_id": [1, 1], "time": [1.0, 3.0]})
    state_change_df = pd.DataFrame(
        {"person_id": [1, 1], "time": [0.5, 2.0], "new_symptom_id": [2, 3]}
    )

    enriched = enrich_with_state_at_time(event_df, state_change_df, id_column="infector_id")

    assert list(enriched["new_symptom_id"]) == [2, 3]


def test_enrich_with_state_at_time_preserves_original_row_order():
    event_df = pd.DataFrame({"infector_id": [1, 1], "time": [3.0, 1.0]})
    state_change_df = pd.DataFrame(
        {"person_id": [1, 1], "time": [0.5, 2.0], "new_symptom_id": [2, 3]}
    )

    enriched = enrich_with_state_at_time(event_df, state_change_df, id_column="infector_id")

    assert list(enriched["time"]) == [3.0, 1.0]
    assert list(enriched["new_symptom_id"]) == [3, 2]


def test_enrich_with_state_at_time_does_not_mutate_inputs():
    event_df = pd.DataFrame({"infector_id": [1], "time": [3.0]})
    state_change_df = pd.DataFrame(
        {"person_id": [1], "time": [0.5], "new_symptom_id": [2]}
    )

    enrich_with_state_at_time(event_df, state_change_df, id_column="infector_id")

    assert list(event_df.columns) == ["infector_id", "time"]
    assert list(state_change_df.columns) == ["person_id", "time", "new_symptom_id"]


@requires_real_file
def test_enrich_with_state_at_time_matches_real_infector_symptom_id_when_logged():
    infections = load_raw_table(REAL_EVENTS_FILE, "events/infections")
    symptom_changes = load_raw_table(REAL_EVENTS_FILE, "events/symptom_changes")

    enriched = enrich_with_state_at_time(
        infections, symptom_changes, id_column="infector_id"
    )

    has_prior_state = enriched["new_symptom_id"].notna()
    assert has_prior_state.sum() > 0
    matches = (
        enriched.loc[has_prior_state, "new_symptom_id"]
        == infections.loc[has_prior_state, "infector_symptom_id"]
    )
    assert matches.mean() > 0.99


@requires_real_file
def test_enrich_with_state_at_time_generalises_to_deaths_call_site():
    deaths = load_raw_table(REAL_EVENTS_FILE, "events/deaths")
    symptom_changes = load_raw_table(REAL_EVENTS_FILE, "events/symptom_changes")

    enriched = enrich_with_state_at_time(deaths, symptom_changes, id_column="person_id")

    assert len(enriched) == len(deaths)
    assert enriched["new_symptom_id"].notna().all()
