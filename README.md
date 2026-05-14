# JUNE 2: open-source individual-based epidemiology simulation

JUNE 2 is a high-performance, event-driven, agent-based disease simulation framework for modelling complex social interactions and disease spread in large-scale populations. It is a from-the-ground-up remake of the original [JUNE](https://github.com/IDAS-Durham/JUNE) engine, optimised for performance, scalability, and modularity. Population/world HDF5 files are built by the companion repository **[MAY](https://github.com/mtcorread/MAY)**.

## What it does

- **Individual-based**: every person is simulated explicitly, with their own demographics, schedule, network ties, and clinical state.
- **Event-driven**: the simulator advances by discrete events (encounters, transitions, policy triggers) rather than coarse time-steps, keeping cost proportional to activity.
- **Data- and config-driven**: scenarios — disease, schedules, contact matrices, networks, policies, vaccines — are described in YAML and CSV. No C++ change is needed to run a new disease, world, or intervention.
- **MPI-parallel**: domain-decomposed across geographic units, with output that is bit-identical across rank counts (given the same random seed).
- **Scenarios shipped**: COVID-19 (2021 census), plague (medieval), and historical 1911 Durham.

## Repository layout

```
configs/<scenario>/   YAML + CSV describing a simulation
data/                 Population-scale CSVs (rates, contact data, geography)
worlds/               Pre-built population HDF5 files (produced by MAY; gitignored)
src/, include/        C++ source
tests/                Unit and integration tests
```

## Prerequisites

- **C++ compiler** supporting C++20 (GCC 10+, Clang 10+)
- **CMake** 3.14+
- **HDF5** with C++ bindings
- **yaml-cpp**
- **MPI** (optional, for parallel runs — OpenMPI or MPICH)
- **METIS** (optional, for graph partitioning in parallel mode)
- **gperftools** (optional, for CPU profiling)

### macOS (Homebrew)

```bash
brew install cmake hdf5 yaml-cpp open-mpi metis gperftools
```

## Build

```bash
./rebuild.sh           # clean rebuild
```

Or manually:

```bash
mkdir -p build && cd build
cmake ..
make -j8
```

The executable is produced at `build/disease_sim`.

## Running a simulation

A run requires **a world file** (HDF5 population) and **a `simulation.yaml`** (entry point that pulls in disease, contacts, schedules, etc. via its `config_paths:` block).

> World files are produced by **[MAY](https://github.com/mtcorread/MAY)**, a separate world-builder repository that constructs JUNE 2-compatible HDF5 populations (households, workplaces, schools, geography, pre-existing networks) from census and administrative data.

### Serial

```bash
./build/disease_sim --world worlds/world_2021.h5 \
                    --config configs/config_2021/simulation.yaml
```

### Parallel (MPI)

```bash
mpirun -n 4 ./build/disease_sim --world worlds/world_2021.h5 \
                                --config configs/config_2021/simulation.yaml
```

### CLI options

| Flag | Purpose |
|---|---|
| `--world <path.h5>` | **Required.** Population/geography world file. |
| `--config <path.yaml>` | **Required.** Path to `simulation.yaml`. |
| `--infection_seeds <path>` | Override the seeds file declared in `simulation.yaml`. |
| `--beta <float>` | Override transmission `beta` (sweep convenience). |
| `--seed <int>` | Override the master random seed. |
| `--days <N>` | Cap simulation length, ignoring `end_date`. |
| `--run-id <name>` | Label the output directory under `runs/`. |
| `--runs-dir <path>` | Where run artefacts are written (default: `runs/`). |

YAML and CSV configs are loaded at **runtime** — you do not need to rebuild after editing them. Source changes (`src/`, `include/`) require `./rebuild.sh`.

## Outputs

- **`simulation_events.h5`** — aggregated HDF5 event log (infections, encounters, transitions). In parallel runs, per-rank outputs are merged automatically on completion.
- **`runs/<run-id>/`** — partition map, summary CSVs, and other per-run artefacts.
- **`cpu_profile.prof`** — produced if profiling is enabled (view with `pprof`).

## Documentation

- **[USER_GUIDE.md](USER_GUIDE.md)** — full reference for users running JUNE 2 without editing source: configuration file structure, disease modelling, contact matrices, networks, seeding, policies, vaccines, parallelism, and how to write a new scenario from scratch. **Start here if you want to run or tune simulations.**

## License

JUNE 2 is released under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for the full text.

## Contributing

Issues and pull requests are welcome. After cloning, run `scripts/setup_hooks.sh` once to enable the project's git pre-push hook.
