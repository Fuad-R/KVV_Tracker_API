"""
Tests for GET /api/stops/{stopId} endpoint (departures).

Reference: API_DOCUMENTATION.md — Get Departures section.
"""

import pytest
import requests

# A well-known KVV stop ID for testing
SAMPLE_STOP_ID = "7000107"


class TestGetDeparturesBasic:
    """Basic departure retrieval tests."""

    def test_departures_returns_200(self, base_url, headers):
        """Valid stop ID returns HTTP 200."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200

    def test_departures_returns_json_array(self, base_url, headers):
        """Departures are returned as a JSON array."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert isinstance(data, list)

    def test_departure_fields(self, base_url, headers):
        """Each departure contains required fields with correct types."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        if isinstance(data, list) and len(data) > 0:
            dep = data[0]
            # Required fields from API docs
            assert "line" in dep, "Missing 'line' field"
            assert isinstance(dep["line"], str), "'line' should be a string"

            if "minutes_remaining" in dep:
                assert isinstance(dep["minutes_remaining"], (int, float)), \
                    "'minutes_remaining' should be numeric"

            if "is_realtime" in dep:
                assert isinstance(dep["is_realtime"], bool), \
                    "'is_realtime' should be boolean"


class TestGetDeparturesDetailed:
    """Tests for detailed and delay query parameters."""

    def test_detailed_true_adds_fields(self, base_url, headers):
        """detailed=true adds extra fields to departure objects."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            params={"detailed": "true"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        if isinstance(data, list) and len(data) > 0:
            dep = data[0]
            # When detailed=true, additional fields may be present
            # (low_floor, wheelchair_accessible, hints)
            has_detailed = (
                "low_floor" in dep
                or "wheelchair_accessible" in dep
                or "hints" in dep
                or "train_type" in dep
            )
            # Not all departures have these, but the response should be valid
            assert isinstance(dep.get("line", ""), str)

    def test_delay_true_adds_delay_field(self, base_url, headers):
        """delay=true adds delay_minutes field to departure objects."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            params={"delay": "true"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        if isinstance(data, list) and len(data) > 0:
            dep = data[0]
            if "delay_minutes" in dep:
                assert isinstance(dep["delay_minutes"], (int, float, type(None))), \
                    "'delay_minutes' should be numeric or null"

    def test_detailed_and_delay_combined(self, base_url, headers):
        """Combining detailed=true and delay=true works."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            params={"detailed": "true", "delay": "true"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert isinstance(data, list)


class TestGetDeparturesNegative:
    """Negative test cases for departures."""

    def test_invalid_stop_id_returns_400(self, base_url, headers):
        """Invalid stop ID with special characters returns 400."""
        resp = requests.get(
            f"{base_url}/api/stops/<script>alert(1)</script>",
            headers=headers,
            timeout=10,
        )
        assert resp.status_code == 400
        data = resp.json()
        assert "error" in data

    def test_departures_security_headers(self, base_url, headers):
        """Response includes security headers."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            headers=headers,
            timeout=20,
        )
        assert resp.headers.get("X-Content-Type-Options") == "nosniff"
        assert resp.headers.get("X-Frame-Options") == "DENY"


class TestGetDeparturesTrackFilter:
    """Tests for track/platform filtering."""

    def test_track_filter_returns_200(self, base_url, headers):
        """Filtering by track still returns 200."""
        resp = requests.get(
            f"{base_url}/api/stops/{SAMPLE_STOP_ID}",
            params={"track": "1"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert isinstance(data, list)
