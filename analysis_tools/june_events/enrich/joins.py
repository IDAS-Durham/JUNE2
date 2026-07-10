def _prefixed_lookup(lookup, id_column: str, prefix: str):
    other_columns = [column for column in lookup.columns if column != id_column]
    renamed = {column: f"{prefix}{column}" for column in other_columns}
    return lookup[[id_column, *other_columns]].rename(columns=renamed)


def enrich_with_venues(event_df, venues_lookup, prefix: str = "venue_"):
    lookup = _prefixed_lookup(venues_lookup, "venue_id", prefix)
    return event_df.merge(lookup, on="venue_id", how="left")


def enrich_with_people(event_df, people_lookup, prefix: str = "person_"):
    lookup = _prefixed_lookup(people_lookup, "person_id", prefix)
    return event_df.merge(lookup, on="person_id", how="left")
