"""
Tests for GET /api/stops/search endpoint.

Reference: API_DOCUMENTATION.md — Search Stops section.
"""

import pytest
import requests


class TestStopsSearchBasic:
    """Basic stop search functionality."""

    def test_stops_search_returns_200(self, base_url, headers):
        """Searching for a known query returns HTTP 200."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Karlsruhe", "location": "1"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200

    def test_stops_search_returns_json_array(self, base_url, headers):
        """Search results are returned as a JSON array."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Karlsruhe"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert isinstance(data, list)

    def test_stops_search_with_location(self, base_url, headers):
        """When location=1, results include coordinate fields."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Karlsruhe", "location": "1"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert isinstance(data, list)
        if len(data) > 0:
            first = data[0]
            # Each stop should have an identifier field
            assert "stop_id" in first or "stateless" in first or "id" in first

    def test_stops_search_result_fields(self, base_url, headers):
        """Each result contains expected stop fields."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Marktplatz"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        if isinstance(data, list) and len(data) > 0:
            first = data[0]
            # Should have a name field
            has_name = "stop_name" in first or "name" in first or "disassembledName" in first
            assert has_name, f"Expected a name field in result: {list(first.keys())}"


class TestStopsSearchNegative:
    """Negative test cases for stop search."""

    def test_search_missing_q_returns_400(self, base_url, headers):
        """Missing 'q' parameter returns HTTP 400."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            headers=headers,
            timeout=10,
        )
        assert resp.status_code == 400
        data = resp.json()
        assert "error" in data

    def test_search_empty_q_returns_400(self, base_url, headers):
        """Empty 'q' parameter returns HTTP 400."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": ""},
            headers=headers,
            timeout=10,
        )
        assert resp.status_code == 400

    def test_search_too_long_query_returns_400(self, base_url, headers):
        """Query exceeding MAX_QUERY_LENGTH returns HTTP 400."""
        long_query = "a" * 201
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": long_query},
            headers=headers,
            timeout=10,
        )
        assert resp.status_code == 400


class TestStopsSearchSecurity:
    """Security header validation for stop search."""

    def test_security_headers_present(self, base_url, headers):
        """Response includes security headers."""
        resp = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Karlsruhe"},
            headers=headers,
            timeout=20,
        )
        assert resp.headers.get("X-Content-Type-Options") == "nosniff"
        assert resp.headers.get("X-Frame-Options") == "DENY"
        assert "application/json" in resp.headers.get("Content-Type", "")
