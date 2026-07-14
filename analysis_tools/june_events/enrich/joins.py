def _prefixed_lookup(lookup, id_column: str, prefix: str, allow_duplicate_ids: bool):
    if not allow_duplicate_ids:
        duplicated = lookup[id_column][lookup[id_column].duplicated()].unique()
        if len(duplicated) > 0:
            raise ValueError(
                f"lookup has duplicate {id_column!r} values: {list(duplicated)!r} "
                f"— this fans out matching event rows; pass allow_duplicate_ids=True "
                f"to force the merge through anyway"
            )
    other_columns = [column for column in lookup.columns if column != id_column]
    renamed = {column: f"{prefix}{column}" for column in other_columns}
    return lookup[[id_column, *other_columns]].rename(columns=renamed)


def enrich_with_venues(
    event_df, venues_lookup, prefix: str = "venue_", allow_duplicate_ids: bool = False
):
    lookup = _prefixed_lookup(venues_lookup, "venue_id", prefix, allow_duplicate_ids)
    return event_df.merge(lookup, on="venue_id", how="left")


def enrich_with_people(
    event_df, people_lookup, prefix: str = "person_", allow_duplicate_ids: bool = False
):
    lookup = _prefixed_lookup(people_lookup, "person_id", prefix, allow_duplicate_ids)
    return event_df.merge(lookup, on="person_id", how="left")
