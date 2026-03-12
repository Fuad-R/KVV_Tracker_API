"""
Shared fixtures for the Transit Tracker API integration tests.

These tests run against a live instance of the API service (typically
started via docker-compose.test.yml).

Environment variables:
    API_BASE_URL  – base URL of the running service (default: http://localhost:8080)
    API_KEY       – optional API key when AUTH is enabled
"""

import os
import pytest
import requests
import time


@pytest.fixture(scope="session")
def base_url():
    """Base URL of the running API service."""
    return os.environ.get("API_BASE_URL", "http://localhost:8080")


@pytest.fixture(scope="session")
def api_key():
    """API key for authenticated endpoints (empty if auth is disabled)."""
    return os.environ.get("API_KEY", "")


@pytest.fixture(scope="session")
def headers(api_key):
    """Request headers including API key if configured."""
    h = {"Accept": "application/json"}
    if api_key:
        h["X-API-Key"] = api_key
    return h


@pytest.fixture(scope="session", autouse=True)
def wait_for_service(base_url):
    """Wait for the API service to become healthy before running tests."""
    url = f"{base_url}/health"
    max_retries = 30
    for i in range(max_retries):
        try:
            resp = requests.get(url, timeout=2)
            if resp.status_code == 200:
                return
        except requests.ConnectionError:
            pass
        time.sleep(2)
    pytest.fail(f"Service at {base_url} did not become healthy within {max_retries * 2}s")
