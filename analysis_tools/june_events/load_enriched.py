import logging

from .decode import decode_registry_column, load_registry
from .enrich import enrich_with_people, enrich_with_venues
from .io import load_people_lookup, load_raw_table, load_venues_lookup

logger = logging.getLogger(__name__)

DEFAULT_REGISTRY_COLUMNS = {
    "encounter_type_id": "encounter_types",
    "infector_symptom_id": "symptoms",
    "old_symptom_id": "symptoms",
    "new_symptom_id": "symptoms",
}


def _decoded_column_name(id_column: str) -> str:
    if id_column.endswith("_id"):
        return id_column[: -len("_id")]
    return id_column


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
        events[_decoded_column_name(id_column)] = decode_registry_column(
            events, id_column, registry
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
