import pandas as pd
import pytest

from june_events.decode import SEED_VENUE_ID
from june_events.enrich import enrich_with_people, enrich_with_venues
from june_events.io import load_people_lookup, load_raw_table, load_venues_lookup

from .conftest import REAL_EVENTS_FIXTURE as REAL_EVENTS_FILE
from .conftest import requires_real_events_fixture as requires_real_file


def test_enrich_with_venues_attaches_prefixed_venue_columns():
    event_df = pd.DataFrame({"venue_id": [1, 2], "time": [0.1, 0.2]})
    venues_lookup = pd.DataFrame({"venue_id": [1, 2], "type": ["school", "hospital"]})

    enriched = enrich_with_venues(event_df, venues_lookup)

    assert list(enriched["venue_type"]) == ["school", "hospital"]


def test_enrich_with_venues_keeps_nan_for_seed_infections():
    event_df = pd.DataFrame({"venue_id": [SEED_VENUE_ID, 2], "time": [0.1, 0.2]})
    venues_lookup = pd.DataFrame({"venue_id": [1, 2], "type": ["school", "hospital"]})

    enriched = enrich_with_venues(event_df, venues_lookup)

    assert pd.isna(enriched["venue_type"].iloc[0])
    assert enriched["venue_type"].iloc[1] == "hospital"


def test_enrich_with_people_attaches_prefixed_person_columns():
    event_df = pd.DataFrame({"person_id": [10, 20], "time": [0.1, 0.2]})
    people_lookup = pd.DataFrame({"person_id": [10, 20], "sex": ["f", "m"]})

    enriched = enrich_with_people(event_df, people_lookup)

    assert list(enriched["person_sex"]) == ["f", "m"]


def test_enrich_with_people_does_not_mutate_inputs():
    event_df = pd.DataFrame({"person_id": [10], "time": [0.1]})
    people_lookup = pd.DataFrame({"person_id": [10], "sex": ["f"]})

    enrich_with_people(event_df, people_lookup)

    assert list(event_df.columns) == ["person_id", "time"]
    assert list(people_lookup.columns) == ["person_id", "sex"]


def test_enrich_with_venues_raises_on_duplicate_lookup_ids():
    event_df = pd.DataFrame({"venue_id": [1], "time": [0.1]})
    venues_lookup = pd.DataFrame({"venue_id": [1, 1], "type": ["school", "hospital"]})

    with pytest.raises(ValueError, match="duplicate"):
        enrich_with_venues(event_df, venues_lookup)


def test_enrich_with_venues_forces_through_duplicate_lookup_ids_when_allowed():
    event_df = pd.DataFrame({"venue_id": [1], "time": [0.1]})
    venues_lookup = pd.DataFrame({"venue_id": [1, 1], "type": ["school", "hospital"]})

    enriched = enrich_with_venues(event_df, venues_lookup, allow_duplicate_ids=True)

    assert len(enriched) == 2


@requires_real_file
def test_enrich_with_venues_labels_real_seed_infections_via_synthetic_venue_row():
    infections = load_raw_table(REAL_EVENTS_FILE, "events/infections")
    venues_lookup = load_venues_lookup(REAL_EVENTS_FILE)

    enriched = enrich_with_venues(infections, venues_lookup)

    is_seed = infections["venue_id"] == SEED_VENUE_ID
    assert is_seed.any()
    assert (enriched.loc[is_seed, "venue_type"] == "infection_seed").all()
    assert len(enriched) == len(infections)


@requires_real_file
def test_enrich_with_people_matches_real_person_sex_for_a_sample_row():
    infections = load_raw_table(REAL_EVENTS_FILE, "events/infections")
    people_lookup = load_people_lookup(REAL_EVENTS_FILE, include_properties=False)

    enriched = enrich_with_people(infections, people_lookup)

    sample_person_id = infections["person_id"].iloc[0]
    expected_sex = people_lookup.loc[
        people_lookup["person_id"] == sample_person_id, "sex"
    ].iloc[0]
    assert enriched["person_sex"].iloc[0] == expected_sex
    assert len(enriched) == len(infections)
