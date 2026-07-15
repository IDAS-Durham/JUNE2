import h5py

from .types import DatasetSummary, FileSummary

_REGISTRIES_PREFIX = "metadata/registries/"


def _decode(value):
    return value.decode() if isinstance(value, bytes) else value


def inspect_file(path: str) -> FileSummary:
    with h5py.File(path, "r") as fh:
        datasets = []
        registries = {}

        def collect(name, obj):
            if not isinstance(obj, h5py.Dataset):
                return
            if name.startswith(_REGISTRIES_PREFIX):
                registry_name = name[len(_REGISTRIES_PREFIX):]
                registries[registry_name] = [_decode(v) for v in obj[:]]
                return
            datasets.append(
                DatasetSummary(
                    path=name,
                    n_rows=obj.shape[0],
                    dtype=str(obj.dtype),
                    nbytes=obj.nbytes,
                )
            )

        fh.visititems(collect)

    return FileSummary(path=path, datasets=datasets, registries=registries)
