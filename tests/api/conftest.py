import os
import time

import pytest
import requests


@pytest.fixture(scope="session")
def base_url() -> str:
    return os.getenv("TEST_BASE_URL", "http://localhost:8080")


@pytest.fixture(scope="session")
def api_key() -> str:
    return os.getenv("TEST_API_KEY", "test-api-key")


@pytest.fixture(scope="session", autouse=True)
def wait_for_service(base_url: str):
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            response = requests.get(f"{base_url}/health", timeout=2)
            if response.status_code == 200:
                return
        except requests.RequestException:
            pass
        time.sleep(1)
    raise RuntimeError("Service did not become healthy within 60 seconds")


@pytest.fixture()
def auth_headers(api_key: str):
    return {"X-API-Key": api_key}
