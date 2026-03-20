# Transit Tracker API - Usage & Functionality Documentation

## Overview
This Transit Tracker API provides real-time transit departure information for the Karlsruhe public transport system. It integrates with any EFA-compatible upstream data provider to deliver normalized, cached departure data with optional filtering and accessibility information.

## Core Functionality

### 1. **Stop Search**
Search for transit stops by name across the network. Results include stop identification and geographic coordinates.
When configured, each stop search also stores any new stops in the PostgreSQL database.

### 2. **Real-time Departures**
Retrieve live departure information for any stop, including line numbers, directions, platforms, and countdown timers.

### 3. **Data Caching**
30-second TTL caching reduces upstream provider load while maintaining fresh data.

### 4. **Accessibility Information**
Query low-floor vehicle availability and wheelchair accessibility for departures.

### 5. **Platform Filtering**
Filter departures by specific platform/track to refine results.

### 6. **Delay Tracking**
Optional real-time delay information for each departure.

### 7. **Service Notifications**
Query current service alerts and disruptions affecting a specific stop from multiple EFA providers.

## Documentation

Detailed API usage instructions and database table/schema documentation are maintained in dedicated files:

- API usage and endpoint reference: [`docs/API_DOCUMENTATION.md`](docs/API_DOCUMENTATION.md)
- Database schema and table documentation: [`docs/DB_DOCUMENTATION.md`](docs/DB_DOCUMENTATION.md)
- Database connection template: [`docs/db_connection.txt.example`](docs/db_connection.txt.example)
