#!/usr/bin/env python3
"""Behavioral check of event levels through old and new SQLite databases."""
import asyncio
import json
import sqlite3
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path


async def run():
    from pydantic import ValidationError
    from tabletopmagnat.observability.bus import EventBus, configure_event_bus_instance, emit
    from tabletopmagnat.observability.context import session_scope
    from tabletopmagnat.observability.events import Event
    from tabletopmagnat.observability.store import EventStore
    from tabletopmagnat.webui.models import EventDTO

    failures = []
    def check(condition, message):
        if not condition:
            failures.append(message)

    now = datetime.now(timezone.utc)
    base = dict(session_id='s', sequence=1, ts=now, event_type='test')
    check(Event(**base).level == 'info', 'default event level')
    try:
        Event(**base, level='invalid')
    except ValidationError:
        pass
    else:
        failures.append('invalid level accepted')

    with tempfile.TemporaryDirectory() as directory:
        old = Path(directory) / 'old.db'
        with sqlite3.connect(old) as conn:
            conn.executescript('''
                CREATE TABLE sessions (id TEXT PRIMARY KEY, question TEXT NOT NULL,
                  status TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
                  final_answer TEXT, error TEXT, mode TEXT NOT NULL DEFAULT 'fast');
                CREATE TABLE events (id INTEGER PRIMARY KEY AUTOINCREMENT,
                  session_id TEXT NOT NULL, sequence INTEGER NOT NULL, ts TEXT NOT NULL,
                  scope_id TEXT, parent_id TEXT, event_type TEXT NOT NULL, status TEXT,
                  payload_json TEXT NOT NULL, UNIQUE(session_id, sequence));
                INSERT INTO events(session_id,sequence,ts,event_type,payload_json)
                  VALUES ('old',1,'2026-01-01T00:00:00+00:00','legacy','{"n":1}');
            ''')
        store = EventStore(old)
        EventStore(old)  # Opening twice must be safe.
        legacy = await store.list_events('old')
        check(len(legacy) == 1 and legacy[0].level == 'info' and legacy[0].payload == {'n': 1},
              'legacy migration and payload')
        bus = EventBus(store)
        queue = bus.subscribe('new')
        first = await bus.publish('new', 'step', payload={'x': 3}, level='warning')
        second = await bus.publish('new', 'done', level='error')
        received = [queue.get_nowait(), queue.get_nowait()]
        persisted = await store.list_events('new')
        check([e.level for e in received] == ['warning', 'error'], 'subscriber levels')
        check([e.level for e in persisted] == ['warning', 'error'], 'persisted levels')
        check([e.sequence for e in persisted] == [1, 2] and persisted[0].payload == {'x': 3},
              'sequence and payload')
        check(EventDTO.from_event(first).model_dump(mode='json')['level'] == 'warning', 'DTO level')
        check(EventDTO.from_event(second).model_dump(mode='json')['level'] == 'error', 'DTO error')
        fresh = EventStore(Path(directory) / 'fresh.db')
        fresh_bus = EventBus(fresh)
        configure_event_bus_instance(fresh_bus)
        await emit('outside', level='error')
        with session_scope('fresh'):
            await emit('inside', level='warning')
            await emit('default')
        rows = await fresh.list_events('fresh')
        check([e.level for e in rows] == ['warning', 'info'], 'emit and fresh store')
        check(not await fresh.list_events('outside'), 'outside session must be a no-op')
        configure_event_bus_instance(None)
    return {'passed': not failures, 'failures': failures}


if __name__ == '__main__':
    try:
        result = asyncio.run(run())
    except Exception as exc:
        result = {'passed': False, 'failures': [f'{type(exc).__name__}: {exc}']}
    print(json.dumps(result))
    raise SystemExit(0 if result['passed'] else 1)
