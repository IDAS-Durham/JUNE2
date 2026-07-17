"""Build the small real-data fixture used by the @requires_real_events_fixture tests.

Trims a full simulation_events.h5 down to a few MB while preserving referential
integrity (lookups filtered to exactly the ids the retained event rows use;
people_properties/* kept positionally aligned with lookups/people) and enough
variety to exercise seed/non-seed infections and known/unset registry indices
(a naive first-N slice would retain only seed infections here, see mission
notes in the PR discussion this fixture was built for).

Not run automatically; rerun manually if the fixture ever needs regenerating
against a different source file:

    python build_fixture.py <source_simulation_events.h5>
"""

import sys

import h5py
import numpy as np

CUTOFF_TIME = 20.0
TARGET_INFECTIONS = 800
TARGET_COORDINATED_ENCOUNTERS = 1000
SEED_VENUE_ID = -999

UNCAPPED_EVENT_TABLES = [
    ("deaths", ["person_id"], "venue_id"),
    ("symptom_changes", ["person_id"], "venue_id"),
    ("hospital_admissions", ["person_id"], "hospital_id"),
    ("hospital_discharges", ["person_id"], "hospital_id"),
    ("icu_admissions", ["person_id"], "hospital_id"),
]


def _time_filtered_indices(dataset, cutoff):
    return np.nonzero(dataset["time"][:] <= cutoff)[0]


def _strided_indices(dataset, cutoff, target_count):
    indices = _time_filtered_indices(dataset, cutoff)
    stride = max(1, len(indices) // target_count)
    return indices[::stride]


def build_fixture(source_path, dest_path):
    with h5py.File(source_path, "r") as src, h5py.File(dest_path, "w") as dst:
        person_ids = set()
        venue_ids = {SEED_VENUE_ID}

        infections_idx = _strided_indices(
            src["events/infections"], CUTOFF_TIME, TARGET_INFECTIONS
        )
        dst.create_dataset("events/infections", data=src["events/infections"][:][infections_idx])
        person_ids.update(src["events/infections"]["person_id"][infections_idx].tolist())
        person_ids.update(src["events/infections"]["infector_id"][infections_idx].tolist())
        venue_ids.update(src["events/infections"]["venue_id"][infections_idx].tolist())

        ce_idx = _strided_indices(
            src["events/coordinated_encounters"], CUTOFF_TIME, TARGET_COORDINATED_ENCOUNTERS
        )
        dst.create_dataset(
            "events/coordinated_encounters",
            data=src["events/coordinated_encounters"][:][ce_idx],
        )
        person_ids.update(src["events/coordinated_encounters"]["person_a"][ce_idx].tolist())
        person_ids.update(src["events/coordinated_encounters"]["person_b"][ce_idx].tolist())

        for name, person_cols, venue_col in UNCAPPED_EVENT_TABLES:
            source_dataset = src[f"events/{name}"]
            mask = source_dataset["time"][:] <= CUTOFF_TIME
            dst.create_dataset(f"events/{name}", data=source_dataset[:][mask])
            for col in person_cols:
                person_ids.update(source_dataset[col][mask].tolist())
            venue_ids.update(source_dataset[venue_col][mask].tolist())

        people_ids_array = src["lookups/people"]["person_id"][:]
        people_mask = np.isin(people_ids_array, list(person_ids))
        dst.create_dataset("lookups/people", data=src["lookups/people"][:][people_mask])
        for prop_dataset in src["lookups/people_properties"].values():
            dst.create_dataset(prop_dataset.name, data=prop_dataset[:][people_mask])

        venue_ids_array = src["lookups/venues"]["venue_id"][:]
        venues_mask = np.isin(venue_ids_array, list(venue_ids))
        dst.create_dataset("lookups/venues", data=src["lookups/venues"][:][venues_mask])

        for name in ["cohabiting_couple", "friendships"]:
            for field in ["person_id", "partner_id"]:
                path = f"lookups/population_networks/{name}/{field}"
                dst.create_dataset(path, data=src[path][:200])
        dst.create_dataset("lookups/population_summary", data=src["lookups/population_summary"][:200])

        for registry_dataset in src["metadata/registries"].values():
            dst.create_dataset(registry_dataset.name, data=registry_dataset[:])


if __name__ == "__main__":
    source_path = sys.argv[1] if len(sys.argv) > 1 else (
        "/home/gavin/Documents/Modern_Day/Xiying_Project/simulation_events.h5"
    )
    dest_path = "simulation_events_fixture.h5"
    build_fixture(source_path, dest_path)
    print(f"wrote {dest_path}")
