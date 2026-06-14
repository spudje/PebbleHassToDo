# Pebble Home Assistant To-Do

A (re)Pebble OS watchapp that lists to do items of a single Home Assistant Local To Do List.

Created by Claude Code.

## Features

- Displays to-do items with large text (GOTHIC_28_BOLD) and visual checkboxes
- Tap SELECT on any item to toggle it checked/unchecked (synced to HA instantly)
- "Delete Completed" action removes all completed items from app and HA
- Settings page in the Pebble Android app lets you configure HA URL, token, and to do list

## Project Structure

```
src/
  main.c               — Watch app (C, Pebble SDK 3)
  js/
    pebble-js-app.js   — Companion JS (runs on Android, calls HA REST API)
appinfo.json           — App metadata and message keys
wscript                — Build script
```

## Setup

### 1. Build

Install the [Pebble SDK](https://developer.rebble.io/developer.pebble.com/sdk/index.html) and run:

```bash
pebble build
pebble install --phone <YOUR_PHONE_IP>
```

### 2. Configure via Settings

Open the Pebble Android app → your app → Settings:

| Field | Example |
|---|---|
| Home Assistant URL | `http://192.168.1.100:8123` |
| Long-Lived Access Token | `ey...` (from HA Profile page) |
| To-Do List Entity ID | `todo.shopping_list` |

Find your entity ID in HA → Developer Tools → States, filter by `todo.`.

### 3. Getting a Long-Lived Token

1. Open Home Assistant
2. Click your profile (bottom-left)
3. Scroll to **Long-Lived Access Tokens**
4. Create one and paste it into the Pebble settings

## Controls

| Button | Action |
|---|---|
| UP / DOWN | Scroll list |
| SELECT | Toggle item checked/unchecked |
| SELECT on "Delete Completed" | Remove all completed items from HA |
| BACK | Exit app |

## HA API Endpoints Used

- `GET  /api/todo_lists/{entity_id}/items` — fetch items
- `POST /api/services/todo/update_item` — toggle status
- `POST /api/services/todo/remove_completed_items` — bulk delete
