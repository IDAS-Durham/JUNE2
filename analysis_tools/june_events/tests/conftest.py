import os

import pytest

REAL_EVENTS_FIXTURE = os.path.join(
    os.path.dirname(__file__), "fixtures", "simulation_events_fixture.h5"
)

requires_real_events_fixture = pytest.mark.skipif(
    not os.path.exists(REAL_EVENTS_FIXTURE),
    reason="real simulation_events.h5 fixture missing from tests/fixtures/",
)
