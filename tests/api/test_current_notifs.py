import requests


def test_current_notifs_none(base_url, auth_headers):
    # API docs: endpoint returns [] when no notifications exist for stop.
    response = requests.get(
        f"{base_url}/api/current_notifs",
        params={"stopID": "9999999"},
        headers=auth_headers,
        timeout=5,
    )

    assert response.status_code == 200
    assert response.json() == []


def test_current_notifs_shape(base_url, auth_headers):
    response = requests.get(
        f"{base_url}/api/current_notifs",
        params={"stopID": "7000107"},
        headers=auth_headers,
        timeout=5,
    )

    assert response.status_code == 200
    payload = response.json()
    assert isinstance(payload, list)
    assert payload

    first = payload[0]
    for field in ("id", "urlText", "content", "subtitle", "providerCode", "priority"):
        assert field in first
