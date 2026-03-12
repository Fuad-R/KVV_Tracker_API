"""
Tests for authentication and the health endpoint.

Reference: API_DOCUMENTATION.md — Authentication section.
"""

import os
import pytest
import requests


class TestHealthEndpoint:
    """Health check endpoint (no auth required)."""

    def test_health_returns_200(self, base_url):
        """Health endpoint returns HTTP 200 without authentication."""
        resp = requests.get(f"{base_url}/health", timeout=5)
        assert resp.status_code == 200

    def test_health_returns_ok_status(self, base_url):
        """Health endpoint body contains status ok."""
        resp = requests.get(f"{base_url}/health", timeout=5)
        data = resp.json()
        assert data.get("status") == "ok"


class TestAuthRequired:
    """Authentication tests (only run when AUTH is enabled)."""

    @pytest.fixture(autouse=True)
    def _skip_if_no_auth(self):
        """Skip these tests if AUTH is not enabled."""
        auth_env = os.environ.get("AUTH", "").lower()
        if auth_env not in ("true", "1", "yes"):
            pytest.skip("AUTH is not enabled; skipping auth tests")

    def test_missing_api_key_returns_401(self, base_url):
        """Endpoint without API key returns 401 when auth is enabled."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Karlsruhe"},
            timeout=10,
        )
        assert resp.status_code == 401

    def test_invalid_api_key_returns_401(self, base_url):
        """Endpoint with invalid API key returns 401."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Karlsruhe"},
            headers={"X-API-Key": "invalid-key-12345"},
            timeout=10,
        )
        assert resp.status_code == 401

    def test_valid_api_key_returns_200(self, base_url, headers):
        """Endpoint with valid API key returns 200."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Karlsruhe"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
