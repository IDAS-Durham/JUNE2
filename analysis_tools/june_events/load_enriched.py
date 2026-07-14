import logging

import pandas as pd

from .decode import (
    NO_INFECTOR_ID,
    UNSET_REGISTRY_INDEX,
    decode_registry_column,
    load_registry,
)
from .enrich import enrich_with_people, enrich_with_venues
from .io import load_people_lookup, load_raw_table, load_venues_lookup

logger = logging.getLogger(__name__)

DEFAULT_REGISTRY_COLUMNS = {
    "encounter_type_id": "encounter_types",
    "infector_symptom_id": "symptoms",
    "old_symptom_id": "symptoms",
    "new_symptom_id": "symptoms",
}

# Only encounter_type_id has a real engine-side sentinel (uint8_t default 255,
# event_logger.h). The symptom_id columns default to 0 ("recovered") — a real
# registry entry, not a sentinel — so they get no entry here; decode_registry_column
# then treats every index as ordinary registry data.
REGISTRY_SENTINELS = {
    "encounter_type_id": UNSET_REGISTRY_INDEX,
}

# encounter_type_id's UNSET_REGISTRY_INDEX sentinel means "ordinary venue-level
# force of infection", not a missing value — decode_registry_column's generic
# "unknown" would understate the majority case.
UNSET_LABEL_OVERRIDES = {
    "encounter_type_id": "regular_non_coordinated_encounter",
}

NO_INFECTOR_LABEL = "no_infector"


def _decoded_column_name(id_column: str) -> str:
    if id_column.endswith("_id"):
        return id_column[: -len("_id")]
    return id_column


def _mask_infector_symptom_for_no_infector(events):
    # infector_symptom_id defaults to 0 ("recovered") whenever infector_id is
    # -1 (seed/fomite/compartmental infections) — the engine has no infector
    # to read a symptom from. Mask those rows before registry decode so they
    # don't come back as a fabricated "recovered" infector.
    if "infector_id" not in events.columns or "infector_symptom_id" not in events.columns:
        return events
    events = events.copy()
    events.loc[events["infector_id"] == NO_INFECTOR_ID, "infector_symptom_id"] = pd.NA
    return events


def load_enriched_events(
    path: str,
    dataset_path: str,
    *,
    with_people: bool = True,
    with_venues: bool = True,
    registry_columns: dict | None = None,
    people_prefix: str = "person_",
    venues_prefix: str = "venue_",
):
    events = load_raw_table(path, dataset_path)
    if events is None:
        return None

    if registry_columns is None:
        registry_columns = DEFAULT_REGISTRY_COLUMNS

    events = _mask_infector_symptom_for_no_infector(events)

    loaded_registries = {}
    for id_column, registry_name in registry_columns.items():
        if id_column not in events.columns:
            continue
        if registry_name not in loaded_registries:
            loaded_registries[registry_name] = load_registry(path, registry_name)
        registry = loaded_registries[registry_name]
        if registry is None:
            logger.warning(
                "registry %r not found in %r, skipping decode of %r",
                registry_name,
                path,
                id_column,
            )
            continue
        decode_kwargs = {}
        if id_column in REGISTRY_SENTINELS:
            decode_kwargs["unset_value"] = REGISTRY_SENTINELS[id_column]
        if id_column in UNSET_LABEL_OVERRIDES:
            decode_kwargs["unset_label"] = UNSET_LABEL_OVERRIDES[id_column]
        if id_column == "infector_symptom_id":
            decode_kwargs["no_match_label"] = NO_INFECTOR_LABEL
        events[_decoded_column_name(id_column)] = decode_registry_column(
            events, id_column, registry, **decode_kwargs
        )

    if with_people and "person_id" in events.columns:
        people_lookup = load_people_lookup(path)
        if people_lookup is not None:
            events = enrich_with_people(events, people_lookup, prefix=people_prefix)

    if with_venues and "venue_id" in events.columns:
        venues_lookup = load_venues_lookup(path)
        if venues_lookup is not None:
            events = enrich_with_venues(events, venues_lookup, prefix=venues_prefix)

    return events
