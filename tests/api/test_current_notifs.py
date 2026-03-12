"""
Tests for GET /api/current_notifs endpoint.

Reference: API_DOCUMENTATION.md — Get Notifications section.
"""

import pytest
import requests

# A well-known KVV stop ID for testing
SAMPLE_STOP_ID = "7000107"


class TestCurrentNotifsBasic:
    """Basic notification retrieval tests."""

    def test_notifications_returns_200(self, base_url, headers):
        """Valid stopID returns HTTP 200."""
        resp = requests.get(
            f"{base_url}/api/current_notifs",
            params={"stopID": SAMPLE_STOP_ID},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200

    def test_notifications_returns_json_array(self, base_url, headers):
        """Notifications are returned as a JSON array."""
        resp = requests.get(
            f"{base_url}/api/current_notifs",
            params={"stopID": SAMPLE_STOP_ID},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert isinstance(data, list)

    def test_notification_fields_if_present(self, base_url, headers):
        """If notifications exist, they contain expected fields."""
        resp = requests.get(
            f"{base_url}/api/current_notifs",
            params={"stopID": SAMPLE_STOP_ID},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        if isinstance(data, list) and len(data) > 0:
            notif = data[0]
            # Notification fields from API docs
            assert "id" in notif or "content" in notif, \
                f"Expected notification fields, got: {list(notif.keys())}"

    def test_notifications_none_for_unknown_stop(self, base_url, headers):
        """Stop with no notifications returns empty array."""
        resp = requests.get(
            f"{base_url}/api/current_notifs",
            params={"stopID": "9999999"},
            headers=headers,
            timeout=20,
        )
        assert resp.status_code == 200
        data = resp.json()
        assert isinstance(data, list)
        assert len(data) == 0


class TestCurrentNotifsNegative:
    """Negative test cases for notifications."""

    def test_missing_stop_id_returns_400(self, base_url, headers):
        """Missing 'stopID' parameter returns HTTP 400."""
        resp = requests.get(
            f"{base_url}/api/current_notifs",
            headers=headers,
            timeout=10,
        )
        assert resp.status_code == 400
        data = resp.json()
        assert "error" in data

    def test_invalid_stop_id_returns_400(self, base_url, headers):
        """Invalid stopID with special chars returns 400."""
        resp = requests.get(
            f"{base_url}/api/current_notifs",
            params={"stopID": "'; DROP TABLE stops;--"},
            headers=headers,
            timeout=10,
        )
        assert resp.status_code == 400

    def test_notifications_security_headers(self, base_url, headers):
        """Response includes security headers."""
        resp = requests.get(
            f"{base_url}/api/current_notifs",
            params={"stopID": SAMPLE_STOP_ID},
            headers=headers,
            timeout=20,
        )
        assert resp.headers.get("X-Content-Type-Options") == "nosniff"
        assert resp.headers.get("X-Frame-Options") == "DENY"
        assert "application/json" in resp.headers.get("Content-Type", "")
