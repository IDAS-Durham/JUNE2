import h5py

from .sentinels import UNSET_REGISTRY_INDEX


def load_registry(path: str, registry_name: str):
    dataset_path = f"metadata/registries/{registry_name}"
    with h5py.File(path, "r") as fh:
        if dataset_path not in fh:
            return None
        return [value.decode() for value in fh[dataset_path][:]]


def decode_registry_column(
    df,
    id_column: str,
    registry,
    unset_value: int = UNSET_REGISTRY_INDEX,
    unset_label: str = "unknown",
):
    lookup = {index: label for index, label in enumerate(registry)}
    return df[id_column].map(
        lambda index: unset_label if index == unset_value else lookup[index]
    )
