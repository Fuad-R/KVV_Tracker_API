import requests


def test_get_departures_basic(base_url, auth_headers):
    # API docs: GET /api/stops/{stopId}
    response = requests.get(
        f"{base_url}/api/stops/7000107", headers=auth_headers, timeout=5
    )

    assert response.status_code == 200
    payload = response.json()
    assert isinstance(payload, list)
    assert payload

    first = payload[0]
    assert isinstance(first["line"], str)
    assert isinstance(first["minutes_remaining"], (int, float))
    assert isinstance(first["is_realtime"], bool)


def test_get_departures_detailed_and_delay(base_url, auth_headers):
    # API docs: detailed=true and delay=true add extra fields.
    response = requests.get(
        f"{base_url}/api/stops/7000107",
        params={"detailed": "true", "delay": "true"},
        headers=auth_headers,
        timeout=5,
    )

    assert response.status_code == 200
    payload = response.json()
    assert isinstance(payload, list)
    assert payload

    first = payload[0]
    assert "low_floor" in first
    assert "wheelchair_accessible" in first
    assert "delay_minutes" in first


def test_get_departures_invalid_stopid(base_url, auth_headers):
    response = requests.get(
        f"{base_url}/api/stops/bad$stop", headers=auth_headers, timeout=5
    )
    assert response.status_code == 400
    assert "error" in response.json()
