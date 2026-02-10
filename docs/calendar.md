# Calendar Support

CrossPoint Reader supports displaying calendar events by fetching them from a JSON API endpoint that processes iCalendar (`.ics`) feeds.

## API Configuration

The calendar feature uses a backend API to fetch and process calendar events. The API endpoint is configured in `src/secrets.h` (see `src/secrets.h.example` for the template).

The API accepts iCal URLs and returns events in a structured JSON format. For the complete API schema specification, see https://www.jorgeorejas.com/calendar/spec.json

### API Endpoints

- **Default Calendar**: `/calendar.json` - Returns events from a default calendar
- **Custom Calendar**: `/calendar/custom.json?url=<ics-url>` - Returns events from any iCal URL

Both endpoints support query parameters:
- `past` (0-30, default: 2): Days before today to include
- `future` (0-30, default: 2): Days after today to include

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

## Event Data

The API returns events with the following properties:

- `id`: Unique event identifier
- `title`: Event summary/title
- `start`: Start time (ISO 8601 format)
- `end`: End time (ISO 8601 format)
- `location`: Event location (optional)
- `description`: Event description (optional)
- `url`: Event URL (optional)
- `isAllDay`: Boolean flag for all-day events

The backend API handles all iCal parsing, including:

- **VEVENT** components (other components like VTODO, VJOURNAL are ignored)
- Standard properties: `DTSTART`, `DTEND`, `DURATION`, `SUMMARY`, `LOCATION`, `DESCRIPTION`, `UID`
- Recurrence: `RRULE`, `RDATE`, `EXDATE`, `RECURRENCE-ID`, `STATUS`

## Recurrence Support

The backend API supports RRULE fields including:

- `FREQ` (DAILY, WEEKLY, MONTHLY, YEARLY)
- `INTERVAL`, `COUNT`, `UNTIL`
- `BYDAY`, `BYMONTHDAY`, `BYMONTH`

Additional recurrence handling:
- `RDATE` adds extra occurrences
- `EXDATE` removes occurrences
- `RECURRENCE-ID` overrides individual instances
- `STATUS:CANCELLED` removes events or instances

The API expands occurrences within the requested date window (configurable via `past` and `future` parameters).

## Timezones

- The device uses a fixed timezone offset for all events
- The API handles timezone conversion and returns times that work with the device's timezone setting
- `Z` (UTC) values are converted appropriately

## All-Day Events

- Events marked as all-day are flagged with `isAllDay: true`
- All-day events use date-only format (YYYY-MM-DD) for start times

## Legacy iCal Parser

The firmware still includes `IcsParser` for backward compatibility, but the calendar sync feature now uses the JSON API by default. The old parser supported direct .ics file parsing with the limitations listed below:

- No full timezone support (TZID/VTIMEZONE)
- No tasks/journals/free-busy/alarms
- BYDAY ordinals (e.g., `1MO`, `-1FR`) were not interpreted

