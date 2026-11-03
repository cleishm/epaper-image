import pytest

from panelgen.palette import builtin_palette


@pytest.fixture(scope="session")
def spectra6():
    return builtin_palette("spectra6")
