import h5py

from .raw_tables import load_raw_table

_PEOPLE_PROPERTIES_GROUP = "lookups/people_properties"


def _decode_value(value):
    return value.decode() if isinstance(value, bytes) else value


def _decode_byte_columns(df):
    for column in df.columns:
        if df[column].dtype.kind in ("S", "O"):
            df[column] = df[column].apply(_decode_value)
    return df


def load_venues_lookup(path: str):
    venues = load_raw_table(path, "lookups/venues")
    if venues is None:
        return None
    return _decode_byte_columns(venues)


def load_people_lookup(path: str, include_properties: bool = True):
    people = load_raw_table(path, "lookups/people")
    if people is None:
        return None
    people = _decode_byte_columns(people)

    if include_properties:
        with h5py.File(path, "r") as fh:
            if _PEOPLE_PROPERTIES_GROUP in fh:
                for property_name, dataset in fh[_PEOPLE_PROPERTIES_GROUP].items():
                    people[property_name] = [_decode_value(v) for v in dataset[:]]

    return people
