import requests


def test_stops_search_basic(base_url, auth_headers):
    # API docs: GET /api/stops/search?q=<query>&location=1
    response = requests.get(
        f"{base_url}/api/stops/search",
        params={"q": "Knielingen", "location": 1},
        headers=auth_headers,
        timeout=5,
    )

    assert response.status_code == 200
    payload = response.json()
    assert isinstance(payload, list)
    assert payload
    first = payload[0]
    assert first["stop_id"] == "7000107"
    assert first["stop_name"] == "Knielingen"
    assert "location" in first
    assert "latitude" in first["location"]
    assert "longitude" in first["location"]


def test_stops_search_missing_query(base_url, auth_headers):
    # API docs require q query parameter.
    response = requests.get(
        f"{base_url}/api/stops/search", headers=auth_headers, timeout=5
    )
    assert response.status_code == 400
    assert "error" in response.json()


def test_auth_required(base_url, auth_headers):
    unauthorized = requests.get(
        f"{base_url}/api/stops/search", params={"q": "Knielingen"}, timeout=5
    )
    assert unauthorized.status_code == 401

    authorized = requests.get(
        f"{base_url}/api/stops/search",
        params={"q": "Knielingen"},
        headers=auth_headers,
        timeout=5,
    )
    assert authorized.status_code == 200


def test_revoked_or_expired_api_key_rejected(base_url):
    for bad_key in ("revoked-key", "expired-test-key"):
        response = requests.get(
            f"{base_url}/api/stops/search",
            params={"q": "Knielingen"},
            headers={"X-API-Key": bad_key},
            timeout=5,
        )
        assert response.status_code == 401
