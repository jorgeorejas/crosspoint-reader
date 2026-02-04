# Calendar (iCal) Support

CrossPoint Reader supports importing iCalendar (`.ics`) feeds and displaying upcoming events on the device.

## Getting Your iCal URL

Most calendar services provide a URL you can use to sync your calendar with CrossPoint Reader.

### Google Calendar

1. Open [Google Calendar](https://calendar.google.com) on your computer
2. On the left sidebar, find the calendar you want to sync
3. Click the three dots (⋮) next to the calendar name
4. Select **Settings and sharing**
5. Scroll down to the **Integrate calendar** section
6. Copy the **Secret address in iCal format** URL

> **Note:** Use the "Secret address" (not the public address) to see all your events, including private ones. Keep this URL private as anyone with it can view your calendar.

### Apple iCloud Calendar

1. Open [iCloud Calendar](https://www.icloud.com/calendar) in a browser
2. Click the share icon next to the calendar name in the sidebar
3. Check **Public Calendar**
4. Copy the URL that appears

### Microsoft Outlook / Office 365

1. Open [Outlook Calendar](https://outlook.live.com/calendar) on the web
2. Click the gear icon → **View all Outlook settings**
3. Go to **Calendar** → **Shared calendars**
4. Under **Publish a calendar**, select your calendar and choose **Can view all details**
5. Click **Publish** and copy the **ICS** link

### Other Calendar Services

Most calendar applications support iCal export. Look for options like:
- "Subscribe to calendar"
- "Get shareable link"
- "Export as ICS"
- "Calendar URL" or "iCal URL"

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

