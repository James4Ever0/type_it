#!/usr/bin/env python3
"""
Macro Keyboard Prototype

A pygame simulation of an LED display + knob macro keyboard.
- Reads profiles from ./profiles (one .txt file per profile).
- Each non-empty line is a copy-paste candidate.
- Knob:
    - turn / drag     -> move highlight
    - short press / right arrow -> open profile (profile view) or copy candidate (candidate view)
    - long press / left arrow   -> go back
- Smart sort by last-usage time; persisted in usage.json.
"""

import json
import math
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import pygame
import pyperclip


PROFILES_DIR = Path("./profiles")
USAGE_FILE = Path("usage.json")

# UI constants
SCREEN_W, SCREEN_H = 420, 640
FPS = 60

# Device body
DEVICE_X, DEVICE_Y = 30, 30
DEVICE_W, DEVICE_H = SCREEN_W - 60, SCREEN_H - 60
DEVICE_RADIUS = 20

# Display screen
SCREEN_MARGIN = 24
DISPLAY_X = DEVICE_X + SCREEN_MARGIN
DISPLAY_Y = DEVICE_Y + SCREEN_MARGIN
DISPLAY_W = DEVICE_W - SCREEN_MARGIN * 2
DISPLAY_H = 320

# Knob — centered in the free space below the display, with margin.
KNOB_CENTER_X = SCREEN_W // 2
KNOB_MARGIN = 26
KNOB_RADIUS = min(
    (DEVICE_Y + DEVICE_H - DISPLAY_Y - DISPLAY_H - KNOB_MARGIN * 2) // 2,
    DISPLAY_W // 2 - KNOB_MARGIN,
)
KNOB_INNER_RADIUS = int(KNOB_RADIUS * 0.72)
KNOB_CENTER_Y = DISPLAY_Y + DISPLAY_H + (DEVICE_Y + DEVICE_H - DISPLAY_Y - DISPLAY_H) // 2

# Colors
COLOR_BG = (40, 42, 46)
COLOR_DEVICE = (30, 32, 36)
COLOR_DISPLAY_BG = (10, 12, 14)
COLOR_DISPLAY_BORDER = (60, 64, 72)
COLOR_TEXT = (200, 210, 220)
COLOR_TEXT_DIM = (100, 110, 120)
COLOR_HIGHLIGHT = (70, 130, 180)
COLOR_KNOB = (80, 84, 92)
COLOR_KNOB_FACE = (110, 115, 125)
COLOR_KNOB_SHADOW = (50, 52, 58)

# Timing
LONG_PRESS_MS = 600
FLASH_MS = 800


def find_cjk_font() -> Optional[str]:
    """Try to locate a system font that covers CJK characters."""
    candidates = [
        "Noto Sans CJK SC",
        "Noto Sans CJK TC",
        "Noto Sans CJK JP",
        "Noto Sans Mono CJK SC",
        "WenQuanYi Micro Hei",
        "WenQuanYi Zen Hei",
        "Source Han Sans SC",
        "Source Han Serif SC",
        "Droid Sans Fallback",
        "SimHei",
        "SimSun",
        "Microsoft YaHei",
        "NSimSun",
        "AR PL UMing CN",
        "AR PL UKai CN",
        "DejaVu Sans",
    ]
    for name in candidates:
        path = pygame.font.match_font(name)
        if path:
            return path
    return None


def set_window_always_on_top() -> None:
    """Raise the pygame window above all others (Linux/X11 via wmctrl)."""
    try:
        info = pygame.display.get_wm_info()
        win_id = info.get("window")
        if win_id is None:
            return
        # wmctrl is commonly installed on X11; xprop fallback.
        import subprocess

        subprocess.run(
            ["wmctrl", "-i", "-r", str(win_id), "-b", "add,above"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        pass


@dataclass
class Candidate:
    text: str
    last_used: int = 0


@dataclass
class Profile:
    name: str
    candidates: list[Candidate] = field(default_factory=list)
    last_used: int = 0


class UsageStore:
    """Persists last-used order numbers for profiles and candidates.

    Uses a monotonic order scheme so ordering survives app restarts and is
    independent of any real-time clock. Order numbers are normalized on save
    to stay small and consecutive.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self.data: dict = {"profiles": {}, "candidates": {}}
        self.load()

    def load(self) -> None:
        if self.path.exists():
            try:
                self.data = json.loads(self.path.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, OSError):
                self.data = {"profiles": {}, "candidates": {}}
        # Ensure required keys exist; drop any legacy counter field.
        self.data.setdefault("profiles", {})
        self.data.setdefault("candidates", {})
        self.data.setdefault("ui_state", {})
        self.data.pop("counter", None)

    def _all_values(self) -> list[int]:
        return [
            int(v)
            for v in list(self.data.get("profiles", {}).values())
            + list(self.data.get("candidates", {}).values())
        ]

    def _next_order(self) -> int:
        values = self._all_values()
        return max(values, default=0) + 1

    def normalize(self) -> None:
        """Compact order numbers to 0..n-1 while preserving relative order."""
        profiles = self.data.get("profiles", {})
        candidates = self.data.get("candidates", {})
        values = sorted(set(profiles.values()) | set(candidates.values()))
        if not values:
            return
        mapping = {old: new for new, old in enumerate(values)}
        self.data["profiles"] = {name: mapping[val] for name, val in profiles.items()}
        self.data["candidates"] = {key: mapping[val] for key, val in candidates.items()}

    def save(self) -> None:
        self.normalize()
        self.path.write_text(json.dumps(self.data, ensure_ascii=False, indent=2), encoding="utf-8")

    def knob_angle(self) -> Optional[float]:
        if "knob_angle" in self.data:
            return float(self.data["knob_angle"])
        return None

    def set_knob_angle(self, angle: float) -> None:
        self.data["knob_angle"] = angle

    def set_ui_state(
        self,
        view: str,
        profile_name: Optional[str],
        candidate_text: Optional[str] = None,
    ) -> None:
        """Persist last highlighted view/profile/candidate for resume."""
        self.data["ui_state"] = {
            "view": view,
            "profile": profile_name,
            "candidate": candidate_text,
        }

    def get_ui_state(self) -> dict:
        """Return saved UI state, defaulting to the profile-select homepage."""
        return self.data.get("ui_state", {}) or {"view": "profiles", "profile": None, "candidate": None}

    def profile_time(self, name: str) -> int:
        return int(self.data.get("profiles", {}).get(name, 0))

    def candidate_time(self, profile_name: str, text: str) -> int:
        key = f"{profile_name}::{text}"
        return int(self.data.get("candidates", {}).get(key, 0))

    def touch_profile(self, name: str) -> int:
        order = self._next_order()
        self.data.setdefault("profiles", {})[name] = order
        return order

    def touch_candidate(self, profile_name: str, text: str) -> int:
        key = f"{profile_name}::{text}"
        order = self._next_order()
        self.data.setdefault("candidates", {})[key] = order
        return order


def load_profiles(store: UsageStore) -> list[Profile]:
    """Load profiles from disk and merge persisted last-used times."""
    profiles: list[Profile] = []
    if not PROFILES_DIR.exists():
        PROFILES_DIR.mkdir(parents=True, exist_ok=True)

    for txt_file in sorted(PROFILES_DIR.glob("*.txt")):
        name = txt_file.stem
        seen: set[str] = set()
        candidates: list[Candidate] = []
        for line in txt_file.read_text(encoding="utf-8").splitlines():
            text = line.strip()
            if not text or text in seen:
                continue
            seen.add(text)
            candidates.append(Candidate(text=text, last_used=store.candidate_time(name, text)))
        # Most recently used first; never used at the end.
        candidates.sort(key=lambda c: c.last_used, reverse=True)
        profiles.append(Profile(name=name, candidates=candidates, last_used=store.profile_time(name)))

    profiles.sort(key=lambda p: p.last_used, reverse=True)
    return profiles


class App:
    def __init__(self) -> None:
        pygame.init()
        pygame.display.set_caption("Macro Keyboard Prototype")
        self.window = pygame.display.set_mode((SCREEN_W, SCREEN_H))
        set_window_always_on_top()
        self.clock = pygame.time.Clock()
        self.font_path = find_cjk_font()

        # Two sizes: title for headers, body for list items.
        self.font_title = pygame.font.Font(self.font_path, 22)
        self.font_body = pygame.font.Font(self.font_path, 18)

        self.store = UsageStore(USAGE_FILE)
        self.profiles = load_profiles(self.store)

        # State
        self.view: str = "profiles"  # "profiles" | "candidates"
        self.profile_index: int = 0
        self.candidate_index: int = 0
        self.flash_until: int = 0
        self.flash_text: str = ""

        # Knob / button state
        self.knob_pressed: bool = False
        self.press_start_ms: int = 0
        self.long_triggered: bool = False
        self.hold_invalidated: bool = False  # true if dial moved during this hold

        # Physical knob angle — independent of list selection.
        saved_angle = self.store.knob_angle()
        self.knob_angle: float = saved_angle if saved_angle is not None else -math.pi / 2
        self.knob_rotation_accum: float = 0.0
        self.knob_step_rad: float = 0.35  # ~20 degrees per detent

        # Mouse drag-knob state
        self.mouse_on_knob: bool = False
        self.mouse_drag_start_pos: Optional[tuple[int, int]] = None
        self.is_dragging: bool = False
        self.last_drag_angle: float = 0.0
        self.drag_start_threshold_px: int = 5

        # Restore last highlighted view / profile / candidate.
        self._restore_ui_state()

    def _restore_ui_state(self) -> None:
        """Resume view and selection from usage.json, falling back to homepage."""
        state = self.store.get_ui_state()
        view = state.get("view", "profiles")
        profile_name = state.get("profile")
        candidate_text = state.get("candidate")

        if view == "candidates" and profile_name:
            for i, prof in enumerate(self.profiles):
                if prof.name == profile_name:
                    self.view = "candidates"
                    self.profile_index = i
                    self.candidate_index = 0
                    if candidate_text:
                        for j, cand in enumerate(prof.candidates):
                            if cand.text == candidate_text:
                                self.candidate_index = j
                                break
                    return

        # Fallback: profile select homepage.
        self.view = "profiles"
        self.profile_index = 0
        self.candidate_index = 0

    def _save_ui_state(self) -> None:
        """Persist current view and highlighted profile/candidate."""
        profile_name: Optional[str] = None
        candidate_text: Optional[str] = None
        if self.view == "candidates":
            prof = self.current_profile()
            if prof:
                profile_name = prof.name
                if 0 <= self.candidate_index < len(prof.candidates):
                    candidate_text = prof.candidates[self.candidate_index].text
        else:
            prof = self.current_profile()
            if prof:
                profile_name = prof.name
        self.store.set_ui_state(self.view, profile_name, candidate_text)
        self.store.save()

    def current_profile(self) -> Optional[Profile]:
        if 0 <= self.profile_index < len(self.profiles):
            return self.profiles[self.profile_index]
        return None

    def _sync_last_used_from_store(self) -> None:
        """Refresh Profile/Candidate last_used from the in-memory store."""
        for p in self.profiles:
            p.last_used = self.store.profile_time(p.name)
            for c in p.candidates:
                c.last_used = self.store.candidate_time(p.name, c.text)

    def open_current_profile(self) -> None:
        prof = self.current_profile()
        if not prof or not prof.candidates:
            return
        current_profile_name = prof.name
        prof.last_used = self.store.touch_profile(prof.name)
        self.profiles.sort(key=lambda p: p.last_used, reverse=True)
        # Keep the highlight on the profile we just entered.
        self.profile_index = next(
            (i for i, p in enumerate(self.profiles) if p.name == current_profile_name),
            self.profile_index,
        )
        self.view = "candidates"
        self.candidate_index = 0
        self._save_ui_state()
        self._sync_last_used_from_store()

    def copy_current_candidate(self) -> None:
        prof = self.current_profile()
        if not prof:
            return
        if not (0 <= self.candidate_index < len(prof.candidates)):
            return
        cand = prof.candidates[self.candidate_index]
        current_profile_name = prof.name
        pyperclip.copy(cand.text)

        now = self.store.touch_candidate(prof.name, cand.text)
        self.store.touch_profile(prof.name)
        cand.last_used = now
        prof.last_used = now
        # Re-sort in-place so next visits feel smart.
        prof.candidates.sort(key=lambda c: c.last_used, reverse=True)
        self.profiles.sort(key=lambda p: p.last_used, reverse=True)
        # Keep the selection pointing at the profile we are still inside.
        self.profile_index = next(
            (i for i, p in enumerate(self.profiles) if p.name == current_profile_name),
            self.profile_index,
        )
        # Highlight the newly-copied (now top) item; knob stays where it is.
        self.candidate_index = 0
        self._save_ui_state()
        self._sync_last_used_from_store()

        self.flash_text = f"Copied: {cand.text[:20]}"
        self.flash_until = pygame.time.get_ticks() + FLASH_MS

    def go_back(self) -> None:
        if self.view == "candidates":
            self.view = "profiles"
            self.candidate_index = 0
            self._save_ui_state()

    def handle_event(self, event: pygame.event.Event) -> bool:
        """Process input. Returns False when the app should quit."""
        now_ms = pygame.time.get_ticks()

        if event.type == pygame.QUIT:
            self._save_ui_state()
            return False

        if event.type == pygame.KEYDOWN:
            if event.key in (pygame.K_UP, pygame.K_w):
                self.rotate_knob(-self.knob_step_rad)
                self.store.save()
            elif event.key in (pygame.K_DOWN, pygame.K_s):
                self.rotate_knob(self.knob_step_rad)
                self.store.save()
            elif event.key == pygame.K_LEFT:
                self.go_back()
            elif event.key == pygame.K_RIGHT:
                self.do_short_press()
            elif event.key in (pygame.K_RETURN, pygame.K_SPACE, pygame.K_KP_ENTER):
                self.press_start_ms = now_ms
                self.knob_pressed = True
                self.long_triggered = False
                self.hold_invalidated = False
            elif event.key in (pygame.K_ESCAPE, pygame.K_BACKSPACE):
                self.go_back()
            elif event.key == pygame.K_q:
                self._save_ui_state()
                return False

        elif event.type == pygame.KEYUP:
            if event.key in (pygame.K_RETURN, pygame.K_SPACE, pygame.K_KP_ENTER) and self.knob_pressed:
                duration = now_ms - self.press_start_ms
                self.knob_pressed = False
                if duration < LONG_PRESS_MS and not self.long_triggered and not self.hold_invalidated:
                    self.do_short_press()

        elif event.type == pygame.MOUSEBUTTONDOWN:
            mx, my = event.pos
            dist = math.hypot(mx - KNOB_CENTER_X, my - KNOB_CENTER_Y)
            if event.button in (1, 3) and dist <= KNOB_RADIUS:
                self.mouse_on_knob = True
                self.mouse_drag_start_pos = event.pos
                self.is_dragging = False
                self.last_drag_angle = math.atan2(my - KNOB_CENTER_Y, mx - KNOB_CENTER_X)
                self.press_start_ms = now_ms
                self.knob_pressed = True
                self.long_triggered = False
                self.hold_invalidated = False

        elif event.type == pygame.MOUSEMOTION:
            if self.mouse_on_knob and self.mouse_drag_start_pos is not None:
                mx, my = event.pos
                dx = mx - self.mouse_drag_start_pos[0]
                dy = my - self.mouse_drag_start_pos[1]
                if not self.is_dragging and math.hypot(dx, dy) > self.drag_start_threshold_px:
                    self.is_dragging = True

                if self.is_dragging:
                    angle = math.atan2(my - KNOB_CENTER_Y, mx - KNOB_CENTER_X)
                    delta = angle - self.last_drag_angle
                    # Normalize to [-pi, pi]
                    while delta > math.pi:
                        delta -= 2 * math.pi
                    while delta < -math.pi:
                        delta += 2 * math.pi
                    self.last_drag_angle = angle
                    self.rotate_knob(delta)
        elif event.type == pygame.MOUSEBUTTONUP:
            if event.button in (1, 3) and self.mouse_on_knob:
                self.mouse_on_knob = False
                self.knob_pressed = False
                if self.is_dragging:
                    self.is_dragging = False
                    self.store.save()
                else:
                    duration = now_ms - self.press_start_ms
                    if duration < LONG_PRESS_MS and not self.long_triggered and not self.hold_invalidated:
                        self.do_short_press()
                self.mouse_drag_start_pos = None

        elif event.type == pygame.MOUSEWHEEL:
            self.rotate_knob(-event.y * self.knob_step_rad)
            self.store.save()

        return True

    def rotate_knob(self, delta_angle: float) -> None:
        """Rotate the physical knob; advance the list highlight by threshold steps."""
        self.knob_angle += delta_angle
        self.store.set_knob_angle(self.knob_angle)
        self.knob_rotation_accum += delta_angle
        while self.knob_rotation_accum >= self.knob_step_rad:
            self.move_highlight(1)
            self.knob_rotation_accum -= self.knob_step_rad
        while self.knob_rotation_accum <= -self.knob_step_rad:
            self.move_highlight(-1)
            self.knob_rotation_accum += self.knob_step_rad

    def move_highlight(self, delta: int) -> None:
        if self.view == "profiles":
            old_index = self.profile_index
            self.profile_index = (self.profile_index + delta) % max(1, len(self.profiles))
            changed = self.profile_index != old_index
        else:
            prof = self.current_profile()
            count = len(prof.candidates) if prof else 0
            old_index = self.candidate_index
            self.candidate_index = (self.candidate_index + delta) % max(1, count)
            changed = self.candidate_index != old_index
        if changed and self.knob_pressed:
            self.hold_invalidated = True
        if changed:
            self._save_ui_state()

    def do_short_press(self) -> None:
        if self.view == "profiles":
            self.open_current_profile()
        else:
            self.copy_current_candidate()

    def update(self) -> None:
        now_ms = pygame.time.get_ticks()
        if self.knob_pressed and not self.long_triggered and not self.hold_invalidated:
            if now_ms - self.press_start_ms >= LONG_PRESS_MS:
                self.long_triggered = True
                self.knob_pressed = False
                self.go_back()

    def draw(self) -> None:
        self.window.fill(COLOR_BG)

        # Device body
        pygame.draw.rect(
            self.window,
            COLOR_DEVICE,
            (DEVICE_X, DEVICE_Y, DEVICE_W, DEVICE_H),
            border_radius=DEVICE_RADIUS,
        )
        pygame.draw.rect(
            self.window,
            (20, 22, 26),
            (DEVICE_X, DEVICE_Y, DEVICE_W, DEVICE_H),
            width=3,
            border_radius=DEVICE_RADIUS,
        )

        # Display screen
        pygame.draw.rect(
            self.window,
            COLOR_DISPLAY_BG,
            (DISPLAY_X, DISPLAY_Y, DISPLAY_W, DISPLAY_H),
            border_radius=8,
        )
        pygame.draw.rect(
            self.window,
            COLOR_DISPLAY_BORDER,
            (DISPLAY_X, DISPLAY_Y, DISPLAY_W, DISPLAY_H),
            width=2,
            border_radius=8,
        )

        # Header
        header = "Profiles" if self.view == "profiles" else (self.current_profile().name if self.current_profile() else "")
        header_surf = self.font_title.render(header, True, COLOR_TEXT)
        self.window.blit(header_surf, (DISPLAY_X + 12, DISPLAY_Y + 10))

        # Thin separator between title and list
        separator_y = DISPLAY_Y + 40
        pygame.draw.line(
            self.window,
            COLOR_DISPLAY_BORDER,
            (DISPLAY_X + 12, separator_y),
            (DISPLAY_X + DISPLAY_W - 12, separator_y),
            width=1,
        )

        # List content
        if self.view == "profiles":
            self.draw_list(
                [p.name for p in self.profiles],
                self.profile_index,
                DISPLAY_X + 12,
                DISPLAY_Y + 48,
                DISPLAY_W - 24,
                DISPLAY_H - 58,
            )
        else:
            prof = self.current_profile()
            items = [c.text for c in prof.candidates] if prof else []
            self.draw_list(
                items,
                self.candidate_index,
                DISPLAY_X + 12,
                DISPLAY_Y + 48,
                DISPLAY_W - 24,
                DISPLAY_H - 58,
            )

        # Knob
        self.draw_knob()

        # Flash overlay
        now_ms = pygame.time.get_ticks()
        if now_ms < self.flash_until:
            flash_surf = self.font_title.render(self.flash_text, True, (50, 50, 50))
            pad = 10
            bg_rect = flash_surf.get_rect(center=(SCREEN_W // 2, DISPLAY_Y + DISPLAY_H // 2))
            bg_rect.inflate_ip(pad * 2, pad * 2)
            pygame.draw.rect(self.window, (230, 240, 200), bg_rect, border_radius=6)
            pygame.draw.rect(self.window, (80, 90, 60), bg_rect, width=2, border_radius=6)
            self.window.blit(flash_surf, flash_surf.get_rect(center=bg_rect.center))

        pygame.display.flip()

    def draw_list(
        self,
        items: list[str],
        selected: int,
        x: int,
        y: int,
        width: int,
        height: int,
    ) -> None:
        line_height = 26
        max_visible = height // line_height
        if not items:
            empty = self.font_body.render("(empty)", True, COLOR_TEXT_DIM)
            self.window.blit(empty, (x, y))
            return

        # Simple scroll window
        start = max(0, selected - max_visible + 2)
        end = min(len(items), start + max_visible)
        visible_items = items[start:end]

        for i, text in enumerate(visible_items):
            idx = start + i
            display_text = text[:32] + "…" if len(text) > 33 else text
            if idx == selected:
                pygame.draw.rect(
                    self.window,
                    COLOR_HIGHLIGHT,
                    (x - 4, y + i * line_height - 2, width, line_height),
                    border_radius=4,
                )
                color = (255, 255, 255)
            else:
                color = COLOR_TEXT
            surf = self.font_body.render(display_text, True, color)
            self.window.blit(surf, (x, y + i * line_height))

    def draw_knob(self) -> None:
        # Outer ring
        pygame.draw.circle(self.window, COLOR_KNOB_SHADOW, (KNOB_CENTER_X, KNOB_CENTER_Y + 4), KNOB_RADIUS)
        pygame.draw.circle(self.window, COLOR_KNOB, (KNOB_CENTER_X, KNOB_CENTER_Y), KNOB_RADIUS)
        pygame.draw.circle(self.window, COLOR_KNOB_FACE, (KNOB_CENTER_X, KNOB_CENTER_Y), KNOB_INNER_RADIUS)

        if self.knob_pressed:
            pygame.draw.circle(self.window, COLOR_HIGHLIGHT, (KNOB_CENTER_X, KNOB_CENTER_Y), KNOB_INNER_RADIUS)

        ix = KNOB_CENTER_X + int(KNOB_INNER_RADIUS * 0.6 * math.cos(self.knob_angle))
        iy = KNOB_CENTER_Y + int(KNOB_INNER_RADIUS * 0.6 * math.sin(self.knob_angle))
        pygame.draw.line(
            self.window,
            (40, 44, 48),
            (KNOB_CENTER_X, KNOB_CENTER_Y),
            (ix, iy),
            width=4,
        )

    def run(self) -> None:
        running = True
        while running:
            for event in pygame.event.get():
                if not self.handle_event(event):
                    running = False
            self.update()
            self.draw()
            self.clock.tick(FPS)
        pygame.quit()
        sys.exit()


if __name__ == "__main__":
    app = App()
    app.run()
