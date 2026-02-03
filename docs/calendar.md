# Calendar (iCal) Support

CrossPoint Reader supports importing iCalendar (`.ics`) feeds and displaying upcoming events on the device.

## Supported Components

- **VEVENT** only
- Other components (VTODO, VJOURNAL, VFREEBUSY, VTIMEZONE, VALARM) are ignored

## Supported Properties

- `DTSTART`, `DTEND`, `DURATION`
- `SUMMARY`, `LOCATION`, `DESCRIPTION`, `UID`
- `RRULE`, `RDATE`, `EXDATE`, `RECURRENCE-ID`, `STATUS`

## Recurrence Support

Supported RRULE fields:

- `FREQ` (DAILY, WEEKLY, MONTHLY, YEARLY)
- `INTERVAL`
- `COUNT`
- `UNTIL`
- `BYDAY`, `BYMONTHDAY`, `BYMONTH`

Additional recurrence handling:

- `RDATE` adds extra occurrences
- `EXDATE` removes occurrences
- `RECURRENCE-ID` overrides individual instances
- `STATUS:CANCELLED` removes the event or instance

The parser expands occurrences only within the cached window (currently 30 days) and caps expansion at 1000
occurrences per event to avoid runaway rules.

## Timezones

- Fixed **device timezone offset** is used for all events.
- `TZID`/`VTIMEZONE` are ignored.
- `Z` (UTC) values are converted using the fixed offset.

## All-Day Events

Events with `VALUE=DATE` are treated as all-day. `DTEND` for all-day events is treated as an exclusive end date,
per RFC5545.

## Limitations

- No full timezone support (TZID/VTIMEZONE)
- No tasks/journals/free-busy/alarms
- BYDAY ordinals (e.g., `1MO`, `-1FR`) are not interpreted

